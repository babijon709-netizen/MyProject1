#!/bin/sh
# Стенд автообновления оффсетов: синтетический процесс-«игра» с живым
# глобальным метадатой, и настоящий резолвер гоняется по нему через
# /proc/<pid>/mem. Запускать на линуксе/в CI: sh tools/offsets/resolver_test.sh
set -e
cd "$(dirname "$0")/../.."
# Каталог кэша первого входа подменяется: на хосте /storage/emulated/0 нет,
# а кэш (взять результат без скана) нужно проверить здесь же.
CACHE_DIR="${TMPDIR:-/tmp}/benzware_offsets_cache"
mkdir -p "$CACHE_DIR"
rm -f "$CACHE_DIR/.offs"
g++ -std=c++17 -O2 -Wall -Wextra \
    -Ijni/include -Ijni/src -Itools/syntax/stub \
    -DOR_CACHE_DIR="\"$CACHE_DIR/\"" \
    tools/offsets/resolver_test.cpp -o /tmp/resolver_test
/tmp/resolver_test
