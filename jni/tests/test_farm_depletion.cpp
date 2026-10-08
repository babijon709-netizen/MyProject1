#include "farm_depletion.h"
#include <cassert>
#include <cstdio>
#include <limits>

int main() {
    using farm_depletion::fraction_confirmed;
    using farm_depletion::health_terminal;

    assert(health_terminal(0.0f, 100.0f));
    assert(health_terminal(2.0f, 100.0f));
    assert(!health_terminal(2.01f, 100.0f));
    assert(health_terminal(0.0f, 0.0f));
    assert(!health_terminal(1.0f, 0.0f));
    assert(!health_terminal(-1.0f, 100.0f));
    assert(!health_terminal(std::numeric_limits<float>::quiet_NaN(), 100.0f));

    int frames = 0;
    for (int i = 0; i < 9; ++i)
        assert(!fraction_confirmed(0.02f, frames, 0.03f, 10));
    assert(frames == 9);
    assert(!fraction_confirmed(0.03f, frames, 0.03f, 10));
    assert(frames == 0);

    assert(!fraction_confirmed(0.02f, frames, 0.03f, 10));
    assert(!fraction_confirmed(-1.0f, frames, 0.03f, 10));
    assert(frames == 0);
    for (int i = 0; i < 9; ++i)
        assert(!fraction_confirmed(0.02f, frames, 0.03f, 10));
    assert(fraction_confirmed(0.02f, frames, 0.03f, 10));
    assert(frames == 10);

    std::puts("farm_depletion: terminal health and fraction debounce ok");
    return 0;
}
