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
#include "swoole_coroutine.h"
#include "socks5.h"
#include "mqtt.h"

#include "ext/standard/basic_functions.h"

enum client_property
{
    client_property_callback = 0,
    client_property_coroutine = 1,
    client_property_socket = 2,
};

using namespace swoole;

ZEND_BEGIN_ARG_INFO_EX(arginfo_swoole_client_coro_void, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_swoole_client_coro_construct, 0, 0, 1)
    ZEND_ARG_INFO(0, type)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_swoole_client_coro_set, 0, 0, 1)
    ZEND_ARG_ARRAY_INFO(0, settings, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_swoole_client_coro_connect, 0, 0, 1)
    ZEND_ARG_INFO(0, host)
    ZEND_ARG_INFO(0, port)
    ZEND_ARG_INFO(0, timeout)
    ZEND_ARG_INFO(0, sock_flag)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_swoole_client_coro_recv, 0, 0, 0)
    ZEND_ARG_INFO(0, timeout)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_swoole_client_coro_send, 0, 0, 1)
    ZEND_ARG_INFO(0, data)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_swoole_client_coro_peek, 0, 0, 0)
    ZEND_ARG_INFO(0, length)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_swoole_client_coro_sendfile, 0, 0, 1)
    ZEND_ARG_INFO(0, filename)
    ZEND_ARG_INFO(0, offset)
    ZEND_ARG_INFO(0, length)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_swoole_client_coro_sendto, 0, 0, 3)
    ZEND_ARG_INFO(0, address)
    ZEND_ARG_INFO(0, port)
    ZEND_ARG_INFO(0, data)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_swoole_client_coro_recvfrom, 0, 0, 2)
    ZEND_ARG_INFO(0, length)
    ZEND_ARG_INFO(1, address)
    ZEND_ARG_INFO(1, port)
ZEND_END_ARG_INFO()

static PHP_METHOD(swoole_client_coro, __construct);
static PHP_METHOD(swoole_client_coro, __destruct);
static PHP_METHOD(swoole_client_coro, set);
static PHP_METHOD(swoole_client_coro, connect);
static PHP_METHOD(swoole_client_coro, recv);
static PHP_METHOD(swoole_client_coro, peek);
static PHP_METHOD(swoole_client_coro, send);
static PHP_METHOD(swoole_client_coro, sendfile);
static PHP_METHOD(swoole_client_coro, sendto);
static PHP_METHOD(swoole_client_coro, recvfrom);
#ifdef SW_USE_OPENSSL
static PHP_METHOD(swoole_client_coro, enableSSL);
static PHP_METHOD(swoole_client_coro, getPeerCert);
static PHP_METHOD(swoole_client_coro, verifyPeerCert);
#endif
#ifdef SWOOLE_SOCKETS_SUPPORT
static PHP_METHOD(swoole_client_coro, getSocket);
#endif
static PHP_METHOD(swoole_client_coro, isConnected);
static PHP_METHOD(swoole_client_coro, getsockname);
static PHP_METHOD(swoole_client_coro, getpeername);
static PHP_METHOD(swoole_client_coro, close);

static void sw_coro_socket_set_ssl(Socket *cli, zval *zset);
static Socket* client_coro_new(zval *zobject, int port = 0);
bool php_swoole_client_coro_socket_free(Socket *cli);

static const zend_function_entry swoole_client_coro_methods[] =
{
    PHP_ME(swoole_client_coro, __construct, arginfo_swoole_client_coro_construct, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_client_coro, __destruct, arginfo_swoole_client_coro_void, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_client_coro, set, arginfo_swoole_client_coro_set, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_client_coro, connect, arginfo_swoole_client_coro_connect, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_client_coro, recv, arginfo_swoole_client_coro_recv, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_client_coro, peek, arginfo_swoole_client_coro_peek, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_client_coro, send, arginfo_swoole_client_coro_send, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_client_coro, sendfile, arginfo_swoole_client_coro_sendfile, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_client_coro, sendto, arginfo_swoole_client_coro_sendto, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_client_coro, recvfrom, arginfo_swoole_client_coro_recvfrom, ZEND_ACC_PUBLIC)
#ifdef SW_USE_OPENSSL
    PHP_ME(swoole_client_coro, enableSSL, arginfo_swoole_client_coro_void, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_client_coro, getPeerCert, arginfo_swoole_client_coro_void, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_client_coro, verifyPeerCert, arginfo_swoole_client_coro_void, ZEND_ACC_PUBLIC)
#endif
    PHP_ME(swoole_client_coro, isConnected, arginfo_swoole_client_coro_void, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_client_coro, getsockname, arginfo_swoole_client_coro_void, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_client_coro, getpeername, arginfo_swoole_client_coro_void, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_client_coro, close, arginfo_swoole_client_coro_void, ZEND_ACC_PUBLIC)
#ifdef SWOOLE_SOCKETS_SUPPORT
    PHP_ME(swoole_client_coro, getSocket, arginfo_swoole_client_coro_void, ZEND_ACC_PUBLIC)
#endif
    PHP_FE_END
};

static zend_class_entry swoole_client_coro_ce;
zend_class_entry *swoole_client_coro_ce_ptr;
static zend_object_handlers swoole_client_coro_handlers;

