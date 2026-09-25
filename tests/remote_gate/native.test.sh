#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
TEMP=$(mktemp -d)
trap 'rm -rf "$TEMP"' EXIT
cat > "$TEMP/test.cpp" <<'CPP'
#include "remote_gate.h"
#include <atomic>
std::atomic<bool> main_thread_flag{true};
int main() {
    if (!RemoteGateStart()) return 1;
    main_thread_flag.store(false);
    return 0;
}
CPP
cat > "$TEMP/curl" <<'SH'
#!/bin/sh
[ "$1" = '-q' ] || exit 7
case "$FAKE_STATUS" in
    enabled) printf 'enabled\n200\n' ;;
    disabled) printf 'disabled\n200\n' ;;
    extra) printf 'enabled\nother\n200\n' ;;
    redirect) printf 'enabled\n302\n' ;;
    error) exit 22 ;;
esac
SH
chmod +x "$TEMP/curl"
# Only a fake executable on PATH is used; no real network is contacted.
g++ -std=c++17 -Wall -Wextra -Werror -pthread \
    -DBENZWARE_STATUS_URL='"https://test.example/status"' \
    -I "$ROOT/jni/include" "$ROOT/jni/src/remote_gate.cpp" "$TEMP/test.cpp" -o "$TEMP/test"
PATH="$TEMP:$PATH" FAKE_STATUS=enabled "$TEMP/test"
for bad in disabled extra redirect error; do
    if PATH="$TEMP:$PATH" FAKE_STATUS="$bad" "$TEMP/test"; then
        echo "Unexpected authorization: $bad" >&2
        exit 1
    fi
done
echo 'Native remote gate tests: OK'
