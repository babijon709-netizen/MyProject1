#include "mem_aim_control.h"

#include <cassert>
#include <cmath>
#include <limits>

int main() {
    using namespace mem_aim_control;

    assert(std::fabs(wrap180(190.f) - (-170.f)) < 1e-4f);
    assert(std::fabs(wrap180(-190.f) - 170.f) < 1e-4f);
    assert(std::fabs(wrap180(45.f) - 45.f) < 1e-4f);

    // Фильтр: шум ±0.3° (размах 0.6°) вокруг 10° на выходе вдвое меньше.
    {
        AngleFilter f;
        const float dt = 1.f / 60.f, tau = 0.05f;
        float minY = 1e9f, maxY = -1e9f;
        for (int i = 0; i < 600; ++i) {
            const float noise = (i & 1) ? 0.3f : -0.3f;
            f.update(10.f + noise, 2.f - noise, dt, tau);
            if (i > 120) {
                if (f.yaw < minY) minY = f.yaw;
                if (f.yaw > maxY) maxY = f.yaw;
            }
        }
        assert(maxY - minY < 0.3f);
        assert(std::fabs(f.yaw - 10.f) < 0.05f);
        assert(std::fabs(f.pitch - 2.f) < 0.05f);
    }

    // Фильтр: при постоянной скорости цели отставание ~ v * dt * (1 - a) / a.
    {
        AngleFilter f;
        const float dt = 1.f / 60.f, tau = 0.05f, v = 30.f;
        const float a = 1.f - std::exp(-dt / tau);
        float t = 0.f, last = 0.f;
        for (int i = 0; i < 200; ++i) { last = t; f.update(t, 0.f, dt, tau); t += v * dt; }
        const float lag = last - f.yaw;
        const float expect = v * dt * (1.f - a) / a;
        assert(std::fabs(lag - expect) < 0.05f);
    }

    // Фильтр: переход через ±180 не даёт скачка на 360.
    {
        AngleFilter f;
        f.update(179.f, 0.f, 1.f / 60.f, 0.035f);
        f.update(-179.f, 0.f, 1.f / 60.f, 0.035f);
        assert(std::fabs(wrap180(f.yaw - 179.f)) < 2.f);
    }

    // Фильтр: нечисло сбрасывает состояние.
    {
        AngleFilter f;
        f.update(5.f, 5.f, 1.f / 60.f, 0.035f);
        f.update(std::numeric_limits<float>::quiet_NaN(), 0.f, 1.f / 60.f, 0.035f);
        assert(!f.have);
    }

    // Лаг: шаг +1 записан, камера ещё не сдвинулась — весь шаг в полёте.
    assert(std::fabs(pending_part(1.f, 0.f) - 1.f) < 1e-6f);
    // Камера отработала — в полёте ничего.
    assert(std::fabs(pending_part(1.f, 1.f)) < 1e-6f);
    // Частично — остаток.
    assert(std::fabs(pending_part(1.f, 0.4f) - 0.6f) < 1e-6f);
    // Камера ушла дальше шага в ту же сторону (чужой ввод) — лага нет.
    assert(std::fabs(pending_part(1.f, 3.f)) < 1e-6f);
    assert(std::fabs(pending_part(1.f, -1.f) - 1.f) < 1e-6f);
    // Знаки отрицательные: симметрия.
    assert(std::fabs(pending_part(-1.f, 0.f) + 1.f) < 1e-6f);
    assert(std::fabs(pending_part(-1.f, -0.25f) + 0.75f) < 1e-6f);
    // Камера ушла в обратную сторону (чужой ввод): лаг не больше самого шага.
    assert(std::fabs(pending_part(-1.f, 2.f) + 1.f) < 1e-6f);
    assert(std::fabs(pending_part(-1.f, -2.f)) < 1e-6f);
    assert(pending_part(0.f, 1.f) == 0.f);
    assert(pending_part(std::numeric_limits<float>::infinity(), 0.f) == 0.f);

    return 0;
}
