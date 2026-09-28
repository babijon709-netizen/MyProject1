#!/bin/sh
set -eu
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
OUT="${TMPDIR:-/tmp}/myproject1-esp-lifecycle-test"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror \
  -I"$ROOT/jni/include" -I"$ROOT/jni/src" \
  "$ROOT/tools/esp/lifecycle_test.cpp" -o "$OUT"
"$OUT"
rm -f "$OUT"
