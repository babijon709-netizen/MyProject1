// Петля «палец -> камера -> экранная ошибка» аимбота, без устройства.
//
// Зачем: на устройстве аим стал быстрым (рабочий коэффициент 0.10 град/px,
// сборка 6c44e8b), но его стало сильно дёргать. Разбор лога 16.09.2026
// (farm_debug.log, 682 строки AIM):
//   * слайдер «Скорость» = 5 -> frac 0.583 поворота за кадр 60 fps; контроллер
//     переводит это в «скорость» k = 1-(1-frac)^(60*dt) и при кадре оверлея
//     dt = 81 мс получает k = 0.986: шаг = 98.6% ошибки;
//   * по логу |sent|*0.10/|err| = 0.982 (медиана, n=167) — подтверждает;
//   * камера откликается не в том же кадре: «поза камеры отстаёт от касания на
//     2 кадра» (замер в контроллере фарма, main.cpp:4996);
//   * значит петля: err(t+1) = err(t) - k*err(t-1) -> z^2 - z + k = 0. При
//     k = 0.986 корни |z| = sqrt(k) = 0.993 (затухание за ~100 кадров) и угол
//     ~80 градусов: незатухающие колебания с периодом ~4.5 кадра = 0.36 с.
//     В логе это видно как смена знака ошибки в 45% пар соседних строк.
//
// Здесь та же петля (тот же код контроллера) считается численно: сколько
// кадров до цели (скорость) и сколько остаётся колебаний/перелёта (дёрганье).
//
// ВЫВОД (замеренная задержка 2 кадра, кадр оверлея 81 мс, потолок 54 px):
//   доля ошибки за такт |  захват 60°  | захват 10° | ведение +-2° | цель 30°/с
//   -------------------+--------------+------------+--------------+-----------
//   0.99 (как было)    | никогда      | никогда    | 5.9° СКЗ     | 3.2°
//   0.50 (предел)      | 1.30 с       | 0.57 с     | 2.9° СКЗ     | 4.9°
//   0.40                | 1.30 с       | 0.57 с     | 2.4° СКЗ     | 6.0°
// «никогда» = ошибка не гаснет, а ходит через ноль с амплитудой до потолка
// (это и есть «очень сильно дергается» в логе 16.09.2026). Предел 0.5 выбран
// потому, что отставание по ровно едущей цели выходит ровно на физический
// минимум (30°/с x 2 кадра = 4.9°), то есть скорость не теряется, а захват
// цели не замедляется: при ошибке больше 5° шаг всё равно упирается в потолок.
//
// Сборка/запуск: g++ -O2 -o /tmp/sim_loop tools/aim/sim_loop.cpp && /tmp/sim_loop

#include <cstdio>
#include <cmath>
#include <algorithm>

struct Plant {
    float gain  = 0.10f;   // град поворота камеры на единицу пальца (измерено)
    int   delay = 2;       // кадров между касанием и видимым поворотом (измерено)
};

// Арифметика контроллера из main.cpp (UpdateAim, секция controller).
struct Ctrl {
    const char* name = "";
    float gain = 0.10f;      // gy — рабочий коэффициент, град/ед.
    float k = 0.986f;        // frac, переведённый в скорость
    float cap = 54.f;        // maxStep, единиц за кадр (0.05*sh без обучения)
    float dt = 0.081f;       // кадр оверлея

    bool  compensate = false;   // не повторять шаг, пока камера не отработала
    float tauTicks = 2.f;       // время жизни «в полёте», кадров
    int   holdTicks = 0;        // пауза на отклик камеры (0 = без паузы)
    bool  ackByError = false;   // пауза снимается, как только ошибка отреагировала
    bool  holdOnReverse = true; // не разворачивать палец по непроверенной оценке
    float kMax = 1.f;           // потолок k
    float pend = 0.f;           // запрошенный, но ещё не видимый поворот, град

    int   waitLeft = 0;
    float waitDeg = 0.f, errAtWait = 0.f;

    void decay(float dtNow) {
        if (!compensate) return;
        pend *= std::exp(-dtNow / (tauTicks * dt));
    }
    // Можно ли подавать новый шаг (такт с подтверждением).
    bool allow(float err) {
        if (holdTicks <= 0) return true;
        if (waitLeft <= 0) { waitLeft = holdTicks; errAtWait = err; return true; }
        if (ackByError) {
            const float moved = err - errAtWait;
            if (std::fabs(moved) >= 0.4f * std::fabs(waitDeg) && moved * waitDeg < 0.f) return true;
            if (--waitLeft <= 0) return true;      // ввод потерялся — продолжаем
            return false;
        }
        return (--waitLeft <= 0);
    }
    float tick(float err, float dtNow) {
        (void)dtNow;
        if (std::fabs(err) < 0.15f) return 0.f;
        float eff = err;
        if (compensate) {
            eff = err - pend;
            // знак ошибки перевернулся из-за оценки — значит оценка «накрыла»
            // цель; палец не разворачиваем (реверс по непроверенному числу и
            // есть рывок)
            if (holdOnReverse && eff * err < 0.f) eff = 0.f;
        }
        const float kk = std::min(k, kMax);
        float step = eff * kk / gain;
        step = std::max(-cap, std::min(cap, step));
        if (compensate) pend += step * gain;
        if (holdTicks > 0 && waitLeft > 0) waitDeg = step * gain;
        return step;
    }
};

