// Стенд «жёсткого лока» (жалоба 23.09, второй заход):
// «тач аим и мемори аим в первые 2 секунды при наводке двигаются как
//  маятник вправо-влево от цели».
//
// Проверяются два кандидата на замену текущим контроллерам:
//
//  ТАЧ: окно «в полёте» = СУММА всех приказов моложе kLife (а не только
//       последний) + защита знака: если приказы в полёте уже покрывают
//       остаток — новый приказ не выдаётся. Прежний вариант «только
//       последний приказ» недозакрывал окно на быстром оверлее (60 fps):
//       в полёте висело 3-4 приказа, а вычитался один — захват перелетал
//       цель и шёл маятником.
//
//  ПАМЯТЬ: жёсткий лок АБСОЛЮТНЫМ углом кости вместо пошагового
//       дробного контроллера (доля остатка + потолок + рампа). Запись в
//       накопитель идемпотентна: сколько раз её ни повтори, камера придёт
//       в одну и ту же точку, поэтому граница фаз (запись видна через
//       такт) раскачать петлю не может в принципе.
//
// Сборка: g++ -O2 -o /tmp/lock_sim tools/aim/lock_sim.cpp && /tmp/lock_sim

#include <cstdio>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <vector>

// ---------- физика ----------
struct Plant {
    float gTrue   = 0.10f;  // град поворота камеры на пиксель пальца
    int   visF    = 4;      // кадров игры от приказа до видимости в боксах
    int   overF   = 5;      // кадров игры на такт оверлея (5 -> ~12 fps)
    float target0 = 30.f;   // ошибка в момент захвата, град
    float targetV = 0.f;    // цель движется, град/с
    float jitter  = 0.f;    // дрожь кости, град (амплитуда, знак чередуется)
    float jitStart = 0.f;   // дрожь включается с этой секунды (после сходимости)
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
    // mode 5 — ПОГЛОЩЕНИЕ: заказ копится, а списывается по фактическому
    //          уменьшению ошибки (камера отработала — видно в остатке).
    //          Нет ни предположений о задержке, ни поедания отработанных
    //          приказов: сколько камера съела, столько и списано;
    // mode 6 — ГИБРИД: минимум из поглощения и суммарного окна. На захвате
    //          поглощение списывает отработанное (окно бы его ело), на
    //          ведении окно не даёт поглощению расти без границы (цель
    //          убегает — остаток не падает, и поглощение не списывается).
    int   mode = 6;
    float kLife = 0.13f;
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
    float pending = 0.f;      // mode 5: заказано, не отработано (град, знак шага)
    float pendAge = 0.f;      // mode 5: сколько заказ живёт без поглощения
    float lastErr = 0.f;      // mode 5: остаток прошлого такта
    bool  haveLastErr = false;
};

