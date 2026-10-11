#include "aim_control.h"

#include <cassert>
#include <cmath>
#include <limits>

int main() {
    using namespace aim_control;

    const bool all_bones[] = {true, true, true};
    assert(configured_bone_slot(0, all_bones) == 0);
    assert(configured_bone_slot(1, all_bones) == 1);
    assert(configured_bone_slot(2, all_bones) == 2);
    const bool head_unavailable[] = {false, true, true};
    assert(configured_bone_slot(0, head_unavailable) == -1);
    assert(configured_bone_slot(1, head_unavailable) == 1);
    assert(configured_bone_slot(-1, all_bones) == -1);
    assert(configured_bone_slot(3, all_bones) == -1);

    // Мигнувшая кость: цель держится прошлой точкой прицела, а не отдаётся
    // другому игроку. Держится ограниченное число тактов — иначе умершего
    // или ушедшего из FOV игрока не отпустить.
    {
        BoneHold h;
        // Цель на месте, кость видна — держать нечего.
        assert(!h.update(true, false, true, kBoneHoldTicks));
        assert(h.ticks == 0);
        // Кость мигнула: четыре такта держим, на пятом отпускаем.
        for (int i = 0; i < kBoneHoldTicks; ++i)
            assert(h.update(false, true, true, kBoneHoldTicks));
        assert(!h.update(false, true, true, kBoneHoldTicks));
        assert(h.ticks == 0);
        // Кость вернулась — счётчик обнулён, и следующее мигание держится снова.
        assert(!h.update(true, false, true, kBoneHoldTicks));
        assert(h.update(false, true, true, kBoneHoldTicks));
        // Игрок пропал из списка (кость не мигала, а цели нет) — не держим.
        h.reset();
        assert(!h.update(false, false, true, kBoneHoldTicks));
        // Прошлой точки прицела нет — держать нечем.
        assert(!h.update(false, true, false, kBoneHoldTicks));
    }

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
    assert(touch_response_window(1.0f / 60.0f) == 0.55f);
    assert(touch_response_window(0.1f) == 0.75f);
    assert(touch_response_window(std::numeric_limits<float>::quiet_NaN()) == 0.55f);
    const float quantum_limited = touch_lock_deadband(0.06f, 100.0f, 0.04f);
    assert(std::fabs(quantum_limited - 0.02f) < 0.00001f);
    const float geometry_limited = touch_lock_deadband(0.06f, 100.0f, 0.0f);
    assert(geometry_limited > 0.0028f && geometry_limited < 0.003f);
    assert(touch_camera_response_acknowledged(0.012f, 0.006f));
    assert(!touch_camera_response_acknowledged(0.012f, -0.006f));
    assert(!touch_camera_response_acknowledged(0.04f, 0.015f));
    assert(touch_camera_response_acknowledged(0.04f, 0.02f));
    float accumulated_response = touch_accumulate_camera_response(0.0f, 0.004f, 0.012f);
    assert(std::fabs(accumulated_response - 0.004f) < 0.00001f);
    assert(!touch_camera_response_acknowledged(0.012f, accumulated_response));
    accumulated_response = touch_accumulate_camera_response(accumulated_response, 0.003f, 0.012f);
    assert(touch_camera_response_acknowledged(0.012f, accumulated_response));
    assert(touch_accumulate_camera_response(accumulated_response, -0.003f, 0.012f) == 0.0f);
    accumulated_response = touch_accumulate_camera_response(0.004f, -0.003f, -0.012f);
    assert(std::fabs(accumulated_response + 0.003f) < 0.00001f);
    int jitter_streak = 0;
    assert(touch_jitter_gate(0.3f, jitter_streak, 0.45f, 2) == 0.0f);
    assert(jitter_streak == 1);
    assert(touch_jitter_gate(0.3f, jitter_streak, 0.45f, 2) == 0.3f);
    assert(jitter_streak == 2);
    assert(touch_jitter_gate(-0.3f, jitter_streak, 0.45f, 2) == 0.0f);
    assert(jitter_streak == -1);
    assert(touch_jitter_gate(0.5f, jitter_streak, 0.45f, 2) == 0.5f);
    assert(jitter_streak == 0);
    assert(touch_jitter_gate(0.0f, jitter_streak, 0.45f, 2) == 0.0f);
    assert(jitter_streak == 0);
    assert(memory_step(0.02f, 1.0f / 60.0f, 10.0f, 5.0f) == 0.0f);
    assert(memory_step(0.04f, 1.0f / 60.0f, 10.0f, 5.0f) == 0.0f);
    assert(memory_step(0.08f, 1.0f / 60.0f, 10.0f, 5.0f) > 0.0f);

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