void swoole_client_coro_init(int module_number)
{
    SWOOLE_INIT_CLASS_ENTRY(swoole_client_coro, "Swoole\\Coroutine\\Client", NULL, "Co\\Client", swoole_client_coro_methods);
    SWOOLE_SET_CLASS_SERIALIZABLE(swoole_client_coro, zend_class_serialize_deny, zend_class_unserialize_deny);
    SWOOLE_SET_CLASS_CLONEABLE(swoole_client_coro, zend_class_clone_deny);
    SWOOLE_SET_CLASS_UNSET_PROPERTY_HANDLER(swoole_client_coro, zend_class_unset_property_deny);

    zend_declare_property_long(swoole_client_coro_ce_ptr, ZEND_STRL("errCode"), 0, ZEND_ACC_PUBLIC);
    zend_declare_property_long(swoole_client_coro_ce_ptr, ZEND_STRL("sock"), -1, ZEND_ACC_PUBLIC);
    zend_declare_property_long(swoole_client_coro_ce_ptr, ZEND_STRL("type"), 0, ZEND_ACC_PUBLIC);
    zend_declare_property_null(swoole_client_coro_ce_ptr, ZEND_STRL("setting"), ZEND_ACC_PUBLIC);
    zend_declare_property_bool(swoole_client_coro_ce_ptr, ZEND_STRL("connected"), 0, ZEND_ACC_PUBLIC);

    zend_declare_class_constant_long(swoole_client_coro_ce_ptr, ZEND_STRL("MSG_OOB"), MSG_OOB);
    zend_declare_class_constant_long(swoole_client_coro_ce_ptr, ZEND_STRL("MSG_PEEK"), MSG_PEEK);
    zend_declare_class_constant_long(swoole_client_coro_ce_ptr, ZEND_STRL("MSG_DONTWAIT"), MSG_DONTWAIT);
    zend_declare_class_constant_long(swoole_client_coro_ce_ptr, ZEND_STRL("MSG_WAITALL"), MSG_WAITALL);
}

static sw_inline Socket* client_get_ptr(zval *zobject, bool silent = false)
{
    Socket *cli = (Socket *) swoole_get_object(zobject);
    if (cli && cli->socket && cli->socket->active == 1)
    {
        return cli;
    }
    else
    {
        if (!silent)
        {
            SwooleG.error = SW_ERROR_CLIENT_NO_CONNECTION;
            zend_update_property_long(swoole_client_coro_ce_ptr, zobject, ZEND_STRL("errCode"), SwooleG.error);
            swoole_php_error(E_WARNING, "client is not connected to server.");
        }
        return nullptr;
    }
}

static Socket* client_coro_new(zval *zobject, int port)
{
    zval *ztype;

    ztype = sw_zend_read_property(Z_OBJCE_P(zobject), zobject, ZEND_STRL("type"), 0);

    if (ztype == NULL || ZVAL_IS_NULL(ztype))
    {
        swoole_php_fatal_error(E_ERROR, "failed to get swoole_client->type.");
        return NULL;
    }

    long type = Z_LVAL_P(ztype);
    if ((type == SW_SOCK_TCP || type == SW_SOCK_TCP6) && (port <= 0 || port > SW_CLIENT_MAX_PORT))
    {
        swoole_php_fatal_error(E_WARNING, "The port is invalid.");
        return NULL;
    }

    Socket *cli = new Socket((enum swSocket_type) type);
    if (unlikely(cli->socket == nullptr))
    {
        swoole_php_fatal_error(E_WARNING, "new Socket() failed. Error: %s [%d]", strerror(errno), errno);
        zend_update_property_long(Z_OBJCE_P(zobject), zobject, ZEND_STRL("errCode"), errno);
        delete cli;
        return NULL;
    }

    zend_update_property_long(Z_OBJCE_P(zobject), zobject, ZEND_STRL("sock"), cli->socket->fd);

#ifdef SW_USE_OPENSSL
    if (type & SW_SOCK_SSL)
    {
        cli->open_ssl = true;
    }
#endif

    swoole_set_object(zobject, cli);

    return cli;
}

static bool client_coro_close(zval *zobject)
{
    Socket *cli = (Socket *) swoole_get_object(zobject);

    zend_update_property_bool(Z_OBJCE_P(zobject), zobject, ZEND_STRL("connected"), 0);

    if (!cli)
    {
        return false;
    }
#ifdef SWOOLE_SOCKETS_SUPPORT
    zval *zsocket = (zval *) swoole_get_property(zobject, client_property_socket);
    if (zsocket)
    {
        sw_zval_free(zsocket);
        swoole_set_property(zobject, client_property_socket, NULL);
    }
#endif
    swoole_set_object(zobject, NULL);

    return php_swoole_client_coro_socket_free(cli);
}

