#pragma once

#include "Vector.h"
#include <algorithm>
#include <cmath>

namespace playermotion {

constexpr double kPredictionDelaySeconds = 0.008;
constexpr double kPredictionMaxSeconds = 0.100;
constexpr float kPredictionMaxDistance = 1.20F;
// Ниже этой скорости (м/с) «скорость» — это дрожь сетевой позиции стоящего
// игрока: экстраполяция по ней уносила бы модель за свою настоящую позицию.
constexpr float kPredictionMinSpeed = 0.25F;

inline Vec3 predict(const Vec3& sample, const Vec3& velocity,
                    double sample_time, double now) {
    double age = now - sample_time - kPredictionDelaySeconds;
    if (!(age > 0.0) || !std::isfinite(age)) return sample;
    const float speed2 = velocity.x * velocity.x + velocity.y * velocity.y + velocity.z * velocity.z;
    if (!(speed2 >= kPredictionMinSpeed * kPredictionMinSpeed)) return sample;
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
        step.x *= scale;
        step.y *= scale;
        step.z *= scale;
    }
    return {sample.x + step.x, sample.y + step.y, sample.z + step.z};
}

} // namespace playermotion
