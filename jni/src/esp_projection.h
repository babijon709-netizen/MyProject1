#pragma once

#include "Vector.h"

#include <cmath>

namespace espprojection {

inline float get(const Mat4& matrix, int row, int column) {
    return matrix.m[column * 4 + row];
}

inline void set(Mat4& matrix, int row, int column, float value) {
    matrix.m[column * 4 + row] = value;
}

inline bool finite(const Vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

inline bool finite(const Vec4& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z) && std::isfinite(value.w);
}

inline bool normalize(Vec4& rotation) {
    const float length_squared = rotation.x * rotation.x + rotation.y * rotation.y +
                                 rotation.z * rotation.z + rotation.w * rotation.w;
    if (!std::isfinite(length_squared) || length_squared < 1e-6F) return false;
    const float inverse_length = 1.0F / std::sqrt(length_squared);
    rotation.x *= inverse_length;
    rotation.y *= inverse_length;
    rotation.z *= inverse_length;
    rotation.w *= inverse_length;
    return true;
}

inline Vec3 rotate(const Vec4& rotation, const Vec3& value) {
    const Vec3 q{rotation.x, rotation.y, rotation.z};
    const Vec3 cross1{q.y * value.z - q.z * value.y,
                      q.z * value.x - q.x * value.z,
                      q.x * value.y - q.y * value.x};
    const Vec3 twice{cross1.x * 2.0F, cross1.y * 2.0F, cross1.z * 2.0F};
    const Vec3 cross2{q.y * twice.z - q.z * twice.y,
                      q.z * twice.x - q.x * twice.z,
                      q.x * twice.y - q.y * twice.x};
    return {value.x + rotation.w * twice.x + cross2.x,
            value.y + rotation.w * twice.y + cross2.y,
            value.z + rotation.w * twice.z + cross2.z};
}

inline bool make_view(const Vec3& position, Vec4 rotation, Mat4& view) {
    if (!finite(position) || !normalize(rotation)) return false;

    const Vec3 right = rotate(rotation, {1.0F, 0.0F, 0.0F});
    const Vec3 up = rotate(rotation, {0.0F, 1.0F, 0.0F});
    const Vec3 forward = rotate(rotation, {0.0F, 0.0F, 1.0F});
    view = {};
    set(view, 0, 0, right.x); set(view, 0, 1, right.y); set(view, 0, 2, right.z);
    set(view, 1, 0, up.x); set(view, 1, 1, up.y); set(view, 1, 2, up.z);
    set(view, 2, 0, -forward.x); set(view, 2, 1, -forward.y); set(view, 2, 2, -forward.z);
    set(view, 0, 3, -(right.x * position.x + right.y * position.y + right.z * position.z));
    set(view, 1, 3, -(up.x * position.x + up.y * position.y + up.z * position.z));
    set(view, 2, 3, forward.x * position.x + forward.y * position.y + forward.z * position.z);
    set(view, 3, 3, 1.0F);

    for (float value : view.m)
        if (!std::isfinite(value)) return false;
    return true;
}

inline bool make_perspective(float fov_degrees, float aspect, float z_near,
                             float z_far, Mat4& projection) {
    projection = {};
    if (!std::isfinite(fov_degrees) || !std::isfinite(aspect) ||
        !std::isfinite(z_near) || !std::isfinite(z_far) ||
        !(fov_degrees > 0.1F && fov_degrees < 179.0F) ||
        !(aspect > 0.05F && aspect < 10.0F) || !(z_near > 0.0F) || !(z_far > z_near))
        return false;

    constexpr float kDegreesToRadians = 0.01745329251F;
    const float cotangent = 1.0F / std::tan(fov_degrees * kDegreesToRadians * 0.5F);
    set(projection, 0, 0, cotangent / aspect);
    set(projection, 1, 1, cotangent);
    set(projection, 2, 2, -(z_far + z_near) / (z_far - z_near));
    set(projection, 2, 3, -(2.0F * z_far * z_near) / (z_far - z_near));
    set(projection, 3, 2, -1.0F);
    for (float value : projection.m)
        if (!std::isfinite(value)) return false;
    return true;
}

inline bool project(const Mat4& view_projection, const Vec3& world,
                    float screen_width, float screen_height, Vec2& screen,
                    bool clip_to_screen = true) {
    if (!finite(world) || !std::isfinite(screen_width) || !std::isfinite(screen_height) ||
        !(screen_width > 0.0F) || !(screen_height > 0.0F))
        return false;

    const float clip_x = get(view_projection, 0, 0) * world.x +
                         get(view_projection, 0, 1) * world.y +
                         get(view_projection, 0, 2) * world.z +
                         get(view_projection, 0, 3);
    const float clip_y = get(view_projection, 1, 0) * world.x +
                         get(view_projection, 1, 1) * world.y +
                         get(view_projection, 1, 2) * world.z +
                         get(view_projection, 1, 3);
    const float clip_w = get(view_projection, 3, 0) * world.x +
                         get(view_projection, 3, 1) * world.y +
                         get(view_projection, 3, 2) * world.z +
                         get(view_projection, 3, 3);
    if (!std::isfinite(clip_x) || !std::isfinite(clip_y) ||
        !std::isfinite(clip_w) || clip_w <= 0.001F)
        return false;

    const float x = ((clip_x / clip_w) + 1.0F) * 0.5F * screen_width;
    const float y = ((-clip_y / clip_w) + 1.0F) * 0.5F * screen_height;
    if (!std::isfinite(x) || !std::isfinite(y)) return false;
    if (clip_to_screen && (x < 0.0F || x > screen_width || y < 0.0F || y > screen_height))
        return false;
    screen = {x, y};
    return true;
}

}  // namespace espprojection