bool php_swoole_client_coro_socket_free(Socket *cli)
{
    bool ret;
    //TODO: move to Socket method, we should not manage it externally
    //socks5 proxy config
    if (cli->socks5_proxy)
    {
        efree(cli->socks5_proxy->host);
        cli->socks5_proxy->host = nullptr;
        if (cli->socks5_proxy->username)
        {
            efree(cli->socks5_proxy->username);
            cli->socks5_proxy->username = nullptr;
        }
        if (cli->socks5_proxy->password)
        {
            efree(cli->socks5_proxy->password);
            cli->socks5_proxy->password = nullptr;
        }
        efree(cli->socks5_proxy);
        cli->socks5_proxy = nullptr;
    }
    //http proxy config
    if (cli->http_proxy)
    {
        efree(cli->http_proxy->proxy_host);
        cli->http_proxy->proxy_host = nullptr;
        if (cli->http_proxy->user)
        {
            efree(cli->http_proxy->user);
            cli->http_proxy->user = nullptr;
        }
        if (cli->http_proxy->password)
        {
            efree(cli->http_proxy->password);
            cli->http_proxy->password = nullptr;
        }
        efree(cli->http_proxy);
        cli->http_proxy = nullptr;
    }
    if (cli->protocol.private_data)
    {
        zval *zcallback = (zval *) cli->protocol.private_data;
        sw_zval_free(zcallback);
        cli->protocol.private_data = nullptr;
    }
    ret = cli->close();
    delete cli;

    return ret;
}