struct Res {
    float settle;   // с, за которые ошибка впервые вошла в +-1 и больше не вышла (-1 = не вошла)
    float ring;     // макс. |err| после этого момента, град (перелёт/дёрганье)
    float rms;      // СКЗ ошибки на участке установления/ведения, град
    float step;     // средний |шаг| пальца, единиц
    float flips;    // доля пар кадров со сменой знака шага
};

// target(t) — угол цели относительно камеры, град. Камера стартует в 0.
// Камера видит команду, поданную delay кадров назад (как в игре).
static Res run(const Plant& p, Ctrl c, float (*target)(float), float dur) {
    const int n = (int)(dur / c.dt);
    float* r = new float[n + 1]();
    float* hist = new float[n + 1]();
    float theta = 0.f;
    double sq = 0.0, ss = 0.0; int cnt = 0, flips = 0; float prevStep = 0.f;
    for (int i = 0; i <= n; ++i) {
        const float t = i * c.dt;
        if (i - p.delay >= 0) theta += r[i - p.delay];       // команда дошла
        c.decay(c.dt);
        const float err = target(t) - theta;
        hist[i] = err;
        float step = 0.f;
        if (c.allow(err)) step = c.tick(err, c.dt);
        r[i] = step * p.gain;
        if (t > dur - 4.f) {                                  // хвост: установление ведения
            sq += err * err; ++cnt; ss += std::fabs(step);
            if (prevStep * step < 0.f && std::fabs(step) > 1.5f) ++flips;
            prevStep = step;
        }
    }
    // установилось: с какого кадра |err| <= 1 больше не выходит за 1
    int start = n + 1;
    for (int i = n; i >= 0; --i) { if (std::fabs(hist[i]) <= 1.0f) start = i; else break; }
    const float settle = (start <= n) ? start * c.dt : -1.f;
    // перелёт: максимум |err| после первого перехода ошибки через ноль
    float ring = 0.f;
    for (int i = 1; i <= n; ++i)
        if (hist[i] * hist[i - 1] < 0.f) {
            for (int j = i; j <= n; ++j) ring = std::max(ring, std::fabs(hist[j]));
            break;
        }
    Res res{};
    res.settle = settle;
    res.ring = ring;
    res.rms = cnt ? (float)std::sqrt(sq / cnt) : 0.f;
    res.step = cnt ? (float)(ss / cnt) : 0.f;
    res.flips = cnt ? (float)flips / cnt : 0.f;
    delete[] r; delete[] hist;
    return res;
}

static float stepBig(float t)  { (void)t; return 60.f; }   // цель влетела в ФОВ
static float stepMid(float t)  { (void)t; return 10.f; }   // смена цели рядом
static float weave2(float t)   { return 2.0f * sinf(2.f * 3.14159265f * 2.0f * t); }
static float weave5(float t)   { return 5.0f * sinf(2.f * 3.14159265f * 1.5f * t); }
static float runRate(float t)  { return 30.f * t; }        // цель едет 30 град/с

static void line(const Ctrl& c, const Plant& p, float (*tgt)(float), float dur) {
    const Res r = run(p, c, tgt, dur);
    std::printf("  %-30s до 1 град: %6.2f с   перелёт %5.2f   СКЗ хвоста %5.2f   шаг %5.1f   реверс %3.0f%%\n",
                c.name, r.settle, r.ring, r.rms, r.step, 100.f * r.flips);
}

int main() {
    Ctrl base;
    base.k = 1.f - std::pow(1.f - 0.58333f, 60.f * base.dt);   // слайдер «Скорость» = 5
    std::printf("контроллер как в логе 16.09: k = %.3f при кадре %.3f с\n", base.k, base.dt);

    Ctrl vars[8];
    int nv = 0;
    vars[nv] = base; vars[nv].name = "как сейчас (k = 0.99)"; ++nv;
    vars[nv] = base; vars[nv].name = "потолок k = 0.4"; vars[nv].kMax = 0.4f; ++nv;
    vars[nv] = base; vars[nv].name = "потолок k = 0.5"; vars[nv].kMax = 0.5f; ++nv;
    vars[nv] = base; vars[nv].name = "потолок k = 0.6"; vars[nv].kMax = 0.6f; ++nv;
    vars[nv] = base; vars[nv].name = "потолок k = 0.7"; vars[nv].kMax = 0.7f; ++nv;
    vars[nv] = base; vars[nv].name = "в полёте + потолок k=0.5";
                     vars[nv].compensate = true; vars[nv].tauTicks = 2.f; vars[nv].kMax = 0.5f; ++nv;

    struct Case { const char* name; float (*tgt)(float); float dur; };
    const Case cases[] = {
        {"захват: цель на 60 град", stepBig, 6.f},
        {"захват: цель на 10 град", stepMid, 6.f},
        {"ведение: +-2 град, 2 Гц", weave2, 10.f},
        {"ведение: +-5 град, 1.5 Гц", weave5, 10.f},
        {"цель едет 30 град/с", runRate, 4.f},
    };
    for (int d = 1; d <= 3; ++d) {
        Plant p; p.gain = 0.10f; p.delay = d;
        std::printf("\n=== камера: задержка %d кадр%s %s\n", d, d == 1 ? "" : "а",
                    d == 2 ? "(замер)" : "");
        for (const Case& cs : cases) {
            std::printf("%s\n", cs.name);
            for (int v = 0; v < nv; ++v) line(vars[v], p, cs.tgt, cs.dur);
        }
    }
    return 0;
}
