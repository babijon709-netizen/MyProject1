#include "device_identity.h"
#include <cassert>
#include <string>

int main() {
    assert(DeviceSha256Hex("") ==
           "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    assert(DeviceSha256Hex("abc") ==
           "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    assert(DeviceSha256Hex(std::string(100, 'x')).size() == 64);
    DeviceIdentity d{"0123456789abcdef", "Pixel \"8\""};
    assert(DeviceIdentityJson(d, true) ==
           "{\"hwid\":\"0123456789abcdef\",\"name\":\"Pixel \\\"8\\\"\",\"register\":true}");
}
