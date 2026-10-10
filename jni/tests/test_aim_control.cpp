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

    float command_yaw = 0.0f, command_pitch = 0.0f;
    memory_command_angles(10.0f, 20.0f, true, 30.0f, 4.0f, 1.0f, 2.0f,
                          command_yaw, command_pitch);
    assert(command_yaw == 31.0f && command_pitch == -6.0f);
    const float first_command_yaw = command_yaw;
    const float first_command_pitch = command_pitch;
    memory_command_angles(-100.0f, 80.0f, true, 30.0f, 4.0f, 1.0f, 2.0f,
                          command_yaw, command_pitch);
    assert(command_yaw == first_command_yaw && command_pitch == first_command_pitch);
    memory_command_angles(10.0f, 20.0f, false, 0.0f, 0.0f, 1.0f, 2.0f,
                          command_yaw, command_pitch);
    assert(command_yaw == 11.0f && command_pitch == 18.0f);
    assert(memory_step(0.02f, 1.0f / 60.0f, 10.0f, 5.0f) == 0.0f);
    assert(memory_step(0.04f, 1.0f / 60.0f, 10.0f, 5.0f) == 0.0f);
    assert(memory_step(0.08f, 1.0f / 60.0f, 10.0f, 5.0f) > 0.0f);

    float flow_yaw = 0.0f, flow_pitch = 0.0f;
    assert(camera_flow_delta(-179.0f, 2.0f, 179.0f, 1.0f, flow_yaw, flow_pitch));
    assert(std::fabs(flow_yaw - 2.0f) < 0.001f && flow_pitch == 1.0f);
    assert(!camera_flow_delta(90.0f, 0.0f, 0.0f, 0.0f, flow_yaw, flow_pitch));
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
