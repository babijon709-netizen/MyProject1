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
        auto set_state = [argv](const char* value) {
            FILE* state = std::fopen(argv[1], "w");
            if (!state) return false;
            std::fputs(value, state);
            std::fclose(state);
            return true;
        };
        if (argc > 2 && std::string(argv[2]) == "transient") {
            if (!set_state("error\n")) return 2;
            std::this_thread::sleep_for(std::chrono::milliseconds(1400));
            if (!main_thread_flag.load() || !set_state("enabled\n")) return 3;
            std::this_thread::sleep_for(std::chrono::milliseconds(1400));
            if (!main_thread_flag.load()) return 4;
            main_thread_flag.store(false);
            return 0;
        }
        if (argc > 2 && std::string(argv[2]) == "outage") {
            if (!set_state("error\n")) return 2;
            for (int i = 0; i < 50 && main_thread_flag.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            return main_thread_flag.load() ? 5 : 0;
        }
        if (!set_state("disabled\n")) return 2;
        for (int i = 0; i < 40 && main_thread_flag.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return main_thread_flag.load() ? 3 : 0;
    }
    main_thread_flag.store(false);
    return 0;
}
CPP
cat > "$TEMP/stub_auth.cpp" <<'CPP'
#include "auth_http.h"
#include <cstdlib>
#include <cstdio>
#include <string>
AuthHttpResult PostDeviceAuth(const std::string& url, const std::string& body,
                              const std::string& nonce) {
    if (url != "https://test.example/auth" || body.find("\"nonce\":\"" + nonce + "\"") == std::string::npos ||
        (body.find("\"register\":true") == std::string::npos &&
         body.find("\"register\":false") == std::string::npos)) return AuthHttpResult::denied;
    const char* state = std::getenv("FAKE_STATUS");
    std::string value = state ? state : "";
    if (const char* file = std::getenv("FAKE_STATE_FILE")) {
        FILE* stream = std::fopen(file, "r");
        if (stream) { char buf[32]{}; std::fgets(buf, sizeof(buf), stream); value = buf; std::fclose(stream); }
    }
    if (value.rfind("enabled", 0) == 0) return AuthHttpResult::allowed;
    if (value.rfind("disabled", 0) == 0) return AuthHttpResult::denied;
    if (value == "replay" || value == "extra") return AuthHttpResult::unavailable;
    if (value == "redirect") return AuthHttpResult::unavailable;
    return AuthHttpResult::unavailable;
}
CPP
# No external curl or PATH is needed even for the test binary.
g++ -std=c++17 -Wall -Wextra -Werror -pthread \
    -DBENZWARE_STATUS_URL='"https://test.example/status"' -DBENZWARE_POLL_SECONDS=1 \
    -DBENZWARE_GRACE_SECONDS=3 -DBENZWARE_RETRY_DELAY_MS=10 \
    -I "$ROOT/jni/include" "$ROOT/jni/src/remote_gate.cpp" \
    "$TEMP/stub_auth.cpp" "$TEMP/test.cpp" -o "$TEMP/test"
PATH=/nonexistent FAKE_STATUS=enabled "$TEMP/test"
for bad in disabled extra redirect error replay; do
    if PATH=/nonexistent FAKE_STATUS="$bad" "$TEMP/test"; then
        echo "Unexpected authorization: $bad" >&2
        exit 1
    fi
done
# Start enabled, revoke while running; the next heartbeat must stop the process.
printf 'enabled\n' > "$TEMP/state"
PATH=/nonexistent FAKE_STATUS=enabled FAKE_STATE_FILE="$TEMP/state" "$TEMP/test" "$TEMP/state"
printf 'enabled\n' > "$TEMP/state"
PATH=/nonexistent FAKE_STATUS=enabled FAKE_STATE_FILE="$TEMP/state" "$TEMP/test" "$TEMP/state" transient
printf 'enabled\n' > "$TEMP/state"
PATH=/nonexistent FAKE_STATUS=enabled FAKE_STATE_FILE="$TEMP/state" "$TEMP/test" "$TEMP/state" outage
g++ -std=c++17 -Wall -Wextra -Werror -I "$ROOT/jni/include" -I "$ROOT/tools/hostcheck/stub" \
    "$ROOT/jni/src/device_identity.cpp" "$ROOT/tests/remote_gate/device_identity.test.cpp" \
    -o "$TEMP/device_identity_test"
"$TEMP/device_identity_test"
sh "$ROOT/tests/remote_gate/launcher.test.sh"
sh "$ROOT/tests/remote_gate/auth_http.test.sh"
echo 'Native remote gate and device identity tests: OK'
