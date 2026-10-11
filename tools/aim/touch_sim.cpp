// Стенд тач-аима: модель петли «палец -> труба ввода Android -> камера игры ->
// остаток до кости» на одном такте оверлея с контроллером из aim_touch.h.
//
// Зачем. Жалобы «перелетает цель» и «двигается рывками» воспроизводятся только
// на петле с запаздыванием: контроллер заказывает поправку, камера отвечает
// через 2-8 кадров игры, и за это время заказ успевает накопиться. На
// устройстве такой прогон стоит минуты и не повторяем, здесь — миллисекунды и
// сетка параметров (частота оверлея 12-60 fps × кадры игры 8-60 fps × задержка
// трубы 2-8 кадров × коэффициент 0.05-0.14 град/px).
//
// Сборка и прогон:  ./scripts/aim_sim.sh
//
// Метрики (считаются раздельно по рысканью и тангажу):
//   перелёт   — максимум |остатка| обратного знака после первого захвата
//   реверсы   — смены знака команды пальца (видимая дрожь)
//   захват    — время до первого |остаток| < полосы захвата
//   СКЗ хвоста— среднеквадратичный остаток после захвата (ведение цели)
//   пик/сред  — наибольший поворот камеры за КАДР ИГРЫ к среднему повороту
//               (после захвата, в расчёт идут только кадры с поворотом):
//               1.0 — камера идёт ровно, 3 и выше — «стойка-прыжок». Считается
//               по кадрам игры, а не по тактам оверлея: в такт попадает то одна
//               пачка ввода, то две, и отношение меряло бы это попадание.
//               Абсолютный бросок для этого не годится: ветка, которая большую
//               часть тактов стоит, бросает мелко и выглядит «ровной»
//   рывок     — СКЗ изменения команды пальца от такта к такту, px: рывок,
//               который видит игрок, это рывок КОМАНДЫ (камера в любом случае
//               применяет ввод пачками раз в несколько кадров)
//   простой   — доля тактов после захвата, где команда не выдана, хотя остаток
//               больше полосы захвата: залипший заказ выглядит здесь как 1.0, а
//               на глаз это «аим не доводит цель и потом дёргает». Такты с
//               остатком внутри полосы не считаются: держать палец у стоящей
//               цели — правильное поведение, а не простой
//
// Модель растения. Касание уходит в трубу ввода не мгновенно: пакеты такта
// распределяются по его длительности (см. split_substeps), попадают в кадр
// игры по времени прихода и ждут там pipe_frames кадров до применения. Камера
// игры крутится на сумму доехавших сдвигов пальца, умноженную на истинный
// коэффициент град/px.
//
// Знаки — те же, что в aim.cpp, и выводятся из проекции: остаток аима это
// «градусы от прицела до кости, +право и +вверх». Заказ dx > 0 крутит камеру
// вправо и остаток падает; сдвиг пальца ВВЕРХ (dy < 0) поднимает камеру и
// уменьшает остаток тангажа. Поток камеры (aim_camera_flow) отдаётся в системе
// остатка: flowYaw = +appliedYaw,
// flowPitch = +appliedPitch. camYawDelta/camPitchDelta такта с подтверждением
// живут в знаках оси камеры, то есть противоположны падению остатка.

#include "aim_touch.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

struct Scenario {
    const char* name;
    float overlay_fps = 12.0f;   // такт контроллера
    float game_fps = 15.0f;      // такт растения (кадры игры)
    int pipe_frames = 4;         // кадров игры до применения касания
    float true_gain = 0.10f;     // град/px у игры
    float base_gain = 0.10f;     // что знает контроллер (из настройки)
    float trim = 1.0f;           // подтяжка замером
    float speed = 5.0f;          // слайдер 1..10
    float start_yaw = 30.0f;     // начальный остаток, град
    float start_pitch = 6.0f;
    float target_vyaw = 0.0f;    // собственное движение цели, град/с
    float target_vpitch = 0.0f;
    float jitter = 0.0f;         // амплитуда дрожи кости, град
    bool flow_ok = true;         // видно ли поток камеры (>= 3 боксов)
    bool have_camera_axis = false;
    bool estimate_only = true;   // коэффициент — из настройки, замеров нет
    float dip_at = -1.0f, dip_len = 0.0f, dip_fps = 12.0f;  // просадка ФПС игры
    float duration = 3.0f;
};

