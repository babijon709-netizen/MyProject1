#!/bin/sh
# Host-прогон юнит-тестов viewmodel/ESP (без устройства/NDK).
# Использование: ./scripts/host_tests.sh
set -e
cd "$(dirname "$0")/.."
CXX=${CXX:-g++}
OUT=/tmp/xvcen_host_tests
mkdir -p "$OUT"
for t in test_fps_memory test_pos_filter test_silent_aim_core test_loot_memory test_weapon_speed_memory; do
    $CXX -std=gnu++17 -Wall -Wextra \
        -I jni/src -I jni/include \
        "jni/tests/$t.cpp" -o "$OUT/$t"
    "$OUT/$t"
done
