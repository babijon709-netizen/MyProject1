#!/usr/bin/env bash
# Runs the ESP reader against a synthetic game image on the host.
#
#   tests/run_host_tests.sh
#
# No Android NDK needed: jni/src/game.cpp only ever touches the game through
# process_vm_readv, so the test points it at this process and feeds it a fake
# il2cpp/Unity object graph. See tests/esp_host_test.cpp for the layout notes.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CXX="${CXX:-g++}"
OUT="${TMPDIR:-/tmp}/xvcen_esp_host_test"

"$CXX" -std=c++17 -O1 -g -Wall -Wextra \
    -I"${ROOT}/jni/include" -I"${ROOT}/jni/src" \
    "${ROOT}/jni/src/game.cpp" "${ROOT}/tests/esp_host_test.cpp" \
    -o "${OUT}"

"${OUT}"