void sw_coro_socket_set(Socket *cli, zval *zset)
{
    HashTable *vht;
    zval *v;
    int value = 1;

    char *bind_address = NULL;
    int bind_port = 0;

    vht = Z_ARRVAL_P(zset);

    //buffer: eof check
    if (php_swoole_array_get_value(vht, "open_eof_check", v))
    {
        cli->open_eof_check = zval_is_true(v);
    }
    //buffer: split package with eof
    if (php_swoole_array_get_value(vht, "open_eof_split", v))
    {
        cli->protocol.split_by_eof = zval_is_true(v);
        if (cli->protocol.split_by_eof)
        {
            cli->open_eof_check = 1;
        }
    }
    //package eof
    if (php_swoole_array_get_value(vht, "package_eof", v))
    {
        convert_to_string(v);
        cli->protocol.package_eof_len = Z_STRLEN_P(v);
        if (cli->protocol.package_eof_len == 0)
        {
            swoole_php_fatal_error(E_ERROR, "pacakge_eof cannot be an empty string");
            return;
        }
        else if (cli->protocol.package_eof_len > SW_DATA_EOF_MAXLEN)
        {
            swoole_php_fatal_error(E_ERROR, "pacakge_eof max length is %d", SW_DATA_EOF_MAXLEN);
            return;
        }
        bzero(cli->protocol.package_eof, SW_DATA_EOF_MAXLEN);
        memcpy(cli->protocol.package_eof, Z_STRVAL_P(v), Z_STRLEN_P(v));
    }
    //open mqtt protocol
    if (php_swoole_array_get_value(vht, "open_mqtt_protocol", v))
    {
        cli->open_length_check = zval_is_true(v);
        cli->protocol.get_package_length = swMqtt_get_package_length;
    }
    //open length check
    if (php_swoole_array_get_value(vht, "open_length_check", v))
    {
        cli->open_length_check = zval_is_true(v);
        cli->protocol.get_package_length = swProtocol_get_package_length;
    }
    //package length size
    if (php_swoole_array_get_value(vht, "package_length_type", v))
    {
        convert_to_string(v);
        cli->protocol.package_length_type = Z_STRVAL_P(v)[0];
        cli->protocol.package_length_size = swoole_type_size(cli->protocol.package_length_type);

        if (cli->protocol.package_length_size == 0)
        {
            swoole_php_fatal_error(E_ERROR, "Unknown package_length_type name '%c', see pack(). Link: http://php.net/pack", cli->protocol.package_length_type);
            return;
        }
    }
    //length function
    if (php_swoole_array_get_value(vht, "package_length_func", v))
    {
        while(1)
        {
            if (Z_TYPE_P(v) == IS_STRING)
            {
                swProtocol_length_function func = (swProtocol_length_function) swoole_get_function(Z_STRVAL_P(v),
                        Z_STRLEN_P(v));
                if (func != NULL)
                {
                    cli->protocol.get_package_length = func;
                    break;
                }
            }

            char *func_name = NULL;
            if (!sw_zend_is_callable(v, 0, &func_name))
            {
                swoole_php_fatal_error(E_ERROR, "function '%s' is not callable", func_name);
                return;
            }
            efree(func_name);
            cli->protocol.get_package_length = php_swoole_length_func;
            Z_TRY_ADDREF_P(v);
            cli->protocol.private_data = sw_zval_dup(v);
            break;
        }

        cli->protocol.package_length_size = 0;
        cli->protocol.package_length_type = '\0';
        cli->protocol.package_length_offset = SW_IPC_BUFFER_SIZE;
    }
    //package length offset
    if (php_swoole_array_get_value(vht, "package_length_offset", v))
    {
        cli->protocol.package_length_offset = (int) zval_get_long(v);
    }
    //package body start
    if (php_swoole_array_get_value(vht, "package_body_offset", v))
    {
        cli->protocol.package_body_offset = (int) zval_get_long(v);
    }
    /**
     * package max length
     */
    if (php_swoole_array_get_value(vht, "package_max_length", v))
    {
        cli->protocol.package_max_length = (int) zval_get_long(v);
    }
    else
    {
        cli->protocol.package_max_length = SW_BUFFER_INPUT_SIZE;
    }
    /**
     * socket send/recv buffer size
     */
    if (php_swoole_array_get_value(vht, "socket_buffer_size", v))
    {
        value = (int) zval_get_long(v);
        if (value <= 0)
        {
            value = SW_MAX_INT;
        }
        swSocket_set_buffer_size(cli->socket->fd, value);
        cli->socket->buffer_size = value;
    }
    /**
     * bind address
     */
    if (php_swoole_array_get_value(vht, "bind_address", v))
    {
        convert_to_string(v);
        bind_address = Z_STRVAL_P(v);
    }
    /**
     * bind port
     */
    if (php_swoole_array_get_value(vht, "bind_port", v))
    {
        bind_port = (int) zval_get_long(v);
    }
    if (bind_address)
    {
        swSocket_bind(cli->socket->fd, cli->type, bind_address, &bind_port);
    }
    /**
     * client: tcp_nodelay
     */
    if (php_swoole_array_get_value(vht, "open_tcp_nodelay", v))
    {
        cli->set_tcp_nodelay(zval_is_true(v));
    }
    else
    {
        cli->set_tcp_nodelay(1);
    }

    /**
     * socks5 proxy
     */
    if (php_swoole_array_get_value(vht, "socks5_host", v))
    {
        convert_to_string(v);
        cli->socks5_proxy = (struct _swSocks5 *) emalloc(sizeof(swSocks5));
        bzero(cli->socks5_proxy, sizeof(swSocks5));
        cli->socks5_proxy->host = estrdup(Z_STRVAL_P(v));
        cli->socks5_proxy->dns_tunnel = 1;

        if (php_swoole_array_get_value(vht, "socks5_port", v))
        {
            cli->socks5_proxy->port = zval_get_long(v);
        }
        else
        {
            swoole_php_fatal_error(E_ERROR, "socks5 proxy require server port option.");
            return;
        }
        if (php_swoole_array_get_value(vht, "socks5_username", v))
        {
            convert_to_string(v);
            cli->socks5_proxy->username = estrdup(Z_STRVAL_P(v));
            cli->socks5_proxy->l_username = Z_STRLEN_P(v);
            cli->socks5_proxy->method = 0x02;
        }
        if (php_swoole_array_get_value(vht, "socks5_password", v))
        {
            convert_to_string(v);
            cli->socks5_proxy->password = estrdup(Z_STRVAL_P(v));
            cli->socks5_proxy->l_password = Z_STRLEN_P(v);
        }
    }
    /**
     * http proxy
     */
    else if (php_swoole_array_get_value(vht, "http_proxy_host", v))
    {
        convert_to_string(v);
        char *host = Z_STRVAL_P(v);
        if (php_swoole_array_get_value(vht, "http_proxy_port", v))
        {
            cli->http_proxy = (struct _http_proxy*) ecalloc(1, sizeof(struct _http_proxy));
            cli->http_proxy->proxy_host = estrdup(host);
            cli->http_proxy->proxy_port = zval_get_long(v);
            if (php_swoole_array_get_value(vht, "http_proxy_user", v))
            {
                convert_to_string(v);
                char *user = Z_STRVAL_P(v);
                zval *v2;
                if (php_swoole_array_get_value(vht, "http_proxy_password", v2))
                {
                    convert_to_string(v);
                    if (Z_STRLEN_P(v) + Z_STRLEN_P(v2) >= 128 - 1)
                    {
                        swoole_php_fatal_error(E_WARNING, "http_proxy user and password is too long.");
                    }
                    cli->http_proxy->l_user = Z_STRLEN_P(v);
                    cli->http_proxy->l_password = Z_STRLEN_P(v2);
                    cli->http_proxy->user = estrdup(user);
                    cli->http_proxy->password = estrdup(Z_STRVAL_P(v2));
                }
                else
                {
                    swoole_php_fatal_error(E_WARNING, "http_proxy_password can not be null.");
                }
            }
            //https proxy
            if (php_swoole_array_get_value(vht, "http_proxy_ssl", v))
            {
                cli->http_proxy->ssl = zval_is_true(v);
            }
        }
        else
        {
            swoole_php_fatal_error(E_WARNING, "http_proxy_port can not be null.");
        }
    }

#ifdef SW_USE_OPENSSL
    if (cli->open_ssl)
    {
        sw_coro_socket_set_ssl(cli, zset);
    }
#endif
}

