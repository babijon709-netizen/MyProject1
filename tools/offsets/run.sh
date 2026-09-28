#!/bin/sh
# Проверка единственного compile-time набора оффсетов текущего релиза.
set -e
cd "$(dirname "$0")/../.."
BUILD=${BUILD:-/tmp/offsets_release}
mkdir -p "$BUILD"

g++ -std=gnu++17 -O1 -g -Wall -Wextra \
    -Ijni/src \
    -o "$BUILD/release_test" tools/offsets/release_test.cpp

"$BUILD/release_test"
