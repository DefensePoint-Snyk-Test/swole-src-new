/*
  +----------------------------------------------------------------------+
  | Swoole                                                               |
  +----------------------------------------------------------------------+
  | This source file is subject to version 2.0 of the Apache license,    |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.apache.org/licenses/LICENSE-2.0.html                      |
  | If you did not receive a copy of the Apache2.0 license and are unable|
  | to obtain it through the world-wide-web, please send a note to       |
  | license@swoole.com so we can mail you a copy immediately.            |
  +----------------------------------------------------------------------+
  | Author: Tianfeng Han  <mikan.tenny@gmail.com>                        |
  +----------------------------------------------------------------------+
*/

#include "php_swoole.h"
#include "swoole_http.h"
#include "swoole_coroutine.h"

extern "C"
{
#include "ext/standard/url.h"
#include "ext/standard/sha1.h"
#include "ext/standard/php_var.h"
#include "ext/standard/php_string.h"
#include "ext/standard/php_math.h"
#include "ext/standard/php_array.h"
#include "ext/date/php_date.h"
#include "ext/standard/md5.h"
}

#include "main/rfc1867.h"
#include "main/php_variables.h"

#include "websocket.h"
#include "connection.h"
#include "base64.h"

#ifdef SW_HAVE_ZLIB
#include <zlib.h>
#endif

#ifdef SW_HAVE_BROTLI
#include <brotli/encode.h>
#endif

#ifdef SW_USE_HTTP2
#include "http2.h"
#endif

#ifdef SW_USE_PICOHTTPPARSER
#include "thirdparty/picohttpparser/picohttpparser.h"
#endif

using namespace swoole;

swString *swoole_http_buffer;
#ifdef SW_HAVE_ZLIB
swString *swoole_zlib_buffer;
#endif
swString *swoole_http_form_data_buffer;

enum http_global_flag
{
    HTTP_GLOBAL_GET       = 1u << 1,
    HTTP_GLOBAL_POST      = 1u << 2,
    HTTP_GLOBAL_COOKIE    = 1u << 3,
    HTTP_GLOBAL_REQUEST   = 1u << 4,
    HTTP_GLOBAL_SERVER    = 1u << 5,
    HTTP_GLOBAL_FILES     = 1u << 6,
};

enum http_upload_errno
{
    HTTP_UPLOAD_ERR_OK = 0,
    HTTP_UPLOAD_ERR_INI_SIZE,
    HTTP_UPLOAD_ERR_FORM_SIZE,
    HTTP_UPLOAD_ERR_PARTIAL,
    HTTP_UPLOAD_ERR_NO_FILE,
    HTTP_UPLOAD_ERR_NO_TMP_DIR = 6,
    HTTP_UPLOAD_ERR_CANT_WRITE,
};

static zend_class_entry swoole_http_server_ce;
zend_class_entry *swoole_http_server_ce_ptr;
zend_object_handlers swoole_http_server_handlers;

static zend_class_entry swoole_http_response_ce;
zend_class_entry *swoole_http_response_ce_ptr;
static zend_object_handlers swoole_http_response_handlers;

static zend_class_entry swoole_http_request_ce;
zend_class_entry *swoole_http_request_ce_ptr;
static zend_object_handlers swoole_http_request_handlers;

static int http_request_on_path(swoole_http_parser *parser, const char *at, size_t length);
static int http_request_on_query_string(swoole_http_parser *parser, const char *at, size_t length);
static int http_request_on_body(swoole_http_parser *parser, const char *at, size_t length);
static int http_request_on_header_field(swoole_http_parser *parser, const char *at, size_t length);
static int http_request_on_header_value(swoole_http_parser *parser, const char *at, size_t length);
static int http_request_on_headers_complete(swoole_http_parser *parser);
static int http_request_message_complete(swoole_http_parser *parser);

static int multipart_body_on_header_field(multipart_parser* p, const char *at, size_t length);
static int multipart_body_on_header_value(multipart_parser* p, const char *at, size_t length);
static int multipart_body_on_data(multipart_parser* p, const char *at, size_t length);
static int multipart_body_on_header_complete(multipart_parser* p);
static int multipart_body_on_data_end(multipart_parser* p);

static http_context* http_get_context(zval *zobject, int check_end);

static void http_parse_cookie(zval *array, const char *at, size_t length);
static void http_build_header(http_context *, zval *zobject, swString *response, int body_length);

static inline void http_header_key_format(char *key, int length)
{
    int i, state = 0;
    for (i = 0; i < length; i++)
    {
        if (state == 0)
        {
            if (key[i] >= 97 && key[i] <= 122)
            {
                key[i] -= 32;
            }
            state = 1;
        }
        else if (key[i] == '-')
        {
            state = 0;
        }
        else
        {
            if (key[i] >= 65 && key[i] <= 90)
            {
                key[i] += 32;
            }
        }
    }
}

static inline char* http_trim_double_quote(char *ptr, int *len)
{
    int i;
    char *tmp = ptr;

    //ltrim('"')
    for (i = 0; i < *len; i++)
    {
        if (tmp[0] == '"')
        {
            (*len)--;
            tmp++;
            continue;
        }
        else
        {
            break;
        }
    }
    //rtrim('"')
    for (i = (*len) - 1; i >= 0; i--)
    {
        if (tmp[i] == '"')
        {
            tmp[i] = 0;
            (*len)--;
            continue;
        }
        else
        {
            break;
        }
    }
    return tmp;
}

#ifdef SW_USE_PICOHTTPPARSER
enum flags
{
    F_CONNECTION_KEEP_ALIVE = 1 << 1,
    F_CONNECTION_CLOSE = 1 << 2,
};

static inline long http_fast_parse(swoole_http_parser *parser, char *data, size_t length)
{
    http_context *ctx = (http_context *) parser->data;
    const char *method;
    size_t method_len;
    int minor_version;
    struct phr_header headers[64];
    size_t num_headers = sizeof(headers) / sizeof(headers[0]);
    const char *path;
    size_t path_len;

    int n = phr_parse_request(data, length, &method, &method_len, &path, &path_len, &minor_version, headers, &num_headers, 0);
    if (n < 0)
    {
        return SW_ERR;
    }

    char *p = memchr(path, '?', path_len);
    if (p)
    {
        http_request_on_path(parser, path, p - path);
        http_request_on_query_string(parser, p + 1, path + path_len - p - 1);
    }
    else
    {
        http_request_on_path(parser, path, path_len);
    }

    int i;
    for (i = 0; i < num_headers; i++)
    {
        if (strncasecmp(headers[i].name, "Connection", headers[i].name_len) == 0
                && strncasecmp(headers[i].value, "keep-alive", headers[i].value_len) == 0)
        {
            parser->flags |= F_CONNECTION_KEEP_ALIVE;
        }
        else
        {
            parser->flags |= F_CONNECTION_CLOSE;
        }
        if (http_request_on_header_field(parser, headers[i].name, headers[i].name_len) < 0)
        {
            return SW_ERR;
        }
        if (http_request_on_header_value(parser, headers[i].value, headers[i].value_len) < 0)
        {
            return SW_ERR;
        }
    }
    parser->method = swHttp_get_method(method, method_len) - 1;
    parser->http_major = 1;
    parser->http_minor = minor_version;
    ctx->request.version = 100 + minor_version;
    if (n < length)
    {
        http_request_on_body(parser, data + n, length - n);
    }
    http_request_on_headers_complete(parser);
    return SW_OK;
}
#endif

static PHP_METHOD(swoole_http_request, getData);
static PHP_METHOD(swoole_http_request, rawcontent);
static PHP_METHOD(swoole_http_request, __destruct);

static PHP_METHOD(swoole_http_response, write);
static PHP_METHOD(swoole_http_response, end);
static PHP_METHOD(swoole_http_response, sendfile);
static PHP_METHOD(swoole_http_response, redirect);
static PHP_METHOD(swoole_http_response, cookie);
static PHP_METHOD(swoole_http_response, rawcookie);
static PHP_METHOD(swoole_http_response, header);
static PHP_METHOD(swoole_http_response, initHeader);
static PHP_METHOD(swoole_http_response, detach);
static PHP_METHOD(swoole_http_response, create);
static PHP_METHOD(swoole_http_response, gzip);
#ifdef SW_USE_HTTP2
static PHP_METHOD(swoole_http_response, trailer);
#endif
static PHP_METHOD(swoole_http_response, status);
static PHP_METHOD(swoole_http_response, __destruct);

static sw_inline const char* http_get_method_name(int method)
{
    switch (method)
    {
    case PHP_HTTP_GET:
        return "GET";
    case PHP_HTTP_POST:
        return "POST";
    case PHP_HTTP_HEAD:
        return "HEAD";
    case PHP_HTTP_PUT:
        return "PUT";
    case PHP_HTTP_DELETE:
        return "DELETE";
    case PHP_HTTP_PATCH:
        return "PATCH";
    case PHP_HTTP_CONNECT:
        return "CONNECT";
    case PHP_HTTP_OPTIONS:
        return "OPTIONS";
    case PHP_HTTP_TRACE:
        return "TRACE";
    case PHP_HTTP_COPY:
        return "COPY";
    case PHP_HTTP_LOCK:
        return "LOCK";
    case PHP_HTTP_MKCOL:
        return "MKCOL";
    case PHP_HTTP_MOVE:
        return "MOVE";
    case PHP_HTTP_PROPFIND:
        return "PROPFIND";
    case PHP_HTTP_PROPPATCH:
        return "PROPPATCH";
    case PHP_HTTP_UNLOCK:
        return "UNLOCK";
        /* subversion */
    case PHP_HTTP_REPORT:
        return "REPORT";
    case PHP_HTTP_MKACTIVITY:
        return "MKACTIVITY";
    case PHP_HTTP_CHECKOUT:
        return "CHECKOUT";
    case PHP_HTTP_MERGE:
        return "MERGE";
        /* upnp */
    case PHP_HTTP_MSEARCH:
        return "MSEARCH";
    case PHP_HTTP_NOTIFY:
        return "NOTIFY";
    case PHP_HTTP_SUBSCRIBE:
        return "SUBSCRIBE";
    case PHP_HTTP_UNSUBSCRIBE:
        return "UNSUBSCRIBE";
    case PHP_HTTP_NOT_IMPLEMENTED:
        return "IMPLEMENTED";
    default:
        return NULL;
    }
}

