#include "fake_lag.h"

#include <cstdio>

int main() {
    if (fake_lag::clamp_percent(-1) != 1) return 1;
    if (fake_lag::clamp_percent(1) != 1) return 1;
    if (fake_lag::clamp_percent(45) != 45) return 1;
    if (fake_lag::clamp_percent(90) != 90) return 1;
    if (fake_lag::clamp_percent(100) != 100) return 1;
    if (fake_lag::clamp_percent(101) != 100) return 1;
    if (fake_lag::blocked_ms(1) != 10) return 1;
    if (fake_lag::blocked_ms(45) != 450) return 1;
    if (fake_lag::blocked_ms(100) != 1000) return 1;
    if (fake_lag::unblocked_ms(45) != 550) return 1;
    if (fake_lag::blocked_ms(45) + fake_lag::unblocked_ms(45) != 1000) return 1;
    std::puts("ALL FAKE-LAG TESTS PASS");
    return 0;
}