struct Axis {
    float overshoot = 0.0f;
    int reversals = 0;
    float acquire_time = -1.0f;
    float tail_rms = 0.0f;
    float max_step = 0.0f;   // наибольший поворот за такт оверлея после захвата, град
    float step_ratio = 0.0f; // пик хода к среднему ходу: 1 — ровно, много — «стойка-прыжок»
    float jerk = 0.0f;       // СКЗ изменения команды пальца от такта к такту, px
    float hold = 0.0f;       // доля тактов после захвата, где команды не было
};

struct Metrics {
    Axis yaw, pitch;
    float travel_px = 0.0f;
};

// Детерминированный шум дрожи кости: сумма двух несоизмеримых синусоид, чтобы
// знак чередовался как у настоящей дрожи, но прогон оставался повторяемым.
float bone_jitter(float t, float amp) {
    if (amp <= 0.0f) return 0.0f;
    return amp * (std::sin(t * 41.0f) * 0.6f + std::sin(t * 97.0f + 1.3f) * 0.4f);
}

struct Packet { float dx, dy, at; };

bool g_trace = false;
std::vector<Scenario> cases();

// Порядковый номер сценария: для --trace, чтобы трейс не тонул в выводе стенда.
int csIndex(const Scenario& sc) {
    static const std::vector<Scenario> all = cases();
    for (size_t i = 0; i < all.size(); ++i)
        if (all[i].name == sc.name) return (int)i;
    return -1;
}

int g_traceIdx = -1;

