#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
out=${TMPDIR:-/tmp}/myproject1-memio-test
${CXX:-g++} -std=c++17 -O2 -pthread -Wall -Wextra -Werror \
  -I"$root/jni/src" "$root/tools/memio/memio_test.cpp" -o "$out"
"$out"
