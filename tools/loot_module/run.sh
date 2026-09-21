#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
for flags in '-O0' '-O3'; do
    ${CXX:-g++} -std=c++17 -Wall -Wextra -Werror $flags -Iexperimental/loot_module \
        tools/loot_module/batch_test.cpp -o "$TMP/batch"
    "$TMP/batch"
done
${CXX:-g++} -std=c++17 -O2 -Wall -Wextra -Werror -Ijni/src -Iexperimental/loot_module \
    tools/loot_module/binding_test.cpp -pthread -o "$TMP/binding"
"$TMP/binding"
${CXX:-g++} -std=c++17 -O2 -Wall -Wextra -Werror -fPIC -shared -fvisibility=hidden \
    -Ijni/src experimental/loot_module/module.cpp -pthread \
    -Wl,-z,relro,-z,now,-z,nodelete -o "$TMP/libarena_loot_host.so"
python3 tools/loot_module/check_module.py "$TMP/libarena_loot_host.so"