Metrics run(const Scenario& sc, bool soft, int substeps) {
    aim_touch::Params p;
    p.screen_height = 1080.0f;
    p.units_per_px = 1.0f;
    p.base_gain = sc.base_gain;
    p.trim = sc.trim;
    p.gain_from_sens = true;
    p.estimate_only = sc.estimate_only;
    p.speed = sc.speed;
    p.deg_per_px = 60.0f / 1080.0f;
    p.world_dist = 20.0f;
    p.have_camera_axis = sc.have_camera_axis;
    p.soft = soft;
    p.substeps = substeps;

    aim_touch::Controller ctl;
    ctl.reset();

    const float dtOverlay = 1.0f / sc.overlay_fps;
    float camYaw = 0.0f, camPitch = 0.0f;      // поворот камеры в знаках остатка
    float tgtYaw = sc.start_yaw, tgtPitch = sc.start_pitch;  // абсолютный угол кости
    std::vector<Packet> pipe;                   // пакеты в трубе ввода
    float queuedYaw = 0.0f, queuedPitch = 0.0f; // доехало, ждёт применения
    int pipeStage = 0;
    float gameAccum = 0.0f;
    float fingerX = 0.74f * 2340.0f, fingerY = 0.5f * 1080.0f;
    float pendDx = 0.0f, pendDy = 0.0f, pendTime = 0.0f;

    Metrics m;
    const float tailFrom = sc.duration * 0.6f;   // хвост считаем по последней трети
    struct Track {
        bool acquired = false;
        float prevErr = 0.0f, lastCmd = 0.0f;
        bool haveCmd = false;
        double tailSum = 0.0; int tailN = 0;
        int rateN = 0, holdN = 0, cmdN = 0;
        float maxStep = 0.0f, lastCmdMag = 0.0f;
        double stepSum = 0.0; int stepN = 0;
        double jerkSum = 0.0;
    } trY, trP;
    trY.prevErr = sc.start_yaw;
    trP.prevErr = sc.start_pitch;

    // Полоса захвата едет за собственной скоростью цели: на ведущей цели
    // остаток не уходит в ноль, а держится на лаге порядка v * задержка трубы.
    const float lockBand = 1.0f + std::fabs(sc.target_vyaw) * 0.10f;
    const float lockBandP = 1.0f + std::fabs(sc.target_vpitch) * 0.10f;

    const int ticks = (int)(sc.duration * sc.overlay_fps);
    for (int i = 0; i < ticks; ++i) {
        const float t = (float)i * dtOverlay;

        // ---- растение: кадры игры внутри такта оверлея ----
        float gameFps = sc.game_fps;
        if (sc.dip_at >= 0.0f && t >= sc.dip_at && t < sc.dip_at + sc.dip_len)
            gameFps = sc.dip_fps;
        gameAccum += dtOverlay;
        float appliedYaw = 0.0f, appliedPitch = 0.0f;
        while (gameAccum >= 1.0f / gameFps) {
            gameAccum -= 1.0f / gameFps;
            const float now = t + dtOverlay - gameAccum;
            for (size_t k = 0; k < pipe.size();) {
                if (pipe[k].at <= now) {
                    queuedYaw += pipe[k].dx;
                    queuedPitch += pipe[k].dy;
                    pipe[k] = pipe.back();
                    pipe.pop_back();
                } else {
                    ++k;
                }
            }
            if (++pipeStage >= sc.pipe_frames) {
                pipeStage = 0;
                // Тангаж: палец вверх (dy < 0) поднимает камеру, остаток падает.
                const float stepYaw = queuedYaw * sc.true_gain;
                const float stepPitch = -queuedPitch * sc.true_gain;
                queuedYaw = queuedPitch = 0.0f;
                // Ровность хода считается по кадрам игры, а не по тактам
                // оверлея: в такт попадает то одна пачка ввода, то две, и
                // отношение пика к среднему меряло бы это попадание, а не то,
                // как камера идёт.
                if (trY.acquired && std::fabs(stepYaw) > 0.02f) {
                    if (std::fabs(stepYaw) > trY.maxStep) trY.maxStep = std::fabs(stepYaw);
                    trY.stepSum += std::fabs((double)stepYaw);
                    ++trY.stepN;
                }
                if (trP.acquired && std::fabs(stepPitch) > 0.02f) {
                    if (std::fabs(stepPitch) > trP.maxStep) trP.maxStep = std::fabs(stepPitch);
                    trP.stepSum += std::fabs((double)stepPitch);
                    ++trP.stepN;
                }
                camYaw += stepYaw;
                camPitch += stepPitch;
                appliedYaw += stepYaw;
                appliedPitch += stepPitch;
            }
        }
        const float flowYaw = appliedYaw;
        const float flowPitch = appliedPitch;

        // ---- цель ----
        tgtYaw += sc.target_vyaw * dtOverlay;
        tgtPitch += sc.target_vpitch * dtOverlay;
        const float rawYaw = tgtYaw - camYaw + bone_jitter(t, sc.jitter);
        const float rawPitch = tgtPitch - camPitch + bone_jitter(t + 3.7f, sc.jitter * 0.7f);

        // ---- контроллер ----
        const bool camMoved = std::fabs(appliedYaw) > 0.02f || std::fabs(appliedPitch) > 0.02f;
        aim_touch::Output out = ctl.tick(p, fingerX, fingerY, dtOverlay,
                                               rawYaw, rawPitch,
                                               camMoved, -appliedYaw, -appliedPitch,
                                               sc.flow_ok, flowYaw, flowPitch,
                                               pendDx, pendDy, pendTime);
        if (g_trace && g_traceIdx >= 0 && csIndex(sc) == g_traceIdx)
            std::printf("  t=%.2f raw=%6.2f err=%6.2f k=%.3f ord=%7.2f D=%.3f age=%.2f vT=%6.2f abs=%5.2f\n",
                        t, rawYaw, out.err_yaw, out.k_gain, out.dx, ctl.flight.delay,
                        ctl.flight.meanAge(), ctl.targetRateYaw, ctl.flight.absorbYaw);
        if (out.action == aim_touch::kMove) {
            fingerX += out.dx;
            fingerY += out.dy;
            m.travel_px += std::fabs(out.dx) + std::fabs(out.dy);
            // Пакеты такта приходят в трубу не одновременно, а размазаны по
            // его длительности: на устройстве split_substeps шлёт их отдельными
            // записями uinput подряд, и игра успевает разобрать их по разным
            // кадрам. Без этого стенд сливал все пакеты такта в одну пачку и
            // дробление не давало ничего.
            const int nParts = (out.parts > 0) ? out.parts : 1;
            for (int part = 0; part < nParts; ++part) {
                const float prevX = part ? out.partX[part - 1] : 0.0f;
                const float prevY = part ? out.partY[part - 1] : 0.0f;
                const float at = t + dtOverlay * ((float)part + 0.5f) / (float)nParts;
                pipe.push_back({out.partX[part] - prevX, out.partY[part] - prevY, at});
            }
            const float cmdYaw = out.dx * p.base_gain * p.trim;
            const float cmdPitch = -out.dy * p.base_gain * p.trim;
            if (trY.haveCmd && cmdYaw != 0.0f && trY.lastCmd != 0.0f &&
                (cmdYaw > 0.0f) != (trY.lastCmd > 0.0f))
                ++m.yaw.reversals;
            if (trP.haveCmd && cmdPitch != 0.0f && trP.lastCmd != 0.0f &&
                (cmdPitch > 0.0f) != (trP.lastCmd > 0.0f))
                ++m.pitch.reversals;
            if (cmdYaw != 0.0f) { trY.lastCmd = cmdYaw; trY.haveCmd = true; }
            if (cmdPitch != 0.0f) { trP.lastCmd = cmdPitch; trP.haveCmd = true; }
        }

        // ---- гладкость команды пальца ----
        // Рывок, который видит игрок, — это рывок КОМАНДЫ: камера применяет
        // ввод пачками раз в несколько кадров игры в любом случае, и пила
        // остатка есть свойство растения, а не контроллера (стенд: остаток
        // 0.4..3.1° при нулевом среднем отставании на ведении 8°/с).
        {
            const float mag = (out.action == aim_touch::kMove)
                            ? std::sqrt(out.dx * out.dx + out.dy * out.dy) : 0.0f;
            if (trY.acquired) {
                const float d = mag - trY.lastCmdMag;
                trY.jerkSum += (double)d * d;
                ++trY.cmdN;
                // Простой считается только там, где команда была должна идти:
                // после захвата стоящей цели остаток в мёртвой зоне, и держать
                // палец — правильное поведение, а не рывок.
                if (mag == 0.0f && std::fabs(rawYaw) > lockBand) ++trY.holdN;
                trY.lastCmdMag = mag;
            }
        }

        // ---- метрики по осям ----
        auto track = [](Axis& ax, Track& tr, float raw, float applied, float dt,
                        float band, float tNow) {
            if (tr.acquired && raw * tr.prevErr < 0.0f && std::fabs(raw) > ax.overshoot)
                ax.overshoot = std::fabs(raw);
            // Ровность хода имеет смысл только после захвата: на сведении
            // камера и должна идти с максимальной скоростью.
            if (tr.acquired) ++tr.rateN;
            if (!tr.acquired && std::fabs(raw) < band) {
                tr.acquired = true;
                ax.acquire_time = tNow;
            }
        };
        track(m.yaw, trY, rawYaw, appliedYaw, dtOverlay, lockBand, t);
        track(m.pitch, trP, rawPitch, appliedPitch, dtOverlay, lockBandP, t);
        trY.prevErr = rawYaw;
        trP.prevErr = rawPitch;
        // Хвост — СКЗ остатка за последнюю треть прогона. Считать его «после
        // захвата» нельзя: на движущейся цели остаток проходит полосу на пути к
        // ней, и метрика доставалась одному случайному провалу (старая ветка на
        // «ведении 8°/с» показывала хвост 0.20 при остатке, который потом
        // качался между 2.9 и 5.5 град).
        if (t >= tailFrom) {
            trY.tailSum += (double)rawYaw * rawYaw; ++trY.tailN;
            trP.tailSum += (double)rawPitch * rawPitch; ++trP.tailN;
        }
    }
    if (trY.tailN) m.yaw.tail_rms = (float)std::sqrt(trY.tailSum / trY.tailN);
    if (trP.tailN) m.pitch.tail_rms = (float)std::sqrt(trP.tailSum / trP.tailN);
    auto finish = [](Axis& ax, Track& tr) {
        // Метрики ровности считаются и при малом числе тактов: ветка, которая
        // после захвата почти не двигает камеру, иначе получила бы нули и
        // автоматически «прошла» бы порог ровности.
        if (tr.rateN < 3) {
            const float mean0 = tr.stepN ? (float)(tr.stepSum / tr.stepN) : 0.0f;
            ax.step_ratio = (tr.stepN >= 3 && mean0 > 0.02f) ? tr.maxStep / mean0 : 1.0f;
            return;
        }
        const float mean = tr.stepN ? (float)(tr.stepSum / tr.stepN) : 0.0f;
        ax.step_ratio = (tr.stepN >= 3 && mean > 0.02f) ? tr.maxStep / mean : 1.0f;
        ax.max_step = tr.maxStep;
        if (tr.cmdN > 1) ax.jerk = (float)std::sqrt(tr.jerkSum / tr.cmdN);
        ax.hold = tr.rateN ? (float)tr.holdN / (float)tr.rateN : 0.0f;
    };
    finish(m.yaw, trY);
    finish(m.pitch, trP);
    return m;
}

