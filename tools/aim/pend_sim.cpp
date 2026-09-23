// Маятник тач-аима: численное воспроизведение жалобы 23.09
// «мемори аим ходит как маятник первые 2-3 секунды и тач аим тоже».
//
// Прежние стенды (sim_loop.cpp) считали петлю в ТАКТАХ ОВЕРЛЕЯ и подставляли
// туда задержку, измеренную в КАДРАХ ИГРЫ. Единицы не совпадали в 5-7 раз:
//   * кадр игры      ~16.7 мс (60 fps);
//   * такт оверлея   ~81 мс  (12 fps на устройстве, лог 07:37/16.09);
//   * касание -> камера 2-8 КАДРОВ ИГРЫ = 33-133 мс (замер контроллера фарма).
// Поэтому окно «в полёте» в 8 ТАКТОВ (0.65 с) вычитало приказы, которые камера
// давно отработала: контроллер недозаказывал, потом разворачивался, окно
// доедало призраки — раскачка на первые секунды. Здесь петля считается в
// КАДРАХ ИГРЫ, контроллер тикает раз в N кадров — как на устройстве.
//
// Сборка: g++ -O2 -o /tmp/pend_sim tools/aim/pend_sim.cpp && /tmp/pend_sim

#include <cstdio>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <vector>

// ---------- физика ----------
struct Plant {
    float gTrue   = 0.10f;  // град поворота камеры на пиксель пальца
    int   visF    = 4;      // кадров игры от Touch_Move до видимости в боксах
    int   overF   = 5;      // кадров игры на такт оверлея (5 -> ~12 fps)
    float target0 = 30.f;   // ошибка в момент захвата, град
    float targetV = 0.f;    // цель движется, град/с (ведение)
};

// ---------- контроллер (арифметика UpdateAimTouch, ось yaw) ----------
struct TouchCtrl {
    // вариант окна «в полёте»:
    //   mode 0 — текущий код: 8 слотов, сдвиг раз в такт;
    //   mode 1 — время жизни приказа kLife с (шаг живёт ровно столько);
    //   mode 2 — окна нет вовсе;
    //   mode 3 — только ПОСЛЕДНИЙ приказ, пока не состарился на kLife.
    int   mode = 0;
    float kLife = 0.12f;
    float probe = 0.10f;    // гейн из настройки чувствительности
    float trim  = 1.f;      // подтяжка [1, 1.4], шаг 0.15/такт
    float trimAt = 0.8f;    // когда GainTrack готов (сек от захвата; 0 = сразу)
    // стационарные части контроллера
    float kStable = 0.45f;
    float flipDamp = 1.f;
    float acquire = 0.f;
    float prevErr = 0.f;
    bool  havePrev = false;
    float window[8] = {0};
    float wTime[8] = {0};   // возраст слота, с
    float pendDeg = 0.f;    // сумма окна (град, в рабочем гейне)

};

struct Res {
    float settle = -1.f;   // с, после которых |err| <= dead и не выходит
    float ring   = 0.f;    // макс |err| после первого перехода через 0
    int   flips  = 0;      // смен знака шага за первые 3 с
    float maxOvershoot = 0.f; // |err| на противоположной стороне от старта
    bool  settled3s = false; // устоялось ли за 3 с
    float trail = -1.f;    // остаток при ведении (ср. |err| за последние 2 с)
};

