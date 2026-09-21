#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
DIR=$(mktemp -d)
trap 'rm -rf "$DIR"' EXIT
for flags in '-O0' '-O3 -ffast-math'; do
    ${CXX:-g++} -std=c++17 -Wall -Wextra $flags -Ijni/include -Ijni/src \
        tools/fast_loot/fast_loot_test.cpp -o "$DIR/test"
    "$DIR/test"
done
${CXX:-g++} -std=c++17 -Wall -Wextra -O2 -Ijni/include -Ijni/src \
    tools/fast_loot/proc_mem_test.cpp -o "$DIR/proc_test"
"$DIR/proc_test"
python3 tools/fast_loot/check_gui.py
