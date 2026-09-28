#include "esp_continuity.h"
#include <cassert>
#include <iostream>

static EspBox player(unsigned long long id, float x) {
    EspBox box{};
    box.id = id;
    box.x1 = x;
    box.has_skeleton = true;
    box.aim_source = 1;
    for (int i = 0; i < 3; ++i) {
        box.aim_valid[i] = true;
        box.aim_world_valid[i] = true;
        box.aim_yaw[i] = 4.0F;
        box.aim_world[i][0] = 8.0F;
    }
    return box;
}

static EspBox bot(unsigned long long id) {
    EspBox box{};
    box.id = id;
    box.is_bot = true;
    return box;
}

int main() {
    EspContinuity continuity;

    // A healthy pre-death frame becomes the visual snapshot; bots are never
    // cached because their own scanner maintains them independently.
    auto live = continuity.publish({player(11, 10.0F), player(22, 20.0F), bot(99)}, true, 100.0);
    assert(live.size() == 3 && continuity.has_snapshot());

    // PlayerManager disappears during death.  The old boxes remain visible,
    // while stale bones/world points can no longer be selected by the aimbot.
    auto death = continuity.publish({bot(100)}, false, 102.0);
    assert(death.size() == 3 && death[0].is_bot);
    assert(death[1].id == 11 && death[2].id == 22);
    for (size_t n = 1; n < death.size(); ++n) {
        assert(!death[n].is_bot);
        assert(death[n].aim_source == 0);
        for (int i = 0; i < 3; ++i) {
            assert(!death[n].aim_valid[i]);
            assert(!death[n].aim_world_valid[i]);
        }
    }

    // If the respawn frame already resolved players, never mix old screen
    // coordinates into it even before the local-player latch is healthy.
    auto partial = continuity.publish({player(33, 30.0F)}, false, 103.0);
    assert(partial.size() == 1 && partial[0].id == 33);

    // Once a healthy respawn frame arrives it atomically replaces the old set.
    auto respawn = continuity.publish({player(44, 40.0F)}, true, 104.0);
    assert(respawn.size() == 1 && respawn[0].id == 44);
    auto second_gap = continuity.publish({}, false, 105.0);
    assert(second_gap.size() == 1 && second_gap[0].id == 44);

    // A confirmed healthy empty population clears the snapshot.  A real long
    // disconnect also expires it rather than drawing ghosts forever.
    assert(continuity.publish({}, true, 106.0).empty());
    assert(continuity.publish({}, false, 107.0).empty());
    continuity.publish({player(55, 50.0F)}, true, 200.0);
    assert(continuity.publish({}, false, 200.0 + EspContinuity::kBridgeSeconds + 0.1).empty());

    continuity.clear();
    assert(!continuity.has_snapshot());
    std::cout << "esp lifecycle tests passed\n";
}