ZEND_BEGIN_ARG_INFO_EX(arginfo_swoole_http_void, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_swoole_http_response_gzip, 0, 0, 0)
    ZEND_ARG_INFO(0, compress_level)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_swoole_http_response_status, 0, 0, 1)
    ZEND_ARG_INFO(0, http_code)
    ZEND_ARG_INFO(0, reason)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_swoole_http_response_header, 0, 0, 2)
    ZEND_ARG_INFO(0, key)
    ZEND_ARG_INFO(0, value)
    ZEND_ARG_INFO(0, ucwords)
ZEND_END_ARG_INFO()

#ifdef SW_USE_HTTP2
ZEND_BEGIN_ARG_INFO_EX(arginfo_swoole_http_response_trailer, 0, 0, 2)
    ZEND_ARG_INFO(0, key)
    ZEND_ARG_INFO(0, value)
    ZEND_ARG_INFO(0, ucwords)
ZEND_END_ARG_INFO()
#endif

ZEND_BEGIN_ARG_INFO_EX(arginfo_swoole_http_response_cookie, 0, 0, 1)
    ZEND_ARG_INFO(0, name)
    ZEND_ARG_INFO(0, value)
    ZEND_ARG_INFO(0, expires)
    ZEND_ARG_INFO(0, path)
    ZEND_ARG_INFO(0, domain)
    ZEND_ARG_INFO(0, secure)
    ZEND_ARG_INFO(0, httponly)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_swoole_http_response_write, 0, 0, 1)
    ZEND_ARG_INFO(0, content)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_swoole_http_response_end, 0, 0, 0)
    ZEND_ARG_INFO(0, content)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_swoole_http_response_sendfile, 0, 0, 1)
    ZEND_ARG_INFO(0, filename)
    ZEND_ARG_INFO(0, offset)
    ZEND_ARG_INFO(0, length)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_swoole_http_response_redirect, 0, 0, 1)
    ZEND_ARG_INFO(0, location)
    ZEND_ARG_INFO(0, http_code)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_swoole_http_response_create, 0, 0, 1)
    ZEND_ARG_INFO(0, fd)
ZEND_END_ARG_INFO()

static const swoole_http_parser_settings http_parser_settings =
{
    NULL,
    http_request_on_path,
    http_request_on_query_string,
    NULL,
    NULL,
    http_request_on_header_field,
    http_request_on_header_value,
    http_request_on_headers_complete,
    http_request_on_body,
    http_request_message_complete
};

static const multipart_parser_settings mt_parser_settings =
{
    multipart_body_on_header_field,
    multipart_body_on_header_value,
    multipart_body_on_data,
    NULL,
    multipart_body_on_header_complete,
    multipart_body_on_data_end,
    NULL,
};

const zend_function_entry swoole_http_request_methods[] =
{
    PHP_ME(swoole_http_request, rawcontent, arginfo_swoole_http_void, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_http_request, getData, arginfo_swoole_http_void, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_http_request, __destruct, arginfo_swoole_http_void, ZEND_ACC_PUBLIC)
    PHP_FE_END
};

