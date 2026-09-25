#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
TEMP=$(mktemp -d)
trap 'rm -rf "$TEMP"' EXIT
cat > "$TEMP/test.cpp" <<'CPP'
#include "remote_gate.h"
#include "device_identity.h"
#include <string>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
std::atomic<bool> main_thread_flag{true};
bool GetDeviceIdentity(DeviceIdentity& out) {
    out = {std::string(64, 'a'), "Pixel 8"};
    return true;
}
std::string DeviceIdentityJson(const DeviceIdentity& out, bool reg, const std::string& nonce) {
    return "{\"hwid\":\"" + out.hwid + "\",\"name\":\"" + out.name
         + "\",\"register\":" + (reg ? "true" : "false")
         + ",\"nonce\":\"" + nonce + "\"}";
}
int main(int argc, char** argv) {
    if (!RemoteGateStart()) return 1;
    if (argc > 1) {
        FILE* state = std::fopen(argv[1], "w");
        if (!state) return 2;
        std::fputs("disabled\n", state);
        std::fclose(state);
        for (int i = 0; i < 40 && main_thread_flag.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return main_thread_flag.load() ? 3 : 0;
    }
    main_thread_flag.store(false);
    return 0;
}
CPP
cat > "$TEMP/curl" <<'SH'
#!/bin/sh
[ "$1" = '-q' ] || exit 7
case "$*" in *'https://test.example/auth'*) ;; *) exit 8 ;; esac
case "$*" in *'"register":true'*|*'"register":false'*) ;; *) exit 9 ;; esac
payload=''
while [ "$#" -gt 0 ]; do
    if [ "$1" = '--data-raw' ]; then shift; payload=$1; break; fi
    shift
done
nonce=$(printf '%s' "$payload" | sed -n 's/.*"nonce":"\([a-f0-9]\{32\}\)".*/\1/p')
[ "${#nonce}" -eq 32 ] || exit 10
if [ -n "${FAKE_STATE_FILE:-}" ]; then
    FAKE_STATUS=$(cat "$FAKE_STATE_FILE")
fi
case "$FAKE_STATUS" in
    enabled) printf 'enabled:%s\n200\n' "$nonce" ;;
    replay) printf 'enabled:00000000000000000000000000000000\n200\n' ;;
    disabled) printf 'disabled\n200\n' ;;
    extra) printf 'enabled\nother\n200\n' ;;
    redirect) printf 'enabled\n302\n' ;;
    error) exit 22 ;;
esac
SH
chmod +x "$TEMP/curl"
# Only a fake executable on PATH is used; no real network is contacted.
g++ -std=c++17 -Wall -Wextra -Werror -pthread \
    -DBENZWARE_STATUS_URL='"https://test.example/status"' -DBENZWARE_POLL_SECONDS=1 \
    -I "$ROOT/jni/include" "$ROOT/jni/src/remote_gate.cpp" "$TEMP/test.cpp" -o "$TEMP/test"
PATH="$TEMP:$PATH" FAKE_STATUS=enabled "$TEMP/test"
for bad in disabled extra redirect error replay; do
    if PATH="$TEMP:$PATH" FAKE_STATUS="$bad" "$TEMP/test"; then
        echo "Unexpected authorization: $bad" >&2
        exit 1
    fi
done
# Start enabled, revoke while running; the next heartbeat must stop the process.
printf 'enabled\n' > "$TEMP/state"
PATH="$TEMP:$PATH" FAKE_STATUS=enabled FAKE_STATE_FILE="$TEMP/state" "$TEMP/test" "$TEMP/state"
g++ -std=c++17 -Wall -Wextra -Werror -I "$ROOT/jni/include" -I "$ROOT/tools/hostcheck/stub" \
    "$ROOT/jni/src/device_identity.cpp" "$ROOT/tests/remote_gate/device_identity.test.cpp" \
    -o "$TEMP/device_identity_test"
"$TEMP/device_identity_test"
sh "$ROOT/tests/remote_gate/launcher.test.sh"
echo 'Native remote gate and device identity tests: OK'
