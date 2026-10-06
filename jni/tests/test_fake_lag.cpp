#include "fake_lag.h"

#include <cstdio>

int main() {
    if (fake_lag::clamp_percent(-1) != 10) return 1;
    if (fake_lag::clamp_percent(10) != 10) return 1;
    if (fake_lag::clamp_percent(45) != 45) return 1;
    if (fake_lag::clamp_percent(85) != 85) return 1;
    if (fake_lag::clamp_percent(100) != 85) return 1;
    std::puts("ALL FAKE-LAG TESTS PASS");
    return 0;
}
