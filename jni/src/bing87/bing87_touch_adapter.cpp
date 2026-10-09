#include "bing87/bing87_touch_adapter.hpp"

#include <cstdint>
#include "Android_touch/TouchHelperA.h"

#include <algorithm>
#include <cmath>

namespace bing87 {
namespace {

constexpr float kRadToDeg = 57.29578f;

float Clamp(float value, float low, float high) {
    return std::max(low, std::min(value, high));
}

bool IsFinite(float value) {
    return std::isfinite(value);
}

bool ValidConfig(const TouchConfig& config) {
    return config.slot >= 6 && config.slot <= 9 &&
           IsFinite(config.screen_width) && config.screen_width > 0.0f &&
           IsFinite(config.screen_height) && config.screen_height > 0.0f &&
           IsFinite(config.anchor_x) && IsFinite(config.anchor_y) &&
           IsFinite(config.pixels_per_degree_x) && config.pixels_per_degree_x > 0.0f &&
           IsFinite(config.pixels_per_degree_y) && config.pixels_per_degree_y > 0.0f &&
           IsFinite(config.yaw_sign) && IsFinite(config.pitch_sign) &&
           IsFinite(config.max_step_px) && config.max_step_px > 0.0f &&
           IsFinite(config.smoothing_hz) && config.smoothing_hz >= 0.0f;
}

} // namespace

bool CalculateTargetAngles(const Vec3& camera, const Vec3& target, Angles& out) {
    const float dx = camera.x - target.x;
    const float dy = camera.y - target.y;
    const float dz = camera.z - target.z;
    const float horizontal = std::sqrt(dx * dx + dy * dy);
    if (!IsFinite(dx) || !IsFinite(dy) || !IsFinite(dz) ||
        !IsFinite(horizontal) || horizontal == 0.0f) {
        return false;
    }

    out.pitch_deg = -std::atan(dz / horizontal) * kRadToDeg;
    out.yaw_deg = std::atan(dy / dx) * kRadToDeg;
    if (dx >= 0.0f)
        out.yaw_deg += 180.0f;

    return IsFinite(out.pitch_deg) && IsFinite(out.yaw_deg);
}

float WrapDegrees180(float degrees) {
    if (!IsFinite(degrees))
        return 0.0f;
    float wrapped = std::fmod(degrees + 180.0f, 360.0f);
    if (wrapped < 0.0f)
        wrapped += 360.0f;
    return wrapped - 180.0f;
}

bool CalculateAimError(const Vec3& camera, const Vec3& target,
                       float camera_yaw_deg, float camera_pitch_deg,
                       AimError& out) {
    Angles desired{};
    if (!CalculateTargetAngles(camera, target, desired) ||
        !IsFinite(camera_yaw_deg) || !IsFinite(camera_pitch_deg)) {
        return false;
    }

    out.yaw_deg = WrapDegrees180(desired.yaw_deg - camera_yaw_deg);
    out.pitch_deg = desired.pitch_deg - camera_pitch_deg;
    return IsFinite(out.yaw_deg) && IsFinite(out.pitch_deg);
}

TouchAimAdapter::TouchAimAdapter(const TouchConfig& config) {
    Configure(config);
}

TouchAimAdapter::~TouchAimAdapter() {
    Reset();
}

void TouchAimAdapter::Configure(const TouchConfig& config) {
    if (down_ && config.slot != config_.slot)
        Release();
    config_ = config;
    if (!ValidConfig(config_))
        Release();
}

bool TouchAimAdapter::Tick(bool enabled, bool target_valid,
                           float yaw_error_deg, float pitch_error_deg,
                           float dt_seconds) {
    if (!enabled || !target_valid || !ValidConfig(config_) ||
        !IsFinite(yaw_error_deg) || !IsFinite(pitch_error_deg) ||
        !Touch_CanInject()) {
        Release();
        return false;
    }

    const float dx = Clamp(yaw_error_deg * config_.pixels_per_degree_x * config_.yaw_sign,
                           -config_.max_step_px, config_.max_step_px);
    const float dy = Clamp(pitch_error_deg * config_.pixels_per_degree_y * config_.pitch_sign,
                           -config_.max_step_px, config_.max_step_px);
    const float target_x = Clamp(config_.anchor_x + dx, 0.0f, config_.screen_width - 1.0f);
    const float target_y = Clamp(config_.anchor_y + dy, 0.0f, config_.screen_height - 1.0f);

    if (!down_) {
        x_ = Clamp(config_.anchor_x, 0.0f, config_.screen_width - 1.0f);
        y_ = Clamp(config_.anchor_y, 0.0f, config_.screen_height - 1.0f);
        Emit(x_, y_, true);
        down_ = true;
        return true;
    }

    float alpha = 1.0f;
    if (dt_seconds > 0.0f && IsFinite(dt_seconds) && config_.smoothing_hz > 0.0f)
        alpha = 1.0f - std::exp(-config_.smoothing_hz * dt_seconds);
    alpha = Clamp(alpha, 0.0f, 1.0f);

    x_ += (target_x - x_) * alpha;
    y_ += (target_y - y_) * alpha;
    Emit(x_, y_, false);
    return true;
}

void TouchAimAdapter::Reset() {
    Release();
}

void TouchAimAdapter::Emit(float x, float y, bool first_down) {
    if (config_.slot == 9) {
        if (first_down)
            Touch_Down(x, y);
        else
            Touch_Move(x, y);
        return;
    }

    const int extra_finger = config_.slot - 6;
    (void)first_down;
    Touch_Down_N(extra_finger, x, y);
}

void TouchAimAdapter::Release() {
    if (!down_)
        return;

    if (config_.slot == 9)
        Touch_Up();
    else
        Touch_Up_N(config_.slot - 6);

    down_ = false;
}

} // namespace bing87