#ifdef SW_USE_OPENSSL
static void sw_coro_socket_set_ssl(Socket *cli, zval *zset)
{
    HashTable *vht = Z_ARRVAL_P(zset);
    zval *v;

    if (php_swoole_array_get_value(vht, "ssl_method", v))
    {
        cli->ssl_option.method = (int) zval_get_long(v);
    }
    if (php_swoole_array_get_value(vht, "ssl_compress", v))
    {
        cli->ssl_option.disable_compress = !zval_is_true(v);
    }
    if (php_swoole_array_get_value(vht, "ssl_cert_file", v))
    {
        convert_to_string(v);
        cli->ssl_option.cert_file = sw_strdup(Z_STRVAL_P(v));
        if (access(cli->ssl_option.cert_file, R_OK) < 0)
        {
            swoole_php_fatal_error(E_ERROR, "ssl cert file[%s] not found.", cli->ssl_option.cert_file);
            return;
        }
    }
    if (php_swoole_array_get_value(vht, "ssl_key_file", v))
    {
        convert_to_string(v);
        cli->ssl_option.key_file = sw_strdup(Z_STRVAL_P(v));
        if (access(cli->ssl_option.key_file, R_OK) < 0)
        {
            swoole_php_fatal_error(E_ERROR, "ssl key file[%s] not found.", cli->ssl_option.key_file);
            return;
        }
    }
    if (php_swoole_array_get_value(vht, "ssl_passphrase", v))
    {
        convert_to_string(v);
        cli->ssl_option.passphrase = sw_strdup(Z_STRVAL_P(v));
    }
#ifdef SSL_CTRL_SET_TLSEXT_HOSTNAME
    if (php_swoole_array_get_value(vht, "ssl_host_name", v))
    {
        convert_to_string(v);
        cli->ssl_option.tls_host_name = sw_strdup(Z_STRVAL_P(v));
    }
#endif
    if (php_swoole_array_get_value(vht, "ssl_verify_peer", v))
    {
        cli->ssl_option.verify_peer = zval_is_true(v);
    }
    if (php_swoole_array_get_value(vht, "ssl_allow_self_signed", v))
    {
        cli->ssl_option.allow_self_signed = zval_is_true(v);
    }
    if (php_swoole_array_get_value(vht, "ssl_cafile", v))
    {
        convert_to_string(v);
        cli->ssl_option.cafile = sw_strdup(Z_STRVAL_P(v));
    }
    if (php_swoole_array_get_value(vht, "ssl_capath", v))
    {
        convert_to_string(v);
        cli->ssl_option.capath = sw_strdup(Z_STRVAL_P(v));
    }
    if (php_swoole_array_get_value(vht, "ssl_verify_depth", v))
    {
        cli->ssl_option.verify_depth = (int) zval_get_long(v);
    }
    if (cli->ssl_option.cert_file && !cli->ssl_option.key_file)
    {
        swoole_php_fatal_error(E_ERROR, "ssl require key file.");
        return;
    }
}
#endif

static PHP_METHOD(swoole_client_coro, __construct)
{
    zend_long type = 0;

    if (zend_parse_parameters(ZEND_NUM_ARGS(), "l|ls", &type) == FAILURE)
    {
        swoole_php_fatal_error(E_ERROR, "socket type param is required.");
        RETURN_FALSE;
    }

    int client_type = php_swoole_socktype(type);
    if (client_type < SW_SOCK_TCP || client_type > SW_SOCK_UNIX_STREAM)
    {
        swoole_php_fatal_error(E_ERROR, "Unknown client type '%d'.", client_type);
    }

    php_swoole_check_reactor();

    zend_update_property_long(swoole_client_coro_ce_ptr, getThis(), ZEND_STRL("type"), type);
    //init
    swoole_set_object(getThis(), NULL);
    swoole_set_property(getThis(), client_property_callback, NULL);
#ifdef SWOOLE_SOCKETS_SUPPORT
    swoole_set_property(getThis(), client_property_socket, NULL);
#endif
    RETURN_TRUE;
}

static PHP_METHOD(swoole_client_coro, __destruct)
{
    SW_PREVENT_USER_DESTRUCT;

    client_coro_close(getThis());
}

static PHP_METHOD(swoole_client_coro, set)
{
    Socket *cli = client_get_ptr(getThis(), true);
    zval *zset, *zsetting;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_ARRAY(zset)
    ZEND_PARSE_PARAMETERS_END_EX(RETURN_FALSE);

    zsetting = sw_zend_read_property_array(swoole_client_coro_ce_ptr, getThis(), ZEND_STRL("setting"), 1);
    php_array_merge(Z_ARRVAL_P(zsetting), Z_ARRVAL_P(zset));
    if (cli)
    {
        sw_coro_socket_set(cli, zset);
    }
    RETURN_TRUE;
}

static PHP_METHOD(swoole_client_coro, connect)
{
    char *host;
    size_t host_len;
    zend_long port = 0;
    double timeout = 0;
    zend_long sock_flag = 0;

    ZEND_PARSE_PARAMETERS_START(1, 4)
        Z_PARAM_STRING(host, host_len)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(port)
        Z_PARAM_DOUBLE(timeout)
        Z_PARAM_LONG(sock_flag)
    ZEND_PARSE_PARAMETERS_END_EX(RETURN_FALSE);

    if (host_len == 0)
    {
        swoole_php_fatal_error(E_ERROR, "The host is empty.");
        RETURN_FALSE;
    }

    Socket *cli = (Socket *) swoole_get_object(getThis());
    if (cli)
    {
        swoole_php_fatal_error(E_WARNING, "connection to the server has already been established.");
        RETURN_FALSE;
    }

    cli = client_coro_new(getThis(), (int) port);
    if (cli == NULL)
    {
        RETURN_FALSE;
    }

    zval *zset = sw_zend_read_property(swoole_client_coro_ce_ptr, getThis(), ZEND_STRL("setting"), 1);
    if (zset && ZVAL_IS_ARRAY(zset))
    {
        sw_coro_socket_set(cli, zset);
    }

    PHPCoroutine::check_bind("client", cli->has_bound());
    cli->set_timeout(timeout == 0 ? PHPCoroutine::socket_connect_timeout : timeout);
    if (!cli->connect(host, port, sock_flag))
    {
        swoole_php_error(E_WARNING, "connect to server[%s:%d] failed. Error: %s[%d]", host, (int )port, cli->errMsg, cli->errCode);
        zend_update_property_long(swoole_client_coro_ce_ptr, getThis(), ZEND_STRL("errCode"), cli->errCode);
        client_coro_close(getThis());
        RETURN_FALSE;
    }
    else
    {
        cli->set_timeout(timeout == 0 ? PHPCoroutine::socket_timeout : timeout);
        zend_update_property_bool(swoole_client_coro_ce_ptr, getThis(), ZEND_STRL("connected"), 1);
        RETURN_TRUE;
    }
}

