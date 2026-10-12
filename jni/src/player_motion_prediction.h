#pragma once

#include "Vector.h"
#include <algorithm>
#include <cmath>

namespace playermotion {

constexpr double kPredictionDelaySeconds = 0.008;
constexpr double kPredictionMaxSeconds = 0.100;
constexpr float kPredictionMaxDistance = 1.20F;
// Потолок упреждения для РИСУЕМОЙ позиции (бокс на экране). Прицелу нужен весь
// запас — он догоняет цель и сам его учитывает, — а боксу упреждение в метр
// только вредит: он едет впереди игрока, а на панораме это выглядит как
// «визуал не держится на объекте». 0.35 м — примерно полшага тела.
constexpr float kDrawnPredictionMaxDistance = 0.35F;

// Recent direction changes matter more than a long average when predicting strafes.
inline Vec3 blend_velocity(const Vec3& previous, const Vec3& measured) {
    const bool previous_finite = std::isfinite(previous.x) && std::isfinite(previous.y) &&
                                 std::isfinite(previous.z);
    const bool measured_finite = std::isfinite(measured.x) && std::isfinite(measured.y) &&
                                 std::isfinite(measured.z);
    if (!measured_finite) return previous_finite ? previous : Vec3{};
    if (!previous_finite) return measured;

    const float previous_speed = std::sqrt(previous.x * previous.x + previous.y * previous.y +
                                           previous.z * previous.z);
    const float measured_speed = std::sqrt(measured.x * measured.x + measured.y * measured.y +
                                           measured.z * measured.z);
    if (!std::isfinite(previous_speed) || !std::isfinite(measured_speed)) return measured;

    const float previous_horizontal_speed =
        std::sqrt(previous.x * previous.x + previous.z * previous.z);
    const float measured_horizontal_speed =
        std::sqrt(measured.x * measured.x + measured.z * measured.z);
    float horizontal_weight = 0.5F;
    if (previous_horizontal_speed < 0.5F) {
        horizontal_weight = 1.0F;
    } else if (measured_horizontal_speed < 0.5F) {
        horizontal_weight = 0.8F;
    } else {
        const float dot = previous.x * measured.x + previous.z * measured.z;
        const float direction_cosine = dot /
            (previous_horizontal_speed * measured_horizontal_speed);
        if (std::isfinite(direction_cosine) && direction_cosine < 0.5F)
            horizontal_weight = 0.85F;
    }

    float vertical_weight = 0.5F;
    if (previous_speed < 0.5F) vertical_weight = 1.0F;
    else if (measured_speed < 0.5F) vertical_weight = 0.8F;
    const float previous_horizontal_weight = 1.0F - horizontal_weight;
    const float previous_vertical_weight = 1.0F - vertical_weight;
    return {
        previous.x * previous_horizontal_weight + measured.x * horizontal_weight,
        previous.y * previous_vertical_weight + measured.y * vertical_weight,
        previous.z * previous_horizontal_weight + measured.z * horizontal_weight
    };
}

inline Vec3 predict(const Vec3& sample, const Vec3& velocity,
                    double sample_time, double now,
                    float max_distance = kPredictionMaxDistance) {
    double age = now - sample_time - kPredictionDelaySeconds;
    if (!(age > 0.0) || !std::isfinite(age)) return sample;
    age = std::min(age, kPredictionMaxSeconds);

    Vec3 step = {
        velocity.x * static_cast<float>(age),
        velocity.y * static_cast<float>(age),
        velocity.z * static_cast<float>(age)
    };
    const float distance2 = step.x * step.x + step.y * step.y + step.z * step.z;
    const float max2 = max_distance * max_distance;
    if (!std::isfinite(distance2)) return sample;
    if (distance2 > max2) {
        const float scale = max_distance / std::sqrt(distance2);
        step.x *= scale;
        step.y *= scale;
        step.z *= scale;
    }
    return {sample.x + step.x, sample.y + step.y, sample.z + step.z};
}

} // namespace playermotion
