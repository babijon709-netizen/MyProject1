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

// targetOff — угол цели от прицела (градусы), cam — угол камеры по ESP,
// base — текущая эффективная позиция накопителя (см. base()).
inline float error(float targetOff, float base, float cam) {
    return targetOff - wrap180(base - cam);
}

} // namespace mem_lag_comp
