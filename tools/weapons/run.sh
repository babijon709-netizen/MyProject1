#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
DIR=$(mktemp -d)
trap 'rm -rf "$DIR"' EXIT
for flags in '-O0' '-O3 -ffast-math'; do
    ${CXX:-g++} -std=c++17 -Wall -Wextra $flags -Ijni/include -Ijni/src \
        tools/weapons/weapon_test.cpp -o "$DIR/test"
    "$DIR/test"
    ${CXX:-g++} -std=c++17 -Wall -Wextra $flags -Ijni/include -Ijni/src \
        tools/weapons/speed_test.cpp -o "$DIR/speed_test"
    "$DIR/speed_test"
done
${CXX:-g++} -std=c++17 -Wall -Wextra -O2 -Ijni/include -Ijni/src \
    tools/weapons/proc_mem_test.cpp -o "$DIR/proc_test"
"$DIR/proc_test"
python3 tools/weapons/check_boundaries.py
