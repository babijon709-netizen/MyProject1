#pragma once
// Компенсация задержки записи в накопитель MouseLook (мемори-аим).
// Запись такта N игра применяет не раньше такта N+1, поэтому чтение камеры
// на следующем кадре ещё не содержит нашего шага. Ошибка, посчитанная от
// старой камеры, повторно учитывает уже записанный шаг -> перелёт и дрожь.
// Пока запись не подтверждена и свежа, базой служит записанное значение.
#include <cmath>

namespace mem_lag_comp {

constexpr int kPendingFrames = 2;

inline float wrap180(float d) {
    if (!std::isfinite(d)) return 0.f;
    d = std::fmod(d + 180.f, 360.f);
    if (d < 0.f) d += 360.f;
    return d - 180.f;
}

inline bool pending(bool haveWrite, bool consumed, int ageFrames) {
    return haveWrite && !consumed && ageFrames <= kPendingFrames;
}

// acc — накопитель по чтению, lastWrite — наша последняя запись по той же оси.
inline float base(float acc, float lastWrite, bool pend) {
    return pend ? lastWrite : acc;
}

// target — угол цели от прицела в градусах (тот же знак, что у best.yaw/pitch).
// lastStep — шаг, который мы записали на прошлом такте, в тех же единицах.
// Он ещё не виден в камере, поэтому вычитаем его из ошибки. Знак накопителя
// здесь не нужен: шаг берётся в градусах прямо из mem_axis_step.
inline float error(float target, float lastStep, bool pend) {
    if (!pend || !std::isfinite(lastStep)) return target;
    return target - lastStep;
}

} // namespace mem_lag_comp