std::vector<Scenario> cases() {
    std::vector<Scenario> v;
    {   // Штатный режим устройства: оси камеры нет, оверлей 12 fps, игра 15 fps,
        // труба 4 кадра (~0.27 с).
        Scenario s; s.name = "захват 30° 12/15 труба4"; v.push_back(s); }
    {   // Игра быстрее оверлея: здесь дробление такта на пакеты и работает.
        Scenario s; s.name = "захват 30° 12/60 труба2";
        s.game_fps = 60.0f; s.pipe_frames = 2; v.push_back(s); }
    {   Scenario s; s.name = "захват 30° 60/60 труба2";
        s.overlay_fps = 60.0f; s.game_fps = 60.0f; s.pipe_frames = 2; v.push_back(s); }
    {   Scenario s; s.name = "захват 30° 12/15 труба8";
        s.pipe_frames = 8; v.push_back(s); }
    {   // Цель идёт навстречу: остаток закрывается и сам.
        Scenario s; s.name = "встречная цель 12°/с";
        s.start_yaw = 20.0f; s.target_vyaw = -12.0f; v.push_back(s); }
    {   // Ведение: цель уходит вбок, дрожь кости ±0.4°.
        Scenario s; s.name = "ведение 8°/с + дрожь";
        s.start_yaw = 3.0f; s.start_pitch = 0.5f; s.target_vyaw = 8.0f;
        s.jitter = 0.4f; s.duration = 4.0f; v.push_back(s); }
    {   // Дуэль 1 на 1: потока камеры нет, поглощение сливается по остатку.
        Scenario s; s.name = "дуэль без потока";
        s.flow_ok = false; v.push_back(s); }
    {   // Просадка ФПС игры: жалоба «маятник при посадке ФПС».
        Scenario s; s.name = "просадка ФПС 1.0-2.0с";
        s.dip_at = 1.0f; s.dip_len = 1.0f; s.dip_fps = 8.0f; s.duration = 4.0f;
        v.push_back(s); }
    {   // Завышенный рабочий коэффициент: подтяжка 1.4x без замера камеры.
        Scenario s; s.name = "trim 1.4 (догадка)";
        s.trim = 1.4f; v.push_back(s); }
    {   // Низкая чувствительность в игре.
        Scenario s; s.name = "чувствительность 0.05";
        s.true_gain = 0.05f; s.base_gain = 0.05f; v.push_back(s); }
    {   // Слайдер скорости на максимуме.
        Scenario s; s.name = "скорость 10";
        s.speed = 10.0f; v.push_back(s); }
    return v;
}