static Res run(const Plant& p, TouchCtrl c, float dead, float* trace = nullptr, int traceN = 0) {
    const float frameDt = 1.f / 60.f;
    const float tickDt  = p.overF * frameDt;
    const int   nFrames = (int)(5.f / frameDt);      // 5 секунд
    const int   nTicks  = nFrames / p.overF + 1;
    std::vector<float> cmd(nFrames + 64, 0.f);
    std::vector<float> errHist(nTicks, 0.f);
    std::vector<float> stepHist(nTicks, 0.f);
    std::vector<float> tHist(nTicks, 0.f);
    float camAngle = 0.f;
    float tgtAngle = p.target0;
    float err = p.target0;
    Res r{};
    float t = 0.f;
    int  nextTick = 0;
    int  tick = 0;
    float prevStep = 0.f;

    for (int f = 0; f < nFrames; ++f) {
        // физика кадра игры
        camAngle += cmd[f];
        tgtAngle += p.targetV * frameDt;
        if (f != nextTick) continue;
        nextTick += p.overF;
        t += tickDt;
        // ---- контроллер ----
        err = tgtAngle - camAngle;
        errHist[tick] = err; tHist[tick] = t; stepHist[tick] = 0.f;
        ++tick;
        // подтяжка гейна
        float want = 1.f;
        if (t >= c.trimAt && c.probe > 1e-4f) {
            want = p.gTrue / c.probe;
            if (want < 1.f) want = 1.f;
            if (want > 1.4f) want = 1.4f;
        }
        c.trim += (want - c.trim) * 0.15f;
        const float gy = c.probe * c.trim;
        // окно «в полёте»
        float pend = 0.f;
        if (c.mode == 0) {
            for (int i = 0; i < 8; ++i) pend += c.window[i];
        } else if (c.mode == 1) {
            for (int i = 0; i < 8; ++i)
                if (c.window[i] != 0.f && c.wTime[i] < c.kLife) pend += c.window[i];
        }
        c.pendDeg = pend * gy;
        float eff = err - c.pendDeg;
        // возраст/сдвиг окна — раз в такт
        if (c.mode == 1) for (int i = 0; i < 8; ++i) c.wTime[i] += tickDt;
        for (int i = 0; i < 7; ++i) { c.window[i] = c.window[i + 1]; c.wTime[i] = c.wTime[i + 1]; }
        c.window[7] = 0.f; c.wTime[7] = 0.f;
        if (std::fabs(eff) < dead) { prevStep = 0.f; continue; }
        // k
        float k = c.kStable;
        // гаситель реверса
        const bool flip = c.havePrev && c.prevErr * eff < 0.f &&
                          std::fabs(eff) > std::fabs(c.prevErr) * 0.35f;
        if (flip) c.flipDamp = 0.45f;
        else if (c.flipDamp < 1.f) {
            c.flipDamp += (1.f - c.flipDamp) * (tickDt * 2.5f);
            if (c.flipDamp > 1.f) c.flipDamp = 1.f;
        }
        c.prevErr = eff; c.havePrev = true;
        k *= c.flipDamp;
        // мягкий старт
        c.acquire += tickDt / 0.35f;
        if (c.acquire > 1.f) c.acquire = 1.f;
        k *= 0.3f + 0.7f * c.acquire;
        float dx = eff * k / gy;
        // снап ближе 3 квантов (unitsPerPx = 1)
        if (std::fabs(eff) < gy * 3.f) dx = std::round(eff / gy);
        // не больше остатка
        if (std::fabs(dx * gy) > std::fabs(eff)) dx = eff / gy;
        // потолок шага 54 px
        if (dx > 54.f) dx = 54.f;
        if (dx < -54.f) dx = -54.f;
        dx = std::round(dx);
        stepHist[tick - 1] = dx;
        if (dx != 0.f) {
            if (prevStep * dx < 0.f && t < 3.f) ++r.flips;
            prevStep = dx;
            // приказ: камера увидит его через visF кадров
            cmd[f + p.visF] += dx * p.gTrue;
            c.window[7] = dx;
        } else prevStep = 0.f;
    }
    // ---- постобработка ----
    int start = nTicks;
    for (int i = nTicks - 1; i >= 0; --i)
        if (std::fabs(errHist[i]) > dead) { start = i + 1; break; }
    if (start <= nTicks - 1) r.settle = tHist[start];
    bool crossedZero = false;
    for (int i = 1; i < nTicks; ++i) {
        if (!crossedZero && errHist[i] * p.target0 < 0.f) crossedZero = true;
        if (crossedZero) r.ring = std::max(r.ring, std::fabs(errHist[i]));
        if (errHist[i] * p.target0 < 0.f) r.maxOvershoot = std::max(r.maxOvershoot, std::fabs(errHist[i]));
    }
    r.settled3s = r.settle >= 0.f && r.settle <= 3.f;
    // остаток при ведении: среднее |err| за последние 2 с
    {
        double s = 0.0; int n = 0;
        for (int i = 0; i < nTicks; ++i)
            if (tHist[i] >= 3.f) { s += std::fabs(errHist[i]); ++n; }
        r.trail = n ? (float)(s / n) : 0.f;
    }
    if (trace && traceN > 0) {
        int n = std::min(traceN, nTicks);
        for (int i = 0; i < n; ++i) trace[i] = errHist[i];
    }
    return r;
}

