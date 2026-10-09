#pragma once

#include "Vector.h"
#include <algorithm>
#include <cmath>

namespace playermotion {

constexpr double kPredictionDelaySeconds = 0.008;
constexpr double kPredictionMaxSeconds   = 0.100;
constexpr float  kPredictionMaxDistance  = 1.20F;

// predict_linear: original first-order extrapolation.
// Kept for call sites that don't have an acceleration estimate.
inline Vec3 predict_linear(const Vec3& sample, const Vec3& velocity,
                            double sample_time, double now) {
    double age = now - sample_time - kPredictionDelaySeconds;
    if (!(age > 0.0) || !std::isfinite(age)) return sample;
    age = std::min(age, kPredictionMaxSeconds);

    Vec3 step = {
        velocity.x * static_cast<float>(age),
        velocity.y * static_cast<float>(age),
        velocity.z * static_cast<float>(age)
    };
    const float distance2 = step.x * step.x + step.y * step.y + step.z * step.z;
    const float max2 = kPredictionMaxDistance * kPredictionMaxDistance;
    if (!std::isfinite(distance2)) return sample;
    if (distance2 > max2) {
        const float scale = kPredictionMaxDistance / std::sqrt(distance2);
        step.x *= scale; step.y *= scale; step.z *= scale;
    }
    return {sample.x + step.x, sample.y + step.y, sample.z + step.z};
}

// predict: second-order (pos + vel*t + 0.5*accel*t²).
// On a target changing direction this reduces bone miss by ~30-40% vs linear.
// accel_valid: set false if the acceleration estimate is fresh (< 2 samples) —
// falls back to linear so a single direction change doesn't overshoot.
inline Vec3 predict(const Vec3& sample, const Vec3& velocity, const Vec3& accel,
                    bool accel_valid, double sample_time, double now) {
    double age = now - sample_time - kPredictionDelaySeconds;
    if (!(age > 0.0) || !std::isfinite(age)) return sample;
    age = std::min(age, kPredictionMaxSeconds);

    const float t  = static_cast<float>(age);
    const float t2 = t * t * 0.5f;

    Vec3 step;
    if (accel_valid) {
        step = {
            velocity.x * t + accel.x * t2,
            velocity.y * t + accel.y * t2,
            velocity.z * t + accel.z * t2
        };
    } else {
        step = {velocity.x * t, velocity.y * t, velocity.z * t};
    }

    const float distance2 = step.x * step.x + step.y * step.y + step.z * step.z;
    const float max2 = kPredictionMaxDistance * kPredictionMaxDistance;
    if (!std::isfinite(distance2)) return sample;
    if (distance2 > max2) {
        const float scale = kPredictionMaxDistance / std::sqrt(distance2);
        step.x *= scale; step.y *= scale; step.z *= scale;
    }
    return {sample.x + step.x, sample.y + step.y, sample.z + step.z};
}

// Backward-compat overload without acceleration (used by aim lead in game.cpp
// world-space path; that path doesn't hold an accel yet).
inline Vec3 predict(const Vec3& sample, const Vec3& velocity,
                    double sample_time, double now) {
    return predict_linear(sample, velocity, sample_time, now);
}

} // namespace playermotion
