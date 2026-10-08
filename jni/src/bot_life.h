#pragma once
#include <cstdint>

namespace botlife {
// NPCHumanoidDeathHandler.death, verified in release 11946 and beta 11943.
inline constexpr std::uint64_t kDeathOffset = 0xE8;

struct State {
    std::uint64_t handler = 0;
    bool dead = false;

    template <typename Read>
    bool refresh(Read read) {
        std::uint8_t value = 0;
        // Failed/invalid reads are not evidence of either death or respawn.
        if (handler && read(handler + kDeathOffset, value) && value <= 1)
            dead = value != 0;
        return dead;
    }
};
} // namespace botlife
