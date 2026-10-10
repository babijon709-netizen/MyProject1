#include "aim_control.h"

#include <cassert>
#include <cmath>
#include <limits>

int main() {
    using namespace aim_control;

    AngleFilter filter;
    float yaw = 179.0f, pitch = 0.0f;
    filter.update(yaw, pitch, 1.0f / 60.0f, 0.02f);
    assert(yaw == 179.0f);
    yaw = -179.0f;
    filter.update(yaw, pitch, 1.0f / 60.0f, 0.02f);
    assert(std::fabs(wrap_degrees(yaw - 179.0f)) < 2.0f);
    assert(wrap_degrees(std::numeric_limits<float>::infinity()) == 0.0f);
    assert(wrap_degrees(1.0e30f) == 0.0f);

    filter.reset();
    yaw = 0.0f; pitch = 0.0f;
    filter.update(yaw, pitch, 1.0f / 60.0f, 0.03f);
    float total_noise = 0.0f;
    for (int i = 0; i < 8; ++i) {
        yaw = (i & 1) ? -0.2f : 0.2f;
        pitch = (i & 1) ? 0.15f : -0.15f;
        filter.update(yaw, pitch, 1.0f / 60.0f, 0.03f);
        total_noise += std::fabs(yaw) + std::fabs(pitch);
    }
    assert(total_noise < 2.0f);

    assert(memory_step(0.02f, 1.0f / 60.0f, 10.0f, 5.0f) == 0.0f);
    const float gentle = memory_step(30.0f, 1.0f / 60.0f, 1.0f, 30.0f);
    const float fast = memory_step(30.0f, 1.0f / 60.0f, 10.0f, 30.0f);
    const float close = memory_step(30.0f, 1.0f / 60.0f, 10.0f, 5.0f);
    assert(gentle > 0.0f && fast > gentle && close >= fast);
    assert(close <= 12.0f);
    assert(memory_step(-30.0f, 1.0f / 60.0f, 10.0f, 5.0f) < 0.0f);

    const float limit = touch_step_limit_pixels(2400.0f, 0.1f, 8.0f);
    assert(limit == 80.0f);
    assert(touch_step_limit_pixels(2400.0f, 0.0f, 8.0f) == 192.0f);
    assert(touch_step_limit_pixels(std::numeric_limits<float>::infinity(), 0.1f, 8.0f) == 0.0f);
    assert(memory_step(std::numeric_limits<float>::infinity(), 1.0f / 60.0f, 10.0f, 5.0f) == 0.0f);
    float invalid_yaw = std::numeric_limits<float>::quiet_NaN();
    float invalid_pitch = 0.0f;
    filter.update(invalid_yaw, invalid_pitch, 1.0f / 60.0f, 0.02f);
    assert(!filter.ready);
    return 0;
}
