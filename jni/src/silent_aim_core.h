#pragma once
#include "silent_aim.h"
#include "aim_ballistics.h"
#include <cmath>
#include <cstddef>
#include <cstring>
#include <initializer_list>

namespace silent_aim {
constexpr uint64_t kTagMask = 0x00FFFFFFFFFFFFFFULL;
constexpr uint64_t kLeaseMs = 100;
// The game refreshes its look direction close to the shot simulation. A 2.5 ms
// writer left a visible race where some bullets kept the camera direction.
constexpr unsigned kTickUs = 500, kHotUs = 250;
inline bool valid_pointer(uint64_t p) { return p >= 0x10000 && p < 0x800000000000ULL; }
inline bool fresh(uint64_t published, uint64_t now) {
    return now >= published && now - published < kLeaseMs;
}
// Android собирается с -ffast-math: std::isfinite там может быть оптимизирован.
inline bool finite(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return (bits & 0x7f800000u) != 0x7f800000u;
}
inline bool direction(const Command& c, float (&out)[3], float age_seconds = 0.f) {
    if (!finite(age_seconds) || age_seconds < 0.f || age_seconds > kLeaseMs * 0.001f) return false;
    float len2 = 0.f;
    for (int i = 0; i < 3; ++i) {
        if (!finite(c.aim[i]) || !finite(c.eye[i]) || !finite(c.velocity[i]) ||
            !finite(c.eye_velocity[i]) || std::fabs(c.velocity[i]) > 30.f ||
            std::fabs(c.eye_velocity[i]) > 30.f ||
            std::fabs(c.aim[i]) > 1e6f || std::fabs(c.eye[i]) > 1e6f) return false;
        out[i] = c.aim[i] - c.eye[i] +
                 (c.velocity[i] - c.eye_velocity[i]) * age_seconds;
        len2 += out[i] * out[i];
    }
    if (!finite(len2) || len2 < 0.25f * 0.25f) return false;
    const float inv = 1.f / std::sqrt(len2);
    for (float& value : out) value *= inv;
    return true;
}
inline bool valid_command(const Command& c) {
    if (c.pid <= 0 || !valid_pointer(c.target)) return false;
    for (uint64_t p : {c.player, c.handler, c.look, c.player_class,
                       c.handler_class, c.look_class})
        if (!valid_pointer(p)) return false;
    for (uint64_t o : {c.player_handler_offset, c.handler_player_offset,
                       c.look_offset, c.value_offset})
        if (o < 0x10 || o > 0x1000) return false;
    float dir[3];
    return direction(c, dir);
}
// Snapshot the trajectory only once per overlay frame. The worker merely
// advances both moving origins for its short lease, never walks weapon memory.
inline bool solve_axis(const Vec3& origin, const Vec3& target,
                       const Vec3& velocity, const Vec3& eye_velocity,
                       const aim_ballistics::Parameters& params,
                       float sample_age, float latency, Command& c) {
    using namespace aim_ballistics;
    if (!aim_ballistics::finite(origin) || !aim_ballistics::finite(target) ||
        !aim_ballistics::finite(velocity) || !aim_ballistics::finite(eye_velocity) || !finite(sample_age) || !finite(latency) ||
        sample_age < 0.f || sample_age > 0.35f ||
        latency < 0.f || latency > 0.15f ||
        length(velocity) > 30.f || length(eye_velocity) > 30.f) return false;
    const Vec3 predicted = add(target, scale(velocity, sample_age + latency));
    Vec3 axis = predicted;
    if (params.valid) {
        Solution shot{};
        if (!solve(origin, predicted, velocity, eye_velocity, params, shot)) return false;
        // Maintain the original target range: the worker adds velocity in metres.
        axis = add(origin, scale(shot.direction, length(sub(predicted, origin))));
    }
    if (!aim_ballistics::finite(axis)) return false;
    c.eye[0] = origin.x; c.eye[1] = origin.y; c.eye[2] = origin.z;
    c.aim[0] = axis.x; c.aim[1] = axis.y; c.aim[2] = axis.z;
    c.velocity[0] = velocity.x; c.velocity[1] = velocity.y; c.velocity[2] = velocity.z;
    c.eye_velocity[0] = eye_velocity.x;
    c.eye_velocity[1] = eye_velocity.y;
    c.eye_velocity[2] = eye_velocity.z;
    float dir[3];
    return direction(c, dir);
}
// read() обязан обходить кадровый кэш. Перед КАЖДОЙ записью повторно
// проверяются class pointers и обратные ссылки PlayerEventHandler. Состояние
// текущего FPObject намеренно не является guard: его layout зависит от типа
// оружия, а LookDirection принадлежит игроку и переживает смену оружия.
template<class Memory>
bool guarded(Memory& mem, const Command& c) {
    auto equal = [&](uint64_t address, uint64_t expected) {
        uint64_t p = 0;
        return mem.read(address, &p, sizeof(p)) && (p & kTagMask) == expected;
    };
    return equal(c.player, c.player_class) &&
           equal(c.player + c.player_handler_offset, c.handler) &&
           equal(c.handler, c.handler_class) &&
           equal(c.handler + c.handler_player_offset, c.player) &&
           equal(c.handler + c.look_offset, c.look) &&
           equal(c.look, c.look_class);
}
enum class Result { written, invalid, guard_failed, write_failed };
template<class Memory>
Result tick(Memory& mem, const Command& c, float age_seconds = 0.f) {
    float dir[3];
    if (!valid_command(c) || !direction(c, dir, age_seconds)) return Result::invalid;
    if (!guarded(mem, c)) return Result::guard_failed;
    return mem.write(c.look + c.value_offset, dir, sizeof(dir))
        ? Result::written : Result::write_failed;
}
}
