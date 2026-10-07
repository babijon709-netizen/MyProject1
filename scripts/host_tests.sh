#!/bin/sh
# Host-прогон юнит-тестов viewmodel/ESP (без устройства/NDK).
# Использование: ./scripts/host_tests.sh
set -e
cd "$(dirname "$0")/.."
CXX=${CXX:-g++}
OUT=/tmp/xvcen_host_tests
mkdir -p "$OUT"
for t in test_fps_memory test_pos_filter test_silent_aim_core test_loot_memory test_weapon_speed_memory test_fake_lag test_camera_local_guard; do
    $CXX -std=gnu++17 -Wall -Wextra \
        -I jni/src -I jni/include \
        "jni/tests/$t.cpp" -o "$OUT/$t"
    "$OUT/$t"
done
$CXX -std=gnu++17 -Wall -Wextra -pthread \
    -I jni/include jni/tests/test_fake_lag_runtime.cpp \
    jni/src/fake_lag.cpp -o "$OUT/test_fake_lag_runtime"
"$OUT/test_fake_lag_runtime"
