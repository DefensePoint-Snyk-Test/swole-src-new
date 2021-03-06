#!/bin/sh -e
__CURRENT__=`pwd`
__DIR__=$(cd "$(dirname "$0")";pwd)

[ -z "${TRAVIS_BRANCH}" ] && export TRAVIS_BRANCH="master"
[ -z "${TRAVIS_BUILD_DIR}" ] && export TRAVIS_BUILD_DIR=$(cd "$(dirname "$0")";cd ../;pwd)
export DOCKER_COMPOSE_VERSION="1.21.0"
export PHP_VERSION_ID=`php -r "echo PHP_VERSION_ID;"`
if [ ${PHP_VERSION_ID} -lt 70300 ]; then
    export PHP_VERSION="`php -r "echo PHP_MAJOR_VERSION;"`.`php -r "echo PHP_MINOR_VERSION;"`"
else
    export PHP_VERSION="rc"
fi
if [ "${TRAVIS_BRANCH}" = "alpine" ]; then
    export PHP_VERSION="${PHP_VERSION}-alpine"
fi

check_docker_dependency(){
    if [ "`docker -v 2>&1 | grep "version"`"x = ""x ]; then
        echo "\nā Docker not found!"
        exit 255
    else
        which "docker-compose" > /dev/null
        if [ $? -ne 0 ]; then
            echo "\nš¤ Can not found docker-compose, try to install it now...\n"
            curl -L https://github.com/docker/compose/releases/download/${DOCKER_COMPOSE_VERSION}/docker-compose-`uname -s`-`uname -m` > docker-compose && \
            chmod +x docker-compose && \
            sudo mv docker-compose /usr/local/bin && \
            docker -v && \
            docker-compose -v
        fi
    fi
}

prepare_data_files(){
    cd ${__DIR__} && \
    remove_data_files && \
    mkdir -p \
    data \
    data/run \
    data/mysqld data/run/mysqld \
    data/redis data/run/redis && \
    chmod -R 777 data
}

remove_data_files(){
    cd ${__DIR__} && \
    rm -rf ../travis/data
}

start_docker_containers(){
    cd ${__DIR__} && \
    remove_docker_containers && \
    docker-compose up -d && \
    docker ps -a
}

remove_docker_containers(){
    cd ${__DIR__} && \
    docker-compose kill > /dev/null 2>&1 && \
    docker-compose rm -f > /dev/null 2>&1
}

run_tests_in_docker(){
    docker exec swoole touch /.travisenv && \
    docker exec swoole /swoole-src/travis/docker-route.sh
}

remove_tests_resources(){
    remove_docker_containers
    remove_data_files
}

check_docker_dependency

echo "\nš Prepare for files...\n"
prepare_data_files

echo "š¦ Start docker containers...\n"
start_docker_containers # && trap "remove_tests_resources"

echo "\nā³ Run tests in docker...\n"
run_tests_in_docker

echo "\nšššCompleted successfullyššš\n"
