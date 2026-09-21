#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>

// Vertical world FOV; hands are offsets from each weapon's original pose, cm.
struct ViewmodelSettings {
    bool fov_enabled = false;
    bool hands_enabled = false;
    float fov = 90.f;
    float x = 0.f, y = 0.f, z = 0.f;
};

namespace viewmodel {
// The Android build uses -ffast-math; do not rely on isfinite under that flag.
inline bool finite(float v) {
    uint32_t bits; std::memcpy(&bits, &v, sizeof(bits));
    return (bits & 0x7f800000u) != 0x7f800000u;
}
inline float bounded(float v, float lo, float hi, float fallback) {
    return !finite(v) ? fallback : (v < lo ? lo : (v > hi ? hi : v));
}
inline ViewmodelSettings sanitize(ViewmodelSettings s) {
    s.fov = bounded(s.fov, 50.f, 120.f, 90.f);
    s.x = bounded(s.x, -50.f, 50.f, 0.f);
    s.y = bounded(s.y, -50.f, 50.f, 0.f);
    s.z = bounded(s.z, -50.f, 50.f, 0.f);
    return s;
}
// Optional trailer after the existing v4 config. Old readers ignore it;
// missing/truncated/unknown trailers load disabled defaults, never stale state.
struct ConfigExtension {
    uint32_t magic = 0x31574d56u; // VMW1
    uint32_t version = 1;
    uint32_t size = 32;
    uint32_t flags = 0;
    float fov = 90.f, x = 0.f, y = 0.f, z = 0.f;
};
static_assert(sizeof(ConfigExtension) == 32, "stable config extension");
inline ConfigExtension encode(ViewmodelSettings s) {
    s = sanitize(s);
    ConfigExtension e;
    e.flags = (s.fov_enabled ? 1u : 0u) | (s.hands_enabled ? 2u : 0u);
    e.fov = s.fov; e.x = s.x; e.y = s.y; e.z = s.z;
    return e;
}
inline ViewmodelSettings decode(const void* bytes, size_t size) {
    ViewmodelSettings s;
    if (size != sizeof(ConfigExtension)) return s;
    ConfigExtension e; std::memcpy(&e, bytes, sizeof(e));
    if (e.magic != 0x31574d56u || e.version != 1 || e.size != sizeof(e) || (e.flags & ~3u)) return s;
    s.fov_enabled = (e.flags & 1u) != 0;
    s.hands_enabled = (e.flags & 2u) != 0;
    s.fov = e.fov; s.x = e.x; s.y = e.y; s.z = e.z;
    return sanitize(s);
}
enum class Status { Off, Waiting, Active, ADS, Failed };
struct Diagnostics { Status fov = Status::Off, hands = Status::Off; };
}

// Main-thread tick, independent of ESP toggles. No background writer.
void esp_viewmodel_tick(const ViewmodelSettings& settings);
viewmodel::Diagnostics esp_viewmodel_diagnostics();
// restore=true only for a live connection (exit/build switch).
// restore=false on lost process/session: forget old addresses without writes.
void esp_viewmodel_reset(bool restore);
