#pragma once

#include <cmath>

// Контур мемори-аима: сглаживание абсолютного угла цели и компенсация лага
// камеры. Чистые функции, без состояния игры — проверяются на хосте.
namespace mem_aim_control {

inline float wrap180(float v) {
    if (!std::isfinite(v)) return v;
    float r = std::fmod(v + 180.f, 360.f);
    if (r < 0.f) r += 360.f;
    return r - 180.f;
}

// Экспоненциальное сглаживание угла с постоянной времени tau (секунды).
// Яв — абсолютный угол камеры + смещение цели, поэтому фильтр не тянет за
// собой движение камеры, как бы ни ходил прицел. Рысканье — по кругу.
struct AngleFilter {
    float yaw = 0.f, pitch = 0.f;
    bool  have = false;

    void reset() { have = false; }

    void update(float yawIn, float pitchIn, float dt, float tau) {
        if (!std::isfinite(yawIn) || !std::isfinite(pitchIn)) { reset(); return; }
        if (!have) { yaw = wrap180(yawIn); pitch = pitchIn; have = true; return; }
        float alpha = 1.f;
        if (dt > 0.f && tau > 0.f) alpha = 1.f - std::exp(-dt / tau);
        if (alpha > 1.f) alpha = 1.f;
        yaw   = wrap180(yaw   + wrap180(yawIn - yaw)   * alpha);
        pitch = pitch + (pitchIn - pitch) * alpha;
    }
};

// Доля последнего шага, ещё не видимая в камере (в тех же знаках, что шаг).
// applied — что записали в прошлом такте, observed — фактическое изменение
// камеры за такт. Если камера ушла дальше шага, это чужой ввод игрока, и
// лаг не выводим: возвращаем 0 или сам шаг, но не больше него по модулю.
inline float pending_part(float applied, float observed) {
    if (!std::isfinite(applied) || !std::isfinite(observed)) return 0.f;
    if (applied == 0.f) return 0.f;
    float p = applied - observed;
    if (applied > 0.f) {
        if (p < 0.f) p = 0.f;
        if (p > applied) p = applied;
    } else {
        if (p > 0.f) p = 0.f;
        if (p < applied) p = applied;
    }
    return p;
}

// Error-diffusion quantiser for the touch finger. The finger moves in whole
// device units; a sub-unit remainder is carried into the next tick instead of
// being rounded away. Plain rounding turns a 0.3-unit/tick wish into a stall
// followed by a 1-unit jump, which is the choppy look.
inline float quantize_units(float wantUnits, float& carry) {
    if (!std::isfinite(wantUnits) || !std::isfinite(carry)) { carry = 0.f; return 0.f; }
    const float total = wantUnits + carry;
    const float whole = std::round(total);
    carry = total - whole;
    if (carry >  0.5f) carry =  0.5f;
    if (carry < -0.5f) carry = -0.5f;
    return whole;
}

}  // namespace mem_aim_control
