#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
TEMP=$(mktemp -d)
trap 'rm -rf "$TEMP"' EXIT
TLS="$ROOT/jni/src/third_party/mbedtls"
mkdir "$TEMP/ca" "$TEMP/obj"
openssl req -x509 -newkey rsa:2048 -nodes -keyout "$TEMP/ca.key" -out "$TEMP/ca/root.pem" \
    -subj '/CN=benzware local test root' -days 1 \
    -addext 'basicConstraints=critical,CA:TRUE' >/dev/null 2>&1
openssl req -new -newkey rsa:2048 -nodes -keyout "$TEMP/leaf.key" -out "$TEMP/leaf.csr" \
    -subj '/CN=localhost' >/dev/null 2>&1
printf 'subjectAltName=DNS:localhost\nextendedKeyUsage=serverAuth\n' > "$TEMP/leaf.ext"
openssl x509 -req -in "$TEMP/leaf.csr" -CA "$TEMP/ca/root.pem" -CAkey "$TEMP/ca.key" \
    -CAcreateserial -out "$TEMP/leaf.pem" -days 1 -extfile "$TEMP/leaf.ext" >/dev/null 2>&1
INCLUDES="-I$TLS/include -I$TLS/library -I$TLS/3rdparty/everest/include -I$TLS/3rdparty/everest/include/everest -I$TLS/3rdparty/everest/include/everest/kremlib -I$TLS/3rdparty/p256-m/p256-m/include -I$TLS/3rdparty/p256-m/p256-m/include/p256-m -I$TLS/3rdparty/p256-m/p256-m_driver_interface"
for src in "$TLS"/library/*.c \
    "$TLS"/3rdparty/everest/library/everest.c \
    "$TLS"/3rdparty/everest/library/x25519.c \
    "$TLS"/3rdparty/everest/library/Hacl_Curve25519_joined.c \
    "$TLS"/3rdparty/p256-m/p256-m_driver_entrypoints.c \
    "$TLS"/3rdparty/p256-m/p256-m/p256-m.c; do
    # shellcheck disable=SC2086
    cc -O0 $INCLUDES -c "$src" -o "$TEMP/obj/$(basename "$src" .c).o"
done
ar rcs "$TEMP/libmbed.a" "$TEMP"/obj/*.o
cat > "$TEMP/client.cpp" <<'CPP'
#include "auth_http.h"
#include <iostream>
#include <string>
int main(int argc, char** argv) {
    if (argc != 4) return 4;
    const std::string nonce(32, 'a');
    std::string body = "{\"hwid\":\"" + std::string(64, 'b') + "\",\"name\":\"Test\",\"register\":true,\"nonce\":\"" + nonce + "\",\"mode\":\"" + argv[3] + "\"}";
    const auto result = PostDeviceAuth(std::string("https://") + argv[2] + ":" + argv[1] + "/auth", body, nonce);
    std::cout << static_cast<int>(result) << '\n';
}
CPP
# enum order: allowed 0, denied 1, unavailable 2.
# shellcheck disable=SC2086
g++ -std=c++17 -Wall -Wextra -Werror -pthread -I"$ROOT/jni/include" $INCLUDES \
    -DBENZWARE_CA_DIR1="\"$TEMP/ca\"" -DBENZWARE_CA_DIR2='"/nonexistent"' \
    "$ROOT/jni/src/auth_http.cpp" "$TEMP/client.cpp" "$TEMP/libmbed.a" -o "$TEMP/client"
node "$ROOT/tests/remote_gate/auth_http.test.mjs" "$TEMP/client" "$TEMP/leaf.key" "$TEMP/leaf.pem"
