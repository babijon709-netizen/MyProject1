#!/bin/sh
# Стенд автообновления оффсетов: синтетический процесс-«игра» с живым
# глобальным метадатой, и настоящий резолвер гоняется по нему через
# /proc/<pid>/mem. Запускать на линуксе/в CI: sh tools/offsets/resolver_test.sh
set -e
cd "$(dirname "$0")/../.."
g++ -std=c++17 -O2 -Wall -Wextra \
    -Ijni/include -Ijni/src -Itools/syntax/stub \
    tools/offsets/resolver_test.cpp -o /tmp/resolver_test
/tmp/resolver_test