static PHP_METHOD(swoole_client_coro, send)
{
    char *data;
    size_t data_len;
    double timeout = 0;

    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_STRING(data, data_len)
        Z_PARAM_OPTIONAL
        Z_PARAM_DOUBLE(timeout)
    ZEND_PARSE_PARAMETERS_END_EX(RETURN_FALSE);

    if (data_len == 0)
    {
        swoole_php_fatal_error(E_WARNING, "data to send is empty.");
        RETURN_FALSE;
    }

    Socket *cli = client_get_ptr(getThis());
    if (!cli)
    {
        RETURN_FALSE;
    }

    //clear errno
    SwooleG.error = 0;
    PHPCoroutine::check_bind("client", cli->has_bound());
    double persistent_timeout = cli->get_timeout();
    cli->set_timeout(timeout);
    int ret = cli->send_all(data, data_len);
    cli->set_timeout(persistent_timeout);
    if (ret < 0)
    {
        swoole_php_sys_error(E_WARNING, "failed to send(%d) %zd bytes.", cli->socket->fd, data_len);
        zend_update_property_long(swoole_client_coro_ce_ptr, getThis(), ZEND_STRL("errCode"), SwooleG.error);
        RETVAL_FALSE;
    }
    else
    {
        RETURN_LONG(ret);
    }
}

static PHP_METHOD(swoole_client_coro, sendto)
{
    char* ip;
    size_t ip_len;
    long port;
    char *data;
    size_t len;

    if (zend_parse_parameters(ZEND_NUM_ARGS(), "sls", &ip, &ip_len, &port, &data, &len) == FAILURE)
    {
        RETURN_FALSE;
    }

    if (len == 0)
    {
        swoole_php_error(E_WARNING, "data to send is empty.");
        RETURN_FALSE;
    }

    Socket *cli = (Socket *) swoole_get_object(getThis());
    if (!cli)
    {
        cli = client_coro_new(getThis(), (int) port);
        if (cli == NULL)
        {
            RETURN_FALSE;
        }
        cli->socket->active = 1;
    }
    PHPCoroutine::check_bind("client", cli->has_bound());
    SW_CHECK_RETURN(cli->sendto(ip, port, data, len));
}

static PHP_METHOD(swoole_client_coro, recvfrom)
{
    zend_long length;
    zval *address, *port;

    if (zend_parse_parameters(ZEND_NUM_ARGS(), "lz/|z/", &length, &address, &port) == FAILURE)
    {
        RETURN_FALSE;
    }

    if (length <= 0)
    {
        swoole_php_error(E_WARNING, "invalid length.");
        RETURN_FALSE;
    }

    Socket *cli = (Socket *) swoole_get_object(getThis());
    if (!cli)
    {
        cli = client_coro_new(getThis());
        if (cli == NULL)
        {
            RETURN_FALSE;
        }
        cli->socket->active = 1;
    }

    zend_string *retval = zend_string_alloc(length + 1, 0);
    PHPCoroutine::check_bind("client", cli->has_bound());
    // cli->set_timeout(timeout, true); TODO
    ssize_t n_bytes = cli->recvfrom(retval->val, length);
    if (n_bytes < 0)
    {
        zend_string_free(retval);
        RETURN_FALSE;
    }
    else
    {
        ZSTR_LEN(retval) = n_bytes;
        ZSTR_VAL(retval)[ZSTR_LEN(retval)] = '\0';
        ZVAL_STRING(address, swConnection_get_ip(cli->socket));
        if (port)
        {
            ZVAL_LONG(port, swConnection_get_port(cli->socket));
        }
        RETURN_STR(retval);
    }
}

