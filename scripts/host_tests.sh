#!/bin/sh
# Host-прогон юнит-тестов viewmodel/ESP (без устройства/NDK).
# Использование: ./scripts/host_tests.sh
set -e
cd "$(dirname "$0")/.."
CXX=${CXX:-g++}
OUT=/tmp/xvcen_host_tests
mkdir -p "$OUT"
for t in test_bot_life test_bot_lead test_fast_fire_memory test_fps_memory test_pos_filter test_player_motion_prediction test_touch_device_filter test_silent_aim_core test_esp_box_hold test_fast_loot_memory test_heap_gate test_no_recoil_memory test_no_ballistics_memory test_manes_aim_source test_mount_latch test_body_clip test_cfg_pick test_mem_lag_comp test_aim_predict; do
    $CXX -std=gnu++17 -Wall -Wextra \
        -I jni/src -I jni/include \
        "jni/tests/$t.cpp" -o "$OUT/$t"
    "$OUT/$t"
done
