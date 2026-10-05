#pragma once
#include "silent_aim.h"
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
inline bool direction(const Command& c, float (&out)[3]) {
    float len2 = 0.f;
    for (int i = 0; i < 3; ++i) {
        if (!finite(c.aim[i]) || !finite(c.eye[i]) ||
            std::fabs(c.aim[i]) > 1e6f || std::fabs(c.eye[i]) > 1e6f) return false;
        out[i] = c.aim[i] - c.eye[i];
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
Result tick(Memory& mem, const Command& c) {
    float dir[3];
    if (!valid_command(c) || !direction(c, dir)) return Result::invalid;
    if (!guarded(mem, c)) return Result::guard_failed;
    return mem.write(c.look + c.value_offset, dir, sizeof(dir))
        ? Result::written : Result::write_failed;
}
}
