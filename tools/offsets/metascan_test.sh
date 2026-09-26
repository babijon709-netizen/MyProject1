#!/bin/sh
# Стенд «игра обновилась»: резолвер без якоря и без имени «global-metadata» в
# карте процесса — блок метаданных ищется по сигнатуре 0xFAB11BAF.
#   sh tools/offsets/metascan_test.sh
set -e
cd "$(dirname "$0")/../.."
CACHE_DIR="${TMPDIR:-/tmp}/benzware_metascan_cache"
mkdir -p "$CACHE_DIR"
rm -f "$CACHE_DIR/.offs"
g++ -std=c++17 -O2 -Wall -Wextra \
    -Ijni/include -Ijni/src -Itools/syntax/stub \
    -DOR_CACHE_DIR="\"$CACHE_DIR/\"" \
    tools/offsets/metascan_test.cpp -o /tmp/metascan_test
/tmp/metascan_test
# Второй сценарий — с чистого листа: кэш первого входа общий по ключу образа.
rm -f "$CACHE_DIR/.offs"
/tmp/metascan_test encrypted
# Третий сценарий — мусор вместо классов: ничего не должно примениться.
rm -f "$CACHE_DIR/.offs"
/tmp/metascan_test garbage
