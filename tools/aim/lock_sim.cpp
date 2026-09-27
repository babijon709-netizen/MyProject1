// Стенд «жёсткого лока» (жалобы 23.09, три захода):
// 1) «тач аим и мемори аим в первые 2 секунды при наводке двигаются как
//    маятник вправо-влево от цели»;
// 2) «при посадке ФПС тач-аим ходит маятником» — при 12-15 фпс игры приказ
//    летит в трубе касания до 0.5-0.7 с;
// 3) «мемори отстаёт от головы» — доле шага у цели не угнаться за идущей
//    костью.
//
// Проверяются принятые решения:
//
//  ТАЧ: ПОГЛОЩЕНИЕ, списываемое по ПОТОКУ КАМЕРЫ (медианный сдвиг боксов:
//       камера повернулась на Х — ровно Х заказанного отработано), а когда
//       боксов меньше трёх (дуэль 1 на 1) — запасное списание по падению
//       остатка. Что вычитается из остатка, выбирает величина остатка: на
//       захвате (> 2.5°) правит поглощение при любой задержке трубы, на
//       ведении — минимум с окном приказов (след за целью короткий). Плюс
//       обнуление висящего заказа при перескоке остатка через ноль и
//       таймаут «потерялся» 1.0 с (перекрывает худшую задержку трубы).
//
//  ПАМЯТЬ: абсолютная цель — фактический угол кости, полный остаток без
//       долей, но крупный поворот ограничен скоростью 120°/с. Дрожь кости
//       отсекает пара фильтров: мёртвая зона и порог УСТОЙЧИВОГО
//       однонаправленного дрейфа в полосе у цели.
//
// Запись в накопитель идемпотентна: сколько раз её ни повтори, камера придёт
// в одну и ту же точку, поэтому граница фаз (запись видна через такт)
// раскачать петлю не может в принципе.
//
// Сборка: g++ -O2 -o /tmp/lock_sim tools/aim/lock_sim.cpp && /tmp/lock_sim

#include <cstdio>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <vector>
#include "aim_touch_geometry.h"

// ---------- физика ----------
struct Plant {
    float gTrue   = 0.10f;  // град поворота камеры на пиксель пальца
    int   visF    = 4;      // кадров игры от приказа до видимости в боксах
    int   overF   = 5;      // кадров игры на такт оверлея (5 -> ~12 fps)
    float target0 = 30.f;   // ошибка в момент захвата, град
    float targetV = 0.f;    // цель движется, град/с
    float rangeM  = 25.f;   // дистанция до цели, влияет на ширину close-jitter band
    float jitter  = 0.f;    // дрожь кости, град (амплитуда, знак чередуется)
    float jitStart = 0.f;   // дрожь включается с этой секунды (после сходимости)
    int   nBoxes  = 5;      // боксов на экране: >= 3 — поток камеры виден
    float secs    = 5.f;    // горизонт прогона, с
};

struct Res {
    float settle = -1.f;
    float lock05 = -1.f;      // момент, когда |err| впервые <= 0.5°
    float ring   = 0.f;       // макс |err| после первого перехода через 0
    int   flips  = 0;         // смен знака шага за первые 3 с
    float maxOvershoot = 0.f;
    bool  settled3s = false;
    float trail = -1.f;       // средний |err| за последние 2 с (ведение)
    float jitCam = 0.f;       // макс ход камеры за такт при дрожи цели
};

// ============================ ТАЧ ===========================================
struct TouchCtrl {
    // mode 0 — текущий код: только ПОСЛЕДНИЙ приказ, жизнь kLife;
    // mode 2 — СУММА приказов моложе kLife + защита знака;
    // mode 3 — сумма + защита + чистка окна, когда приказы покрыли остаток;
    // mode 5 — ПОГЛОЩЕНИЕ (старое): заказ копится, списывается по падению
    //          остатка; вычитается целиком. Оставлен как эталон «было»;
    // mode 6 — ГИБРИД (текущий код): поглощение списывается ПО ПОТОКУ
    //          КАМЕРЫ (камера повернулась на Х — Х заказанного отработано),
    //          а при < 3 боксов — по падению остатка; из остатка вычитается
    //          поглощение на захвате (|остаток| > 2.5°) и минимум с окном
    //          приказов на ведении; висящий заказ обнуляется при перескоке
    //          остатка через ноль; «потерялся» после 1.0 с без отклика.
    int   mode = 6;
    float kLife = 0.13f;
    float inflightBand = 2.5f;  // захват/ведение, град
    float stallTime = 1.0f;     // заказ без отклика, с
    float probe = 0.10f;
    float trim  = 1.f;
    float trimAt = 0.8f;
    float trimStep = 0.25f;
    float kStable = 0.45f;
    float softT = 0.20f;      // мягкий старт, с
    float flipDamp = 1.f;
    float acquire = 0.f;
    float prevErr = 0.f;
    bool  havePrev = false;
    struct Ord { float px; float age; };
    std::vector<Ord> ord;
    float pending = 0.f;      // mode 5/6: заказано, не отработано (град, знак шага)
    float pendAge = 0.f;      // сколько заказ живёт без поглощения
    float lastErr = 0.f;      // остаток прошлого такта
    bool  haveLastErr = false;
    float lastCam = 0.f;      // mode 6: угол камеры прошлого такта (поток)
    bool  haveCamFlow = false;
    float jitterBand = 0.45f; // mode 6: полоса дрожи у цели, град
    int   driftStreak = 3;    // mode 6: порог устойчивого остатка в полосе
    int   streak = 0;
    float rampBand = 2.5f;    // mode 6: спад доли шага у цели
    float rampMin  = 0.40f;
};

