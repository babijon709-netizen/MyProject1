#include "player_esp_watchdog.h"

#include <cstdio>

namespace {
int failures = 0;
#define CHECK(condition, label) do { \
    if (!(condition)) { ++failures; std::printf("FAIL: %s (line %d)\n", label, __LINE__); } \
} while (0)
}

int main() {
    PlayerEspWatchdog watchdog;

    // A short complete-player read gap is recoverable. It must not flush the
    // active resolver/position caches and manufacture a blank ESP frame.
    CHECK(!watchdog.observe(true, 0.0).reset_resolvers, "healthy starts clear");
    CHECK(!watchdog.observe(false, 1.0).reset_resolvers, "gap begins without reset");
    CHECK(!watchdog.observe(false, 3.99).reset_resolvers, "gap below three seconds held");
    const PlayerEspWatchdogAction first = watchdog.observe(false, 4.0);
    CHECK(first.reset_resolvers && !first.deep_recovery, "three second failure resets once");

    CHECK(!watchdog.observe(false, 6.99).reset_resolvers, "repeat window held");
    CHECK(watchdog.observe(false, 7.0).reset_resolvers, "persistent failure can retry recovery");

    CHECK(!watchdog.observe(true, 7.1).reset_resolvers, "healthy snapshot clears failure state");
    CHECK(!watchdog.observe(false, 7.2).reset_resolvers, "next transient starts clean");

    if (failures) {
        std::printf("%d FAILURES\n", failures);
        return 1;
    }
    std::puts("ALL PLAYER-ESP-WATCHDOG TESTS PASS");
    return 0;
}
