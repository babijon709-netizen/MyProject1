// Юнит-тесты контроллера тач-аима (jni/src/aim_touch.h).
//
// Проверяют отдельные куски математики и замкнутую петлю на модели трубы ввода.
// Полный A/B старой и новой ветки по 11 сценариям живёт в стенде
// tools/aim/touch_sim.cpp (запуск: scripts/aim_sim.sh); здесь — то, что должно
// держаться независимо от сценария.
#include "aim_touch.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace aim_touch;

// ---- растение: касание применяется через pipe_frames кадров игры ------------
struct Plant {
    float camYaw = 0.0f, camPitch = 0.0f;
    float tgtYaw = 30.0f, tgtPitch = 6.0f;
    float gain = 0.10f;
    int pipeFrames = 4;
    float gameFps = 15.0f;
    struct Packet { float dx, dy; float at; };
    std::vector<Packet> q;
    float queuedYaw = 0.0f, queuedPitch = 0.0f;
    int pipeStage = 0;
    float acc = 0.0f;
    float now = 0.0f;
    float appliedYaw = 0.0f, appliedPitch = 0.0f;

    // Заказ такта: пакеты приходят в трубу не одновременно, а размазаны по
    // длительности такта (на устройстве split_substeps шлёт их отдельными
    // записями uinput подряд).
    void order(float dt, float dx, float dy, int parts) {
        if (parts < 1) parts = 1;
        for (int i = 0; i < parts; ++i) {
            const float x = dx * (float)(i + 1) / (float)parts;
            const float y = dy * (float)(i + 1) / (float)parts;
            const float px = i ? dx * (float)i / (float)parts : 0.0f;
            const float py = i ? dy * (float)i / (float)parts : 0.0f;
            q.push_back({x - px, y - py, now + dt * ((float)i + 0.5f) / (float)parts});
        }
    }

    // Один такт оверлея: игра прокручивает свои кадры и применяет доехавшее.
    void step(float dt) {
        now += dt;
        appliedYaw = appliedPitch = 0.0f;
        acc += dt;
        while (acc >= 1.0f / gameFps) {
            acc -= 1.0f / gameFps;
            for (size_t i = 0; i < q.size();) {
                if (q[i].at <= now) { queuedYaw += q[i].dx; queuedPitch += q[i].dy; q.erase(q.begin() + i); }
                else ++i;
            }
            if (++pipeStage >= pipeFrames) {
                pipeStage = 0;
                // Знаки те же, что в стенде: тангаж у игры «вверх — больше»,
                // поэтому палец вверх (dy < 0) уменьшает остаток тангажа.
                camYaw += queuedYaw * gain;
                camPitch += -queuedPitch * gain;
                appliedYaw += queuedYaw * gain;
                appliedPitch += -queuedPitch * gain;
                queuedYaw = queuedPitch = 0.0f;
            }
        }
    }
    float rawYaw() const { return tgtYaw - camYaw; }
    float rawPitch() const { return tgtPitch - camPitch; }
};

static void test_envelope() {
    assert(soft_envelope(0.0f, 0.2f) == 0.0f);
    assert(soft_envelope(1.0f, 0.2f) == 1.0f);
    assert(soft_envelope(-1.0f, 0.2f) == 0.0f);
    assert(std::fabs(soft_envelope(0.1f, 0.2f) - 0.5f) < 1.0e-5f);
    // Нулевая производная на старте: первая четверть разгона даёт меньше
    // десятой доли — это и есть мягкий старт вместо линейного.
    assert(soft_envelope(0.05f, 0.2f) < 0.16f);
    float prev = -1.0f;
    for (int i = 0; i <= 40; ++i) {
        const float v = soft_envelope(0.2f * (float)i / 40.0f, 0.2f);
        assert(v >= prev);
        prev = v;
    }
    // Время разгона по остатку: дальний захват — полный разгон, довод — короткий.
    assert(soft_envelope_time(30.0f, 60.0f) > 0.20f);
    assert(soft_envelope_time(0.5f, 60.0f) < 0.06f);
    assert(soft_envelope_time(1.0f / 0.0f, 60.0f) >= 0.0f);   // не падает на мусоре
    std::printf("envelope ok\n");
}