static Res runTouch(const Plant& p, TouchCtrl c, float dead) {
    const float frameDt = 1.f / 60.f;
    const float tickDt  = p.overF * frameDt;
    const int   nFrames = (int)(p.secs / frameDt);
    const int   nTicks  = nFrames / p.overF + 1;
    std::vector<float> cmd(nFrames + 64, 0.f);
    std::vector<float> errHist(nTicks, 0.f), tHist(nTicks, 0.f);
    float camAngle = 0.f, tgtAngle = p.target0;
    Res r{};
    float t = 0.f;
    int nextTick = 0, tick = 0;
    float prevStep = 0.f;
    int jitSign = 1;

    for (int f = 0; f < nFrames; ++f) {
        camAngle += cmd[f];
        tgtAngle += p.targetV * frameDt;
        if (f != nextTick) continue;
        nextTick += p.overF;
        t += tickDt;
        jitSign = -jitSign;
        const float jit = p.jitter * jitSign;
        float err = tgtAngle + jit - camAngle;
        errHist[tick] = err; tHist[tick] = t;
        ++tick;
        // подтяжка гейна (только вверх, как в коде)
        float want = 1.f;
        if (t >= c.trimAt && c.probe > 1e-4f) {
            want = p.gTrue / c.probe;
            if (want < 1.f) want = 1.f;
            if (want > 1.4f) want = 1.4f;
        }
        c.trim += (want - c.trim) * c.trimStep;
        const float gy = c.probe * c.trim;
        // окно «в полёте»
        float pendDeg = 0.f;
        if (c.mode == 5 || c.mode == 6) {
            // Поток камеры этого такта: насколько камера повернулась с
            // прошлого такта (в игре — медианный сдвиг боксов по совпавшему
            // id; здесь угол камеры известен точно). Виден при >= 3 боксов.
            float flow = 0.f;
            bool flowOk = false;
            if (c.haveCamFlow && p.nBoxes >= 3) { flow = camAngle - c.lastCam; flowOk = true; }
            c.lastCam = camAngle; c.haveCamFlow = true;
            if (c.haveLastErr) {
                if (c.mode == 5) {
                    // Старый способ: списание по падению остатка.
                    const float absorbed = c.lastErr - err;   // >0 — остаток упал
                    if (absorbed > 0.f && c.pending != 0.f) {
                        if (c.pending > 0.f) c.pending = std::max(0.f, c.pending - absorbed);
                        else                 c.pending = std::min(0.f, c.pending + absorbed);
                        c.pendAge = 0.f;
                    }
                } else {
                    // Гибрид: списание в первую очередь по потоку камеры —
                    // остаток может стоять и у применённого приказа (цель сама
                    // ушла, пока приказ летел), а поворот камеры не врёт.
                    if (flowOk) {
                        if (c.pending > 0.f && flow > 0.f) {
                            c.pending = (flow >= c.pending) ? 0.f : c.pending - flow;
                            c.pendAge = 0.f;
                        } else if (c.pending < 0.f && flow < 0.f) {
                            c.pending = (-flow >= -c.pending) ? 0.f : c.pending - flow;
                            c.pendAge = 0.f;
                        }
                    } else {
                        // Дуэль 1 на 1: поток не посчитался, запасной способ.
                        const float absorbed = c.lastErr - err;
                        if (c.pending > 0.f && absorbed > 0.f) {
                            c.pending = std::max(0.f, c.pending - absorbed); c.pendAge = 0.f;
                        } else if (c.pending < 0.f && absorbed < 0.f) {
                            c.pending = std::min(0.f, c.pending - absorbed); c.pendAge = 0.f;
                        }
                    }
                    // Остаток перескочил ноль — висящий заказ толкнёт дальше.
                    if (c.lastErr * err < 0.f) c.pending = 0.f;
                }
            }
            c.lastErr = err; c.haveLastErr = true;
            if (c.pending != 0.f) {
                c.pendAge += tickDt;
                const float stall = (c.mode == 6) ? c.stallTime : 0.4f;
                if (c.pendAge > stall) c.pending = 0.f;   // ввод потерялся
            }
            if (c.mode == 6 && flowOk) {
                // Заказ не может превышать текущий остаток: цель сближается
                // сама. Только при видимом потоке камеры: без потока падение
                // остатка — единственный сигнал отработки (двойное списание).
                if (c.pending > 0.f && c.pending > err) c.pending = (err > 0.f) ? err : 0.f;
                if (c.pending < 0.f && c.pending < err) c.pending = (err < 0.f) ? err : 0.f;
            }
            pendDeg = c.pending;
            if (c.mode == 6) {
                float winSum = 0.f;
                for (auto& o : c.ord) o.age += tickDt;
                c.ord.erase(std::remove_if(c.ord.begin(), c.ord.end(),
                            [&](const TouchCtrl::Ord& o) { return o.age >= c.kLife; }), c.ord.end());
                for (const auto& o : c.ord) winSum += o.px * gy;
                // С потоком: захват правит поглощение (любая задержка
                // трубы), ведение — минимум с окном. Без потока поглощению
                // сливаться нечем — оценка всегда минимум с окном.
                if ((std::fabs(err) <= c.inflightBand || !flowOk) &&
                    std::fabs(winSum) < std::fabs(pendDeg)) pendDeg = winSum;
            }
        } else {
            for (auto& o : c.ord) o.age += tickDt;
            c.ord.erase(std::remove_if(c.ord.begin(), c.ord.end(),
                        [&](const TouchCtrl::Ord& o) { return o.age >= c.kLife; }), c.ord.end());
            for (const auto& o : c.ord) pendDeg += o.px * gy;
        }
        float eff = err;
        if (c.mode >= 1) eff = err - pendDeg;
        if (c.mode >= 2 && err * eff < 0.f) { prevStep = 0.f; continue; } // защита знака
        if (std::fabs(eff) < dead) { c.streak = 0; prevStep = 0.f; continue; }
        const float jitterBand = (p.rangeM > 0.f && p.rangeM < 2.5f) ? 1.2f : c.jitterBand;
        if (c.mode == 6 && std::fabs(eff) < jitterBand) {
            // Полоса дрожи: шаг только при устойчивом однонаправленном остатке.
            const int sg = (eff > 0.f) ? 1 : -1;
            c.streak = (c.streak * sg > 0) ? c.streak + sg : sg;
            if (c.streak * sg < c.driftStreak) { prevStep = 0.f; continue; }
        } else if (c.mode == 6) c.streak = 0;
        float k = c.kStable;
        const bool flip = c.havePrev && c.prevErr * eff < 0.f &&
                          std::fabs(eff) > std::fabs(c.prevErr) * 0.35f;
        if (flip) c.flipDamp = 0.45f;
        else if (c.flipDamp < 1.f) {
            c.flipDamp += (1.f - c.flipDamp) * (tickDt * 2.5f);
            if (c.flipDamp > 1.f) c.flipDamp = 1.f;
        }
        c.prevErr = eff; c.havePrev = true;
        k *= c.flipDamp;
        c.acquire += tickDt / c.softT;
        if (c.acquire > 1.f) c.acquire = 1.f;
        k *= 0.3f + 0.7f * c.acquire;
        if (c.mode == 6) {
            // Доля шага спадает вместе с остатком: поздние прилёты трубы.
            float ramp = std::fabs(eff) / c.rampBand;
            if (ramp > 1.f) ramp = 1.f;
            if (ramp < c.rampMin) ramp = c.rampMin;
            k *= ramp;
        }
        float dx = eff * k / gy;
        if (std::fabs(eff) < gy * 3.f) dx = std::round(eff / gy);
        if (std::fabs(dx * gy) > std::fabs(eff)) dx = eff / gy;
        if (dx > 54.f) dx = 54.f;
        if (dx < -54.f) dx = -54.f;
        dx = std::round(dx);
        if (dx != 0.f) {
            if (prevStep * dx < 0.f && t < 3.f) ++r.flips;
            prevStep = dx;
            cmd[f + p.visF] += dx * p.gTrue;
            if (c.mode == 5) { c.pending += dx * gy; c.pendAge = 0.f; }
            else if (c.mode == 6) { c.pending += dx * gy; c.pendAge = 0.f; c.ord.push_back({dx, 0.f}); }
            else c.ord.push_back({dx, 0.f});
        } else prevStep = 0.f;
    }
    int start = nTicks;
    for (int i = nTicks - 1; i >= 0; --i)
        if (std::fabs(errHist[i]) > dead) { start = i + 1; break; }
    if (start <= nTicks - 1) r.settle = tHist[start];
    for (int i = 0; i < nTicks; ++i)
        if (std::fabs(errHist[i]) <= 0.5f) { r.lock05 = tHist[i]; break; }
    bool crossedZero = false;
    for (int i = 1; i < nTicks; ++i) {
        if (!crossedZero && errHist[i] * p.target0 < 0.f) crossedZero = true;
        if (crossedZero) r.ring = std::max(r.ring, std::fabs(errHist[i]));
        if (errHist[i] * p.target0 < 0.f) r.maxOvershoot = std::max(r.maxOvershoot, std::fabs(errHist[i]));
    }
    r.settled3s = r.settle >= 0.f && r.settle <= 3.f;
    double s = 0.0; int n = 0;
    for (int i = 0; i < nTicks; ++i)
        if (tHist[i] >= 3.f) { s += std::fabs(errHist[i]); ++n; }
    r.trail = n ? (float)(s / n) : 0.f;
    return r;
}

