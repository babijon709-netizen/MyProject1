#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
DIR=$(mktemp -d)
trap 'rm -rf "$DIR"' EXIT
for flags in '-O0' '-O3 -ffast-math'; do
  ${CXX:-g++} -std=c++17 -Wall -Wextra $flags -Ijni/include -Ijni/src \
    tools/silent/silent_test.cpp -o "$DIR/test"
  "$DIR/test"
done
echo 'silent guarded writer: OK'
