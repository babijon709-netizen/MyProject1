#pragma once

#include <cmath>
#include <limits>

namespace manes_aim_source {

enum class EntityName {
    None,
    EliteCrate,
    MilitaryCrate,
    ItemPickup,
    CompositeStone,
};

inline char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

inline bool contains_ascii_case_insensitive(const char* text, const char* needle) {
    if (!text || !needle || !needle[0]) return false;
    for (const char* start = text; *start; ++start) {
        const char* a = start;
        const char* b = needle;
        while (*a && *b && ascii_lower(*a) == ascii_lower(*b)) {
            ++a;
            ++b;
        }
        if (!*b) return true;
    }
    return false;
}

inline EntityName classify_entity_name(const char* name) {
    if (contains_ascii_case_insensitive(name, "Elite Crate") ||
        contains_ascii_case_insensitive(name, "EliteCrate"))
        return EntityName::EliteCrate;
    if (contains_ascii_case_insensitive(name, "Military Crate") ||
        contains_ascii_case_insensitive(name, "MilitaryCrate"))
        return EntityName::MilitaryCrate;
    if (contains_ascii_case_insensitive(name, "ItemPickup"))
        return EntityName::ItemPickup;
    if (contains_ascii_case_insensitive(name, "MineableCompositeStone"))
        return EntityName::CompositeStone;
    return EntityName::None;
}

inline float angular_distance_degrees(float yaw_offset, float pitch_offset) {
    if (!std::isfinite(yaw_offset) || !std::isfinite(pitch_offset))
        return std::numeric_limits<float>::infinity();

    constexpr float kDegToRad = 0.01745329251994329577f;
    constexpr float kRadToDeg = 57.29577951308232f;
    const float yaw = yaw_offset * kDegToRad;
    const float pitch = pitch_offset * kDegToRad;
    float forward_dot = std::cos(yaw) * std::cos(pitch);
    if (forward_dot < -1.0f) forward_dot = -1.0f;
    if (forward_dot > 1.0f) forward_dot = 1.0f;
    return std::acos(forward_dot) * kRadToDeg;
}

inline float fov_radius_degrees(float full_fov_degrees) {
    if (!std::isfinite(full_fov_degrees)) return 0.0f;
    if (full_fov_degrees < 0.0f) full_fov_degrees = 0.0f;
    if (full_fov_degrees > 180.0f) full_fov_degrees = 180.0f;
    return full_fov_degrees * 0.5f;
}

inline bool inside_angular_fov(float yaw_offset, float pitch_offset,
                               float full_fov_degrees, float extra_radius_degrees = 0.0f) {
    const float angle = angular_distance_degrees(yaw_offset, pitch_offset);
    if (!std::isfinite(angle)) return false;
    if (!std::isfinite(extra_radius_degrees) || extra_radius_degrees < 0.0f)
        extra_radius_degrees = 0.0f;
    return angle <= fov_radius_degrees(full_fov_degrees) + extra_radius_degrees;
}

inline float frame_gain(float smooth_fraction, float dt) {
    if (!std::isfinite(smooth_fraction) || !std::isfinite(dt) || dt <= 0.0f)
        return 0.0f;
    if (smooth_fraction < 0.0f) smooth_fraction = 0.0f;
    if (smooth_fraction > 1.0f) smooth_fraction = 1.0f;
    if (smooth_fraction == 0.0f) return 0.0f;
    if (smooth_fraction == 1.0f) return 1.0f;
    return 1.0f - std::pow(1.0f - smooth_fraction, dt * 60.0f);
}

} // namespace manes_aim_source