static Res runTouch(const Plant& p, TouchCtrl c, float dead) {
    const float frameDt = 1.f / 60.f;
    const float tickDt  = p.overF * frameDt;
    const int   nFrames = (int)(5.f / frameDt);
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
            // Поглощение: камера отработала часть заказа — это видно по тому,
            // насколько остаток уменьшился с прошлого такта. Заказ списывается
            // ровно по факту отработки: ни предположений о задержке, ни
            // поедания уже отработанных приказов (болезнь суммарного окна на
            // быстром оверлее).
            if (c.haveLastErr) {
                const float absorbed = c.lastErr - err;   // >0 — остаток упал
                if (absorbed > 0.f && c.pending != 0.f) {
                    if (c.pending > 0.f) c.pending = std::max(0.f, c.pending - absorbed);
                    else                 c.pending = std::min(0.f, c.pending + absorbed);
                    c.pendAge = 0.f;
                }
            }
            c.lastErr = err; c.haveLastErr = true;
            if (c.pending != 0.f) {
                c.pendAge += tickDt;
                if (c.pendAge > 0.4f) c.pending = 0.f;   // ввод потерялся
            }
            pendDeg = c.pending;
            if (c.mode == 6) {
                float winSum = 0.f;
                for (auto& o : c.ord) o.age += tickDt;
                c.ord.erase(std::remove_if(c.ord.begin(), c.ord.end(),
                            [&](const TouchCtrl::Ord& o) { return o.age >= c.kLife; }), c.ord.end());
                for (const auto& o : c.ord) winSum += o.px * gy;
                // минимум по величине: что консервативнее, тем и пользуемся
                if (std::fabs(winSum) < std::fabs(pendDeg)) pendDeg = winSum;
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
        if (std::fabs(eff) < dead) { prevStep = 0.f; continue; }
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
    // mode 0 — текущий код: доля остатка 0.5, потолок 2->12 с рампой 0.6 с,
    //          окно «последний приказ» 0.10 с, гаситель 0.85;
    // mode 1 — ЖЁСТКИЙ ЛОК: каждый такт пишется абсолютный угол кости
    //          (видимая камера + остаток == константа при неподвижной цели).
    int   mode = 1;
    float dead = 0.12f;
    float tau  = 0.030f;
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
    // память текущего кода
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
            // ---- жёсткий лок абсолютным углом ----
            // Цель пишется СРАЗУ по текущему остатку (без сглаживания: фильтр
            // тащил бы в запись запаздывающий «момент» и давал перелёт на
            // быстром оверлее). Дрожь кости гасится мёртвой зоной и долей у
            // самой цели: абсолютная запись сходится монотонно и так.
            float eff = errRaw;
            if (std::fabs(eff) < c.dead) { prevStep = 0.f; continue; }
            float off = eff - (eff > 0.f ? c.dead : -c.dead);
            // У самой цели — доля от шага: абсолютная запись сходится
            // монотонно и так, а дрожь кости в камеру проходит ослабленной.
            // За полосой — полный шаг: захват за один такт.
            if (std::fabs(eff) < 0.6f) off *= 0.4f;
            const float absTarget = accVisible + off;
            const float step = absTarget - acc;
            if (prevStep * step < 0.f && t < 3.f && std::fabs(step) > c.dead) ++r.flips;
            prevStep = step;
            setv[f + 1] = accVisible + off;            // игра применит след. кадром
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
        {"текущий: последний приказ 0.12с", 0},
        {"поглощение по уменьшению ошибки", 5},
        {"гибрид: мин(поглощение, окно)",   6},
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

    printf("\n===== ПАМЯТЬ, захват 30°: текущий код против жёсткого лока =====\n");
    printf("%-26s %-9s %-9s %-8s %-7s %s\n",
           "вариант", "0.5°,с", "захват,с", "перелёт°", "реверсы", "устоялось");
    for (int overF : {1, 5}) {
        for (int visF : {1, 2, 5, 7}) {
            Plant p; p.target0 = 30.f; p.visF = visF; p.overF = overF;
            MemCtrl c0; c0.mode = 0;
            Res r0 = runMem(p, c0);
            MemCtrl c1; c1.mode = 1;
            Res r1 = runMem(p, c1);
            printf("текущий (доля+потолок)     %-9.2f %-9.2f %-8.2f %-7d %s  (%dк/%dк)\n",
                   r0.lock05, r0.settle, r0.maxOvershoot, r0.flips,
                   r0.settled3s ? "да" : "НЕТ", overF, visF);
            printf("жёсткий лок (абсолют)      %-9.2f %-9.2f %-8.2f %-7d %s  (%dк/%dк)\n",
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
            MemCtrl c1; c1.mode = 1; Res r1 = runMem(p, c1);
            printf("оверлей %dк, задержка %dк: текущий шлейф %.2f° (реверсов %d) | лок шлейф %.2f° (реверсов %d)\n",
                   overF, visF, r0.trail, r0.flips, r1.trail, r1.flips);
        }
    }
    printf("\n===== ПАМЯТЬ, дрожь кости ±0.15° и ±0.40° (сначала сходимость, потом дрожь) =====\n");
    for (float amp : {0.15f, 0.40f}) {
        Plant p; p.target0 = 8.f; p.visF = 1; p.overF = 5; p.jitter = amp; p.jitStart = 1.5f;
        MemCtrl c1; c1.mode = 1;
        Res r1 = runMem(p, c1);
        printf("жёсткий лок, дрожь ±%.2f°: камера ходит не больше %.3f° за такт %s\n",
               amp, r1.jitCam, r1.jitCam < amp ? "(меньше дрожи — ок)" : "(ПОВТОРЯЕТ ДРОЖЬ!)");
    }
    return 0;
}
