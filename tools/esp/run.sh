#!/bin/sh
set -eu
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
LIFECYCLE_OUT="${TMPDIR:-/tmp}/myproject1-esp-lifecycle-test"
GEOMETRY_OUT="${TMPDIR:-/tmp}/myproject1-overlay-geometry-test"

"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror \
  -I"$ROOT/jni/include" -I"$ROOT/jni/src" \
  "$ROOT/tools/esp/lifecycle_test.cpp" -o "$LIFECYCLE_OUT"
"$LIFECYCLE_OUT"

"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror \
  -I"$ROOT/jni/include" \
  "$ROOT/tools/esp/overlay_geometry_test.cpp" -o "$GEOMETRY_OUT"
"$GEOMETRY_OUT"

python3 "$ROOT/tools/esp/source_contract_test.py"

rm -f "$LIFECYCLE_OUT" "$GEOMETRY_OUT"
