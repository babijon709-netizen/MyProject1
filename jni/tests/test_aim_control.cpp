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

    assert(target_within_range_window(15.0f, 10.0f));
    assert(!target_within_range_window(20.0f, 10.0f));
    assert(!target_within_range_window(80.0f, 10.0f));
    assert(!target_within_range_window(std::numeric_limits<float>::infinity(), 10.0f));
    const float smart_close = target_priority_score(0, 0.95f, 10.0f, true, false);
    const float smart_far = target_priority_score(0, 0.05f, 80.0f, true, true);
    assert(smart_close < smart_far);
    const float nearest_close = target_priority_score(2, 0.95f, 10.0f, true, false);
    const float nearest_far = target_priority_score(2, 0.05f, 80.0f, true, true);
    assert(nearest_close < nearest_far);
    assert(target_priority_score(2, 0.95f, 100.0f, true, false) <
           target_priority_score(2, 0.05f, 101.0f, true, false));
    const float crosshair_center = target_priority_score(1, 0.05f, 15.0f, true, false);
    const float crosshair_edge = target_priority_score(1, 0.80f, 10.0f, true, false);
    assert(crosshair_center < crosshair_edge);
    assert(target_priority_score(0, 0.50f, 20.0f, true, true) <
           target_priority_score(0, 0.48f, 20.0f, true, false));

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

    const float gentle = memory_step(30.0f, 1.0f / 60.0f, 1.0f, 30.0f);
    const float fast = memory_step(30.0f, 1.0f / 60.0f, 10.0f, 30.0f);
    const float close = memory_step(30.0f, 1.0f / 60.0f, 10.0f, 5.0f);
    assert(gentle > 0.0f && fast > gentle && close >= fast);
    assert(close <= 12.0f);
    assert(memory_step(-30.0f, 1.0f / 60.0f, 10.0f, 5.0f) < 0.0f);

    assert(memory_step(std::numeric_limits<float>::infinity(), 1.0f / 60.0f, 10.0f, 5.0f) == 0.0f);

    // ---- компенсация отдачи -------------------------------------------------
    const float tick = 1.0f / 12.0f;   // такт оверлея, а не игры
    RecoilControl rcs;
    assert(rcs.term() == 0.0f);

    // Кнопка отпущена — член нулевой, как бы долго ни держалась ошибка: без огня
    // возмущения нет, а заряд на пустом месте выкинул бы первые выстрелы.
    for (int i = 0; i < 60; ++i) rcs.update(1.0f, false, tick, 4.0f, 20.0f);
    assert(rcs.term() == 0.0f);

    // Огонь держится, прицел сел: остаток ошибки одного знака копит член того же
    // знака (команда подписана как ошибка, поэтому прибавляется без инверсии).
    for (int i = 0; i < 40; ++i) rcs.update(0.4f, true, tick, 4.0f, 20.0f);
    assert(rcs.term() > 0.0f);
    const float charged = rcs.term();
    rcs.update(-0.4f, true, tick, 4.0f, 20.0f);
    assert(rcs.term() < charged);

    // Вне полосы «прицел сел» заряд не растёт, только стекает: наведение на новую
    // цель не должно наматывать интеграл.
    rcs.reset();
    for (int i = 0; i < 40; ++i) rcs.update(0.4f, true, tick, 4.0f, 20.0f);
    const float before_acquisition = rcs.term();
    float during_acquisition = before_acquisition;
    for (int i = 0; i < 40; ++i)
        during_acquisition = rcs.update(9.0f, true, tick, 4.0f, 20.0f);
    assert(before_acquisition > 0.0f && during_acquisition < before_acquisition);

    // Насыщение команды не докручивает интеграл (анти-виндап).
    rcs.reset();
    float saturated = 0.0f;
    for (int i = 0; i < 40; ++i)
        saturated = rcs.update(0.4f, true, tick, 4.0f, 20.0f, true);
    assert(saturated == 0.0f);

    // Кламп держит член в пределах области, каким бы долгим ни был огонь.
    rcs.reset();
    float clamped = 0.0f;
    for (int i = 0; i < 2000; ++i) clamped = rcs.update(3.0f, true, tick, 4.0f, 1.5f);
    assert(clamped == 1.5f);
    rcs.reset();
    clamped = 0.0f;
    for (int i = 0; i < 2000; ++i) clamped = rcs.update(-3.0f, true, tick, 4.0f, 1.5f);
    assert(clamped == -1.5f);

    // Мусор на входе сбрасывает член, а не уносит его в бесконечность; degenerate
    // пределы не дают заряда.
    rcs.reset();
    for (int i = 0; i < 40; ++i) rcs.update(0.4f, true, tick, 4.0f, 20.0f);
    rcs.update(std::numeric_limits<float>::infinity(), true, tick, 4.0f, 20.0f);
    assert(rcs.term() == 0.0f);
    rcs.update(0.4f, true, tick, 4.0f, 0.0f);
    assert(rcs.term() == 0.0f);
    rcs.update(0.4f, true, tick, 0.0f, 20.0f);
    assert(rcs.term() == 0.0f);
    assert(rcs.update(std::numeric_limits<float>::quiet_NaN(), true, tick, 4.0f, 20.0f) == 0.0f);

    // Отпущенная кнопка стекает к нулю, а не держит заряд до следующей очереди.
    rcs.reset();
    for (int i = 0; i < 40; ++i) rcs.update(0.4f, true, tick, 4.0f, 20.0f);
    assert(rcs.term() > 0.1f);
    float bled = rcs.term();
    for (int i = 0; i < 120; ++i) bled = rcs.update(0.0f, false, tick, 4.0f, 20.0f);
    assert(bled < 1.0e-3f);

    // Замкнутая петля с постоянным возмущением — ради чего всё и добавлено.
    // Растение: за такт камера уходит на команду и поднимается отдачей на climb.
    // Пропорциональное звено оставляет статическую ошибку climb/kp; интеграл
    // снимает её на порядок. Подъём 0.4 град/так — середина реального диапазона.
    auto settle = [tick](bool with_integral) {
        RecoilControl loop;
        const float climb = 0.40f, kp = 0.70f;
        float camera = 0.0f;
        for (int i = 0; i < 600; ++i) {
            const float error = camera;   // кость стоит в нуле
            const float term = with_integral
                ? loop.update(error, true, tick, 4.0f, 20.0f) : loop.term();
            camera = camera - (kp * error + term) + climb;
        }
        return std::fabs(camera);
    };
    const float proportional_only = settle(false);
    const float with_integral = settle(true);
    assert(std::fabs(proportional_only - 0.40f / 0.70f) < 1.0e-3f);
    assert(with_integral > 0.0f);
    assert(with_integral < proportional_only * 0.15f);

    float invalid_yaw = std::numeric_limits<float>::quiet_NaN();
    float invalid_pitch = 0.0f;
    filter.update(invalid_yaw, invalid_pitch, 1.0f / 60.0f, 0.02f);
    assert(!filter.ready);
    return 0;
}
