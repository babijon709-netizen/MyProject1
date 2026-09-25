#pragma once
#include <cmath>

// A few centimetres of bone animation at point-blank range span many degrees.
// Keep the old long-range threshold; close up, tolerate motion within the
// target's hit area (cap the radius so a moving enemy can still be followed).
inline float AimTouchDeadDeg(float deg_per_px, float range_m, float quantum_deg) {
    float dead = deg_per_px * 1.5f;
    if (range_m > 1.f && std::isfinite(range_m)) {
        const float d = std::atan(0.03f / range_m) * 57.29577951f;
        if (d < dead) dead = d;
    }
    if (range_m > 0.f && range_m < 3.f && std::isfinite(range_m)) {
        const float d = std::atan(0.03f / range_m) * 57.29577951f;
        const float close = std::fmin(d * 0.6f, 1.2f);
        if (close > dead) dead = close;
    }
    if (quantum_deg * 2.f > dead) dead = quantum_deg * 2.f;
    return dead;
}

// At the last 2-3 quanta a full-error jump can overshoot an animated bone,
// especially if the measured sensitivity lags the game's actual gain. Move
// only one device unit, then wait for the camera/flight feedback.
inline float AimTouchFinalGridStep(float err_deg, float gain, float quantum_deg,
                                   float units_per_px, float dead_deg, float usual_step) {
    if (quantum_deg > 0.f && std::fabs(err_deg) >= dead_deg &&
        std::fabs(err_deg) < quantum_deg * 3.f &&
        std::fabs(gain) > 1e-5f && units_per_px > 0.f)
        return std::copysign(1.f / units_per_px, err_deg / gain);
    return usual_step;
}
