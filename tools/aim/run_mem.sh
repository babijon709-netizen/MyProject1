#!/bin/sh
# Стенд мемори-аима без устройства: собирает и запускает tools/aim/mem_loop.cpp.
#
# В отличие от sim_loop.cpp (тот повторяет арифметику тач-аима текстом), здесь
# компилируется НАСТОЯЩИЙ jni/src/aim/memory.cpp — тот самый файл, что уезжает в
# сборку. Заглушки Android берутся из tools/syntax/stub: контроллер их не
# касается, но их тянет app/common.h, который он подключает как модуль меню.
#
# Использование: sh tools/aim/run_mem.sh
set -e
cd "$(dirname "$0")/../.."
STUB=tools/syntax/stub
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

g++ -std=c++17 -O2 -Wall -Wno-unused-variable \
    -I"$STUB" -Ijni/include -Ijni/include/ImGui -Ijni/src \
    -c jni/src/aim/memory.cpp -o "$OUT/memory.o"
g++ -std=c++17 -O2 -Wall \
    -I"$STUB" -Ijni/include -Ijni/include/ImGui -Ijni/src \
    tools/aim/mem_loop.cpp "$OUT/memory.o" -o "$OUT/mem_loop"
"$OUT/mem_loop"