// ============================ ПАМЯТЬ =========================================
struct MemCtrl {
    // mode 0 — старый код: доля остатка 0.5, потолок 2->12 с рампой 0.6 с,
    //          окно «последний приказ» 0.10 с, гаситель 0.85;
    // mode 1 — текущая «Память»: абсолютная цель кости, полный остаток
    //          (без вычета мёртвой зоны и без долей), с ограничением шага
    //          120°/с. Дрожь отсекает полоса у цели: внутри неё полный шаг
    //          выдаётся только при устойчивом однонаправленном дрейфе.
    int   mode = 1;
    float dead = 0.12f;
    float tau  = 0.030f;
    float turnRateDegPerSec = 120.f;
    float maxTurnDeg = 12.f;
    float jitterBand = 0.45f;   // полоса «дрожь кости», град
    int   driftStreak = 3;      // однонаправленных тактов для полного шага в полосе
    // mode 3 — «гросс»: текущий код (mode 0), но первые grossSecs секунд после
    //          захвата доля шага k повышена (0.9 с при 12 фпс ≈ 11 тактов —
    //          через 10-11 тактов соотношение углов и так установится).
    // Итог эксперимента (решение 27.09): гросс ХУЖЕ текущего по всей сетке —
    // при (1к/2к) перелёт 10.7° и 27 реверсов против 8.1° и 2; жирный
    // стартовый шаг качает петлю в обе стороны. Поэтому в prod пошли ДРОБНЫЕ
    // множители (огонь 0.5 / перезарядка 0.4 / доджи 0.15) и мягкий старт
    // тач-аима, а не усиление старта.
    float grossK = 1.5f;
    float grossSecs = 0.9f;
};