static PHP_METHOD(swoole_client_coro, sendfile)
{
    char *file;
    size_t file_len;
    zend_long offset = 0;
    zend_long length = 0;

    if (zend_parse_parameters(ZEND_NUM_ARGS(), "s|ll", &file, &file_len, &offset, &length) == FAILURE)
    {
        RETURN_FALSE;
    }
    if (file_len == 0)
    {
        swoole_php_fatal_error(E_WARNING, "file to send is empty.");
        RETURN_FALSE;
    }

    Socket *cli = client_get_ptr(getThis());
    if (!cli)
    {
        RETURN_FALSE;
    }
    //only stream socket can sendfile
    if (!(cli->type == SW_SOCK_TCP || cli->type == SW_SOCK_TCP6 || cli->type == SW_SOCK_UNIX_STREAM))
    {
        swoole_php_error(E_WARNING, "dgram socket cannot use sendfile.");
        RETURN_FALSE;
    }
    //clear errno
    SwooleG.error = 0;
    PHPCoroutine::check_bind("client", cli->has_bound());
    int ret = cli->sendfile(file, offset, length);
    if (ret < 0)
    {
        SwooleG.error = errno;
        swoole_php_fatal_error(E_WARNING, "sendfile() failed. Error: %s [%d]", strerror(SwooleG.error), SwooleG.error);
        zend_update_property_long(swoole_client_coro_ce_ptr, getThis(), ZEND_STRL("errCode"), SwooleG.error);
        RETVAL_FALSE;
    }
    else
    {
        RETVAL_TRUE;
    }
}

static PHP_METHOD(swoole_client_coro, recv)
{
    double timeout = 0;

    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_DOUBLE(timeout)
    ZEND_PARSE_PARAMETERS_END_EX(RETURN_FALSE);

    Socket *cli = client_get_ptr(getThis());
    if (!cli)
    {
        RETURN_FALSE;
    }
    PHPCoroutine::check_bind("client", cli->has_bound());
    ssize_t retval ;
    if (cli->open_length_check || cli->open_eof_check)
    {
        cli->set_timer(Socket::TIMER_LV_GLOBAL, timeout);
        retval = cli->recv_packet();
        cli->del_timer(Socket::TIMER_LV_GLOBAL);
        if (retval > 0)
        {
            RETVAL_STRINGL(cli->read_buffer->str, retval);
        }
    }
    else
    {
        zend_string *result = zend_string_alloc(SW_PHP_CLIENT_BUFFER_SIZE - sizeof(zend_string), 0);
        double persistent_timeout = cli->get_timeout();
        cli->set_timeout(timeout);
        retval = cli->recv(result->val, SW_PHP_CLIENT_BUFFER_SIZE - sizeof(zend_string));
        cli->set_timeout(persistent_timeout);
        if (retval > 0)
        {
            result->val[retval] = 0;
            result->len = retval;
            RETURN_STR(result);
        }
        else
        {
            zend_string_free(result);
        }
    }
    if (retval < 0)
    {
        SwooleG.error = cli->errCode;
        swoole_php_error(E_WARNING, "recv() failed. Error: %s [%d]", strerror(SwooleG.error), SwooleG.error);
        zend_update_property_long(swoole_client_coro_ce_ptr, getThis(), ZEND_STRL("errCode"), SwooleG.error);
        RETURN_FALSE;
    }
    else if (retval == 0)
    {
        RETURN_EMPTY_STRING();
    }
}

static PHP_METHOD(swoole_client_coro, peek)
{
    zend_long buf_len = SW_PHP_CLIENT_BUFFER_SIZE;
    int ret;
    char *buf = NULL;

    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(buf_len)
    ZEND_PARSE_PARAMETERS_END_EX(RETURN_FALSE);

    Socket *cli = client_get_ptr(getThis());
    if (!cli)
    {
        RETURN_FALSE;
    }

    SwooleG.error = 0;
    buf = (char *) emalloc(buf_len + 1);
    ret = cli->peek(buf, buf_len);
    if (ret < 0)
    {
        SwooleG.error = errno;
        swoole_php_error(E_WARNING, "recv() failed. Error: %s [%d]", strerror(SwooleG.error), SwooleG.error);
        zend_update_property_long(swoole_client_coro_ce_ptr, getThis(), ZEND_STRL("errCode"),
                SwooleG.error);
        efree(buf);
        RETURN_FALSE;
    }
    else
    {
        buf[ret] = 0;
        RETVAL_STRINGL(buf, ret);
        efree(buf);
    }
}

static PHP_METHOD(swoole_client_coro, isConnected)
{
    Socket *cli = (Socket *) swoole_get_object(getThis());
    if (cli && cli->socket && cli->socket->active == 1)
    {
        RETURN_TRUE;
    }
    else
    {
        RETURN_FALSE;
    }
}

static PHP_METHOD(swoole_client_coro, getsockname)
{
    Socket *cli = client_get_ptr(getThis());
    if (!cli)
    {
        RETURN_FALSE;
    }

    if (cli->type == SW_SOCK_UNIX_STREAM || cli->type == SW_SOCK_UNIX_DGRAM)
    {
        swoole_php_fatal_error(E_WARNING, "getsockname() only support AF_INET family socket.");
        RETURN_FALSE;
    }

    cli->socket->info.len = sizeof(cli->socket->info.addr);
    if (getsockname(cli->socket->fd, (struct sockaddr*) &cli->socket->info.addr, &cli->socket->info.len) < 0)
    {
        swoole_php_sys_error(E_WARNING, "getsockname() failed.");
        RETURN_FALSE;
    }

    array_init(return_value);
    if (cli->type == SW_SOCK_UDP6 || cli->type == SW_SOCK_TCP6)
    {
        add_assoc_long(return_value, "port", ntohs(cli->socket->info.addr.inet_v6.sin6_port));
        char tmp[INET6_ADDRSTRLEN];
        if (inet_ntop(AF_INET6, &cli->socket->info.addr.inet_v6.sin6_addr, tmp, sizeof(tmp)))
        {
            add_assoc_string(return_value, "host", tmp);
        }
        else
        {
            swoole_php_fatal_error(E_WARNING, "inet_ntop() failed.");
        }
    }
    else
    {
        add_assoc_long(return_value, "port", ntohs(cli->socket->info.addr.inet_v4.sin_port));
        add_assoc_string(return_value, "host", inet_ntoa(cli->socket->info.addr.inet_v4.sin_addr));
    }
}

