#pragma once
#include <cstdint>

// Изолированный писатель сайлента. Главный поток публикует неизменяемый
// снимок; оба потока читают игру через выбранный транспорт, минуя кадровый кэш.
namespace silent_aim {
// LookDirection belongs to PlayerEventHandler, not to FPHitscan.  Do not put a
// concrete current-weapon chain in this snapshot: projectile weapons (such as
// the bow) may have a different FPObject graph while the shared shot axis is
// still valid.  The player/handler/look ownership chain below is rechecked
// immediately before every write.
struct Command {
    int pid = -1;
    uint64_t player = 0, handler = 0, look = 0;
    uint64_t player_class = 0, handler_class = 0, look_class = 0;
    uint64_t player_handler_offset = 0, handler_player_offset = 0;
    uint64_t look_offset = 0, value_offset = 0;
    uint64_t target = 0;
    float eye[3] = {}, aim[3] = {}, velocity[3] = {}, eye_velocity[3] = {};
    bool recent_shot = false;
};

// Automatic no-spread follows the silent-mode toggle, rather than the writer's
// transient target lease.  This intentionally stays true when no target is in
// the aim FOV; explicit no-spread is combined with it by the viewmodel layer.
inline bool wants_no_spread(bool aim_enabled, bool silent_mode, bool freecam) {
    return aim_enabled && silent_mode && !freecam;
}
struct Stats {
    uint64_t writes = 0, failures = 0, guard_failures = 0;
    bool active = false, axis = false;
};
// Guarded immediate write and worker publication are serialized as one operation.
bool publish_sync(const Command& command);
void clear();
void shutdown();
Stats stats();
}
