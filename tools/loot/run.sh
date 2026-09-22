#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
DIR=$(mktemp -d)
trap 'rm -rf "$DIR"' EXIT
for flags in '-O0' '-O3 -ffast-math'; do
    # The real controller, with both normal and Android optimisation flags.
    ${CXX:-g++} -std=c++17 -Wall -Wextra $flags -Ijni/include -Ijni/src \
        tools/loot/loot_test.cpp -o "$DIR/test"
    "$DIR/test"
    # The auto-tap (take-all) gating logic: same flags.
    ${CXX:-g++} -std=c++17 -Wall -Wextra $flags -Ijni/include -Ijni/src \
        tools/loot/loot_tap_test.cpp -o "$DIR/tap_test"
    "$DIR/tap_test"
done
# If the game images are extracted next to the repo (tools/offsets/PROVENANCE.md
# says how), re-derive every address in the table from them. Skipped otherwise:
# no images are checked in.
if [ -f release/libil2cpp.so ] && [ -f beta/libil2cpp.so ] &&
   python3 -c 'import capstone' 2>/dev/null; then
    python3 tools/loot/check_native.py release/libil2cpp.so beta/libil2cpp.so
    # The same table, re-derived from the bytes instead of re-read from it.
    python3 tools/loot/find_table.py --check release/libil2cpp.so beta/libil2cpp.so
else
    echo "loot: no game images / capstone here — native address check skipped"
fi