#ifdef SWOOLE_SOCKETS_SUPPORT
static PHP_METHOD(swoole_client_coro, getSocket)
{
    zval *zsocket = (zval *) swoole_get_property(getThis(), client_property_socket);
    if (zsocket)
    {
        RETURN_ZVAL(zsocket, 1, NULL);
    }
    Socket *cli = client_get_ptr(getThis());
    if (!cli)
    {
        RETURN_FALSE;
    }
    php_socket *socket_object = swoole_convert_to_socket(cli->socket->fd);
    if (!socket_object)
    {
        RETURN_FALSE;
    }
    SW_ZEND_REGISTER_RESOURCE(return_value, (void * ) socket_object, php_sockets_le_socket());
    zsocket = sw_zval_dup(return_value);
    Z_TRY_ADDREF_P(zsocket);
    swoole_set_property(getThis(), client_property_socket, zsocket);
}
#endif

static PHP_METHOD(swoole_client_coro, getpeername)
{
    Socket *cli = client_get_ptr(getThis());
    if (!cli)
    {
        RETURN_FALSE;
    }

    if (cli->type == SW_SOCK_UDP)
    {
        array_init(return_value);
        add_assoc_long(return_value, "port", ntohs(cli->socket->info.addr.inet_v4.sin_port));
        add_assoc_string(return_value, "host", inet_ntoa(cli->socket->info.addr.inet_v4.sin_addr));
    }
    else if (cli->type == SW_SOCK_UDP6)
    {
        array_init(return_value);
        add_assoc_long(return_value, "port", ntohs(cli->socket->info.addr.inet_v6.sin6_port));
        char tmp[INET6_ADDRSTRLEN];

        if (inet_ntop(AF_INET6, &cli->socket->info.addr.inet_v6.sin6_addr, tmp, sizeof(tmp)))
        {
            add_assoc_string(return_value, "host", tmp);
        }
        else
        {
            swoole_php_fatal_error(E_WARNING, "inet_ntop() failed.");
        }
    }
    else
    {
        swoole_php_fatal_error(E_WARNING, "only supports SWOOLE_SOCK_UDP or SWOOLE_SOCK_UDP6.");
        RETURN_FALSE;
    }
}

static PHP_METHOD(swoole_client_coro, close)
{
    RETURN_BOOL(client_coro_close(getThis()));
}

#ifdef SW_USE_OPENSSL
static PHP_METHOD(swoole_client_coro, enableSSL)
{
    Socket *cli = client_get_ptr(getThis());
    if (!cli)
    {
        RETURN_FALSE;
    }
    if (cli->type != SW_SOCK_TCP && cli->type != SW_SOCK_TCP6)
    {
        swoole_php_fatal_error(E_WARNING, "cannot use enableSSL.");
        RETURN_FALSE;
    }
    if (cli->socket->ssl)
    {
        swoole_php_fatal_error(E_WARNING, "SSL has been enabled.");
        RETURN_FALSE;
    }
    cli->open_ssl = true;
    zval *zset = sw_zend_read_property(swoole_client_coro_ce_ptr, getThis(), ZEND_STRL("setting"), 0);
    if (ZVAL_IS_ARRAY(zset))
    {
        sw_coro_socket_set_ssl(cli, zset);
    }
    PHPCoroutine::check_bind("client", cli->has_bound());
    if (cli->ssl_handshake() == false)
    {
        RETURN_FALSE;
    }
    RETURN_TRUE;
}

static PHP_METHOD(swoole_client_coro, getPeerCert)
{
    Socket *cli = client_get_ptr(getThis());
    if (!cli)
    {
        RETURN_FALSE;
    }
    if (!cli->socket->ssl)
    {
        swoole_php_fatal_error(E_WARNING, "SSL is not ready.");
        RETURN_FALSE;
    }
    char buf[8192];
    int n = swSSL_get_client_certificate(cli->socket->ssl, buf, sizeof(buf));
    if (n < 0)
    {
        RETURN_FALSE;
    }
    RETURN_STRINGL(buf, n);
}

static PHP_METHOD(swoole_client_coro, verifyPeerCert)
{
    Socket *cli = client_get_ptr(getThis());
    if (!cli)
    {
        RETURN_FALSE;
    }
    if (!cli->socket->ssl)
    {
        swoole_php_fatal_error(E_WARNING, "SSL is not ready.");
        RETURN_FALSE;
    }
    zend_bool allow_self_signed = 0;
    if (zend_parse_parameters(ZEND_NUM_ARGS(), "|b", &allow_self_signed) == FAILURE)
    {
        RETURN_FALSE;
    }
    SW_CHECK_RETURN(cli->ssl_verify(allow_self_signed));
}
#endif
