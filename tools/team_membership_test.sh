#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
BUILD=${BUILD:-/tmp/team_membership_test}
mkdir -p "$BUILD"
g++ -std=c++17 -O2 -Wall -Wextra -Ijni/include \
    -o "$BUILD/team_membership_test" tools/team_membership_test.cpp
"$BUILD/team_membership_test"
