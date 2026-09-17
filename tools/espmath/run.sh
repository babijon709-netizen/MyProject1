#!/bin/sh
# Стенд мировой→экранной математики: собирает настоящий jni/src/esp/math.cpp и
# проверяет числа (см. tools/espmath/espmath_test.cpp — там же история про
# ошибку знака в переводе матрицы вида, которую этот стенд и ловит).
#
#   sh tools/espmath/run.sh
set -e
cd "$(dirname "$0")/../.."
STUB=tools/syntax/stub
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

g++ -std=c++17 -O2 -Wall -Wno-unused-variable \
    -I"$STUB" -Ijni/include -Ijni/include/ImGui -Ijni/src \
    -c jni/src/esp/math.cpp -o "$OUT/math.o"
g++ -std=c++17 -O2 -Wall \
    -I"$STUB" -Ijni/include -Ijni/include/ImGui -Ijni/src \
    tools/espmath/espmath_test.cpp "$OUT/math.o" -o "$OUT/espmath_test"
"$OUT/espmath_test"
