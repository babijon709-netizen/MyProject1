#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
DIR=$(mktemp -d)
trap 'rm -rf "$DIR"' EXIT
for flags in '-O0' '-O3 -ffast-math'; do
    # Check the real controller with both normal and Android optimisation flags.
    ${CXX:-g++} -std=c++17 -Wall -Wextra $flags -Ijni/include -Ijni/src \
        tools/viewmodel/viewmodel_test.cpp -o "$DIR/test"
    "$DIR/test"
done