static Res runMem(const Plant& p, MemCtrl c, float* trace = nullptr, int traceN = 0) {
    const float frameDt = 1.f / 60.f;
    const float tickDt = p.overF * frameDt;
    const int nFrames = (int)(5.f / frameDt);
    const int nTicks = nFrames / p.overF + 1;
    std::vector<float> cmd(nFrames + 64, 0.f);      // добавка к накопителю (режим 0)
    std::vector<float> setv(nFrames + 64, NAN);     // абсолютная запись (режим 1): последнее значение побеждает
    std::vector<float> errHist(nTicks, 0.f), tHist(nTicks, 0.f);
    float acc = 0.f, tgt = p.target0;
    Res r{};
    float t = 0.f;
    int nextTick = 0, tick = 0;
    float prevStep = 0.f;
    int jitSign = 1;
    int streak = 0;             // однонаправленный дрейф (жёсткий лок)
    // память старого кода
    float filt = 0.f; bool haveFilt = false;
    float lastOrder = 0.f, lastAge = 1e9f;
    float prevErr = 0.f; bool havePrev = false;
    float flipDamp = 1.f, capLevel = 0.f;
    bool responded = false;
    float lastWrite = 0.f; bool haveWrite = false;
    // история накопителя для задержки видимости
    std::vector<float> hist;

    for (int f = 0; f < nFrames; ++f) {
        // кадр игры: применить запись. Режим 1 пишет АБСОЛЮТНЫЙ угол —
        // несколько записей до применения не складываются (последняя
        // побеждает), режим 0 пишет добавку.
        if (std::isfinite(setv[f])) acc = setv[f];
        acc += cmd[f];
        hist.push_back(acc);
        tgt += p.targetV * frameDt;
        if (f != nextTick) continue;
        nextTick += p.overF;
        t += tickDt;
        jitSign = -jitSign;
        const float jit = (p.jitter > 0.f && t >= p.jitStart) ? p.jitter * jitSign : 0.f;
        // видимая камера отстаёт на visF кадров
        const int idx = (int)hist.size() - 1 - p.visF;
        const float accVisible = hist[idx > 0 ? idx : 0];
        const float errRaw = tgt + jit - accVisible;
        errHist[tick] = errRaw; tHist[tick] = t;
        ++tick;

        if (c.mode == 1) {
            // ---- абсолютная цель + ограничение скорости ----------------------
            // Полный остаток задаёт конечный угол; slew-limit только растягивает
            // большой разворот во времени, а повторные абсолютные записи не
            // складываются на задержке применения.
            if (std::fabs(errRaw) < c.dead) { streak = 0; prevStep = 0.f; continue; }
            float off;
            if (std::fabs(errRaw) >= c.jitterBand) {
                off = errRaw;                          // настоящее движение
            } else {
                const int s = (errRaw > 0.f) ? 1 : -1; // дрожь-зона
                streak = (streak * s > 0) ? streak + s : s;
                if (streak * s >= c.driftStreak) off = errRaw;
                else { prevStep = 0.f; continue; }     // дрожь — стоим
            }
            const float absTarget = accVisible + off;
            const float maxTurn = std::fmin(c.turnRateDegPerSec * tickDt, c.maxTurnDeg);
            float step = absTarget - acc;
            if (step > maxTurn) step = maxTurn;
            if (step < -maxTurn) step = -maxTurn;
            if (prevStep * step < 0.f && t < 3.f && std::fabs(step) > c.dead) ++r.flips;
            prevStep = step;
            setv[f + 1] = acc + step;                  // игра применит след. кадром
            if (t >= p.jitStart && std::fabs(step) > r.jitCam) r.jitCam = std::fabs(step);
            continue;
        }

        // ---- текущий код (доля остатка + потолок + рампа) ----
        if (haveWrite && std::fabs(acc - lastWrite) < 0.1f) responded = true;
        lastAge += tickDt;
        const float inflight = (lastAge < 0.10f) ? lastOrder : 0.f;
        const float a = 1.f - std::exp(-tickDt / c.tau);
        if (!haveFilt) { filt = errRaw; haveFilt = true; }
        else filt += (errRaw - filt) * a;
        float eff = filt - inflight;
        if (std::fabs(eff) < c.dead) eff = 0.f;
        else eff -= (eff > 0.f ? c.dead : -c.dead);
        if (eff == 0.f) { prevStep = 0.f; continue; }
        float k = 0.5f;
        if (c.mode == 3 && t < c.grossSecs)   // гросс: стартовые секунды на повышенной доле
            k = c.grossK;
        const bool flip = havePrev && prevErr * eff < 0.f &&
                          std::fabs(eff) > std::fabs(prevErr) * 0.35f;
        if (flip) { flipDamp = 0.85f; if (responded) capLevel *= 0.5f; }
        else {
            if (flipDamp < 1.f) { flipDamp += (1.f - flipDamp) * (tickDt * 2.5f); if (flipDamp > 1.f) flipDamp = 1.f; }
            if (responded && capLevel < 1.f) { capLevel += tickDt / 0.6f; if (capLevel > 1.f) capLevel = 1.f; }
        }
        prevErr = eff; havePrev = true;
        k *= flipDamp;
        float step = eff * k;
        const float cap = responded ? 2.f + 10.f * capLevel : 2.f;
        if (step >  cap) step =  cap;
        if (step < -cap) step = -cap;
        if (prevStep * step < 0.f && t < 3.f) ++r.flips;
        prevStep = step;
        cmd[f + 1] += step;
        lastOrder = step; lastAge = 0.f;
        lastWrite = acc + step; haveWrite = true;
    }
    int start = nTicks;
    for (int i = nTicks - 1; i >= 0; --i)
        if (std::fabs(errHist[i]) > c.dead + 1e-3f) { start = i + 1; break; }
    if (start <= nTicks - 1) r.settle = tHist[start];
    for (int i = 0; i < nTicks; ++i)
        if (std::fabs(errHist[i]) <= 0.5f) { r.lock05 = tHist[i]; break; }
    bool crossedZero = false;
    for (int i = 1; i < nTicks; ++i) {
        if (!crossedZero && errHist[i] * p.target0 < 0.f) crossedZero = true;
        if (crossedZero) r.ring = std::max(r.ring, std::fabs(errHist[i]));
        if (errHist[i] * p.target0 < 0.f) r.maxOvershoot = std::max(r.maxOvershoot, std::fabs(errHist[i]));
    }
    r.settled3s = r.settle >= 0.f && r.settle <= 3.f;
    double s2 = 0.0; int n2 = 0;
    for (int i = 0; i < nTicks; ++i)
        if (tHist[i] >= 3.f) { s2 += std::fabs(errHist[i]); ++n2; }
    r.trail = n2 ? (float)(s2 / n2) : 0.f;
    if (trace && traceN > 0) {
        int n = std::min(traceN, nTicks);
        for (int i = 0; i < n; ++i) trace[i] = errHist[i];
    }
    return r;
}

