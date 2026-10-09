#pragma once

#include <cmath>

namespace bing87 {

struct Vec3 {
    float x;
    float y;
    float z;
};

struct Angles {
    float pitch_deg;
    float yaw_deg;
};

struct AimError {
    float pitch_deg;
    float yaw_deg;
};

// Reconstructs the binary's vector-to-angle routine at ELF VA 0x611914.
bool CalculateTargetAngles(const Vec3& camera, const Vec3& target, Angles& out);
float WrapDegrees180(float degrees);
bool CalculateAimError(const Vec3& camera, const Vec3& target,
                       float camera_yaw_deg, float camera_pitch_deg,
                       AimError& out);

struct TouchConfig {
    // Existing MyProject1 mapping: slot 9 uses Touch_Down/Move/Up.
    // Set 6..8 to use Touch_Down_N; slot 8 matches the recovered main worker.
    int slot = 9;
    float screen_width = 0.0f;
    float screen_height = 0.0f;
    float anchor_x = 0.0f;
    float anchor_y = 0.0f;

    // Must be calibrated for the host game/device; these are not known exactly
    // from the stripped payload.
    float pixels_per_degree_x = 0.0f;
    float pixels_per_degree_y = 0.0f;
    float yaw_sign = 1.0f;
    float pitch_sign = -1.0f;
    float max_step_px = 40.0f;
    float smoothing_hz = 12.0f;
};

// Thin adapter for the existing TouchHelperA API. It does not read game memory:
// the host supplies target/camera data and the calculated angular error.
class TouchAimAdapter {
public:
    explicit TouchAimAdapter(const TouchConfig& config);
    ~TouchAimAdapter();

    TouchAimAdapter(const TouchAimAdapter&) = delete;
    TouchAimAdapter& operator=(const TouchAimAdapter&) = delete;

    void Configure(const TouchConfig& config);
    bool Tick(bool enabled, bool target_valid,
              float yaw_error_deg, float pitch_error_deg, float dt_seconds);
    void Reset();

private:
    void Emit(float x, float y, bool first_down);
    void Release();

    TouchConfig config_{};
    bool down_ = false;
    float x_ = 0.0f;
    float y_ = 0.0f;
};

} // namespace bing87
