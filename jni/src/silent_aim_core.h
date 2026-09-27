#pragma once
#include "silent_aim.h"
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <cstring>

namespace silent_aim {
constexpr uint64_t kTagMask = 0x00FFFFFFFFFFFFFFULL;
constexpr uint64_t kLeaseMs = 100; // Do not keep firing at a stale 600ms snapshot.
// Реактивный цикл: слепая пере-запись оси каждые 2500/1200 мкс давала окно до
// ~3 мс после того, как игра перезаписала ось направлением камеры — выстрел,
// попавший в это окно, улетал ровно в прицел («смотрю в сторону, пули летят
// куда смотрю»). Теперь ось пишется по факту: новый снимок — пишем сразу;
// дальше только ЧИТАЕМ ось каждые kPollUs и переписываем (с полными гвардами),
// как только игра её перезаписала. Чтение всегда безопасно, а запись по-прежнему
// идёт только после проверки принадлежности/классов/оружия этой же команды.
constexpr unsigned kPollUs = 150;
// Ось считается «нашей», пока компоненты отличаются не более чем на kAxisSlop:
// перезапись игрой (камера != цель почти всегда) ловится с запасом, а дрожание
// цели на младших битах float не гоняет запись каждый полл.
constexpr float kAxisSlop = 4e-4f;
inline bool valid_pointer(uint64_t p) { return p >= 0x10000 && p < 0x800000000000ULL; }
inline bool fresh(uint64_t published, uint64_t now) {
    return now >= published && now - published < kLeaseMs;
}
// Android uses -ffast-math; std::isfinite can be optimized away there.
inline bool finite(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return (bits & 0x7f800000u) != 0x7f800000u;
}
inline bool direction(const Command& c, float (&out)[3]) {
    float len2 = 0;
    for (int i = 0; i < 3; ++i) {
        if (!finite(c.aim[i]) || !finite(c.eye[i]) ||
            std::fabs(c.aim[i]) > 1e6f || std::fabs(c.eye[i]) > 1e6f) return false;
        out[i] = c.aim[i] - c.eye[i];
        len2 += out[i] * out[i];
    }
    if (!finite(len2) || len2 < 0.25f * 0.25f) return false;
    const float inv = 1.f / std::sqrt(len2);
    for (float& v : out) v *= inv;
    return true;
}
inline bool valid_command(const Command& c) {
    if (c.pid <= 0 || !valid_pointer(c.target)) return false;
    for (uint64_t p : {c.player, c.handler, c.look, c.player_class,
                       c.handler_class, c.look_class, c.fp_manager, c.weapon})
        if (!valid_pointer(p)) return false;
    for (uint64_t o : {c.player_handler_offset, c.handler_player_offset,
                       c.look_offset, c.value_offset, c.fp_manager_offset, c.weapon_offset})
        if (o < 0x10 || o > 0x1000) return false;
    float dir[3];
    return direction(c, dir);
}
// read() MUST bypass any frame cache. Identity and ownership are checked on
// EVERY write, including the weapon sentinel (no two-tick debounce).
// External read/write is not atomic with the game's GC: these are guards,
// not a guarantee against all object-lifetime races.
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
           equal(c.look, c.look_class) &&
           equal(c.player + c.fp_manager_offset, c.fp_manager) &&
           equal(c.fp_manager + c.weapon_offset, c.weapon);
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
// Безопасная проверка «ось всё ещё наша»: ОДНО чтение по уже подтверждённому
// маршруту. Отказ чтения считается рассинхроном — вызывающий тогда делает
// полный guarded-такт с записью, так что ложного доверия к мёртвому адресу нет.
template<class Memory>
bool axis_matches(Memory& mem, const Command& c, const float dir[3]) {
    float cur[3];
    if (!mem.read(c.look + c.value_offset, cur, sizeof(cur))) return false;
    for (int i = 0; i < 3; ++i)
        if (!finite(cur[i]) || std::fabs(cur[i] - dir[i]) > kAxisSlop) return false;
    return true;
}
}