static void test_taper() {
    int streak = 0;
    // Дрожь: знак чередуется, streak не набирается, команда не выдаётся.
    for (int i = 0; i < 20; ++i) {
        const float e = (i & 1) ? 0.2f : -0.2f;
        assert(jitter_taper(e, streak, 0.45f, 2, 4) == 0.0f);
    }
    assert(streak == -1 || streak == 1);
    // Дрейф в одну сторону: непрерывно, без скачка с нуля на единицу.
    streak = 0;
    float prev = -1.0f;
    for (int i = 0; i < 3; ++i) {
        const float v = jitter_taper(0.2f, streak, 0.45f, 2, 4);
        assert(v >= prev);
        prev = v;
    }
    assert(prev > 0.3f && prev < 1.0f);
    // Отпуск: устойчивое однонаправленное ведение возвращает полную долю,
    // иначе команда у цели навсегда осталась бы уменьшенной.
    for (int i = 0; i < 8; ++i) assert(jitter_taper(0.2f, streak, 0.45f, 2, 4) == 1.0f);
    // Вне полосы — сразу единица.
    streak = 0;
    assert(jitter_taper(1.0f, streak, 0.45f, 2, 4) == 1.0f);
    assert(streak == 0);
    std::printf("taper ok\n");
}

static void test_slew() {
    SoftSlew s;
    // Первый такт не может выдать больше accel*dt.
    const float out = s.apply(10.0f, 0.1f, 20.0f);
    assert(std::fabs(out - 2.0f) < 1.0e-4f);
    assert(std::fabs(s.apply(10.0f, 0.1f, 20.0f) - 4.0f) < 1.0e-4f);
    // Доезжает до цели и стоит.
    for (int i = 0; i < 10; ++i) s.apply(10.0f, 0.1f, 20.0f);
    assert(std::fabs(s.apply(10.0f, 0.1f, 20.0f) - 10.0f) < 1.0e-4f);
    // Реверс начинается от нуля, а не от прежней скорости: иначе на смене цели
    // палец сначала доехал бы в старую сторону.
    assert(s.apply(-10.0f, 0.1f, 20.0f) == -2.0f);
    // Мусор на входе не ломает состояние.
    s.reset();
    assert(s.apply(0.0f / 0.0f, 0.1f, 20.0f) == 0.0f);
    std::printf("slew ok\n");
}

static void test_substeps() {
    float x[4], y[4];
    assert(split_substeps(3.0f, 0.0f, x, y, 4) == 1);
    assert(x[0] == 3.0f);
    assert(split_substeps(0.0f, 0.0f, x, y, 4) == 0);
    const int n = split_substeps(43.0f, -12.0f, x, y, 4);
    assert(n == 4);
    // Позиции накопленные, монотонные, последняя — полная команда.
    for (int i = 1; i < n; ++i) assert(x[i] > x[i - 1] && y[i] < y[i - 1]);
    assert(std::fabs(x[n - 1] - 43.0f) < 1.0e-3f);
    assert(std::fabs(y[n - 1] + 12.0f) < 1.0e-3f);
    assert(split_substeps(1.0f, 1.0f, nullptr, y, 4) == 0);
    std::printf("substeps ok\n");
}

static void test_delay_and_inflight() {
    Flight f;
    const float dt = 1.0f / 12.0f;
    // Три такта заказов, отклик на четвёртом: задержка — возраст головы очереди.
    for (int i = 0; i < 3; ++i) {
        f.update(dt, 10.0f, 0.0f, true, 0.0f, 0.0f);
        f.age(dt, 0.6f);
        f.place(1.0f, 0.0f, 1.0f);
    }
    f.update(dt, 10.0f - 1.0f, 0.0f, true, 1.0f, 0.0f);
    assert(f.delay > 0.1f && f.delay < 0.4f);
    assert(std::fabs(f.absorbYaw - 2.0f) < 1.0e-4f);   // 3 заказано, 1 отработано

    // Летящий заказ — это приказы моложе задержки, а не всё окно.
    Flight g;
    for (int i = 0; i < 10; ++i) { g.age(dt, 0.6f); g.place(1.0f, 0.0f, 1.0f); }
    const float infl = g.inflightMag(3.0f * dt);
    assert(infl > 2.5f && infl < 3.5f);

    // Насыщение: учёт полёта не имеет права поменять знак остатка.
    float eYaw = 1.0f, ePitch = -1.0f;
    Flight h;
    h.flowOk = true;
    h.age(dt, 0.6f);
    h.place(5.0f, -5.0f, 1.0f);
    h.subtract(eYaw, ePitch, h.delay > 0.0f ? h.delay : 0.6f);
    assert(eYaw == 0.0f && ePitch == 0.0f);
    std::printf("delay/inflight ok\n");
}

