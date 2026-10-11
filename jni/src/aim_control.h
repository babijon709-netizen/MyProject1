#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace aim_control {

inline bool finite(float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return (bits & 0x7f800000u) != 0x7f800000u;
}

inline float wrap_degrees(float value) {
    if (!finite(value) || !(value > -1.0e6f && value < 1.0e6f)) return 0.0f;
    if (value > -180.0f && value <= 180.0f) return value;
    return value - 360.0f * std::floor((value + 180.0f) / 360.0f);
}

// The configured bone is exact; an unavailable slot is not substituted.
inline int configured_bone_slot(int requested, const bool valid[3]) {
    return requested >= 0 && requested < 3 && valid[requested] ? requested : -1;
}

struct AngleFilter {
    float yaw = 0.0f;
    float pitch = 0.0f;
    bool ready = false;

    void reset() { *this = AngleFilter{}; }

    void update(float& input_yaw, float& input_pitch, float dt, float time_constant) {
        if (!finite(input_yaw) || !finite(input_pitch)) {
            reset();
            input_yaw = input_pitch = 0.0f;
            return;
        }
        if (!(dt > 0.0f) || !finite(dt)) dt = 1.0f / 60.0f;
        if (dt > 0.1f) dt = 0.1f;
        if (!(time_constant > 0.0f) || !finite(time_constant)) time_constant = 0.02f;
        if (time_constant < 0.004f) time_constant = 0.004f;
        if (time_constant > 0.2f) time_constant = 0.2f;
        if (!ready) {
            yaw = wrap_degrees(input_yaw);
            pitch = input_pitch;
            ready = true;
        } else {
            const float alpha = 1.0f - std::exp(-dt / time_constant);
            float yaw_delta = wrap_degrees(input_yaw - yaw);
            float pitch_delta = input_pitch - pitch;
            if (std::fabs(yaw_delta) < 0.006f) yaw_delta = 0.0f;
            if (std::fabs(pitch_delta) < 0.006f) pitch_delta = 0.0f;
            yaw = wrap_degrees(yaw + yaw_delta * alpha);
            pitch += pitch_delta * alpha;
        }
        input_yaw = yaw;
        input_pitch = pitch;
    }
};

inline void memory_command_angles(float accumulator_yaw, float accumulator_pitch,
                                  bool have_camera, float camera_yaw, float camera_pitch,
                                  float delta_yaw, float delta_pitch,
                                  float& out_yaw, float& out_pitch) {
    float base_yaw = accumulator_yaw;
    float base_pitch = accumulator_pitch;
    if (have_camera && finite(camera_yaw) && finite(camera_pitch)) {
        base_yaw = wrap_degrees(camera_yaw);
        base_pitch = wrap_degrees(-camera_pitch);
    }
    out_yaw = wrap_degrees(base_yaw + delta_yaw);
    out_pitch = wrap_degrees(base_pitch - delta_pitch);
}

inline float memory_step(float error_degrees, float dt, float speed,
                         float range_metres) {
    if (!finite(error_degrees)) return 0.0f;
    if (!(dt > 0.0f) || !finite(dt)) dt = 1.0f / 60.0f;
    if (dt > 0.1f) dt = 0.1f;
    if (!(speed >= 1.0f)) speed = 1.0f;
    if (speed > 10.0f) speed = 10.0f;
    float deadband = 0.015f;
    if (range_metres > 0.0f && finite(range_metres)) {
        const float bone_tolerance = std::atan(0.004f / range_metres) * 57.2957795f;
        deadband = std::max(deadband, bone_tolerance);
    }
    if (std::fabs(error_degrees) <= deadband) return 0.0f;

    const float smooth = 0.25f + (speed - 1.0f) / 9.0f * 0.75f;
    const float gain = 1.0f - std::pow(1.0f - smooth, dt * 60.0f);
    float max_rate = 120.0f + 48.0f * speed;
    if (range_metres > 0.0f && range_metres < 12.0f) max_rate *= 1.15f;
    float max_step = max_rate * dt;
    if (max_step > 12.0f) max_step = 12.0f;
    const float step = error_degrees * gain;
    return std::max(-max_step, std::min(max_step, step));
}


} // namespace aim_control