int main() {
    const float dead = 0.2f;
    printf("===== ТАЧ, захват 30°: текущий код против суммы+защиты знака =====\n");
    printf("%-40s %-9s %-9s %-8s %-7s %-8s %s\n",
           "вариант", "0.5°,с", "захват,с", "перелёт°", "реверсы", "шлейф°", "устоялось");
    struct TRow { const char* name; int mode; } ;
    const TRow trows[] = {
        {"старый: последний приказ 0.12с",  0},
        {"поглощение по уменьшению ошибки", 5},
        {"гибрид: поток+арбитраж (текущий)", 6},
    };
    for (int overF : {1, 2, 5}) {
        for (int visF : {2, 4, 6, 8}) {
            for (const TRow& row : trows) {
                Plant p; p.target0 = 30.f; p.visF = visF; p.overF = overF;
                TouchCtrl c; c.mode = row.mode;
                Res r = runTouch(p, c, dead);
                printf("%-40s %-9.2f %-9.2f %-8.2f %-7d %-8.2f %s  (оверлей %dк, задержка %dк)\n",
                       row.name, r.lock05, r.settle, r.maxOvershoot, r.flips, r.trail,
                       r.settled3s ? "да" : "НЕТ", overF, visF);
            }
        }
    }
    // гейн неверен (запасной из настройки против фактического)
    printf("\n===== ТАЧ, неверный запасной гейн (оверлей 5к, задержка 4к) =====\n");
    for (float probe : {0.05f, 0.071f, 0.10f, 0.20f}) {
        for (const TRow& row : trows) {
            Plant p; p.target0 = 30.f; p.visF = 4; p.overF = 5;
            TouchCtrl c; c.mode = row.mode; c.probe = probe;
            Res r = runTouch(p, c, dead);
            printf("%-40s гейн=%.3f: 0.5° за %.2fс, захват %.2fс, перелёт %.2f°, реверсов %d %s\n",
                   row.name, probe, r.lock05, r.settle, r.maxOvershoot, r.flips,
                   r.settled3s ? "" : " НЕ УСТОЯЛОСЬ");
        }
    }
    // Крайний случай: запасной гейн завышен вдвое и оси камеры нет вовсе
    // (учиться нечему). Поглощение оценивает заказ в завышенных градусах и
    // контроллер ползёт к цели мелкими партиями после каждого таймаута
    // «потерялся». Обязано быть монотонно (без перелётов), пусть и медленно.
    {
        Plant p; p.target0 = 30.f; p.visF = 4; p.overF = 5; p.secs = 10.f;
        TouchCtrl c; c.mode = 6; c.probe = 0.20f;
        Res r = runTouch(p, c, dead);
        printf("\nгибрид, гейн 0.20 (завышен вдвое), горизонт 10 с: 0.5° за %.2f с, перелёт %.2f°, реверсов %d %s\n",
               r.lock05, r.maxOvershoot, r.flips,
               (r.lock05 > 0.f && r.maxOvershoot < 0.5f && r.flips == 0)
                   ? "(ползёт, но без маятника)" : "(ПЛОХО)");
    }
    // ведение цели
    printf("\n===== ТАЧ, ведение цели 30°/с =====\n");
    for (int overF : {1, 5}) {
        for (const TRow& row : trows) {
            Plant p; p.target0 = 5.f; p.targetV = 30.f; p.visF = 4; p.overF = overF;
            TouchCtrl c; c.mode = row.mode;
            Res r = runTouch(p, c, dead);
            printf("%-40s оверлей %dк: шлейф %.2f°, реверсов %d, перелёт %.2f°\n",
                   row.name, overF, r.trail, r.flips, r.maxOvershoot);
        }
    }
    // дрожь кости у цели
    printf("\n===== ТАЧ, дрожь кости ±0.25° у цели (захват 5°) =====\n");
    for (const TRow& row : trows) {
        Plant p; p.target0 = 5.f; p.visF = 4; p.overF = 5; p.jitter = 0.25f;
        TouchCtrl c; c.mode = row.mode;
        Res r = runTouch(p, c, dead);
        printf("%-40s шлейф %.2f°, реверсов %d\n", row.name, r.trail, r.flips);
    }
    // дуэль 1 на 1: поток камеры не считается (один бокс), работает запасной
    // способ списания + окно на ведении.
    printf("\n===== ТАЧ, дуэль 1 на 1 (поток не считается): захват 30° и ведение 30°/с =====\n");
    for (const TRow& row : trows) {
        Plant p; p.target0 = 30.f; p.visF = 4; p.overF = 5; p.nBoxes = 1;
        TouchCtrl c; c.mode = row.mode;
        Res r = runTouch(p, c, dead);
        Plant p2; p2.target0 = 5.f; p2.targetV = 30.f; p2.visF = 4; p2.overF = 5; p2.nBoxes = 1;
        Res r2 = runTouch(p2, c, dead);
        printf("%-40s захват: перелёт %.2f°, реверсов %d %s | ведение: шлейф %.2f°\n",
               row.name, r.maxOvershoot, r.flips,
               r.settled3s ? "" : " НЕ УСТОЯЛОСЬ", r2.trail);
    }
    // Динамический захват (жалоба 24.09, четвёртый заход по маятнику): цель
    // дрожит с самого начала, идёт навстречу, убегает; быстрый оверлей.
    // Мера маятника — реверсы шага и кольцо после перескока через ноль.
    printf("\n===== ТАЧ, динамический захват 30° =====\n");
    struct DRow { const char* name; Plant p; };
    const DRow drows[] = {
        {"дрожь ±0.25° с начала",          [] { Plant p; p.visF = 4; p.overF = 5; p.jitter = 0.25f; return p; }()},
        {"дрожь ±0.40°, быстрый оверлей",  [] { Plant p; p.visF = 4; p.overF = 1; p.jitter = 0.40f; return p; }()},
        {"цель идёт навстречу 10°/с",      [] { Plant p; p.visF = 4; p.overF = 5; p.targetV = -10.f; return p; }()},
        {"цель убегает 10°/с",             [] { Plant p; p.visF = 4; p.overF = 5; p.targetV = 10.f; p.secs = 8.f; return p; }()},
        {"чистый, задержка 8к (12 фпс)",   [] { Plant p; p.visF = 8; p.overF = 5; return p; }()},
    };
    for (const DRow& row : drows) {
        TouchCtrl c; c.mode = 6;
        Res r = runTouch(row.p, c, dead);
        printf("гибрид: %-32s перелёт %5.2f°, реверсов %2d, кольцо %.2f°, 0.5° за %.2f с %s\n",
               row.name, r.maxOvershoot, r.flips, r.ring, r.lock05,
               (r.lock05 > 0.f) ? "" : "(не сошлось)");
        if (r.flips > 1) printf("        МАЯТНИК: реверсов больше одного!\n");
    }

    // Close-range bone jitter is measured in degrees, not screen pixels.
    // Verify the production dead-zone and final-step helpers separately from
    // the existing long-range/latency scenarios above; close tolerance must no
    // longer swallow a one-degree residual.
    {
        const float far = AimTouchDeadDeg(0.0556f, 30.f, 0.1f);
        const float close = AimTouchDeadDeg(0.0556f, 0.8f, 0.1f);
        const float closeStep = AimTouchFinalGridStep(0.28f, 0.1f, 0.1f, 1.f, 0.2f, 1.3f);
        const float farStep = AimTouchFinalGridStep(1.0f, 0.1f, 0.1f, 1.f, 0.2f, 1.3f);
        Plant p; p.target0 = 8.f; p.rangeM = 0.8f; p.jitter = 0.7f; p.jitStart = 1.5f;
        TouchCtrl c;
        const Res r = runTouch(p, c, close);
        printf("\nТАЧ вплотную: дальняя зона %.2f°, ближняя %.2f°, шаг %.1f квант, реверсов %d\n",
               far, close, closeStep, r.flips);
        if (!(far <= 0.21f && close > far && close <= 0.31f &&
              closeStep == 1.f && farStep == 1.3f && r.flips <= 1)) return 1;
    }

    printf("\n===== ПАМЯТЬ, захват 30°: текущий код против жёсткого лока =====\n");
    printf("%-26s %-9s %-9s %-8s %-7s %s\n",
           "вариант", "0.5°,с", "захват,с", "перелёт°", "реверсы", "устоялось");
    for (int overF : {1, 5}) {
        for (int visF : {1, 2, 5, 7}) {
            Plant p; p.target0 = 30.f; p.visF = visF; p.overF = overF;
            MemCtrl c0; c0.mode = 0;
            Res r0 = runMem(p, c0);
            MemCtrl cG; cG.mode = 3; Res rG = runMem(p, cG);
            MemCtrl c1; c1.mode = 1;
            Res r1 = runMem(p, c1);
            printf("текущий (доля+потолок)     %-9.2f %-9.2f %-8.2f %-7d %s  (%dк/%dк)\n",
                   r0.lock05, r0.settle, r0.maxOvershoot, r0.flips,
                   r0.settled3s ? "да" : "НЕТ", overF, visF);
            printf("гросс (доля 1.5 первые 0.9с) %-9.2f %-9.2f %-8.2f %-7d %s  (%dк/%dк)\n",
                   rG.lock05, rG.settle, rG.maxOvershoot, rG.flips,
                   rG.settled3s ? "да" : "НЕТ", overF, visF);
            printf("абсолютная цель + slew     %-9.2f %-9.2f %-8.2f %-7d %s  (%dк/%dк)\n",
                   r1.lock05, r1.settle, r1.maxOvershoot, r1.flips,
                   r1.settled3s ? "да" : "НЕТ", overF, visF);
        }
    }
    // трасса границы фаз
    {
        float tr[40];
        Plant p; p.target0 = 30.f; p.visF = 7; p.overF = 5;
        MemCtrl c0; c0.mode = 0;
        runMem(p, c0, tr, 40);
        printf("\nтрасса текущей памяти (граница фаз 5к/7к), ° на такт:\n  ");
        for (int i = 0; i < 40; ++i) printf("%6.1f", tr[i]);
        printf("\n");
        MemCtrl c1; c1.mode = 1;
        runMem(p, c1, tr, 40);
        printf("трасса жёсткого лока, ° на такт:\n  ");
        for (int i = 0; i < 40; ++i) printf("%6.1f", tr[i]);
        printf("\n");
    }
    printf("\n===== ПАМЯТЬ, ведение цели 30°/с =====\n");
    for (int overF : {1, 5}) {
        for (int visF : {1, 4}) {
            Plant p; p.target0 = 5.f; p.targetV = 30.f; p.visF = visF; p.overF = overF;
            MemCtrl c0; c0.mode = 0; Res r0 = runMem(p, c0);
            MemCtrl cG; cG.mode = 3; Res rG = runMem(p, cG);
            MemCtrl c1; c1.mode = 1; Res r1 = runMem(p, c1);
            printf("оверлей %dк, задержка %dк: текущий шлейф %.2f° (реверсов %d) | гросс шлейф %.2f° (реверсов %d) | лок шлейф %.2f° (реверсов %d)\n",
                   overF, visF, r0.trail, r0.flips, rG.trail, rG.flips, r1.trail, r1.flips);
        }
    }
    printf("\n===== ПАМЯТЬ, дрожь кости ±0.15° и ±0.40° (сначала сходимость, потом дрожь) =====\n");
    for (float amp : {0.15f, 0.40f}) {
        Plant p; p.target0 = 8.f; p.visF = 1; p.overF = 5; p.jitter = amp; p.jitStart = 1.5f;
        MemCtrl c1; c1.mode = 1;
        Res r1 = runMem(p, c1);
        printf("жёсткий лок, дрожь ±%.2f°: камера ходит не больше %.3f° за такт %s\n",
               amp, r1.jitCam, r1.jitCam < 0.005f ? "(дрожь НЕ проходит — ок)" :
               r1.jitCam < amp ? "(меньше дрожи — ок)" : "(ПОВТОРЯЕТ ДРОЖЬ!)");
    }
    // медленный дрейф кости (цель идёт пешком): полоса дрожи обязана пропускать
    // устойчивое однонаправленное движение, иначе лок отстаёт от кости.
    printf("\n===== ПАМЯТЬ, медленный дрейф кости 3°/с (порог полосы дрейфа) =====\n");
    for (int overF : {1, 5}) {
        for (int visF : {1, 4}) {
            Plant p; p.target0 = 5.f; p.targetV = 3.f; p.visF = visF; p.overF = overF;
            MemCtrl c1; c1.mode = 1;
            MemCtrl cG; cG.mode = 3;
            Res r1 = runMem(p, c1);
            Res rG = runMem(p, cG);
            printf("оверлей %dк, задержка %dк: лок %.3f° | гросс %.3f° %s\n",
                   overF, visF, r1.trail, rG.trail,
                   (r1.trail < 0.8f && rG.trail < 0.8f) ? "(держат кость — ок)" : "(ОТСТАВАНИЕ!)");
        }
    }
    return 0;
}
