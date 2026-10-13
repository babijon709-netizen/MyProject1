#pragma once
// Триггер («автоогонь при наведении»): чистая машина времени, без памяти игры и
// без ImGui — прогоняется хост-тестом (jni/tests/test_trigger_fire.cpp).
//
// Вход — «прицел на противнике» каждый кадр, выход — держать ли кнопку атаки.
//
// Почему «зажать», а не «один тап на одно наведение». Кнопка атаки в этой игре
// — защёлка (PlayerInputHandler+0x68, см. farm_joy.h): пока она зажата, игра
// САМА зовёт TryAttackContinuously() каждый кадр, а темп режет кулдаун ствола.
// Значит «зажато, пока цель в прицеле» — это штатная длинная очередь:
// полуавтомат стреляет часто, как умеет он, автомат — как у него задано. Своей
// тайминговой самодеятельности (своих интервалов между «тапами») тут быть не
// должно: они только ломают родной темп и дают очередь быстрее, чем позволяет
// оружие.
//
// Две защиты:
//   * arm_delay — цель обязана продержаться на прицеле N мс подряд. Иначе
//     выстрел уходит на каждом кадре, где противник только мелькнул из-за угла;
//   * release_grace — столько же разрешается НЕ видеть цель, чтобы не рвать
//     очередь из-за одного кадра без бокса. Битый кадр ESP — обычное дело
//     (esp_box_hold.h держит записи ровно поэтому), и без_grace триггер
//     щёлкал бы зажатием на каждом таком кадре.
//
// Проверки «простреливается ли» здесь нет намеренно: решение принимает луч
// игры, и экранное попадание в бокс — ровно то же условие, что видит игрок.
// Стрельба через стену здесь стоит патрон, а не палит в противника.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace triggerfire {

// Как в aim_control.h: сборка Android идёт с -ffast-math, и под ним
// std::isfinite оптимизируется в «всегда true». Проверка по экспоненте.
inline bool finite(float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return (bits & 0x7f800000u) != 0x7f800000u;
}

class Machine {
public:
    struct Config {
        float arm_delay = 0.0f;      // сек: сколько цель обязана висеть на прицеле
        float release_grace = 0.12f; // сек: сколько терпим пропажу цели
    };

    // dt — секунды кадра. true — кнопку держать зажатой этот кадр.
    bool step(bool engaged, float dt, const Config& cfg) {
        float d = dt;
        if (!finite(d) || d <= 0.0f) d = 1.0f / 60.0f;
        if (d > 0.1f) d = 0.1f;   // длинный кадр не должен отмерять задержку втрое
        const float delay = finite(cfg.arm_delay)
            ? std::max(0.0f, std::min(cfg.arm_delay, 1.0f)) : 0.0f;
        const float grace = finite(cfg.release_grace)
            ? std::max(0.0f, std::min(cfg.release_grace, 0.5f)) : 0.0f;

        on_target_ = engaged;
        if (engaged) {
            lost_ = 0.0f;
            dwell_ += d;
            if (!firing_ && dwell_ >= delay) {
                firing_ = true;
                ++activations_;
            }
        } else {
            lost_ += d;
            if (lost_ > grace) {
                firing_ = false;
                dwell_ = 0.0f;   // цель потеряна всерьёз: задержка считается заново
            }
            // В пределах милосердия dwell НЕ обнуляем: выпавший кадр ESP — это
            // не «появилась новая цель», и каждый такой кадр иначе отбрасывал
            // бы задержку взвода назад (цель, мигающая на кромке отсева, вообще
            // бы не взвелась).
        }
        return firing_;
    }

    bool firing() const { return firing_; }
    bool on_target() const { return on_target_; }
    float dwell() const { return dwell_; }
    // Сколько раз триггер ВЗВОДИЛСЯ (не сколько пуль — их считает темп ствола).
    unsigned long activations() const { return activations_; }
    void reset() { *this = Machine{}; }

private:
    bool firing_ = false;
    bool on_target_ = false;
    float dwell_ = 0.0f;
    float lost_ = 0.0f;
    unsigned long activations_ = 0;
};

// Радиусы считаются в процентах короткой стороны, а не в пикселях: телефон
// 1080x2400 и планшет 2560x1600 обязаны вести себя одинаково.
inline float radius_px(float short_side, float percent) {
    if (!finite(short_side) || !finite(percent)) return 0.0f;
    if (short_side <= 0.0f) return 0.0f;
    if (percent < 0.0f) percent = 0.0f;
    return short_side * 0.01f * percent;
}

// Крестик попал в точку наведения (кость) — круг радиуса radius_px.
inline bool point_within(float cx, float cy, float px, float py, float radius_px_) {
    if (!finite(cx) || !finite(cy) || !finite(px) || !finite(py)) return false;
    const float dx = cx - px, dy = cy - py;
    const float r = finite(radius_px_) && radius_px_ > 0.0f ? radius_px_ : 0.0f;
    return dx * dx + dy * dy <= r * r;
}

// Крестик внутри бокса, раздутого на pad. Плохой прямоугольник (не-конечные
// углы, вывернутый) — false, а не «попал»: пропуск выстрела лучше выстрела
// в никуда по мусору чтения.
inline bool rect_contains(float cx, float cy, float x1, float y1, float x2, float y2,
                          float pad) {
    if (!finite(cx) || !finite(cy) || !finite(x1) || !finite(y1) ||
        !finite(x2) || !finite(y2)) return false;
    if (x2 < x1) std::swap(x1, x2);
    if (y2 < y1) std::swap(y1, y2);
    const float p = finite(pad) && pad > 0.0f ? pad : 0.0f;
    return cx >= x1 - p && cx <= x2 + p && cy >= y1 - p && cy <= y2 + p;
}

} // namespace triggerfire