float worse(float a, float b) { return a > b ? a : b; }

void printRow(const char* label, const Metrics& a, const Metrics& b) {
    std::printf("%-26s перелёт %5.2f->%5.2f  реверсы %3d->%3d  захват %5.2f->%5.2f  "
                "хвост %5.2f->%5.2f  рывок %5.2f->%5.2f  простой %4.2f->%4.2f\n",
                label,
                worse(a.yaw.overshoot, a.pitch.overshoot), worse(b.yaw.overshoot, b.pitch.overshoot),
                a.yaw.reversals + a.pitch.reversals, b.yaw.reversals + b.pitch.reversals,
                worse(a.yaw.acquire_time, a.pitch.acquire_time),
                worse(b.yaw.acquire_time, b.pitch.acquire_time),
                worse(a.yaw.tail_rms, a.pitch.tail_rms), worse(b.yaw.tail_rms, b.pitch.tail_rms),
                a.yaw.jerk, b.yaw.jerk, a.yaw.hold, b.yaw.hold);
}

} // namespace

int main(int argc, char** argv) {
    int substeps = 4;
    if (argc > 1) substeps = std::atoi(argv[1]);
    std::printf("Стенд тач-аима: soft=false (прежняя математика, 1 пакет) -> soft=true (%d пакетов на такт)\n",
                substeps);
    std::printf("хвост — СКЗ остатка за последнюю треть, рывок — СКЗ изменения команды пальца (px/такт),\n"
                "простой — доля тактов после захвата без команды\n\n");
    const auto cs = cases();
    const bool csv = (argc > 2 && std::strcmp(argv[2], "--csv") == 0);
    g_trace = (argc > 3 && std::strcmp(argv[3], "--trace") == 0);
    g_traceIdx = (argc > 4) ? std::atoi(argv[4]) : 3;
    int bad = 0;
    for (const auto& sc : cs) {
        const Metrics a = run(sc, false, 1);
        const Metrics b = run(sc, true, substeps);
        if (csv) {
            std::printf("%s;%.3f;%.3f;%.3f;%.3f;%.3f;%.3f;%.3f;%.3f;%d;%d\n",
                        sc.name,
                        worse(a.yaw.overshoot, a.pitch.overshoot), worse(b.yaw.overshoot, b.pitch.overshoot),
                        worse(a.yaw.acquire_time, a.pitch.acquire_time), worse(b.yaw.acquire_time, b.pitch.acquire_time),
                        worse(a.yaw.tail_rms, a.pitch.tail_rms), worse(b.yaw.tail_rms, b.pitch.tail_rms),
                        a.yaw.max_step, b.yaw.max_step,
                        a.yaw.reversals + a.pitch.reversals, b.yaw.reversals + b.pitch.reversals);
            continue;
        }
        printRow(sc.name, a, b);
        if (!csv)
            std::printf("%-26s   по осям: захват Y %.2f->%.2f P %.2f->%.2f, хвост Y %.2f->%.2f "
                        "P %.2f->%.2f, пик/сред Y %.2f->%.2f, перелёт Y %.2f->%.2f\n", "",
                        a.yaw.acquire_time, b.yaw.acquire_time, a.pitch.acquire_time, b.pitch.acquire_time,
                        a.yaw.tail_rms, b.yaw.tail_rms, a.pitch.tail_rms, b.pitch.tail_rms,
                        a.yaw.step_ratio, b.yaw.step_ratio, a.yaw.overshoot, b.yaw.overshoot);
    }
    if (csv) return 0;
    std::printf("\n");

    // Пороги regression-проверки: новая ветка обязана быть не хуже прежней по
    // перелёту, рывкам и скорости захвата во всех сценариях сразу.
    float worstOldOver = 0.0f, worstNewOver = 0.0f, worstOldStep = 0.0f, worstNewStep = 0.0f;

    for (const auto& sc : cs) {
        const Metrics a = run(sc, false, 1);
        const Metrics b = run(sc, true, substeps);
        const float overA = worse(a.yaw.overshoot, a.pitch.overshoot);
        const float overB = worse(b.yaw.overshoot, b.pitch.overshoot);
        if (overA > worstOldOver) worstOldOver = overA;
        if (overB > worstNewOver) worstNewOver = overB;
        if (a.yaw.max_step > worstOldStep) worstOldStep = a.yaw.max_step;
        if (b.yaw.max_step > worstNewStep) worstNewStep = b.yaw.max_step;

        if (overB > overA + 0.15f) {
            std::printf("ПРОВАЛ %s: перелёт вырос %.2f -> %.2f\n", sc.name, overA, overB);
            ++bad;
        }
        // Ровность хода: пик поворота камеры к среднему повороту за такт.
        // Абсолютный бросок здесь не годится — прежняя ветка стояла 84% тактов
        // и потому «бросала» мало: стоять плавно не получается. Отношение
        // штрафует именно режим «стойка-прыжок», каким бы мелким ни был прыжок.
        // Ровность хода спрашивается только там, где камера обязана идти
        // непрерывно: на захвате стоящей цели после сведения простой — это и
        // есть результат, а отношение пика к среднему там меряет округление
        // последней поправки.
        const bool moving = std::fabs(sc.target_vyaw) > 0.5f || std::fabs(sc.target_vpitch) > 0.5f;
        // Допуск 0.75 взят не с потолка: пачка ввода на 12 fps оверлея и трубе
        // 0.27 с собирает 3.2 такта заказов, поэтому даже ровная команда даёт
        // отношение около 1.25, а дрожь кости добавляет ещё пару десятых.
        // Ловить порог должен режим «стойка-прыжок», а он даёт 5 и выше.
        if (moving && b.yaw.step_ratio > a.yaw.step_ratio + 0.75f && b.yaw.step_ratio > 2.0f) {
            std::printf("ПРОВАЛ %s: ход камеры неровный, пик/среднее %.2f -> %.2f "
                        "(бросок %.2f -> %.2f град)\n",
                        sc.name, a.yaw.step_ratio, b.yaw.step_ratio,
                        a.yaw.max_step, b.yaw.max_step);
            ++bad;
        }
        for (int axis = 0; axis < 2; ++axis) {
            const Axis& ax = axis ? b.pitch : b.yaw;
            const Axis& ay = axis ? a.pitch : a.yaw;
            if (ax.acquire_time < 0.0f) {
                std::printf("ПРОВАЛ %s: %s не захвачена\n", sc.name, axis ? "тангаж" : "рысканье");
                ++bad;
            } else if (ay.acquire_time > 0.0f &&
                       ax.acquire_time > ay.acquire_time * 1.35f + 0.25f) {
                std::printf("ПРОВАЛ %s: %s замедлилось %.2f -> %.2f\n", sc.name,
                            axis ? "тангаж" : "рысканье", ay.acquire_time, ax.acquire_time);
                ++bad;
            }
            if (ax.tail_rms > ay.tail_rms + 0.25f) {
                std::printf("ПРОВАЛ %s: хвост %s вырос %.2f -> %.2f\n", sc.name,
                            axis ? "тангажа" : "рысканья", ay.tail_rms, ax.tail_rms);
                ++bad;
            }
        }
    }
    std::printf("худший перелёт %.2f -> %.2f град, худший бросок %.2f -> %.2f град\n",
                worstOldOver, worstNewOver, worstOldStep, worstNewStep);
    if (bad) {
        std::printf("сценариев с провалом: %d\n", bad);
        return 1;
    }
    std::printf("пороги пройдены во всех %zu сценариях\n", cs.size());
    return 0;
}
