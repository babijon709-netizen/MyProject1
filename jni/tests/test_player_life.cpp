#include "player_life.h"

#include <cstdio>

namespace {
int failures = 0;
#define CHECK(condition, name) do { \
    if (!(condition)) { ++failures; std::printf("FAIL: %s (line %d)\n", name, __LINE__); } \
} while (0)

constexpr int32_t kDeadState = 7;
constexpr int32_t kAliveState = 0;
}

int main() {
    CHECK(!playerlife::confirmed_dead(false, -1, -1, kDeadState),
          "unknown reads do not invent a death");
    CHECK(!playerlife::confirmed_dead(false, 0, kAliveState, kDeadState),
          "live signals remain live");
    CHECK(playerlife::confirmed_dead(true, -1, kAliveState, kDeadState),
          "respawning excludes the player");
    CHECK(playerlife::confirmed_dead(false, 1, kAliveState, kDeadState),
          "death handler excludes the player");
    CHECK(playerlife::confirmed_dead(false, 0, kDeadState, kDeadState),
          "KCC dead state excludes the player");

    if (failures) {
        std::printf("%d FAILURES\n", failures);
        return 1;
    }
    std::puts("ALL PLAYER-LIFE TESTS PASS");
    return 0;
}
