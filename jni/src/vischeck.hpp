#pragma once

#include <cmath>

namespace manes {

struct Vec2 {
    float x = 0.f;
    float y = 0.f;
};

struct Vec3 {
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
};

constexpr float kInvisibleSentinel = -0.1f;
constexpr float kNearPlane = 1.1f;
constexpr float kMinFovRad = 0.012f;
constexpr float kPi = 3.14159265358979323846f;

inline float project_point(Vec3 p, Vec2& out_px,
                           float fov_rad, float axis_scale, float k) {
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) ||
        !std::isfinite(fov_rad) || !std::isfinite(axis_scale) || !std::isfinite(k))
        return kInvisibleSentinel;

    if (p.z < kNearPlane || fov_rad < kMinFovRad) {
        if (fov_rad < kMinFovRad) {
            const float clamped_fov = fov_rad < 0.01f ? 0.01f : fov_rad;
            const float tangent = std::tan(clamped_fov * 0.5f);
            return 4.f * tangent * tangent;
        }
        return kInvisibleSentinel;
    }

    const float scale = 180.f / (kPi * fov_rad);
    const float inverse_depth = 1.f / (p.z - 1.f);
    out_px.x = axis_scale * 0.5f + k * (p.x * scale * inverse_depth);
    out_px.y = k * (0.5f - p.y * scale * inverse_depth);
    return out_px.x;
}

inline bool is_visible(Vec3 camera_space_pos, Vec2& out_px,
                       float fov_rad, float axis_scale, float k) {
    if (!std::isfinite(camera_space_pos.x) || !std::isfinite(camera_space_pos.y) ||
        !std::isfinite(camera_space_pos.z) || !std::isfinite(fov_rad) ||
        !std::isfinite(axis_scale) || !std::isfinite(k) ||
        camera_space_pos.z < kNearPlane || fov_rad < kMinFovRad)
        return false;

    project_point(camera_space_pos, out_px, fov_rad, axis_scale, k);
    return std::isfinite(out_px.x) && std::isfinite(out_px.y);
}

} // namespace manes