// ---------- контроллер памяти (арифметика UpdateAimMemory, ось yaw) ----------
// Накопитель в градусах: гейна нет, L = 1. Задержка применения записи — в
// кадрах игры (заявлено «в следующем же кадре»). Варианты окна:
//   mode 0 — текущий код: слот [1] обнулён, шаг НИКОГДА не попадает в окно;
//   mode 1 — окно починено как задумано: 2 слота, сдвиг раз в такт;
//   mode 2 — время жизни kLife секунд;
//   mode 3 — время жизни + ack: если накопитель совпал с прошлой записью,
//            игра её применила — окно чистится, responded защёлкивается.
struct MemCtrl {
    int   mode = 0;      // 4 = текущий код + работающая ось камеры
    float kLife = 0.05f;
    float kStable = 0.5f;
    float flipDamp = 1.f;
    float prevErr = 0.f;
    bool  havePrev = false;
    float filt = 0.f;
    bool  haveFilt = false;
    float window[2] = {0.f, 0.f};
    float wAge[2] = {0.f, 0.f};
    bool  responded = false;
    float capLevel = 0.f;     // 0..1: потолок растёт, пока нет перескоков
    float lastWrite = 0.f;
    bool  haveWrite = false;
    float capNow() const {
        if (!responded) return 2.f;
        return 2.f + (12.f - 2.f) * capLevel;
    }
};

struct MemRes {
    float settle = -1.f;
    float ring = 0.f;
    int   flips = 0;
    bool  settled3s = false;
    float trail = -1.f;   // среднее |err| за последние 2 с (ведение)
};

