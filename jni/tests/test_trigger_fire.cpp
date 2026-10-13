#include "trigger_fire.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>

using namespace triggerfire;

namespace {

constexpr float kFrame = 1.0f / 60.0f;

// Сколько кадров потребовалось машинке, чтобы взвеститься (0 — не взвелась
// и за limit). Считаем кадрами, а не «ceil задержки на кадр»: на 60 fps
// 0.1 с — это ровно 6 кадров, и именно это надо проверить, а не арифметику
// самого теста.
int frames_until_firing(Machine& m, const Machine::Config& cfg, int limit) {
    for (int i = 1; i <= limit; ++i)
        if (m.step(true, kFrame, cfg)) return i;
    return 0;
}

} // namespace

int main() {
    // --- задержку взвода отмеряет dwell, накопленный по кадрам ---
    {
        Machine m;
        Machine::Config cfg;
        cfg.arm_delay = 0.1f;
        cfg.release_grace = 0.0f;
        assert(frames_until_firing(m, cfg, 30) == 6);
        assert(m.firing());
        assert(m.activations() == 1);
    }

    // без задержки стреляет с первого кадра
    {
        Machine m;
        Machine::Config cfg;
        cfg.arm_delay = 0.0f;
        assert(m.step(true, kFrame, cfg));
    }

    // --- цель, пропавшая на несколько кадров, очередь не рвёт ---
    {
        Machine m;
        Machine::Config cfg;
        cfg.arm_delay = 0.0f;
        cfg.release_grace = 0.12f;
        assert(m.step(true, kFrame, cfg));
        for (int i = 0; i < 5; ++i) assert(m.step(false, kFrame, cfg));
        assert(m.firing());
        // дольше милосердия — отпускаем
        for (int i = 0; i < 8; ++i) m.step(false, kFrame, cfg);
        assert(!m.firing());
    }

    // --- off-кадр внутри милосердия не обнуляет dwell: цель, вернувшаяся
    //     на прицел, стреляет сразу, без повторной задержки ---
    {
        Machine m;
        Machine::Config cfg;
        cfg.arm_delay = 0.2f;
        cfg.release_grace = 0.12f;
        for (int i = 0; i < 11; ++i) assert(!m.step(true, kFrame, cfg));
        assert(!m.step(false, kFrame, cfg));       // один выпавший кадр
        assert(m.step(true, kFrame, cfg));          // 12-й кадр: 12/60 = 0.2 с
    }

    // --- сброс: ни зажатия, ни накопленного dwell ---
    {
        Machine m;
        Machine::Config cfg;
        cfg.arm_delay = 0.3f;
        cfg.release_grace = 0.12f;
        assert(frames_until_firing(m, cfg, 30) == 18);
        m.reset();
        assert(!m.firing());
        assert(m.dwell() == 0.0f);
        assert(m.activations() == 0);
        // после reset счётчик задержки начинается заново
        assert(!m.step(true, kFrame, cfg));
    }

    // --- второй захват цели — второе взведение (для диагностики) ---
    {
        Machine m;
        Machine::Config cfg;
        cfg.arm_delay = 0.0f;
        cfg.release_grace = 0.0f;
        assert(m.step(true, kFrame, cfg));
        while (m.firing()) m.step(false, kFrame, cfg);
        assert(m.step(true, kFrame, cfg));
        assert(m.activations() == 2);
    }

    // --- плохой dt и NaN: не разгоняют и не застревают ---
    {
        Machine m;
        Machine::Config cfg;
        cfg.arm_delay = 0.1f;
        cfg.release_grace = 0.12f;
        // dt=0 и NaN считаются одним обычным кадром, а не «проходом без изменений»
        assert(!m.step(true, 0.0f, cfg));
        assert(!m.step(true, std::numeric_limits<float>::quiet_NaN(), cfg));
        assert(!m.step(true, -1.0f, cfg));
        assert(m.dwell() > 0.0f && m.dwell() < 0.1f);
        // гигантский dt клампится: без клампа один пропущенный кадр (10 с)
        // сам взвёл бы триггер с задержкой в 0.25 с
        Machine big;
        Machine::Config lagged = cfg;
        lagged.arm_delay = 0.25f;
        assert(!big.step(true, 10.0f, lagged));
        assert(big.dwell() <= 0.1f + 1e-6f);
        // NaN в настройках = «без задержки», отрицательное милосердие = 0
        Machine huge_delay;
        Machine::Config broken;
        broken.arm_delay = std::numeric_limits<float>::quiet_NaN();
        broken.release_grace = -5.0f;
        assert(huge_delay.step(true, kFrame, broken));
    }

    // --- радиус в процентах короткой стороны ---
    assert(radius_px(1080.0f, 1.6f) > 16.0f && radius_px(1080.0f, 1.6f) < 19.0f);
    assert(radius_px(1080.0f, -3.0f) == 0.0f);
    assert(radius_px(0.0f, 5.0f) == 0.0f);
    assert(radius_px(1080.0f, std::numeric_limits<float>::quiet_NaN()) == 0.0f);

    // --- крестик в точке наведения ---
    assert(point_within(540.0f, 1200.0f, 545.0f, 1204.0f, 17.0f));
    assert(!point_within(540.0f, 1200.0f, 600.0f, 1200.0f, 17.0f));
    assert(!point_within(540.0f, 1200.0f, std::numeric_limits<float>::quiet_NaN(),
                         1200.0f, 17.0f));
    // нулевой радиус — только точное совпадение (то есть «по кости»)
    assert(point_within(540.0f, 1200.0f, 540.0f, 1200.0f, 0.0f));
    assert(!point_within(540.0f, 1200.0f, 541.0f, 1200.0f, 0.0f));

    // --- крестик в боксе (углы приходят в любом порядке) ---
    assert(rect_contains(540.0f, 1200.0f, 500.0f, 1100.0f, 580.0f, 1300.0f, 8.0f));
    assert(rect_contains(540.0f, 1095.0f, 500.0f, 1100.0f, 580.0f, 1300.0f, 8.0f));
    assert(!rect_contains(540.0f, 1090.0f, 500.0f, 1100.0f, 580.0f, 1300.0f, 8.0f));
    assert(rect_contains(540.0f, 1200.0f, 580.0f, 1300.0f, 500.0f, 1100.0f, 0.0f));
    assert(!rect_contains(540.0f, 1200.0f, std::numeric_limits<float>::infinity(),
                          1100.0f, 580.0f, 1300.0f, 8.0f));
    std::printf("test_trigger_fire: ok\n");
    return 0;
}
