#include "hit_sound_tracker.h"

#include <cstdio>
#include <limits>

namespace {
int failures = 0;

#define CHECK(condition, name) do { \
    if (!(condition)) { \
        ++failures; \
        std::printf("FAIL: %s (line %d)\n", name, __LINE__); \
    } \
} while (0)
}

int main() {
    using hitsound::HealthSample;
    using hitsound::Tracker;

    {
        Tracker tracker;
        HealthSample enemy{1, 100.0f, false};
        CHECK(!tracker.observe_health(&enemy, 1, false, 1.0),
              "first health sample establishes a baseline");
        enemy.health = 90.0f;
        CHECK(!tracker.observe_health(&enemy, 1, false, 1.1),
              "health loss without a local shot is ignored");

        tracker.observe_shot(true, 10.0f, 1.2);
        tracker.observe_shot(true, 10.1f, 1.3);
        enemy.health = 80.0f;
        CHECK(tracker.observe_health(&enemy, 1, false, 1.31),
              "health loss after a shot reports a hit");
        CHECK(!tracker.observe_health(&enemy, 1, false, 1.32),
              "the same health value does not report twice");
    }

    {
        Tracker tracker;
        HealthSample ally{2, 100.0f, true};
        tracker.observe_health(&ally, 1, false, 2.0);
        tracker.observe_shot(true, 20.0f, 2.1);
        tracker.observe_shot(true, 20.1f, 2.2);
        ally.health = 70.0f;
        CHECK(!tracker.observe_health(&ally, 1, false, 2.21),
              "ally health loss is ignored");
    }

    {
        Tracker tracker;
        HealthSample enemy{3, 100.0f, false};
        tracker.observe_health(&enemy, 1, false, 3.0);
        tracker.observe_shot(true, 30.0f, 3.1);
        tracker.observe_shot(true, 30.1f, 3.2);
        enemy.health = 75.0f;
        CHECK(!tracker.observe_health(&enemy, 1, true, 3.21),
              "transient frame does not report a hit");
        CHECK(tracker.observe_health(&enemy, 1, false, 3.22),
              "next healthy frame catches the health loss");
    }

    {
        Tracker tracker;
        HealthSample enemy{4, 100.0f, false};
        tracker.observe_health(&enemy, 1, false, 4.0);
        tracker.observe_shot(true, 40.0f, 4.1);
        tracker.observe_shot(true, 40.1f, 4.2);
        enemy.health = 80.0f;
        CHECK(!tracker.observe_health(&enemy, 1, false, 5.0),
              "expired shot window does not report unrelated damage");
    }

    {
        Tracker tracker;
        HealthSample enemy{5, 100.0f, false};
        tracker.observe_health(&enemy, 1, false, 5.0);
        tracker.observe_shot(true, 50.0f, 5.1);
        tracker.observe_shot(true, 50.1f, 5.2);
        tracker.observe_shot(true, 1.0f, 5.3);
        enemy.health = 60.0f;
        CHECK(!tracker.observe_health(&enemy, 1, false, 5.31),
              "game-time reset clears the pending shot window");
    }

    {
        Tracker tracker;
        HealthSample invalid{6, std::numeric_limits<float>::quiet_NaN(), false};
        CHECK(!tracker.observe_health(&invalid, 1, false, 6.0),
              "invalid health is ignored");
    }

    if (failures) {
        std::printf("%d FAILURES\n", failures);
        return 1;
    }
    std::puts("ALL HIT-SOUND TRACKER TESTS PASS");
    return 0;
}
