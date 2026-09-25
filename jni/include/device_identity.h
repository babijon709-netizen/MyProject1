#pragma once
#include <string>

struct DeviceIdentity {
    // Pseudonymous hash, not a hardware attestation or an unforgeable ID.
    std::string hwid;
    std::string name;
};

bool GetDeviceIdentity(DeviceIdentity& out);
std::string DeviceIdentityJson(const DeviceIdentity& identity, bool register_device,
                               const std::string& nonce);

// Exposed for deterministic host tests; never hashes/stores a raw ID on disk.
std::string DeviceSha256Hex(const std::string& input);
