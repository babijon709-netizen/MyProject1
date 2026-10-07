#include "camera_local_guard.h"

#include <cstdio>
#include <limits>

int main() {
    unsigned streak = 0;
    if (camera_local_guard::confirm_mismatch(true, streak)) return 1;
    if (!camera_local_guard::confirm_mismatch(true, streak)) return 1;
    if (camera_local_guard::confirm_mismatch(false, streak) || streak != 0) return 1;

    if (!camera_local_guard::within(10.f, 20.f, 30.f, 10.f, 21.6f, 30.f, 50.f)) return 1;
    if (camera_local_guard::within(0.f, 20.f, 0.f, 100.f, 20.f, 0.f, 50.f)) return 1;
    if (camera_local_guard::within(std::numeric_limits<float>::infinity(), 0.f, 0.f,
                                   0.f, 0.f, 0.f, 50.f)) return 1;
    if (camera_local_guard::within(0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f)) return 1;
    std::puts("ALL CAMERA-LOCAL GUARD TESTS PASS");
    return 0;
}