static MemRes runMem(const Plant& p, MemCtrl c, float dead, float* trace = nullptr, int traceN = 0) {
    const float frameDt = 1.f / 60.f;
    const float tickDt = p.overF * frameDt;
    const int nFrames = (int)(5.f / frameDt);
    const int nTicks = nFrames / p.overF + 1;
    std::vector<float> cmd(nFrames + 64, 0.f);
    std::vector<float> errHist(nTicks, 0.f), tHist(nTicks, 0.f), stepHist(nTicks, 0.f);
    // «камера» здесь — накопитель: игра разворачивает запись в поворот в кадре
    // применения, а боксы это ОТРАЖАЮТ (со своей задержкой рендера visF).
    float acc = 0.f;
    float tgt = p.target0;
    MemRes r{};
    float t = 0.f;
    int nextTick = 0, tick = 0;
    float prevStep = 0.f;

    for (int f = 0; f < nFrames; ++f) {
        acc += cmd[f];
        tgt += p.targetV * frameDt;
        if (f != nextTick) continue;
        nextTick += p.overF;
        t += tickDt;
        // ошибка видна с задержкой рендера: боксы кадра f показывают позу,
        // которая была visF кадров назад
        float accVisible = acc;
        for (int back = 1; back <= p.visF; ++back)
            accVisible -= cmd[std::max(0, f - back + 1)];
        float err = tgt - accVisible;
        errHist[tick] = err; tHist[tick] = t; stepHist[tick] = 0.f;
        ++tick;
        // ack по накопителю (mode 3): записанное применилось, если накопитель
        // совпадает с прошлой записью (пользователь ввод не добавлял)
        if (c.mode == 4 && t >= 0.5f) c.responded = true;   // ось сказала «наш поворот»
        if (c.mode == 3) {
            // ack: игра приняла запись (накопитель совпал) — responded = 1.
            // Окно НЕ чистим: «применено» ещё не «видно в остатке» (рендер
            // отстаёт на кадр-два), и на быстром оверлее чистка заглядывала
            // вперёд. Приказы истекают по времени жизни сами.
            if (c.haveWrite && std::fabs(acc - c.lastWrite) < 0.1f) {
                c.responded = true;
            }
        }
        float inflight = 0.f;
        if (c.mode == 0) {
            inflight = 0.f;                      // баг: окно всегда пусто
        } else if (c.mode == 1) {
            inflight = c.window[0] + c.window[1];
        } else if (c.mode == 5) {
            // только САМЫЙ МОЛОДОЙ приказ: он мог не стать видимым, старшие —
            // уже видны (иначе окно съедает скорость ведения на 60 fps)
            float best = 0.f, bestAge = 1e9f;
            for (int i = 0; i < 2; ++i)
                if (c.window[i] != 0.f && c.wAge[i] < bestAge) { bestAge = c.wAge[i]; best = c.window[i]; }
            if (bestAge < c.kLife) inflight = best;
        } else {
            if (c.wAge[0] < c.kLife) inflight += c.window[0];
            if (c.wAge[1] < c.kLife) inflight += c.window[1];
        }
        // сглаживание остатка (kSmoothTau = 30 мс)
        const float a = 1.f - std::exp(-tickDt / 0.030f);
        if (!c.haveFilt) { c.filt = err; c.haveFilt = true; }
        else c.filt += (err - c.filt) * a;
        float eff = c.filt - inflight;
        if (std::fabs(eff) < dead) { prevStep = 0.f; goto shift; }
        {
            float k = c.kStable;
            const bool flip = c.havePrev && c.prevErr * eff < 0.f &&
                              std::fabs(eff) > std::fabs(c.prevErr) * 0.35f;
            // потолок растёт только без перескоков: прыгнули через цель —
            // считаем, что видимость отстаёт, и идём мельче
            if (flip && c.responded) c.capLevel *= 0.5f;
            else if (c.responded) c.capLevel += tickDt / 0.6f;
            if (c.capLevel > 1.f) c.capLevel = 1.f;
            if (flip) c.flipDamp = 0.85f;
            else if (c.flipDamp < 1.f) {
                c.flipDamp += (1.f - c.flipDamp) * (tickDt * 2.5f);
                if (c.flipDamp > 1.f) c.flipDamp = 1.f;
            }
            c.prevErr = eff; c.havePrev = true;
            k *= c.flipDamp;
            float step = eff * k;
            const float cap = c.capNow();
            if (step > cap) step = cap;
            if (step < -cap) step = -cap;
            stepHist[tick - 1] = step;
            if (step != 0.f) {
                if (prevStep * step < 0.f && t < 3.f) ++r.flips;
                prevStep = step;
                cmd[f + 1] += step;               // запись применяется в след. кадре игры
                if (c.mode >= 1) c.window[1] = step, c.wAge[1] = 0.f;
                c.lastWrite = acc + step; c.haveWrite = true;
            } else prevStep = 0.f;
        }
    shift:
        c.wAge[0] += tickDt; c.wAge[1] += tickDt;
        c.window[0] = c.window[1]; c.wAge[0] = c.wAge[1];
        c.window[1] = 0.f;
    }
    int start = nTicks;
    for (int i = nTicks - 1; i >= 0; --i)
        if (std::fabs(errHist[i]) > dead) { start = i + 1; break; }
    if (start <= nTicks - 1) r.settle = tHist[start];
    bool crossedZero = false;
    for (int i = 1; i < nTicks; ++i) {
        if (!crossedZero && errHist[i] * p.target0 < 0.f) crossedZero = true;
        if (crossedZero) r.ring = std::max(r.ring, std::fabs(errHist[i]));
    }
    r.settled3s = r.settle >= 0.f && r.settle <= 3.f;
    {
        double s2 = 0.0; int n2 = 0;
        for (int i = 0; i < nTicks; ++i)
            if (tHist[i] >= 3.f) { s2 += std::fabs(errHist[i]); ++n2; }
        r.trail = n2 ? (float)(s2 / n2) : 0.f;
    }
    if (trace && traceN > 0) {
        int n = std::min(traceN, nTicks);
        for (int i = 0; i < n; ++i) trace[i] = errHist[i];
    }
    return r;
}

