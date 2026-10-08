#include "aim_visibility.hpp"

#include <cassert>
#include <cmath>
#include <limits>

int main() {
    using namespace manes::aim_visibility;

    assert(sensor_timestamps_show_line_of_sight(12.00f, 12.02f));
    assert(sensor_timestamps_show_line_of_sight(12.03f, 12.00f));
    assert(!sensor_timestamps_show_line_of_sight(12.00f, 12.06f));
    assert(!sensor_timestamps_show_line_of_sight(0.f, 0.f));
    assert(!sensor_timestamps_show_line_of_sight(
        std::numeric_limits<float>::quiet_NaN(), 12.f));
    assert(!sensor_timestamps_show_line_of_sight(
        12.f, std::numeric_limits<float>::infinity()));

    SensorFreshness state{};
    assert(!sensor_sample_is_current(state, 0x10000, 0x20000, 20.f, 20.f, 1.0));
    assert(state.initialized && !state.rect_advanced);
    assert(sensor_sample_is_current(state, 0x10000, 0x20000, 20.016f, 20.016f, 1.016));
    assert(sensor_sample_is_current(state, 0x10000, 0x20000, 20.016f, 20.016f, 1.045));
    assert(!sensor_sample_is_current(state, 0x10000, 0x20000, 20.016f, 20.016f, 1.067));
    assert(!sensor_sample_is_current(state, 0x10000, 0x20000, 20.016f, 20.016f, 1.21));

    assert(!sensor_sample_is_current(state, 0x10000, 0x20000, 21.f, 21.1f, 1.22));
    assert(sensor_sample_is_current(state, 0x10000, 0x20000, 21.12f, 21.12f, 1.23));

    assert(!sensor_sample_is_current(state, 0x30000, 0x40000, 30.f, 30.f, 1.24));
    assert(!sensor_sample_is_current(state, 0x30000, 0x40000, 30.f, 30.f, 1.25));
    assert(sensor_sample_is_current(state, 0x30000, 0x40000, 30.02f, 30.02f, 1.26));

    assert(!sensor_sample_is_current(state, 0x30000, 0x40000, 0.f, 0.f, 1.27));
    assert(!state.initialized);
    return 0;
}
