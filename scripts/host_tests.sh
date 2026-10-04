#!/bin/sh
# Host-прогон юнит-тестов viewmodel (без устройства/NDK).
# Использование: ./scripts/host_tests.sh
set -e
cd "$(dirname "$0")/.."
CXX=${CXX:-g++}
mkdir -p /tmp/xvcen_host_tests
$CXX -std=gnu++17 -Wall -Wextra \
    -I jni/src -I jni/include \
    jni/tests/test_fps_memory.cpp -o /tmp/xvcen_host_tests/test_fps_memory
/tmp/xvcen_host_tests/test_fps_memory
