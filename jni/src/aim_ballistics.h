#pragma once

#include "Vector.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace aim_ballistics {

constexpr float kIntegratorStep = 0.03125f;
constexpr float kGravityY = -9.81f;
constexpr float kMaxFlightSeconds = 4.0f;
constexpr int kMaxSteps = static_cast<int>(kMaxFlightSeconds / kIntegratorStep);

struct Parameters {
    float muzzle_speed = 0.0f;
    float gravity = 0.0f;
    float air_drag = 0.0f;
    float underwater_drag = 0.0f;
    float max_range = 0.0f;
    bool valid = false;
};

struct Solution {
    Vec3 direction{};
    float flight_time = 0.0f;
    bool valid = false;
};

inline bool finite(float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return (bits & 0x7f800000u) != 0x7f800000u;
}

inline bool finite(const Vec3& value) {
    return finite(value.x) && finite(value.y) && finite(value.z);
}

inline Vec3 add(const Vec3& a, const Vec3& b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

inline Vec3 sub(const Vec3& a, const Vec3& b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

inline Vec3 scale(const Vec3& value, float amount) {
    return {value.x * amount, value.y * amount, value.z * amount};
}

inline float length(const Vec3& value) {
    return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
}

inline float drag_scale(float drag, float dt) {
    const float value = 1.0f - drag * dt;
    return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
}

inline bool parameters_valid(const Parameters& params) {
    return params.valid && finite(params.muzzle_speed) &&
           params.muzzle_speed >= 1.0f && params.muzzle_speed <= 2000.0f &&
           finite(params.gravity) && params.gravity >= -20.0f && params.gravity <= 20.0f &&
           finite(params.air_drag) && params.air_drag >= 0.0f && params.air_drag <= 100.0f &&
           finite(params.underwater_drag) && params.underwater_drag >= 0.0f &&
           params.underwater_drag <= 100.0f && finite(params.max_range) &&
           params.max_range >= 0.0f && params.max_range <= 5000.0f;
}

inline float residual(float time, float factor, float drop,
                      const Vec3& origin, const Vec3& target,
                      const Vec3& target_velocity, const Vec3& inherited_velocity,
                      const Parameters& params, Vec3* required = nullptr) {
    const Vec3 moving_target = add(target, scale(target_velocity, time));
    Vec3 needed = sub(moving_target, origin);
    needed.y -= drop;
    needed = sub(needed, scale(inherited_velocity, factor));
    if (required) *required = needed;
    return length(needed) - params.muzzle_speed * factor;
}

inline bool solve_with_medium_path(const Vec3& origin, const Vec3& target,
                                   const Vec3& target_velocity,
                                   const Vec3& inherited_velocity,
                                   const Parameters& params, const Vec3& guess_direction,
                                   Solution& out) {
    bool air_path[kMaxSteps]{};
    Vec3 position = origin;
    Vec3 velocity = add(scale(guess_direction, params.muzzle_speed), inherited_velocity);
    for (int index = 0; index < kMaxSteps; ++index) {
        const bool above_surface = position.y >= 0.0f;
        air_path[index] = above_surface;
        const float dt = kIntegratorStep;
        const float drag = above_surface ? params.air_drag : params.underwater_drag;
        const float damping = drag_scale(drag, dt);
        const Vec3 old_velocity = velocity;
        if (above_surface) velocity.y += kGravityY * params.gravity * dt;
        velocity = scale(velocity, damping);
        position = add(position, scale(add(old_velocity, velocity), 0.5f * dt));
        if (!finite(position) || !finite(velocity)) return false;
    }

    float factor = 0.0f;
    float factor_velocity = 1.0f;
    float drop = 0.0f;
    float drop_velocity = 0.0f;
    float previous_time = 0.0f;
    float previous_factor = 0.0f;
    float previous_drop = 0.0f;
    float previous_residual = residual(0.0f, 0.0f, 0.0f, origin, target,
                                       target_velocity, inherited_velocity, params);

    for (int index = 1; index <= kMaxSteps; ++index) {
        const float dt = kIntegratorStep;
        const bool above_surface = air_path[index - 1];
        const float drag = above_surface ? params.air_drag : params.underwater_drag;
        const float damping = drag_scale(drag, dt);

        const float next_factor_velocity = factor_velocity * damping;
        const float next_factor = factor +
            (factor_velocity + next_factor_velocity) * 0.5f * dt;
        const float gravity_delta = above_surface ? kGravityY * params.gravity * dt : 0.0f;
        const float next_drop_velocity = (drop_velocity + gravity_delta) * damping;
        const float next_drop = drop +
            (drop_velocity + next_drop_velocity) * 0.5f * dt;
        const float time = static_cast<float>(index) * dt;
        const float current_residual = residual(time, next_factor, next_drop,
            origin, target, target_velocity, inherited_velocity, params);

        if (previous_residual >= 0.0f && current_residual <= 0.0f) {
            float low = previous_time;
            float high = time;
            for (int iteration = 0; iteration < 14; ++iteration) {
                const float middle = (low + high) * 0.5f;
                const float alpha = (middle - previous_time) / dt;
                const float middle_factor = previous_factor + (next_factor - previous_factor) * alpha;
                const float middle_drop = previous_drop + (next_drop - previous_drop) * alpha;
                const float middle_residual = residual(middle, middle_factor, middle_drop,
                    origin, target, target_velocity, inherited_velocity, params);
                if (middle_residual > 0.0f) low = middle;
                else high = middle;
            }
            const float flight_time = (low + high) * 0.5f;
            const float alpha = (flight_time - previous_time) / dt;
            const float flight_factor = previous_factor + (next_factor - previous_factor) * alpha;
            const float flight_drop = previous_drop + (next_drop - previous_drop) * alpha;
            Vec3 needed{};
            residual(flight_time, flight_factor, flight_drop, origin, target,
                     target_velocity, inherited_velocity, params, &needed);
            const float distance = length(needed);
            const float travelled = params.muzzle_speed * flight_factor;
            if (!finite(distance) || distance < 0.01f || !finite(travelled) ||
                (params.max_range > 0.0f && travelled > params.max_range)) return false;
            out.direction = scale(needed, 1.0f / distance);
            out.flight_time = flight_time;
            out.valid = finite(out.direction) && flight_time > 0.0f;
            return out.valid;
        }

        previous_time = time;
        previous_factor = next_factor;
        previous_drop = next_drop;
        previous_residual = current_residual;
        factor = next_factor;
        factor_velocity = next_factor_velocity;
        drop = next_drop;
        drop_velocity = next_drop_velocity;
        if (!finite(previous_residual) || !finite(factor) || !finite(drop)) return false;
    }
    return false;
}

inline bool solve(const Vec3& origin, const Vec3& target,
                  const Vec3& target_velocity, const Vec3& inherited_velocity,
                  const Parameters& params, Solution& out) {
    out = {};
    if (!parameters_valid(params) || !finite(origin) || !finite(target) ||
        !finite(target_velocity) || !finite(inherited_velocity)) return false;

    const float target_distance = length(sub(target, origin));
    const float inherited_speed = length(inherited_velocity);
    const float target_speed = length(target_velocity);
    if (!finite(target_distance) || target_distance < 0.01f ||
        !finite(inherited_speed) || inherited_speed > 30.0f ||
        !finite(target_speed) || target_speed > 30.0f) return false;

    float estimate = target_distance / params.muzzle_speed;
    if (estimate > kMaxFlightSeconds) estimate = kMaxFlightSeconds;
    Vec3 guess = sub(add(target, scale(target_velocity, estimate)), origin);
    float guess_length = length(guess);
    if (!finite(guess_length) || guess_length < 0.01f) return false;
    guess = scale(guess, 1.0f / guess_length);

    Solution candidate{};
    for (int iteration = 0; iteration < 3; ++iteration) {
        if (!solve_with_medium_path(origin, target, target_velocity, inherited_velocity,
                                    params, guess, candidate)) return false;
        const float agreement = candidate.direction.x * guess.x +
                               candidate.direction.y * guess.y +
                               candidate.direction.z * guess.z;
        out = candidate;
        if (agreement > 0.9999f) return true;
        guess = candidate.direction;
    }
    return out.valid;
}

} // namespace aim_ballistics
