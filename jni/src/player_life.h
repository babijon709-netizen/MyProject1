#pragma once

#include <cstdint>

namespace playerlife {

// A failed/unknown field is represented by -1 and must not classify a living
// player as dead. move_state_dead is supplied by the selected game offset set.
inline bool confirmed_dead(bool respawning, int death_flag, int32_t move_state,
                           int32_t move_state_dead) {
    return respawning || death_flag == 1 || move_state == move_state_dead;
}

}  // namespace playerlife