static void test_closed_loop() {
    // Захват 30° на трубе 4 кадра игры: расходится, перелёт мал, реверсов нет.
    for (int softFlag = 0; softFlag <= 1; ++softFlag) {
        Controller c;
        c.reset();
        Plant pl;
        Params p;
        p.soft = (softFlag != 0);
        p.substeps = softFlag ? 4 : 1;
        p.have_camera_axis = false;   // штатный режим устройства: настоящей оси нет
        p.estimate_only = true;       // коэффициент — из настройки чувствительности
        const float dt = 1.0f / 12.0f;
        float pendX = 0.0f, pendY = 0.0f, pendT = 0.0f;
        float peak = 0.0f;
        int reversals = 0;
        float lastCmd = 0.0f;
        bool haveCmd = false;
        int settled = -1;
        for (int i = 0; i < 60; ++i) {
            pl.step(dt);
            const Output o = c.tick(p, 0.0f, 0.0f, dt, pl.rawYaw(), pl.rawPitch(),
                                    std::fabs(pl.appliedYaw) > 0.02f, -pl.appliedYaw, -pl.appliedPitch,
                                    false, 0.0f, 0.0f, pendX, pendY, pendT);
            if (o.action == kMove) {
                pl.order(dt, o.dx, o.dy, o.parts);
                const float cmd = o.dx * p.base_gain;
                if (haveCmd && cmd != 0.0f && lastCmd != 0.0f && (cmd > 0.0f) != (lastCmd > 0.0f))
                    ++reversals;
                if (cmd != 0.0f) { lastCmd = cmd; haveCmd = true; }
            }
            const float raw = std::fabs(pl.rawYaw());
            if (settled < 0 && raw < 1.0f) settled = i;
            if (settled >= 0 && raw > peak) peak = raw;
        }
        assert(settled >= 0);                 // цель захвачена
        assert(peak < 1.0f);                  // перелёт меньше градуса
        assert(reversals == 0);               // без маятника
        std::printf("loop soft=%d: захват за %d тактов, перелёт %.2f°, реверсов %d\n",
                    softFlag, settled, peak, reversals);
    }
}

static void test_soft_start_is_softer() {
    // Первый приказ мягкой ветки меньше первого приказа прежней: разгон
    // начинается с нуля, а не с трети шага.
    Params p;
    float pendX = 0.0f, pendY = 0.0f, pendT = 0.0f;
    float first[2] = {0.0f, 0.0f};
    for (int softFlag = 0; softFlag <= 1; ++softFlag) {
        Controller c;
        c.reset();
        p.soft = (softFlag != 0);
        p.substeps = 1;
        const Output o = c.tick(p, 0.0f, 0.0f, 1.0f / 12.0f, 30.0f, 6.0f,
                                false, 0.0f, 0.0f, false, 0.0f, 0.0f, pendX, pendY, pendT);
        assert(o.action == kMove);
        first[softFlag] = o.dx;
        pendX = pendY = pendT = 0.0f;
    }
    assert(first[1] < first[0]);
    std::printf("soft start: первый шаг %.1f px -> %.1f px\n", first[0], first[1]);
}

static void test_min_quantum() {
    // Остаток больше мёртвой зоны, но команда меньше кванта ввода: приказ
    // обязан быть в один квант, иначе довод встаёт навсегда.
    Controller c;
    c.reset();
    Params p;
    p.soft = true;
    p.units_per_px = 1.0f;
    p.base_gain = 0.10f;
    p.deg_per_px = 60.0f / 1080.0f;
    p.world_dist = 20.0f;
    p.screen_height = 1080.0f;
    float pendX = 0.0f, pendY = 0.0f, pendT = 0.0f;
    // Прогон через огибающую и ограничитель: остаток держится в полосе, где
    // команда прежней ветки округлилась бы в ноль.
    int moved = 0;
    for (int i = 0; i < 20; ++i) {
        const Output o = c.tick(p, 0.0f, 0.0f, 1.0f / 12.0f, 0.8f, 0.0f,
                                false, 0.0f, 0.0f, true, 0.0f, 0.0f, pendX, pendY, pendT);
        if (o.action == kMove && std::fabs(o.dx) >= 1.0f) ++moved;
        pendX = pendY = 0.0f; pendT = 0.0f;   // растение не моделируем: проверяем сам приказ
        c.flight.reset();                      // и учёт полёта здесь не при чём
    }
    assert(moved > 10);
    std::printf("min quantum: приказов в один квант %d из 20\n", moved);
}

int main() {
    test_envelope();
    test_taper();
    test_slew();
    test_substeps();
    test_delay_and_inflight();
    test_soft_start_is_softer();
    test_min_quantum();
    test_closed_loop();
    std::printf("aim_touch: все проверки пройдены\n");
    return 0;
}