int main() {
    const float dead = 0.2f;   // два кванта при гейне 0.10 град/px
    struct Row { const char* name; int mode; float life; float probe; };
    const Row rows[] = {
        {"окно 8 тактов (текущий код), гейн верен     ", 0, 0.f,   0.10f},
        {"окно 8 тактов, гейн занижен 1.4x            ", 0, 0.f,   0.071f},
        {"окно 8 тактов, гейн занижен 2x              ", 0, 0.f,   0.05f},
        {"окно 8 тактов, гейн завышен 2x              ", 0, 0.f,   0.20f},
        {"время жизни 0.12с, гейн верен               ", 1, 0.12f, 0.10f},
        {"время жизни 0.12с, гейн занижен 2x          ", 1, 0.12f, 0.05f},
        {"время жизни 0.12с, гейн завышен 2x          ", 1, 0.12f, 0.20f},
        {"окна нет, гейн верен                        ", 2, 0.f,   0.10f},
        {"окна нет, гейн занижен 2x                   ", 2, 0.f,   0.05f},
        {"ПОСЛЕДНИЙ приказ 0.12с, гейн верен          ", 3, 0.12f, 0.10f},
        {"ПОСЛЕДНИЙ приказ 0.12с, гейн занижен 2x     ", 3, 0.12f, 0.05f},
        {"ПОСЛЕДНИЙ приказ 0.12с, гейн завышен 2x     ", 3, 0.12f, 0.20f},
    };
    printf("захват 30°, такт оверлея 5 кадров игры, касание видно через 4 кадра:\n");
    printf("%-46s %-10s %-8s %-7s %-9s %s\n", "вариант", "захват,с", "перелёт°", "реверсы", "макс.кач", "устоялось к 3с");
    for (const Row& row : rows) {
        for (float trimAt : {0.0f, 0.8f}) {
            Plant p; p.target0 = 30.f; p.visF = 4; p.overF = 5;
            TouchCtrl c; c.mode = row.mode; c.kLife = row.life; c.probe = row.probe; c.trimAt = trimAt;
            Res r = run(p, c, dead);
            printf("%-46s %-10.2f %-8.2f %-7d %-9.2f %s%s\n", row.name,
                   r.settle, r.ring, r.flips, r.maxOvershoot,
                   r.settled3s ? "да" : "НЕТ",
                   trimAt > 0.f ? "  (подтяжка с 0.8с)" : "");
        }
    }
    printf("\nведение цели 30°/с (цель бежит, ошибка старта 5°):\n");
    printf("%-46s %-10s %-8s %-7s %s\n", "вариант", "шлейф°", "перелёт°", "реверсы", "устоялось к 3с");
    for (const Row& row : rows) {
        Plant p; p.target0 = 5.f; p.targetV = 30.f; p.visF = 4; p.overF = 5;
        TouchCtrl c; c.mode = row.mode; c.kLife = row.life; c.probe = row.probe; c.trimAt = 0.8f;
        Res r = run(p, c, dead);
        printf("%-46s %-10.2f %-8.2f %-7d %s\n", row.name,
               r.trail, r.ring, r.flips, r.settled3s ? "да" : "НЕТ");
    }
    // ведение на быстром оверлее: сумма окон против жизни 0.12с
    printf("\nведение 30°/с при оверлее 60 fps (visF=4):\n");
    for (const Row& row : rows) {
        Plant p; p.target0 = 5.f; p.targetV = 30.f; p.visF = 4; p.overF = 1;
        TouchCtrl c; c.mode = row.mode; c.kLife = row.life; c.probe = row.probe; c.trimAt = 0.8f;
        Res r = run(p, c, dead);
        printf("%-46s шлейф %.2f°, реверсов %d\n", row.name, r.trail, r.flips);
    }
    // быстрый оверлей (60 fps = 1 кадр игры на такт): жизнь 0.12с = 7 тактов,
    // как исходные 8 слотов — регресса быть не должно
    printf("\n===== ТАЧ при оверлее 60 fps (overF=1) =====\n");
    printf("%-46s %-10s %-8s %-7s %s\n", "вариант", "захват,с", "перелёт°", "реверсы", "устоялось к 3с");
    for (const Row& row : rows) {
        Plant p; p.target0 = 30.f; p.visF = 4; p.overF = 1;
        TouchCtrl c; c.mode = row.mode; c.kLife = row.life; c.probe = row.probe; c.trimAt = 0.8f;
        Res r = run(p, c, dead);
        printf("%-46s %-10.2f %-8.2f %-7d %s\n", row.name, r.settle, r.ring, r.flips, r.settled3s ? "да" : "НЕТ");
    }
    // чуствительность к задержке: сколько кадров игры от касания до боксов
    printf("\nокно 8 тактов, гейн верен, задержка касания в кадрах игры:\n");
    for (int visF : {2, 4, 6, 8, 12}) {
        Plant p; p.target0 = 30.f; p.visF = visF; p.overF = 5;
        TouchCtrl c; c.mode = 0; c.probe = 0.10f; c.trimAt = 0.8f;
        Res r = run(p, c, dead);
        printf("  задержка %2d кадров (%3.0f мс): захват %.2fс, перелёт %.2f°, реверсов %d, устоялось: %s\n",
               visF, visF * 16.7f, r.settle, r.ring, r.flips, r.settled3s ? "да" : "НЕТ");
    }
    printf("\nвремя жизни 0.12с, гейн верен, задержка касания в кадрах игры:\n");
    for (int visF : {2, 4, 6, 8, 12}) {
        Plant p; p.target0 = 30.f; p.visF = visF; p.overF = 5;
        TouchCtrl c; c.mode = 1; c.kLife = 0.12f; c.probe = 0.10f; c.trimAt = 0.8f;
        Res r = run(p, c, dead);
        printf("  задержка %2d кадров (%3.0f мс): захват %.2fс, перелёт %.2f°, реверсов %d, устоялось: %s\n",
               visF, visF * 16.7f, r.settle, r.ring, r.flips, r.settled3s ? "да" : "НЕТ");
    }
    // трасса ошибки: ход маятника глазами
    {
        float tr[40];
        Plant p; p.target0 = 30.f; p.visF = 4; p.overF = 5;
        TouchCtrl c; c.mode = 0; c.probe = 0.10f; c.trimAt = 0.8f;
        run(p, c, dead, tr, 40);
        printf("\nтрасса ошибки (текущее окно 8 тактов), ° на такт (81 мс):\n  ");
        for (int i = 0; i < 40; ++i) printf("%6.1f", tr[i]);
        printf("\n");
        TouchCtrl c2; c2.mode = 1; c2.kLife = 0.12f; c2.probe = 0.10f; c2.trimAt = 0.8f;
        run(p, c2, dead, tr, 40);
        printf("трасса ошибки (жизнь 0.12с), ° на такт:\n  ");
        for (int i = 0; i < 40; ++i) printf("%6.1f", tr[i]);
        printf("\n");
    }
    // ================== память ==================
    const float mdead = 0.12f;   // kDeadDeg
    printf("\n===== ПАМЯТЬ: захват 30°, окно/ack варианты =====\n");
    printf("%-44s %-10s %-8s %-7s %s\n", "вариант", "захват,с", "перелёт°", "реверсы", "устоялось к 3с");
    struct MRow { const char* name; int mode; float life; };
    const MRow mrows[] = {
        {"текущий код (окно мертво), отклика нет", 0, 0.f},
        {"текущий код, ack по накопителю",           3, 0.05f},
        {"окно починено как задумано (2 слота)",     1, 0.f},
        {"время жизни 0.05с",                        2, 0.05f},
        {"ИТОГ: жизнь 0.10с + ack + рампа",          3, 0.10f},
    };
    for (int visF : {1, 2, 5, 10}) {
        printf("-- задержка видимости %d кадров игры (%.0f мс):\n", visF, visF * 16.7f);
        for (const MRow& row : mrows) {
            Plant p; p.target0 = 30.f; p.visF = visF; p.overF = 5;
            MemCtrl c; c.mode = row.mode; c.kLife = row.life;
            MemRes r = runMem(p, c, mdead);
            printf("  %-42s %-10.2f %-8.2f %-7d %s\n", row.name,
                   r.settle, r.ring, r.flips, r.settled3s ? "да" : "НЕТ");
        }
    }
    // быстрый оверлей (60 fps): жизнь не должна пересуживать
    printf("\n-- оверлей 60 fps (overF=1), задержка видимости 1-2 кадра:\n");
    // граница фаз: игра 12 fps, запись видна только ЧЕРЕЗ такт оверлея
    printf("\n-- игра 12 fps (overF=5), запись видна через такт (visF=7):\n");
    {
        Plant p; p.target0 = 30.f; p.visF = 7; p.overF = 5;
        MemCtrl c; c.mode = 0;   // текущий код: окно мертво, отклика нет
        MemRes r = runMem(p, c, mdead);
        printf("  текущий код, оси нет (капа 2°): захват %.2fс, перелёт %.2f°, реверсов %d, устоялось: %s\n",
               r.settle, r.ring, r.flips, r.settled3s ? "да" : "НЕТ");
    }
    {
        Plant p; p.target0 = 30.f; p.visF = 7; p.overF = 5;
        MemCtrl c; c.mode = 4;   // текущий код, ось читается: капа поднялась
        MemRes r = runMem(p, c, mdead);
        printf("  текущий код, ось есть (капа 12°): захват %.2fс, перелёт %.2f°, реверсов %d, устоялось: %s\n",
               r.settle, r.ring, r.flips, r.settled3s ? "да" : "НЕТ");
        float tr[40];
        runMem(p, c, mdead, tr, 40);
        printf("  трасса: ");
        for (int i = 0; i < 40; ++i) printf("%6.1f", tr[i]);
        printf("\n");
    }
    for (float life : {0.05f, 0.10f, 0.15f}) {
        Plant p; p.target0 = 30.f; p.visF = 7; p.overF = 5;
        MemCtrl c; c.mode = 3; c.kLife = life;
        MemRes r = runMem(p, c, mdead);
        printf("  жизнь %.2fс: захват %.2fс, перелёт %.2f°, реверсов %d, устоялось: %s\n",
               life, r.settle, r.ring, r.flips, r.settled3s ? "да" : "НЕТ");
    }
    // один-единственный молодой приказ вместо суммы
    printf("\n-- окно «только последний приказ» (жизнь 0.10с):\n");
    for (int overF : {5, 1}) {
        for (int visF : {1, 2, 7}) {
            if (overF == 1 && visF == 7) continue;
            Plant p; p.target0 = 30.f; p.visF = visF; p.overF = overF;
            MemCtrl c; c.mode = 5; c.kLife = 0.10f;
            MemRes r = runMem(p, c, mdead);
            printf("  оверлей %2d кадр, видимость %d: захват %.2fс, перелёт %.2f°, реверсов %d, устоялось: %s\n",
                   overF, visF, r.settle, r.ring, r.flips, r.settled3s ? "да" : "НЕТ");
        }
    }
    // ведение 30°/с на 60 fps оверлея — где сумма шагов давала шлейф
    {
        Plant p; p.target0 = 5.f; p.targetV = 30.f; p.visF = 1; p.overF = 1;
        MemCtrl c; c.mode = 5; c.kLife = 0.10f;
        MemRes r = runMem(p, c, mdead);
        printf("  ведение 30°/с (оверлей 60 fps): шлейф %.2f°, устоялось: %s\n",
               r.trail, r.settled3s ? "да" : "НЕТ");
        MemCtrl c3; c3.mode = 3; c3.kLife = 0.10f;
        MemRes r3 = runMem(p, c3, mdead);
        printf("  ведение 30°/с, СУММА окон:      шлейф %.2f°, устоялось: %s\n",
               r3.trail, r3.settled3s ? "да" : "НЕТ");
        MemCtrl c0; c0.mode = 0;
        MemRes r0 = runMem(p, c0, mdead);
        printf("  ведение 30°/с, окна нет:        шлейф %.2f°\n", r0.trail);
    }
    {
        struct { const char* nm; int mode; } vs[2] = {{"текущий код", 0}, {"итог (ack+рампа+жизнь 0.10)", 3}};
        for (const auto& v : vs) {
            for (int visF : {1, 2}) {
                Plant p; p.target0 = 30.f; p.visF = visF; p.overF = 1;
                MemCtrl c; c.mode = v.mode; c.kLife = 0.10f;
                MemRes r = runMem(p, c, mdead);
                printf("  %-28s видимость %d кадр: захват %.2fс, перелёт %.2f°, реверсов %d, устоялось: %s\n",
                       v.nm, visF, r.settle, r.ring, r.flips, r.settled3s ? "да" : "НЕТ");
            }
        }
    }
    {
        float tr[40];
        Plant p; p.target0 = 30.f; p.visF = 5; p.overF = 5;
        MemCtrl c; c.mode = 0;
        runMem(p, c, mdead, tr, 40);
        printf("\nтрасса ошибки (память, текущий код), ° на такт (81 мс):\n  ");
        for (int i = 0; i < 40; ++i) printf("%6.1f", tr[i]);
        printf("\n");
        MemCtrl c2; c2.mode = 3; c2.kLife = 0.10f;
        runMem(p, c2, mdead, tr, 40);
        printf("трасса ошибки (память, ack + жизнь 0.05с), ° на такт:\n  ");
        for (int i = 0; i < 40; ++i) printf("%6.1f", tr[i]);
        printf("\n");
    }
    return 0;
}
