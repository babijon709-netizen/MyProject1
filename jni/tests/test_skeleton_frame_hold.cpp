#include "skeleton_frame_hold.h"

#include <cstdio>

namespace {
int failures = 0;
#define CHECK(cond, name) do { \
    if (!(cond)) { ++failures; std::printf("FAIL: %s (line %d)\n", name, __LINE__); } \
} while (0)
}  // namespace

int main() {
    CHECK(skeletonframehold::kMaxWorldAge == 30, "world hold window is bounded at 30 frames");
    CHECK(skeletonframehold::kRebuildAfterFailures == 90, "rig rebuild needs sustained failure");

    std::uint8_t age = 0;
    CHECK(!skeletonframehold::usable(age), "empty bone is not usable");
    CHECK(!skeletonframehold::reuse(age), "empty bone cannot be reused");

    skeletonframehold::refresh(age);
    CHECK(age == 1, "fresh read resets age");
    CHECK(skeletonframehold::usable(age), "fresh bone is usable");

    for (std::uint8_t expected = 2; expected <= skeletonframehold::kMaxWorldAge; ++expected) {
        CHECK(skeletonframehold::reuse(age), "cached world point holds through window");
        CHECK(age == expected, "reuse increments age once");
        CHECK(skeletonframehold::usable(age), "held world point remains usable");
    }
    CHECK(!skeletonframehold::reuse(age), "world point expires at bounded age");

    skeletonframehold::refresh(age);
    CHECK(age == 1, "fresh sample renews exhausted hold");

    if (failures) {
        std::printf("%d FAILURES\n", failures);
        return 1;
    }
    std::puts("ALL SKELETON-FRAME-HOLD TESTS PASS");
    return 0;
}
