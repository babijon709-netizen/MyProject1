#pragma once
#include <cstdint>

// DLP2 adapter for this project. Only immutable frame snapshots cross into
// the worker; it never touches g_mem, camera globals or live offset tables.
namespace silent_aim {
struct Command {
    int pid = -1;
    uint64_t player = 0, handler = 0, look = 0;
    uint64_t player_class = 0, handler_class = 0, look_class = 0;
    uint64_t fp_manager = 0, weapon = 0;
    uint64_t player_handler_offset = 0, handler_player_offset = 0;
    uint64_t look_offset = 0, value_offset = 0;
    uint64_t fp_manager_offset = 0, weapon_offset = 0;
    uint64_t target = 0;
    float eye[3] = {}, aim[3] = {};
    bool recent_shot = false;
};
struct Stats {
    uint64_t writes = 0, failures = 0, guard_failures = 0;
    bool active = false, axis = false;
};
bool publish(const Command& command);
// Synchronous cancellation: no write from the previous command after return.
void clear();
void shutdown();
Stats stats();
}
