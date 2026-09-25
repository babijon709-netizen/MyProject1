#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
BUILD=$(mktemp -d)
trap 'rm -rf "$BUILD"' EXIT
CXX=${CXX:-g++}
FLAGS="-std=c++17 -O2 -Wall -Wextra -Werror -pthread -Ijni/include -Ijni/src"
$CXX $FLAGS tools/silent/core_test.cpp -o "$BUILD/core"
"$BUILD/core"
# Match the production fast-math assumption: invalid floats must STILL fail.
$CXX $FLAGS -ffast-math tools/silent/core_test.cpp -o "$BUILD/core_fast"
"$BUILD/core_fast"
$CXX $FLAGS tools/silent/worker_test.cpp jni/src/silent_aim.cpp -o "$BUILD/worker"
"$BUILD/worker"