const zend_function_entry swoole_http_response_methods[] =
{
    PHP_ME(swoole_http_response, initHeader, arginfo_swoole_http_void, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_http_response, cookie, arginfo_swoole_http_response_cookie, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_http_response, rawcookie, arginfo_swoole_http_response_cookie, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_http_response, status, arginfo_swoole_http_response_status, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_http_response, gzip, arginfo_swoole_http_response_gzip, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_http_response, header, arginfo_swoole_http_response_header, ZEND_ACC_PUBLIC)
#ifdef SW_USE_HTTP2
    PHP_ME(swoole_http_response, trailer, arginfo_swoole_http_response_trailer, ZEND_ACC_PUBLIC)
#endif
    PHP_ME(swoole_http_response, write, arginfo_swoole_http_response_write, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_http_response, end, arginfo_swoole_http_response_end, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_http_response, sendfile, arginfo_swoole_http_response_sendfile, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_http_response, redirect, arginfo_swoole_http_response_redirect, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_http_response, detach, arginfo_swoole_http_void, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_http_response, create, arginfo_swoole_http_response_create, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(swoole_http_response, __destruct, arginfo_swoole_http_void, ZEND_ACC_PUBLIC)
    PHP_FE_END
};

static int http_request_on_path(swoole_http_parser *parser, const char *at, size_t length)
{
    http_context *ctx = (http_context *) parser->data;
    ctx->request.path = estrndup(at, length);
    ctx->request.path_len = length;
    return 0;
}

static int http_request_on_query_string(swoole_http_parser *parser, const char *at, size_t length)
{
    http_context *ctx = (http_context *) parser->data;

    //no need free, will free by treat_data
    char *query = estrndup(at, length);
    add_assoc_stringl_ex(ctx->request.zserver, ZEND_STRL("query_string"), query, length);

    zval *zrequest_object = ctx->request.zobject;
    zval *zget;
    swoole_http_server_array_init(get, request);

    //parse url params
    sapi_module.treat_data(PARSE_STRING, query, zget);

    return 0;
}

static int http_request_on_header_field(swoole_http_parser *parser, const char *at, size_t length)
{
    http_context *ctx = (http_context *) parser->data;
    ctx->current_header_name = (char *) at;
    ctx->current_header_name_len = length;
    return 0;
}

int swoole_http_parse_form_data(http_context *ctx, const char *boundary_str, int boundary_len)
{
    multipart_parser *mt_parser = multipart_parser_init(boundary_str, boundary_len, &mt_parser_settings);
    if (!mt_parser)
    {
        swoole_php_fatal_error(E_WARNING, "multipart_parser_init() failed.");
        return SW_ERR;
    }

    ctx->mt_parser = mt_parser;
    mt_parser->data = ctx;

    return SW_OK;
}

static void http_parse_cookie(zval *array, const char *at, size_t length)
{
    char keybuf[SW_HTTP_COOKIE_KEYLEN];
    char valbuf[SW_HTTP_COOKIE_VALLEN];
    char *_c = (char *) at;

    char *_value;
    int klen = 0;
    int vlen = 0;
    int state = -1;

    int i = 0, j = 0;
    while (_c < at + length)
    {
        if (state <= 0 && *_c == '=')
        {
            klen = i - j + 1;
            if (klen >= SW_HTTP_COOKIE_KEYLEN)
            {
                swWarn("cookie key is too large.");
                return;
            }
            memcpy(keybuf, at + j, klen - 1);
            keybuf[klen - 1] = 0;

            j = i + 1;
            state = 1;
        }
        else if (state == 1 && *_c == ';')
        {
            vlen = i - j;
            if (vlen >= SW_HTTP_COOKIE_VALLEN)
            {
                swWarn("cookie value is too large.");
                return;
            }
            memcpy(valbuf, (char *) at + j, vlen);
            valbuf[vlen] = 0;
            _value = http_trim_double_quote(valbuf, &vlen);
            vlen = php_url_decode(_value, vlen);
            if (klen > 1)
            {
                add_assoc_stringl_ex(array, keybuf, klen - 1, _value, vlen);
            }
            j = i + 1;
            state = -1;
        }
        else if (state < 0)
        {
            if (isspace(*_c))
            {
                //Remove leading spaces from cookie names
                ++j;
            }
            else
            {
                state = 0;
            }
        }
        _c++;
        i++;
    }
    if (j < (off_t) length)
    {
        vlen = i - j;
        if (klen >= SW_HTTP_COOKIE_KEYLEN)
        {
            swWarn("cookie key is too large.");
            return;
        }
        keybuf[klen - 1] = 0;
        if (vlen >= SW_HTTP_COOKIE_VALLEN)
        {
            swWarn("cookie value is too large.");
            return;
        }
        memcpy(valbuf, (char *) at + j, vlen);
        valbuf[vlen] = 0;
        _value = http_trim_double_quote(valbuf, &vlen);
        vlen = php_url_decode(_value, vlen);
        if (klen > 1)
        {
            add_assoc_stringl_ex(array, keybuf, klen - 1, _value, vlen);
        }
    }
}

static int http_request_on_header_value(swoole_http_parser *parser, const char *at, size_t length)
{
    size_t offset = 0;
    http_context *ctx = (http_context *) parser->data;
    zval *zrequest_object = ctx->request.zobject;
    zval *header = ctx->request.zheader;
    size_t header_len = ctx->current_header_name_len;
    char *header_name = zend_str_tolower_dup(ctx->current_header_name, header_len);

    if (strncmp(header_name, "cookie", header_len) == 0)
    {
        zval *zcookie;
        swoole_http_server_array_init(cookie, request);
        http_parse_cookie(zcookie, at, length);
        efree(header_name);
        return 0;
    }
    else if (strncmp(header_name, "upgrade", header_len) == 0 && strncasecmp(at, "websocket", length) == 0)
    {
        swConnection *conn = swWorker_get_connection(SwooleG.serv, ctx->fd);
        if (!conn)
        {
            swWarn("connection[%d] is closed.", ctx->fd);
            return SW_ERR;
        }
        swListenPort *port = (swListenPort *) SwooleG.serv->connection_list[conn->from_fd].object;
        if (port->open_websocket_protocol)
        {
            conn->websocket_status = WEBSOCKET_STATUS_CONNECTION;
        }
    }
    else if (parser->method == PHP_HTTP_POST || parser->method == PHP_HTTP_PUT || parser->method == PHP_HTTP_DELETE || parser->method == PHP_HTTP_PATCH)
    {
        if (strncmp(header_name, "content-type", header_len) == 0)
        {
            if (http_strncasecmp("application/x-www-form-urlencoded", at, length))
            {
                ctx->request.post_form_urlencoded = 1;
            }
            else if (http_strncasecmp("multipart/form-data", at, length))
            {
                // start offset
                offset = sizeof("multipart/form-data;") - 1;
                while (at[offset] == ' ')
                {
                    offset++;
                }
                offset += sizeof("boundary=") - 1;

                int boundary_len = length - offset;
                char *boundary_str = (char *) at + offset;

                // find ';'
                char *tmp = (char*) memchr(boundary_str, ';', boundary_len);
                if (tmp)
                {
                    boundary_len = tmp - boundary_str;
                }
                if (boundary_len <= 0)
                {
                    swWarn("invalid multipart/form-data body fd:%d.", ctx->fd);
                    return 0;
                }
                // trim '"'
                if (boundary_len >= 2 && boundary_str[0] == '"' && *(boundary_str + boundary_len - 1) == '"')
                {
                    boundary_str++;
                    boundary_len -= 2;
                }
                swoole_http_parse_form_data(ctx, boundary_str, boundary_len);
            }
        }
    }
#ifdef SW_HAVE_ZLIB
    else if (SwooleG.serv->http_compression && strncmp(header_name, "accept-encoding", header_len) == 0)
    {
        swoole_http_get_compression_method(ctx, at, length);
    }
#endif

    add_assoc_stringl_ex(header, header_name, header_len, (char *) at, length);

    efree(header_name);

    return 0;
}

static int http_request_on_headers_complete(swoole_http_parser *parser)
{
    http_context *ctx = (http_context *) parser->data;
    ctx->current_header_name = NULL;

    return 0;
}

static int multipart_body_on_header_field(multipart_parser* p, const char *at, size_t length)
{
    http_context *ctx = (http_context *) p->data;
    return http_request_on_header_field(&ctx->parser, at, length);
}

static int multipart_body_on_header_value(multipart_parser* p, const char *at, size_t length)
{
    char value_buf[SW_HTTP_COOKIE_KEYLEN];
    int value_len;

    http_context *ctx = (http_context *) p->data;
    /**
     * Hash collision attack
     */
    if (ctx->input_var_num > PG(max_input_vars))
    {
        swoole_php_error(E_WARNING, "Input variables exceeded " ZEND_LONG_FMT ". "
                "To increase the limit change max_input_vars in php.ini.", PG(max_input_vars));
        return SW_OK;
    }
    else
    {
        ctx->input_var_num++;
    }

    size_t header_len = ctx->current_header_name_len;
    char *headername = zend_str_tolower_dup(ctx->current_header_name, header_len);

    if (strncasecmp(headername, "content-disposition", header_len) == 0)
    {
        //not form data
        if (swoole_strnpos((char *) at, length, (char *) ZEND_STRL("form-data;")) < 0)
        {
            return SW_OK;
        }

        zval *tmp_array;
        SW_MAKE_STD_ZVAL(tmp_array);
        array_init(tmp_array);
        http_parse_cookie(tmp_array, (char *) at + sizeof("form-data;") - 1, length - sizeof("form-data;") + 1);

        zval *form_name;
        if (!(form_name = zend_hash_str_find(Z_ARRVAL_P(tmp_array), ZEND_STRL("name"))))
        {
            return SW_OK;
        }

        if (Z_STRLEN_P(form_name) >= SW_HTTP_COOKIE_KEYLEN)
        {
            swWarn("form_name[%s] is too large.", Z_STRVAL_P(form_name));
            return SW_OK;
        }

        strncpy(value_buf, Z_STRVAL_P(form_name), Z_STRLEN_P(form_name));
        value_len = Z_STRLEN_P(form_name);
        char *tmp = http_trim_double_quote(value_buf, &value_len);

        zval *filename;
        //POST form data
        if (!(filename = zend_hash_str_find(Z_ARRVAL_P(tmp_array), ZEND_STRL("filename"))))
        {
            ctx->current_form_data_name = estrndup(tmp, value_len);
            ctx->current_form_data_name_len = value_len;
        }
        //upload file
        else
        {
            if (Z_STRLEN_P(filename) >= SW_HTTP_COOKIE_KEYLEN)
            {
                swWarn("filename[%s] is too large.", Z_STRVAL_P(filename));
                return SW_OK;
            }
            ctx->current_input_name = estrndup(tmp, value_len);

            zval *multipart_header = sw_malloc_zval();
            array_init(multipart_header);

            add_assoc_string(multipart_header, "name", (char *) "");
            add_assoc_string(multipart_header, "type", (char *) "");
            add_assoc_string(multipart_header, "tmp_name", (char *) "");
            add_assoc_long(multipart_header, "error", HTTP_UPLOAD_ERR_OK);
            add_assoc_long(multipart_header, "size", 0);

            strncpy(value_buf, Z_STRVAL_P(filename), Z_STRLEN_P(filename));
            value_len = Z_STRLEN_P(filename);
            tmp = http_trim_double_quote(value_buf, &value_len);

            add_assoc_stringl(multipart_header, "name", tmp, value_len);

            ctx->current_multipart_header = multipart_header;
        }
        zval_ptr_dtor(tmp_array);
    }

    if (strncasecmp(headername, "content-type", header_len) == 0 && ctx->current_multipart_header)
    {
        add_assoc_stringl(ctx->current_multipart_header, "type", (char * ) at, length);
    }

    efree(headername);

    return 0;
}

static int multipart_body_on_data(multipart_parser* p, const char *at, size_t length)
{
    http_context *ctx = (http_context *) p->data;
    if (ctx->current_form_data_name)
    {
        swString_append_ptr(swoole_http_form_data_buffer, (char*) at, length);
        return 0;
    }
    if (p->fp == NULL)
    {
        return 0;
    }
    int n = fwrite(at, sizeof(char), length, (FILE *) p->fp);
    if (n != (off_t) length)
    {
        zval *multipart_header = ctx->current_multipart_header;
        add_assoc_long(multipart_header, "error", HTTP_UPLOAD_ERR_CANT_WRITE);

        fclose((FILE *) p->fp);
        p->fp = NULL;

        swWarn("write upload file failed. Error %s[%d]", strerror(errno), errno);
    }
    return 0;
}

#if 0
static void get_random_file_name(char *des, const char *src)
{
    unsigned char digest[16] = {0};
    char buf[19] = {0};
    int n = sprintf(buf, "%s%d", src, swoole_system_random(0, 9999));

    PHP_MD5_CTX ctx;
    PHP_MD5Init(&ctx);
    PHP_MD5Update(&ctx, buf, n);
    PHP_MD5Final(digest, &ctx);
    make_digest_ex(des, digest, 16);
}
#endif

static int multipart_body_on_header_complete(multipart_parser* p)
{
    http_context *ctx = (http_context *) p->data;
    if (!ctx->current_input_name)
    {
        return 0;
    }

    zval *multipart_header = ctx->current_multipart_header;
    zval *zrequest_object = ctx->request.zobject;
    zval *zerr = NULL;
    if (!(zerr = zend_hash_str_find(Z_ARRVAL_P(multipart_header), ZEND_STRL("error"))))
    {
        return 0;
    }
    if (Z_TYPE_P(zerr) == IS_LONG && Z_LVAL_P(zerr) != HTTP_UPLOAD_ERR_OK)
    {
        return 0;
    }

    char file_path[SW_HTTP_UPLOAD_TMPDIR_SIZE];
    snprintf(file_path, SW_HTTP_UPLOAD_TMPDIR_SIZE, "%s/swoole.upfile.XXXXXX", SwooleG.serv->upload_tmp_dir);
    int tmpfile = swoole_tmpfile(file_path);
    if (tmpfile < 0)
    {
        return 0;
    }

    FILE *fp = fdopen(tmpfile, "wb+");
    if (fp == NULL)
    {
        add_assoc_long(multipart_header, "error", HTTP_UPLOAD_ERR_NO_TMP_DIR);
        swWarn("fopen(%s) failed. Error %s[%d]", file_path, strerror(errno), errno);
        return 0;
    }

    p->fp = fp;
    add_assoc_string(multipart_header, "tmp_name", file_path);

    zval *ztmpfiles = sw_zend_read_property(swoole_http_request_ce_ptr, zrequest_object, ZEND_STRL("tmpfiles"), 1);
    if (ztmpfiles == NULL || ZVAL_IS_NULL(ztmpfiles))
    {
        swoole_http_server_array_init(tmpfiles, request);
    }

    int file_path_len = strlen(file_path);
    add_next_index_stringl(ztmpfiles, file_path, file_path_len);

    // support is_upload_file
    zend_hash_str_add_ptr(SG(rfc1867_uploaded_files), file_path, file_path_len, (char *) file_path);

    return 0;
}

static int multipart_body_on_data_end(multipart_parser* p)
{
    http_context *ctx = (http_context *) p->data;
    zval *zrequest_object = ctx->request.zobject;
    if (ctx->current_form_data_name)
    {
        zval *zpost = sw_zend_read_property(swoole_http_request_ce_ptr, zrequest_object, ZEND_STRL("post"), 1);
        if (ZVAL_IS_NULL(zpost))
        {
            swoole_http_server_array_init(post, request);
        }

        php_register_variable_safe(ctx->current_form_data_name, swoole_http_form_data_buffer->str,
                swoole_http_form_data_buffer->length, zpost);

        efree(ctx->current_form_data_name);
        ctx->current_form_data_name = NULL;
        ctx->current_form_data_name_len = 0;
        swString_clear(swoole_http_form_data_buffer);
        return 0;
    }

    if (!ctx->current_input_name)
    {
        return 0;
    }

    zval *multipart_header = ctx->current_multipart_header;
    if (p->fp != NULL)
    {
        long size = swoole_file_get_size((FILE *) p->fp);
        add_assoc_long(multipart_header, "size", size);
        if (size == 0)
        {
            add_assoc_long(multipart_header, "error", HTTP_UPLOAD_ERR_NO_FILE);
        }

        fclose((FILE *) p->fp);
        p->fp = NULL;
    }

    zval *zfiles = ctx->request.zfiles;
    if (!zfiles)
    {
        swoole_http_server_array_init(files, request);
    }

    php_register_variable_ex(ctx->current_input_name, multipart_header, zfiles);

    efree(ctx->current_input_name);
    ctx->current_input_name = NULL;
    efree(ctx->current_multipart_header);
    ctx->current_multipart_header = NULL;

    return 0;
}

static int http_request_on_body(swoole_http_parser *parser, const char *at, size_t length)
{
    http_context *ctx = (http_context *) parser->data;
    zval *zrequest_object = ctx->request.zobject;
    char *body;

    ctx->request.post_length = length;
    if (SwooleG.serv->http_parse_post && ctx->request.post_form_urlencoded)
    {
        zval *zpost;
        swoole_http_server_array_init(post, request);
        body = estrndup(at, length);

        sapi_module.treat_data(PARSE_STRING, body, zpost);
    }
    else if (ctx->mt_parser != NULL)
    {
        multipart_parser *multipart_parser = ctx->mt_parser;
        char *c = (char *) at;
        while (*c == '\r' && *(c + 1) == '\n')
        {
            c += 2;
        }
        size_t n = multipart_parser_execute(multipart_parser, c, length);
        if (n != length)
        {
            swoole_error_log(SW_LOG_WARNING, SW_ERROR_SERVER_INVALID_REQUEST, "parse multipart body failed, n=%zu.", n);
        }
    }

    return 0;
}

static int http_request_message_complete(swoole_http_parser *parser)
{
    http_context *ctx = (http_context *) parser->data;
    ctx->request.version = parser->http_major * 100 + parser->http_minor;

    const char *vpath = ctx->request.path, *end = vpath + ctx->request.path_len, *p = end;
    ctx->request.ext = end;
    ctx->request.ext_len = 0;
    while (p > vpath)
    {
        --p;
        if (*p == '.')
        {
            ++p;
            ctx->request.ext = p;
            ctx->request.ext_len = end - p;
            break;
        }
    }

    if (ctx->mt_parser)
    {
        multipart_parser_free(ctx->mt_parser);
        ctx->mt_parser = NULL;
    }

    return 0;
}

int php_swoole_http_onReceive(swServer *serv, swEventData *req)
{
    int fd = req->info.fd;
    int from_fd = req->info.from_fd;

    swConnection *conn = swServer_connection_verify_no_ssl(serv, fd);
    if (!conn)
    {
        swoole_error_log(SW_LOG_NOTICE, SW_ERROR_SESSION_NOT_EXIST, "connection[%d] is closed.", fd);
        return SW_ERR;
    }

    swListenPort *port = (swListenPort *) serv->connection_list[from_fd].object;
    //other server port
    if (!port->open_http_protocol)
    {
        return php_swoole_onReceive(serv, req);
    }
    //websocket client
    if (conn->websocket_status == WEBSOCKET_STATUS_ACTIVE)
    {
        return swoole_websocket_onMessage(serv, req);
    }
#ifdef SW_USE_HTTP2
    if (conn->http2_stream)
    {
        return swoole_http2_onFrame(conn, req);
    }
#endif

    http_context *ctx = swoole_http_context_new(fd);
    swoole_http_parser *parser = &ctx->parser;
    zval *zserver = ctx->request.zserver;

    parser->data = ctx;

    zval *zdata = sw_malloc_zval();
    php_swoole_get_recv_data(zdata, req, NULL, 0);

    swTrace("http request from %d with %d bytes: <<EOF\n%.*s\nEOF", fd, (int)Z_STRLEN_P(zdata), (int)Z_STRLEN_P(zdata), Z_STRVAL_P(zdata));

#ifdef SW_USE_PICOHTTPPARSER
    long n = http_fast_parse(parser, Z_STRVAL_P(zdata), Z_STRLEN_P(zdata));
#else
    swoole_http_parser_init(parser, PHP_HTTP_REQUEST);
    long n = swoole_http_parser_execute(parser, &http_parser_settings, Z_STRVAL_P(zdata), Z_STRLEN_P(zdata));
#endif

    if (n < 0)
    {
        sw_zval_free(zdata);
        swWarn("swoole_http_parser_execute failed.");
        if (conn->websocket_status == WEBSOCKET_STATUS_CONNECTION)
        {
            return swServer_tcp_close(serv, fd, 1);
        }
    }
    else
    {
        zval _zrequest_object = *ctx->request.zobject, *zrequest_object = &_zrequest_object;
        zval _zresponse_object = *ctx->response.zobject, *zresponse_object = &_zresponse_object;

        ctx->keepalive = swoole_http_should_keep_alive(parser);
        const char *method_name = http_get_method_name(parser->method);

        add_assoc_string(zserver, "request_method", (char * ) method_name);
        add_assoc_stringl(zserver, "request_uri", ctx->request.path, ctx->request.path_len);
        add_assoc_stringl(zserver, "path_info", ctx->request.path, ctx->request.path_len);
        add_assoc_long_ex(zserver, ZEND_STRL("request_time"), serv->gs->now);

        // Add REQUEST_TIME_FLOAT
        double now_float = swoole_microtime();
        add_assoc_double_ex(zserver, ZEND_STRL("request_time_float"), now_float);

        swConnection *conn = swWorker_get_connection(serv, fd);
        if (!conn)
        {
            sw_zval_free(zdata);
            swWarn("connection[%d] is closed.", fd);
            return SW_ERR;
        }

        swoole_set_property(zrequest_object, 0, zdata);

        add_assoc_long(zserver, "server_port", swConnection_get_port(&serv->connection_list[conn->from_fd]));
        add_assoc_long(zserver, "remote_port", swConnection_get_port(conn));
        add_assoc_string(zserver, "remote_addr", swConnection_get_ip(conn));
        add_assoc_long(zserver, "master_time", conn->last_time);
        add_assoc_string(zserver, "server_protocol", (char *) (ctx->request.version == 101 ? "HTTP/1.1" : "HTTP/1.0"));

        // begin to check and call registerd callback
        zend_fcall_info_cache *fci_cache = NULL;

        if (conn->websocket_status == WEBSOCKET_STATUS_CONNECTION)
        {
            fci_cache = php_swoole_server_get_fci_cache(serv, from_fd, SW_SERVER_CB_onHandShake);
            if (fci_cache == NULL)
            {
                swoole_websocket_onHandshake(serv, port, ctx);
                goto _free_object;
            }
            else
            {
                conn->websocket_status = WEBSOCKET_STATUS_HANDSHAKE;
                ctx->upgrade = 1;
            }
        }
        else
        {
            fci_cache = php_swoole_server_get_fci_cache(serv, from_fd, SW_SERVER_CB_onRequest);
            if (fci_cache == NULL)
            {
                swoole_websocket_onRequest(ctx);
                goto _free_object;
            }
        }

        zval args[2];
        args[0] = *zrequest_object;
        args[1] = *zresponse_object;

        if (SwooleG.enable_coroutine)
        {
            if (PHPCoroutine::create(fci_cache, 2, args) < 0)
            {
                swoole_php_error(E_WARNING, "create Http onRequest coroutine error.");
                swServer_tcp_close(serv, fd, 0);
            }
        }
        else
        {
            zval _retval, *retval = &_retval;
            if (sw_call_user_function_fast_ex(NULL, fci_cache, retval, 2, args) == FAILURE)
            {
                swoole_php_error(E_WARNING, "Http onRequest handler error.");
            }
            zval_ptr_dtor(retval);
        }
        _free_object:
        zval_ptr_dtor(zrequest_object);
        zval_ptr_dtor(zresponse_object);
    }

    return SW_OK;
}

void php_swoole_http_onClose(swServer *serv, swDataHead *ev)
{
    int fd = ev->fd;
    swConnection *conn = swWorker_get_connection(serv, fd);
    if (!conn)
    {
        return;
    }
#ifdef SW_USE_HTTP2
    if (conn->http2_stream)
    {
        swoole_http2_free(conn);
    }
#endif
    php_swoole_onClose(serv, ev);
}

void swoole_http_server_init(int module_number)
{
    SWOOLE_INIT_CLASS_ENTRY_EX(swoole_http_server, "Swoole\\Http\\Server", "swoole_http_server", NULL, NULL, swoole_server);
    SWOOLE_SET_CLASS_SERIALIZABLE(swoole_http_server, zend_class_serialize_deny, zend_class_unserialize_deny);
    SWOOLE_SET_CLASS_CLONEABLE(swoole_http_server, zend_class_clone_deny);
    SWOOLE_SET_CLASS_UNSET_PROPERTY_HANDLER(swoole_http_server, zend_class_unset_property_deny);

    zend_declare_property_null(swoole_http_server_ce_ptr, ZEND_STRL("onRequest"), ZEND_ACC_PUBLIC);
    zend_declare_property_null(swoole_http_server_ce_ptr, ZEND_STRL("onHandshake"), ZEND_ACC_PUBLIC);
    zend_declare_property_null(swoole_http_server_ce_ptr, ZEND_STRL("setting"), ZEND_ACC_PUBLIC);

    SWOOLE_INIT_CLASS_ENTRY(swoole_http_request, "Swoole\\Http\\Request", "swoole_http_request", NULL, swoole_http_request_methods);
    SWOOLE_SET_CLASS_SERIALIZABLE(swoole_http_request, zend_class_serialize_deny, zend_class_unserialize_deny);
    SWOOLE_SET_CLASS_CLONEABLE(swoole_http_request, zend_class_clone_deny);
    SWOOLE_SET_CLASS_UNSET_PROPERTY_HANDLER(swoole_http_request, zend_class_unset_property_deny);

    zend_declare_property_long(swoole_http_request_ce_ptr, ZEND_STRL("fd"), 0, ZEND_ACC_PUBLIC);
#ifdef SW_USE_HTTP2
    zend_declare_property_long(swoole_http_request_ce_ptr, ZEND_STRL("streamId"), 0, ZEND_ACC_PUBLIC);
#endif
    zend_declare_property_null(swoole_http_request_ce_ptr, ZEND_STRL("header"), ZEND_ACC_PUBLIC);
    zend_declare_property_null(swoole_http_request_ce_ptr, ZEND_STRL("server"), ZEND_ACC_PUBLIC);
    zend_declare_property_null(swoole_http_request_ce_ptr, ZEND_STRL("request"), ZEND_ACC_PUBLIC);
    zend_declare_property_null(swoole_http_request_ce_ptr, ZEND_STRL("cookie"), ZEND_ACC_PUBLIC);
    zend_declare_property_null(swoole_http_request_ce_ptr, ZEND_STRL("get"), ZEND_ACC_PUBLIC);
    zend_declare_property_null(swoole_http_request_ce_ptr, ZEND_STRL("files"), ZEND_ACC_PUBLIC);
    zend_declare_property_null(swoole_http_request_ce_ptr, ZEND_STRL("post"), ZEND_ACC_PUBLIC);
    zend_declare_property_null(swoole_http_request_ce_ptr, ZEND_STRL("tmpfiles"), ZEND_ACC_PUBLIC);

    SWOOLE_INIT_CLASS_ENTRY(swoole_http_response, "Swoole\\Http\\Response", "swoole_http_response", NULL, swoole_http_response_methods);
    SWOOLE_SET_CLASS_SERIALIZABLE(swoole_http_response, zend_class_serialize_deny, zend_class_unserialize_deny);
    SWOOLE_SET_CLASS_CLONEABLE(swoole_http_response, zend_class_clone_deny);
    SWOOLE_SET_CLASS_UNSET_PROPERTY_HANDLER(swoole_http_response, zend_class_unset_property_deny);

    zend_declare_property_long(swoole_http_response_ce_ptr, ZEND_STRL("fd"), 0,  ZEND_ACC_PUBLIC);
    zend_declare_property_null(swoole_http_response_ce_ptr, ZEND_STRL("header"), ZEND_ACC_PUBLIC);
    zend_declare_property_null(swoole_http_response_ce_ptr, ZEND_STRL("cookie"), ZEND_ACC_PUBLIC);
    zend_declare_property_null(swoole_http_response_ce_ptr, ZEND_STRL("trailer"), ZEND_ACC_PUBLIC);
}

http_context* swoole_http_context_new(int fd)
{
    http_context *ctx = (http_context *) emalloc(sizeof(http_context));
    if (!ctx)
    {
        swoole_error_log(SW_LOG_ERROR, SW_ERROR_MALLOC_FAIL, "emalloc(%ld) failed.", sizeof(http_context));
        return NULL;
    }
    bzero(ctx, sizeof(http_context));

    zval *zrequest_object;
    zrequest_object = &ctx->request._zobject;
    ctx->request.zobject = zrequest_object;
    object_init_ex(zrequest_object, swoole_http_request_ce_ptr);
    swoole_set_object(zrequest_object, ctx);

    zval *zresponse_object;
    zresponse_object = &ctx->response._zobject;
    ctx->response.zobject = zresponse_object;
    object_init_ex(zresponse_object, swoole_http_response_ce_ptr);
    swoole_set_object(zresponse_object, ctx);

    //socket fd
    zend_update_property_long(swoole_http_response_ce_ptr, zresponse_object, ZEND_STRL("fd"), fd);
    zend_update_property_long(swoole_http_request_ce_ptr, zrequest_object, ZEND_STRL("fd"), fd);

#if PHP_MEMORY_DEBUG
    php_vmstat.new_http_request ++;
#endif

    zval *zheader;
    swoole_http_server_array_init(header, request);

    zval *zserver;
    swoole_http_server_array_init(server, request);

    ctx->fd = fd;

    return ctx;
}

void swoole_http_context_free(http_context *ctx)
{
    swoole_set_object(ctx->response.zobject, NULL);
    http_request *req = &ctx->request;
    http_response *res = &ctx->response;
    if (req->path)
    {
        efree(req->path);
    }
#ifdef SW_USE_HTTP2
    if (req->post_buffer)
    {
        swString_free(req->post_buffer);
    }
#endif
    if (res->reason)
    {
        efree(res->reason);
    }
    efree(ctx);
}

static const char *http_status_message(int code)
{
    switch (code)
    {
    case 100:
        return "100 Continue";
    case 101:
        return "101 Switching Protocols";
    case 201:
        return "201 Created";
    case 202:
        return "202 Accepted";
    case 203:
        return "203 Non-Authoritative Information";
    case 204:
        return "204 No Content";
    case 205:
        return "205 Reset Content";
    case 206:
        return "206 Partial Content";
    case 207:
        return "207 Multi-Status";
    case 208:
        return "208 Already Reported";
    case 226:
        return "226 IM Used";
    case 300:
        return "300 Multiple Choices";
    case 301:
        return "301 Moved Permanently";
    case 302:
        return "302 Found";
    case 303:
        return "303 See Other";
    case 304:
        return "304 Not Modified";
    case 305:
        return "305 Use Proxy";
    case 307:
        return "307 Temporary Redirect";
    case 400:
        return "400 Bad Request";
    case 401:
        return "401 Unauthorized";
    case 402:
        return "402 Payment Required";
    case 403:
        return "403 Forbidden";
    case 404:
        return "404 Not Found";
    case 405:
        return "405 Method Not Allowed";
    case 406:
        return "406 Not Acceptable";
    case 407:
        return "407 Proxy Authentication Required";
    case 408:
        return "408 Request Timeout";
    case 409:
        return "409 Conflict";
    case 410:
        return "410 Gone";
    case 411:
        return "411 Length Required";
    case 412:
        return "412 Precondition Failed";
    case 413:
        return "413 Request Entity Too Large";
    case 414:
        return "414 Request URI Too Long";
    case 415:
        return "415 Unsupported Media Type";
    case 416:
        return "416 Requested Range Not Satisfiable";
    case 417:
        return "417 Expectation Failed";
    case 418:
        return "418 I'm a teapot";
    case 421:
        return "421 Misdirected Request";
    case 422:
        return "422 Unprocessable Entity";
    case 423:
        return "423 Locked";
    case 424:
        return "424 Failed Dependency";
    case 426:
        return "426 Upgrade Required";
    case 428:
        return "428 Precondition Required";
    case 429:
        return "429 Too Many Requests";
    case 431:
        return "431 Request Header Fields Too Large";
    case 500:
        return "500 Internal Server Error";
    case 501:
        return "501 Method Not Implemented";
    case 502:
        return "502 Bad Gateway";
    case 503:
        return "503 Service Unavailable";
    case 504:
        return "504 Gateway Timeout";
    case 505:
        return "505 HTTP Version Not Supported";
    case 506:
        return "506 Variant Also Negotiates";
    case 507:
        return "507 Insufficient Storage";
    case 508:
        return "508 Loop Detected";
    case 510:
        return "510 Not Extended";
    case 511:
        return "511 Network Authentication Required";
    case 200:
    default:
        return "200 OK";
    }
}

void php_swoole_http_server_before_start(swServer *serv, zval *zobject)
{
    swoole_http_buffer = swString_new(SW_HTTP_RESPONSE_INIT_SIZE);
    if (!swoole_http_buffer)
    {
        swoole_php_fatal_error(E_ERROR, "[1] swString_new(%d) failed.", SW_HTTP_RESPONSE_INIT_SIZE);
        return;
    }

    swoole_http_form_data_buffer = swString_new(SW_HTTP_RESPONSE_INIT_SIZE);
    if (!swoole_http_form_data_buffer)
    {
        swoole_php_fatal_error(E_ERROR, "[2] swString_new(%d) failed.", SW_HTTP_RESPONSE_INIT_SIZE);
        return;
    }

    //for is_uploaded_file and move_uploaded_file
    ALLOC_HASHTABLE(SG(rfc1867_uploaded_files));
    zend_hash_init(SG(rfc1867_uploaded_files), 8, NULL, NULL, 0);
}

static PHP_METHOD(swoole_http_request, rawcontent)
{
    http_context *ctx = http_get_context(getThis(), 0);
    if (!ctx)
    {
        RETURN_FALSE;
    }

    http_request *req = &ctx->request;
    if (req->post_length > 0)
    {
        zval *zdata = (zval *) swoole_get_property(getThis(), 0);
        RETVAL_STRINGL(Z_STRVAL_P(zdata) + Z_STRLEN_P(zdata) - req->post_length, req->post_length);
    }
#ifdef SW_USE_HTTP2
    else if (req->post_buffer)
    {
        RETVAL_STRINGL(req->post_buffer->str, req->post_buffer->length);
    }
#endif
    else
    {
        RETURN_FALSE;
    }
}

static PHP_METHOD(swoole_http_request, getData)
{
    zval *zdata = (zval *) swoole_get_property(getThis(), 0);
    if (zdata)
    {
        RETURN_STRINGL(Z_STRVAL_P(zdata), Z_STRLEN_P(zdata));
    }
    else
    {
        RETURN_FALSE;
    }
}

static PHP_METHOD(swoole_http_request, __destruct)
{
    SW_PREVENT_USER_DESTRUCT;

    zval *ztmpfiles = sw_zend_read_property(swoole_http_request_ce_ptr, getThis(), ZEND_STRL("tmpfiles"), 1);
    //upload files
    if (ztmpfiles && Z_TYPE_P(ztmpfiles) == IS_ARRAY)
    {
        zval *file_path;
        SW_HASHTABLE_FOREACH_START(Z_ARRVAL_P(ztmpfiles), file_path)
        {
            if (Z_TYPE_P(file_path) != IS_STRING)
            {
                continue;
            }
            unlink(Z_STRVAL_P(file_path));
            if (SG(rfc1867_uploaded_files))
            {
                zend_hash_str_del(SG(rfc1867_uploaded_files), Z_STRVAL_P(file_path), Z_STRLEN_P(file_path));
            }
        }
        SW_HASHTABLE_FOREACH_END();
    }
    zval *zdata = (zval *) swoole_get_property(getThis(), 0);
    if (zdata)
    {
        sw_zval_free(zdata);
        swoole_set_property(getThis(), 0, NULL);
    }
    swoole_set_object(getThis(), NULL);
}

static PHP_METHOD(swoole_http_response, write)
{
    zval *zdata;
    if (zend_parse_parameters(ZEND_NUM_ARGS(), "z", &zdata) == FAILURE)
    {
        RETURN_FALSE;
    }

    http_context *ctx = http_get_context(getThis(), 0);
    if (!ctx)
    {
        return;
    }

#ifdef SW_USE_HTTP2
    if (ctx->stream)
    {
        swoole_php_error(E_WARNING, "Http2 client does not support HTTP-CHUNK.");
        RETURN_FALSE;
    }
#endif

#ifdef SW_HAVE_ZLIB
    ctx->enable_compression = 0;
#endif

    if (!ctx->send_header)
    {
        ctx->chunk = 1;
        swString_clear(swoole_http_buffer);
        http_build_header(ctx, getThis(), swoole_http_buffer, -1);
        if (swServer_tcp_send(SwooleG.serv, ctx->fd, swoole_http_buffer->str, swoole_http_buffer->length) < 0)
        {
            ctx->chunk = 0;
            ctx->send_header = 0;
            RETURN_FALSE;
        }
    }

    swString http_body;
    size_t length = php_swoole_get_send_data(zdata, &http_body.str);

    if (length == 0)
    {
        swoole_php_error(E_WARNING, "data to send is empty.");
        RETURN_FALSE;
    }
    else
    {
        http_body.length = length;
    }

    // Why not enable compression?
    // If both compression and chunked encoding are enabled,
    // then the content stream is first compressed, then chunked;
    // so the chunk encoding itself is not compressed,
    // **and the data in each chunk is not compressed individually.**
    // The remote endpoint then decodes the stream by concatenating the chunks and uncompressing the result.
    swString_clear(swoole_http_buffer);
    char *hex_string = swoole_dec2hex(http_body.length, 16);
    int hex_len = strlen(hex_string);
    //"%*s\r\n%*s\r\n", hex_len, hex_string, body.length, body.str
    swString_append_ptr(swoole_http_buffer, hex_string, hex_len);
    swString_append_ptr(swoole_http_buffer, ZEND_STRL("\r\n"));
    swString_append_ptr(swoole_http_buffer, http_body.str, http_body.length);
    swString_append_ptr(swoole_http_buffer, ZEND_STRL("\r\n"));
    sw_free(hex_string);

    swServer *serv = SwooleG.serv;
    int ret = swServer_tcp_send(serv, ctx->fd, swoole_http_buffer->str, swoole_http_buffer->length);
#ifdef SW_COROUTINE
    if (ret < 0 && SwooleG.error == SW_ERROR_OUTPUT_BUFFER_OVERFLOW && serv->send_yield)
    {
        zval _yield_data;
        ZVAL_STRINGL(&_yield_data, swoole_http_buffer->str, swoole_http_buffer->length);
        php_swoole_server_send_yield(serv, ctx->fd, &_yield_data, return_value);
        if (Z_TYPE_P(return_value) == IS_FALSE)
        {
            ctx->chunk = 0;
            ctx->send_header = 0;
        }
        return;
    }
#endif

    SW_CHECK_RETURN(ret);
}

static http_context* http_get_context(zval *zobject, int check_end)
{
    http_context *ctx = (http_context *) swoole_get_object(zobject);
    if (!ctx)
    {
        swoole_php_fatal_error(E_WARNING, "Http request is finished.");
        return NULL;
    }
    if (check_end && ctx->end)
    {
        swoole_php_fatal_error(E_WARNING, "Http request is finished.");
        return NULL;
    }
    return ctx;
}

static void http_build_header(http_context *ctx, zval *zobject, swString *response, int body_length)
{
    swServer *serv = SwooleG.serv;
    char *buf = SwooleTG.buffer_stack->str;
    size_t l_buf = SwooleTG.buffer_stack->size;
    int n;
    char *date_str;

    assert(ctx->send_header == 0);

    /**
     * http status line
     */
    if (!ctx->response.reason)
    {
        n = sw_snprintf(buf, l_buf, "HTTP/1.1 %s\r\n", http_status_message(ctx->response.status));
    }
    else
    {
        n = sw_snprintf(buf, l_buf, "HTTP/1.1 %d %s\r\n", ctx->response.status, ctx->response.reason);
    }
    swString_append_ptr(response, buf, n);

    /**
     * http header
     */
    zval *zheader = sw_zend_read_property(swoole_http_response_ce_ptr, ctx->response.zobject, ZEND_STRL("header"), 1);
    uint32_t header_flag = 0x0;
    if (ZVAL_IS_ARRAY(zheader))
    {
        HashTable *ht = Z_ARRVAL_P(zheader);
        zval *value = NULL;
        char *key = NULL;
        uint32_t keylen = 0;
        int type;

        SW_HASHTABLE_FOREACH_START2(ht, key, keylen, type, value)
        {
            // TODO: numeric key name neccessary?
            if (!key)
            {
                break;
            }
            if (strncasecmp(key, "Server", keylen) == 0)
            {
                header_flag |= HTTP_HEADER_SERVER;
            }
            else if (strncasecmp(key, "Connection", keylen) == 0)
            {
                header_flag |= HTTP_HEADER_CONNECTION;
            }
            else if (strncasecmp(key, "Date", keylen) == 0)
            {
                header_flag |= HTTP_HEADER_DATE;
            }
            else if (strncasecmp(key, "Content-Length", keylen) == 0)
            {
                continue; // ignore
            }
            else if (strncasecmp(key, "Content-Type", keylen) == 0)
            {
                header_flag |= HTTP_HEADER_CONTENT_TYPE;
            }
            else if (strncasecmp(key, "Transfer-Encoding", keylen) == 0)
            {
                header_flag |= HTTP_HEADER_TRANSFER_ENCODING;
            }
            if (!ZVAL_IS_NULL(value))
            {
                convert_to_string(value);
                n = sw_snprintf(buf, l_buf, "%*s: %*s\r\n", keylen - 1, key, (int)Z_STRLEN_P(value), Z_STRVAL_P(value));
                swString_append_ptr(response, buf, n);
            }
        }
        SW_HASHTABLE_FOREACH_END();
        (void)type;
    }

    if (!(header_flag & HTTP_HEADER_SERVER))
    {
        swString_append_ptr(response, ZEND_STRL("Server: " SW_HTTP_SERVER_SOFTWARE "\r\n"));
    }
    //websocket protocol
    if (ctx->upgrade == 1)
    {
        swString_append_ptr(response, ZEND_STRL("\r\n"));
        ctx->send_header = 1;
        return;
    }
    if (!(header_flag & HTTP_HEADER_CONNECTION))
    {
        if (ctx->keepalive)
        {
            swString_append_ptr(response, ZEND_STRL("Connection: keep-alive\r\n"));
        }
        else
        {
            swString_append_ptr(response, ZEND_STRL("Connection: close\r\n"));
        }
    }
    if (!(header_flag & HTTP_HEADER_CONTENT_TYPE))
    {
        swString_append_ptr(response, ZEND_STRL("Content-Type: text/html\r\n"));
    }
    if (!(header_flag & HTTP_HEADER_DATE))
    {
        date_str = sw_php_format_date((char *) ZEND_STRL(SW_HTTP_DATE_FORMAT), serv->gs->now, 0);
        n = sw_snprintf(buf, l_buf, "Date: %s\r\n", date_str);
        swString_append_ptr(response, buf, n);
        efree(date_str);
    }

    if (ctx->chunk)
    {
        if (!(header_flag & HTTP_HEADER_TRANSFER_ENCODING))
        {
            swString_append_ptr(response, ZEND_STRL("Transfer-Encoding: chunked\r\n"));
        }
    }
    else
    // Content-Length
    {
#ifdef SW_HAVE_ZLIB
        if (ctx->enable_compression)
        {
            body_length = swoole_zlib_buffer->length;
        }
#endif
        n = sw_snprintf(buf, l_buf, "Content-Length: %d\r\n", body_length);
        swString_append_ptr(response, buf, n);
    }

    //http cookies
    zval *zcookie = sw_zend_read_property(swoole_http_response_ce_ptr, ctx->response.zobject, ZEND_STRL("cookie"), 1);
    if (ZVAL_IS_ARRAY(zcookie))
    {
        zval *value;
        SW_HASHTABLE_FOREACH_START(Z_ARRVAL_P(zcookie), value)
        {
            if (Z_TYPE_P(value) != IS_STRING)
            {
                continue;
            }
            swString_append_ptr(response, ZEND_STRL("Set-Cookie: "));
            swString_append_ptr(response, Z_STRVAL_P(value), Z_STRLEN_P(value));
            swString_append_ptr(response, ZEND_STRL("\r\n"));
        }
        SW_HASHTABLE_FOREACH_END();
    }
#ifdef SW_HAVE_ZLIB
    //http compress
    if (ctx->enable_compression)
    {
        const char *content_encoding = swoole_http_get_content_encoding(ctx);
        swString_append_ptr(response, ZEND_STRL("Content-Encoding: "));
        swString_append_ptr(response, (char*) content_encoding, strlen(content_encoding));
        swString_append_ptr(response, ZEND_STRL("\r\n"));
    }
#endif
    swString_append_ptr(response, ZEND_STRL("\r\n"));
    ctx->send_header = 1;
}

#ifdef SW_HAVE_ZLIB
void swoole_http_get_compression_method(http_context *ctx, const char *accept_encoding, size_t length)
{
#ifdef SW_HAVE_BROTLI
    if (swoole_strnpos((char *) accept_encoding, length, (char *) ZEND_STRL("br")) >= 0)
    {
        ctx->enable_compression = 1;
        ctx->compression_level = SwooleG.serv->http_compression_level;
        ctx->compression_method = HTTP_COMPRESS_BR;
    }
    else
#endif
    if (swoole_strnpos((char *) accept_encoding, length, (char *) ZEND_STRL("gzip")) >= 0)
    {
        ctx->enable_compression = 1;
        ctx->compression_level = SwooleG.serv->http_compression_level;
        ctx->compression_method = HTTP_COMPRESS_GZIP;
    }
    else if (swoole_strnpos((char *) accept_encoding, length, (char *) ZEND_STRL("deflate")) >= 0)
    {
        ctx->enable_compression = 1;
        ctx->compression_level = SwooleG.serv->http_compression_level;
        ctx->compression_method = HTTP_COMPRESS_DEFLATE;
    }
    else
    {
        ctx->enable_compression = 0;
    }
}

const char* swoole_http_get_content_encoding(http_context *ctx)
{
    if (ctx->compression_method == HTTP_COMPRESS_GZIP)
    {
       return "gzip";
    }
    else if (ctx->compression_method == HTTP_COMPRESS_DEFLATE)
    {
        return "deflate";
    }
#ifdef SW_HAVE_BROTLI
    else if (ctx->compression_method == HTTP_COMPRESS_BR)
    {
        return "br";
    }
#endif
    else
    {
        return NULL;
    }
}

int swoole_http_response_compress(swString *body, int method, int level)
{
    int encoding;
    //gzip: 0x1f
    if (method == HTTP_COMPRESS_GZIP)
    {
        encoding = 0x1f;
    }
    //deflate: -0xf
    else if (method == HTTP_COMPRESS_DEFLATE)
    {
        encoding = -0xf;
    }
#ifdef SW_HAVE_BROTLI
    else if (method == HTTP_COMPRESS_BR)
    {
        if (level < BROTLI_MIN_QUALITY)
        {
            level = BROTLI_MAX_QUALITY;
        }
        else if (level > BROTLI_MAX_QUALITY)
        {
            level = BROTLI_MAX_QUALITY;
        }

        size_t memory_size = BrotliEncoderMaxCompressedSize(body->length);
        if (memory_size > swoole_zlib_buffer->size)
        {
            if (swString_extend(swoole_zlib_buffer, memory_size) < 0)
            {
                return SW_ERR;
            }
        }

        size_t input_size = body->length;
        const uint8_t *input_buffer = (uint8_t *) body->str;
        size_t encoded_size = swoole_zlib_buffer->size;
        uint8_t *encoded_buffer = (uint8_t *) swoole_zlib_buffer->str;

        if (BROTLI_TRUE != BrotliEncoderCompress(
            level, BROTLI_DEFAULT_WINDOW, BROTLI_DEFAULT_MODE,
            input_size, input_buffer, &encoded_size, encoded_buffer
        ))
        {
            swWarn("BrotliEncoderCompress() failed.");
            return SW_ERR;
        }
        else
        {
            swoole_zlib_buffer->length = encoded_size;
            return SW_OK;
        }
    }
#endif
    else
    {
        swWarn("Unknown compression method");
        return SW_ERR;
    }

    // ==== ZLIB ====
    if (level == Z_NO_COMPRESSION)
    {
        level = Z_DEFAULT_COMPRESSION;
    }
    else if (level > Z_BEST_COMPRESSION)
    {
        level = Z_BEST_COMPRESSION;
    }

    size_t memory_size = ((size_t) ((double) body->length * (double) 1.015)) + 10 + 8 + 4 + 1;
    if (memory_size > swoole_zlib_buffer->size)
    {
        if (swString_extend(swoole_zlib_buffer, memory_size) < 0)
        {
            return SW_ERR;
        }
    }

    z_stream zstream;
    memset(&zstream, 0, sizeof(zstream));

    int status;
    zstream.zalloc = php_zlib_alloc;
    zstream.zfree = php_zlib_free;

    int retval = deflateInit2(&zstream, level, Z_DEFLATED, encoding, MAX_MEM_LEVEL, Z_DEFAULT_STRATEGY);

    if (Z_OK == retval)
    {
        zstream.next_in = (Bytef *) body->str;
        zstream.next_out = (Bytef *) swoole_zlib_buffer->str;
        zstream.avail_in = body->length;
        zstream.avail_out = swoole_zlib_buffer->size;

        status = deflate(&zstream, Z_FINISH);
        deflateEnd(&zstream);

        if (Z_STREAM_END == status)
        {
            swoole_zlib_buffer->length = zstream.total_out;
            return SW_OK;
        }
    }
    else
    {
        swWarn("deflateInit2() failed, Error: [%d].", retval);
    }
    return SW_ERR;
}
#endif

static PHP_METHOD(swoole_http_response, initHeader)
{
    http_context *ctx = http_get_context(getThis(), 0);
    if (!ctx)
    {
        RETURN_FALSE;
    }
    zval *zresponse_object = ctx->response.zobject;
    zval *zheader = sw_zend_read_property(swoole_http_response_ce_ptr, zresponse_object, ZEND_STRL("header"), 1);
    if (!ZVAL_IS_ARRAY(zheader))
    {
        swoole_http_server_array_init(header, response);
    }

    zval *zcookie = sw_zend_read_property(swoole_http_response_ce_ptr, zresponse_object, ZEND_STRL("cookie"), 1);
    if (!ZVAL_IS_ARRAY(zcookie))
    {
        swoole_http_server_array_init(cookie, response);
    }

    zval *ztrailer = sw_zend_read_property(swoole_http_response_ce_ptr, zresponse_object, ZEND_STRL("trailer"), 1);
    if (!ZVAL_IS_ARRAY(ztrailer))
    {
        swoole_http_server_array_init(trailer, response);
    }
}

static PHP_METHOD(swoole_http_response, end)
{
    zval *zdata = NULL;
    int ret;

    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_ZVAL(zdata)
    ZEND_PARSE_PARAMETERS_END_EX(RETURN_FALSE);

    http_context *ctx = http_get_context(getThis(), 0);
    if (!ctx)
    {
        RETURN_FALSE;
    }

    swString http_body;

    if (zdata)
    {
        http_body.length = php_swoole_get_send_data(zdata, &http_body.str);
    }
    else
    {
        http_body.length = 0;
        http_body.str = NULL;
    }

#ifdef SW_USE_HTTP2
    if (ctx->stream)
    {
        swoole_http2_do_response(ctx, &http_body);
        RETURN_TRUE;
    }
#endif

    if (ctx->chunk)
    {
        ret = swServer_tcp_send(SwooleG.serv, ctx->fd, (char*) ZEND_STRL("0\r\n\r\n"));
        if (ret < 0)
        {
            RETURN_FALSE;
        }
        ctx->chunk = 0;
    }
    //no http chunk
    else
    {
        swString_clear(swoole_http_buffer);
#ifdef SW_HAVE_ZLIB
        if (ctx->enable_compression)
        {
            if (http_body.length == 0 || swoole_http_response_compress(&http_body, ctx->compression_method, ctx->compression_level) != SW_OK)
            {
                ctx->enable_compression = 0;
            }
        }
#endif
        http_build_header(ctx, getThis(), swoole_http_buffer, http_body.length);
        if (http_body.length > 0)
        {
#ifdef SW_HAVE_ZLIB
            if (ctx->enable_compression)
            {
                swString_append(swoole_http_buffer, swoole_zlib_buffer);
            }
            else
#endif
            {
                swString_append(swoole_http_buffer, &http_body);
            }
        }

        ret = swServer_tcp_send(SwooleG.serv, ctx->fd, swoole_http_buffer->str, swoole_http_buffer->length);
        if (ret < 0)
        {
            ctx->send_header = 0;
            RETURN_FALSE;
        }
    }

    if (ctx->upgrade)
    {
        swConnection *conn = swWorker_get_connection(SwooleG.serv, ctx->fd);
        if (conn && conn->websocket_status == WEBSOCKET_STATUS_HANDSHAKE && ctx->response.status == 101)
        {
            conn->websocket_status = WEBSOCKET_STATUS_ACTIVE;
        }
    }

    if (!ctx->keepalive)
    {
        swServer_tcp_close(SwooleG.serv, ctx->fd, 0);
    }
    swoole_http_context_free(ctx);
    RETURN_TRUE;
}

static PHP_METHOD(swoole_http_response, sendfile)
{
    char *filename;
    size_t filename_length;
    zend_long offset = 0;
    zend_long length = 0;

    if (zend_parse_parameters(ZEND_NUM_ARGS(), "s|ll", &filename, &filename_length, &offset, &length) == FAILURE)
    {
        RETURN_FALSE;
    }
    if (filename_length <= 0)
    {
        swoole_php_error(E_WARNING, "file name is empty.");
        RETURN_FALSE;
    }

    http_context *ctx = http_get_context(getThis(), 0);
    if (!ctx)
    {
        RETURN_FALSE;
    }

#ifdef SW_HAVE_ZLIB
    ctx->enable_compression = 0;
#endif

    if (ctx->chunk)
    {
        swoole_php_fatal_error(E_ERROR, "can't use sendfile when Http-Chunk is enabled.");
        RETURN_FALSE;
    }

    struct stat file_stat;
    if (stat(filename, &file_stat) < 0)
    {
        swoole_php_sys_error(E_WARNING, "stat(%s) failed.", filename);
        RETURN_FALSE;
    }
    if (file_stat.st_size == 0)
    {
        swoole_php_sys_error(E_WARNING, "can't send empty file[%s].", filename);
        RETURN_FALSE;
    }
    if (file_stat.st_size <= offset)
    {
        swoole_php_error(E_WARNING, "parameter $offset[%ld] exceeds the file size.", offset);
        RETURN_FALSE;
    }
    if (length > file_stat.st_size - offset)
    {
        swoole_php_sys_error(E_WARNING, "parameter $length[%ld] exceeds the file size.", length);
        RETURN_FALSE;
    }
    if (length == 0)
    {
        length = file_stat.st_size - offset;
    }

    swString_clear(swoole_http_buffer);
    http_build_header(ctx, getThis(), swoole_http_buffer, length);

    int ret = swServer_tcp_send(SwooleG.serv, ctx->fd, swoole_http_buffer->str, swoole_http_buffer->length);
    if (ret < 0)
    {
        ctx->send_header = 0;
        RETURN_FALSE;
    }

    ret = swServer_tcp_sendfile(SwooleG.serv, ctx->fd, filename, filename_length, offset, length);
    if (ret < 0)
    {
        ctx->send_header = 0;
        RETURN_FALSE;
    }
    if (!ctx->keepalive)
    {
        swServer_tcp_close(SwooleG.serv, ctx->fd, 0);
    }
    swoole_http_context_free(ctx);
    RETURN_TRUE;
}

static void swoole_http_response_cookie(INTERNAL_FUNCTION_PARAMETERS, bool url_encode = true)
{
    char *name, *value = NULL, *path = NULL, *domain = NULL;
    zend_long expires = 0;
    size_t name_len, value_len = 0, path_len = 0, domain_len = 0;
    zend_bool secure = 0, httponly = 0;

    ZEND_PARSE_PARAMETERS_START(1, 7)
        Z_PARAM_STRING(name, name_len)
        Z_PARAM_OPTIONAL
        Z_PARAM_STRING(value, value_len)
        Z_PARAM_LONG(expires)
        Z_PARAM_STRING(path, path_len)
        Z_PARAM_STRING(domain, domain_len)
        Z_PARAM_BOOL(secure)
        Z_PARAM_BOOL(httponly)
    ZEND_PARSE_PARAMETERS_END_EX(RETURN_FALSE);

    http_context *ctx = http_get_context(getThis(), 0);
    if (!ctx)
    {
        RETURN_FALSE;
    }
    zval *zresponse_object = ctx->response.zobject;
    zval *zcookie = sw_zend_read_property(swoole_http_response_ce_ptr, zresponse_object, ZEND_STRL("cookie"), 1);
    if (!ZVAL_IS_ARRAY(zcookie))
    {
        swoole_http_server_array_init(cookie, response);
    }

    int cookie_size = name_len + value_len + path_len + domain_len + 100;
    char *cookie = (char *) emalloc(cookie_size), *encoded_value = NULL, *date = NULL;

    if (name_len > 0 && strpbrk(name, "=,; \t\r\n\013\014") != NULL)
    {
        swoole_php_error(E_WARNING, "Cookie names can't contain any of the following '=,; \\t\\r\\n\\013\\014'");
        RETURN_FALSE;
    }
    if (value_len == 0)
    {
        date = sw_php_format_date((char *) ZEND_STRL("D, d-M-Y H:i:s T"), 1, 0);
        snprintf(cookie, cookie_size, "%s=deleted; expires=%s", name, date);
        efree(date);
    }
    else
    {
        if (url_encode)
        {
            int encoded_value_len;
            encoded_value = sw_php_url_encode(value, value_len, &encoded_value_len);
        }
        snprintf(cookie, cookie_size, "%s=%s", name, encoded_value ? encoded_value : value);
        if (expires > 0)
        {
            strlcat(cookie, "; expires=", cookie_size);
            date = sw_php_format_date((char *) ZEND_STRL("D, d-M-Y H:i:s T"), expires, 0);
            const char *p = (const char *) zend_memrchr(date, '-', strlen(date));
            if (!p || *(p + 5) != ' ')
            {
                swoole_php_error(E_WARNING, "Expiry date can't be a year greater than 9999");
                efree(date);
                efree(cookie);
                if (encoded_value)
                {
                    efree(encoded_value);
                }
                RETURN_FALSE;
            }
            strlcat(cookie, date, cookie_size);
            efree(date);
        }
    }
    if (path_len > 0)
    {
        strlcat(cookie, "; path=", cookie_size);
        strlcat(cookie, path, cookie_size);
    }
    if (domain_len > 0)
    {
        strlcat(cookie, "; domain=", cookie_size);
        strlcat(cookie, domain, cookie_size);
    }
    if (secure)
    {
        strlcat(cookie, "; secure", cookie_size);
    }
    if (httponly)
    {
        strlcat(cookie, "; httponly", cookie_size);
    }
    add_next_index_stringl(zcookie, cookie, strlen(cookie));
    efree(cookie);
    if (encoded_value)
    {
        efree(encoded_value);
    }
    RETURN_TRUE;
}

static PHP_METHOD(swoole_http_response, cookie)
{
    swoole_http_response_cookie(INTERNAL_FUNCTION_PARAM_PASSTHRU);
}

static PHP_METHOD(swoole_http_response, rawcookie)
{
    swoole_http_response_cookie(INTERNAL_FUNCTION_PARAM_PASSTHRU, false);
}

static PHP_METHOD(swoole_http_response, status)
{
    zend_long http_status;
    char* reason = NULL;
    size_t reason_len = 0;

    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_LONG(http_status)
        Z_PARAM_OPTIONAL
        Z_PARAM_STRING(reason, reason_len)
    ZEND_PARSE_PARAMETERS_END_EX(RETURN_FALSE);

    http_context *ctx = http_get_context(getThis(), 0);
    if (!ctx)
    {
        RETURN_FALSE;
    }

    ctx->response.status = http_status;
    if (reason_len > 0)
    {
        ctx->response.reason = (char *) emalloc(32);
        strncpy(ctx->response.reason, reason, 32 - 1);
    }
}

static PHP_METHOD(swoole_http_response, header)
{
    char *k, *v;
    size_t klen, vlen;
    zend_bool ucwords = 1;

    ZEND_PARSE_PARAMETERS_START(2, 3)
        Z_PARAM_STRING(k, klen)
        Z_PARAM_STRING(v, vlen)
        Z_PARAM_OPTIONAL
        Z_PARAM_BOOL(ucwords)
    ZEND_PARSE_PARAMETERS_END_EX(RETURN_FALSE);

    http_context *ctx = http_get_context(getThis(), 0);
    if (!ctx)
    {
        RETURN_FALSE;
    }

    zval *zresponse_object = ctx->response.zobject;
    zval *zheader = sw_zend_read_property(swoole_http_response_ce_ptr, zresponse_object, ZEND_STRL("header"), 1);
    if (!ZVAL_IS_ARRAY(zheader))
    {
        swoole_http_server_array_init(header, response);
    }
    if (klen > SW_HTTP_HEADER_KEY_SIZE - 1)
    {
        swoole_php_error(E_WARNING, "header key is too long.");
        RETURN_FALSE;
    }
    if (vlen > SW_HTTP_HEADER_VALUE_SIZE)
    {
        swoole_php_error(E_WARNING, "header value is too long.");
        RETURN_FALSE;
    }

    if (ucwords)
    {
        char key_buf[SW_HTTP_HEADER_KEY_SIZE];
        memcpy(key_buf, k, klen);
        key_buf[klen] = '\0';
#ifdef SW_USE_HTTP2
        if (ctx->stream)
        {
            swoole_strtolower(key_buf, klen);
        }
        else
#endif
        {
            http_header_key_format(key_buf, klen);
        }
        add_assoc_stringl_ex(zheader, key_buf, klen, v, vlen);
    }
    else
    {
        add_assoc_stringl_ex(zheader, k, klen, v, vlen);
    }

}

#ifdef SW_USE_HTTP2
static PHP_METHOD(swoole_http_response, trailer)
{
    char *k, *v;
    size_t klen, vlen;
    zend_bool ucwords = 1;

    if (zend_parse_parameters(ZEND_NUM_ARGS(), "ss|b", &k, &klen, &v, &vlen, &ucwords) == FAILURE)
    {
        RETURN_FALSE;
    }

    http_context *ctx = http_get_context(getThis(), 0);
    if (!ctx)
    {
        RETURN_FALSE;
    }

    zval *zresponse_object = ctx->response.zobject;
    zval *ztrailer = sw_zend_read_property(swoole_http_response_ce_ptr, zresponse_object, ZEND_STRL("trailer"), 1);
    if (!ZVAL_IS_ARRAY(ztrailer))
    {
        swoole_http_server_array_init(trailer, response);
    }
    if (klen > SW_HTTP_HEADER_KEY_SIZE - 1)
    {
        swoole_php_error(E_WARNING, "trailer key is too long.");
        RETURN_FALSE;
    }
    if (vlen > SW_HTTP_HEADER_VALUE_SIZE)
    {
        swoole_php_error(E_WARNING, "trailer value is too long.");
        RETURN_FALSE;
    }

    if (ucwords)
    {
        char key_buf[SW_HTTP_HEADER_KEY_SIZE];
        memcpy(key_buf, k, klen);
        key_buf[klen] = '\0';
#ifdef SW_USE_HTTP2
        if (ctx->stream)
        {
            swoole_strtolower(key_buf, klen);
        }
        else
#endif
        {
            http_header_key_format(key_buf, klen);
        }
        add_assoc_stringl_ex(ztrailer, key_buf, klen, v, vlen);
    }
    else
    {
        add_assoc_stringl_ex(ztrailer, k, klen, v, vlen);
    }
}
#endif

static PHP_METHOD(swoole_http_response, gzip)
{
    swoole_php_error(E_DEPRECATED, "gzip() is deprecated, use option[http_compression] instead.");
    RETURN_FALSE;
}

static PHP_METHOD(swoole_http_response, detach)
{
    http_context *context = http_get_context(getThis(), 0);
    if (!context)
    {
        RETURN_FALSE;
    }
    context->detached = 1;
    RETURN_TRUE;
}

static PHP_METHOD(swoole_http_response, create)
{
    zend_long fd;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(fd)
    ZEND_PARSE_PARAMETERS_END_EX(RETURN_FALSE);

    http_context *ctx = (http_context *) emalloc(sizeof(http_context));
    if (!ctx)
    {
        swoole_error_log(SW_LOG_ERROR, SW_ERROR_MALLOC_FAIL, "emalloc(%ld) failed.", sizeof(http_context));
        RETURN_FALSE;
    }
    bzero(ctx, sizeof(http_context));
    ctx->fd = (int) fd;

    object_init_ex(return_value, swoole_http_response_ce_ptr);
    swoole_set_object(return_value, ctx);
    ctx->response.zobject = return_value;
    sw_copy_to_stack(ctx->response.zobject, ctx->response._zobject);

    zend_update_property_long(swoole_http_response_ce_ptr, return_value, ZEND_STRL("fd"), ctx->fd);
}

static PHP_METHOD(swoole_http_response, redirect)
{
    zval *url;
    zval *http_code = NULL;

    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_ZVAL(url)
        Z_PARAM_OPTIONAL
        Z_PARAM_ZVAL(http_code)
    ZEND_PARSE_PARAMETERS_END_EX(RETURN_FALSE);

    http_context *ctx = http_get_context(getThis(), 0);
    if (!ctx)
    {
        RETURN_FALSE;
    }

    //status
    if (http_code)
    {
        ctx->response.status = zval_get_long(http_code);
    }
    else
    {
        ctx->response.status = 302;
    }

    //header
    zval key;
    ZVAL_STRINGL(&key, "Location", 8);
    zend_call_method_with_2_params(getThis(), NULL, NULL, "header", return_value, &key, url);
    zval_ptr_dtor(&key);
    if (!ZVAL_IS_NULL(return_value))
    {
        return;
    }

    //end
    zend_call_method_with_0_params(getThis(), NULL, NULL, "end", return_value);
}

static PHP_METHOD(swoole_http_response, __destruct)
{
    SW_PREVENT_USER_DESTRUCT;

    http_context *context = (http_context *) swoole_get_object(getThis());
    if (context)
    {
        swConnection *conn = swWorker_get_connection(SwooleG.serv, context->fd);
        if (!conn || conn->closed || conn->removed || context->detached)
        {
            swoole_http_context_free(context);
        }
        else
        {
            if (context->response.status == 0)
            {
                context->response.status = 500;
            }

            zval *zobject = getThis();
            zval *retval = NULL;
            sw_zend_call_method_with_0_params(&zobject, swoole_http_response_ce_ptr, NULL, "end", &retval);
            if (retval)
            {
                zval_ptr_dtor(retval);
            }

            context = (http_context *) swoole_get_object(getThis());
            if (context)
            {
                swoole_http_context_free(context);
            }
        }
    }
}
