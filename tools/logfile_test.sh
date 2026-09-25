#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
BUILD=${BUILD:-/tmp/benzware_logfile_test}
mkdir -p "$BUILD"
g++ -std=c++17 -O2 -Wall -Wextra \
    -DBENZWARE_ENABLE_FILE_LOGGING -DBENZWARE_LOG_TESTING \
    -Ijni/include -o "$BUILD/logfile_test" tools/logfile_test.cpp
"$BUILD/logfile_test" "$BUILD/benzware.log"
