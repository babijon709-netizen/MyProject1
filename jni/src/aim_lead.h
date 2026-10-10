#pragma once
// Коэффициент упреждения тач-аима. Вынесен из aim.cpp, чтобы хост-тест
// (jni/tests/test_aim_lead.cpp) проверял ту же формулу, что и устройство.
#include <cmath>

namespace aimlead {

// Доля шага упреждения, когда цель разворачивается: сглаженная скорость
// и сырой прирост за такт смотрят в разные стороны (dot < 0). Тогда
// упреждение по старому курсу уводит прицел за цель; режем его вдвое.
constexpr float kReversalScale = 0.5f;
constexpr float kMaxLeadFrames = 1.5f;

// baseK   — kAimVelocityLead
// leadMin — порог «реального» движения (2 град/px)
// vMag    — модуль сглаженной угловой скорости, град/такт
// rawDot  — скалярное произведение сырого прироста на сглаженную скорость
inline float LeadFrames(float baseK, float leadMin, float vMag, float rawDot) {
    if (!(vMag > leadMin) || !std::isfinite(vMag)) return 0.f;
    float k = baseK * (1.f - leadMin / vMag);
    if (rawDot < 0.f) k *= kReversalScale;
    if (k > kMaxLeadFrames) k = kMaxLeadFrames;
    return k;
}

} // namespace aimlead
