#include "game_offsets_active.h"
#include <cassert>
#include <cstdint>
#include <iostream>
#include <type_traits>

int main() {
    static_assert(std::is_same_v<decltype(go_active::PLAYER_POSITION),
                                 const std::uint64_t>);
    static_assert(go_active::PLAYER_POSITION == game_offsets::PLAYER_POSITION);
    // Shipped release combat layout differs from generated metadata by -4.
    static_assert(go_active::PLAYER_POSITION == 0x1D0);
    // Native Transform fields are fixed from the supported release libunity;
    // Candidate probing must never replace these compile-time values.
    static_assert(go_active::TRANSFORM_HIERARCHY_DATA == 0x38);
    static_assert(go_active::TRANSFORM_HIERARCHY_INDEX == 0x40);
    static_assert(go_active::TRANSFORM_HIERARCHY_MATRICES == 0x18);
    static_assert(go_active::TRANSFORM_HIERARCHY_INDICES == 0x20);
    static_assert(go_active::PLAYER_MANAGER_TYPEINFO_RVA ==
                  game_offsets::PLAYER_MANAGER_TYPEINFO_RVA);
    static_assert(go_active::FPMOTION_AIM_OFFSET ==
                  game_offsets::FPMOTION_AIM_OFFSET);
    assert(go_active::PLAYER_MANAGER_TYPEINFO_RVA != 0);
    std::cout << "release-only compile-time offsets: OK\n";
}
