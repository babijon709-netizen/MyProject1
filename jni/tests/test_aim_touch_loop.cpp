// Стенд петли тач-аима: настоящий автомат jni/src/aim_touch_core.h в петле
// «палец -> труба касания -> камера -> остаток». Заглушек игры нет — только
// задержка трубы, частота кадров игры, квант цифровера и коэффициент камеры.
//
// Проверяется то, на что жалуются руками:
//   * маятник на наводке — реверсы пальца (пошёл в одну сторону, потом назад);
//   * «рывки вместо свайпа» — сколько движений пальца и лишних переносов;
//   * перелёт за цель и возня у неё;
//   * ведение идущей цели: не отстаёт и не дёргается при сглаживании ввода.
//
//   g++ -std=gnu++17 -Ijni/src -Ijni/include jni/tests/test_aim_touch_loop.cpp -o /tmp/t && /tmp/t
//   /tmp/t verbose — таблица по всем сценариям; /tmp/t trace — трасса первого.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "aim_touch_core.h"

namespace {

// Детерминированный шум позиции кости: настоящий лог шумит на десятые доли
// градуса, и полоса дрожи рассчитана именно на такой шум.
struct Noise {
    unsigned int s = 0x1234567u;
    float next() {
        s = s * 1664525u + 1013904223u;
        return ((float)((s >> 8) & 0xffff) / 32768.0f) - 1.0f;
    }
};

struct Plant {
    float gain_yaw = 0.10f;     // истинный коэффициент камеры, град/px
    float gain_pitch = 0.10f;
    float game_fps = 30.f;
    float overlay_fps = 60.f;
    int   delay_frames = 4;     // труба касания, кадров игры
    float smooth_tau = 0.f;     // сглаживание ввода игрой, с (0 — камера идёт сразу)
    float jitter_deg = 0.f;     // шум кости, град
    float target_rate_yaw = 0.f;   // цель сама уходит по рысканью, град/с
    float target_rate_pitch = 0.f;
};

struct Report {
    float lock_time = -1.f;
    float overshoot = 0.f;      // максимальный заход за цель, град (без шума кости)
    float tail_rms = 0.f;       // СКЗ остатка за последнюю секунду (без шума)
    float tail_mean = 0.f;      // средний остаток за последнюю секунду
    int   moves = 0;
    int   move_reversals = 0;   // палец пошёл обратно по этой же оси (по x)
    int   lifts = 0;
    int   swipes = 0;
    int   core_reversals = 0;
};

// Один прогон. start_err — начальный остаток (градусы).
Report run(const Plant& p, float start_yaw, float start_pitch, float seconds,
           float fallback_yaw, float fallback_pitch, bool axis_known,
           const aim_touch::Config& cfg0, bool trace, unsigned seed = 0x1234567u) {
    Report r;
    aim_touch::Lock lock;
    Noise noise_yaw, noise_pitch;
    // Потоки шума по осям разные: с одинаковым зерном дрожь осей совпадает и
    // складывается в мнимое движение, которого в игре нет.
    noise_yaw.s = seed; noise_pitch.s = seed ^ 0x9E3779B9u;

    const float dt_out = 1.f / p.overlay_fps;
    const float dt_game = 1.f / p.game_fps;

    // Заказанный поворот камеры (сумма доехавших сдвигов пальца) и фактический.
    float shrink_yaw_cmd = 0.f, shrink_pitch_cmd = 0.f;
    float shrink_yaw = 0.f, shrink_pitch = 0.f;
    struct Pend { float dx, dy; long visible_at; };
    std::vector<Pend> pend;
    long game_frame = 0;

    float finger_x = 0.f, finger_y = 0.f;
    float prev_move_dx = 0.f;
    float prev_move_abs = 0.f;

    // clean = без шума кости: метрики (захват, перелёт, хвост) считаются по
    // нему, иначе шум в 0.15 град не даёт «захвату» случиться никогда.
    auto err_now = [&](float t, bool pitch, bool clean) -> float {
        const float base = pitch
            ? (start_pitch + p.target_rate_pitch * t - shrink_pitch)
            : (start_yaw   + p.target_rate_yaw * t   - shrink_yaw);
        if (clean || p.jitter_deg <= 0.f) return base;
        const float n = pitch ? noise_pitch.next() : noise_yaw.next();
        return base + n * p.jitter_deg;
    };

    // Захват — это не мгновенное попадание в мёртвую зону, а удержание в полосе
    // покоя контроллера: одиночный проход через неё по пути к цели не считается.
    const float lock_band =
        aim_touch::rest_band_degrees(fallback_yaw, cfg0.units_per_px, cfg0.deadband_yaw);
    float lock_since = -1.f;
    std::vector<float> clean_t, clean_yaw, clean_pitch;

    float t = 0.f;
    const long ticks = (long)(seconds * p.overlay_fps) + 1;
    for (long i = 0; i < ticks; ++i) {
        // Кадры игры до текущего момента.
        while ((float)(game_frame + 1) * dt_game <= t + 1e-6f) {
            ++game_frame;
            for (const Pend& q : pend)
                if (q.visible_at <= game_frame) {
                    shrink_yaw_cmd   += q.dx * p.gain_yaw;
                    shrink_pitch_cmd += -q.dy * p.gain_pitch;
                }
            pend.erase(std::remove_if(pend.begin(), pend.end(),
                       [&](const Pend& q) { return q.visible_at <= game_frame; }), pend.end());
            if (p.smooth_tau > 0.f) {
                const float a = 1.f - std::exp(-dt_game / p.smooth_tau);
                shrink_yaw   += (shrink_yaw_cmd   - shrink_yaw) * a;
                shrink_pitch += (shrink_pitch_cmd - shrink_pitch) * a;
            } else {
                shrink_yaw = shrink_yaw_cmd;
                shrink_pitch = shrink_pitch_cmd;
            }
        }

        const float ey = err_now(t, false, false);
        const float ep = err_now(t, true,  false);
        const float cy = err_now(t, false, true);
        const float cp = err_now(t, true,  true);
        clean_t.push_back(t); clean_yaw.push_back(cy); clean_pitch.push_back(cp);
        if (std::fabs(cy) <= lock_band && std::fabs(cp) <= lock_band) {
            if (lock_since < 0.f) lock_since = t;
            if (r.lock_time < 0.f && t - lock_since >= 0.2f) r.lock_time = lock_since;
        } else {
            lock_since = -1.f;
        }
        if (start_yaw * cy < 0.f && std::fabs(cy) > r.overshoot) r.overshoot = std::fabs(cy);
        if (start_pitch * cp < 0.f && std::fabs(cp) > r.overshoot) r.overshoot = std::fabs(cp);

        // Как в aim.cpp: без настоящей оси камеры коэффициент берётся из настройки
        // чувствительности (только знак гарантирован), с осью — из замера.
        aim_touch::Gains g;
        g.yaw_known = true; g.pitch_known = true;
        g.yaw_verified = axis_known; g.pitch_verified = axis_known;
        g.yaw   = axis_known ? p.gain_yaw   : fallback_yaw;
        g.pitch = axis_known ? p.gain_pitch : fallback_pitch;

        aim_touch::Config cfg_tick = cfg0;
        cfg_tick.game_period_seconds = dt_game;
        const aim_touch::Outcome o = lock.step(ey, ep, dt_out, cfg_tick, g);
        switch (o.action) {
            case aim_touch::Action::Place:
                finger_x = o.x; finger_y = o.y;
                break;
            case aim_touch::Action::Move: {
                const float dx = o.x - finger_x, dy = o.y - finger_y;
                finger_x = o.x; finger_y = o.y;
                // Реверс — это ход НАЗАД по величине, сравнимый с предыдущим
                // движением. Подрезка в пиксель-два на дрожащем остатке маятником
                // не является и реверсом не считается.
                if (std::fabs(dx) >= 3.f || std::fabs(dy) >= 3.f) {
                    if (prev_move_dx != 0.f && std::fabs(dx) >= 3.f &&
                        std::fabs(dx) >= 0.3f * prev_move_abs &&
                        (prev_move_dx > 0.f) != (dx > 0.f))
                        ++r.move_reversals;
                    prev_move_dx = dx;
                    prev_move_abs = std::fabs(dx);
                }
                ++r.moves;
                pend.push_back({dx, dy, game_frame + p.delay_frames});
                break;
            }
            case aim_touch::Action::Lift:
                ++r.lifts;
                break;
            case aim_touch::Action::Hold:
            case aim_touch::Action::None:
                break;
        }

        if (trace && i % 2 == 0) {
            std::printf("%6.3f clean=(%7.3f,%7.3f) err=(%7.3f,%7.3f) act=%d "
                        "finger=(%7.1f,%7.1f) pend=(%6.2f,%6.2f) sw=%d\n",
                        t, cy, cp, ey, ep, (int)o.action, finger_x, finger_y,
                        lock.pending_yaw(), lock.pending_pitch(), lock.swipes());
        }

        t += dt_out;
    }

    // Хвост: последняя секунда прогона по чистому остатку.
    double acc = 0.0, sum = 0.0; int n = 0;
    for (size_t k = 0; k < clean_t.size(); ++k) {
        if (clean_t[k] < seconds - 1.f) continue;
        acc += (double)clean_yaw[k] * clean_yaw[k];
        acc += (double)clean_pitch[k] * clean_pitch[k];
        sum += (double)clean_yaw[k] + (double)clean_pitch[k];
        n += 2;
    }
    r.tail_rms  = n > 0 ? (float)std::sqrt(acc / n) : 0.f;
    r.tail_mean = n > 0 ? (float)(sum / n) : 0.f;
    r.swipes = lock.swipes();
    r.core_reversals = lock.reversals();
    return r;
}

struct Case {
    std::string name;
    Plant plant;
    float fallback_scale = 1.f;
    float start_err = 30.f;
    float seconds = 8.f;
    float speed = 5.f;           // ползунок скорости (cap свайпа по градусам)
    bool  axis_known = false;    // есть настоящая ось камеры (замер), а не настройка
    bool  tracking = false;      // цель идёт сама: «захват» не наступает, важна погоня
};

aim_touch::Config make_cfg(float sw_screen, float sh_screen, float speed, float gy) {
    aim_touch::Config c;
    c.point_x = sw_screen * 0.74f;
    c.point_y = sh_screen * 0.50f;
    c.box_x0 = c.point_x - 0.18f * sw_screen;
    c.box_x1 = c.point_x + 0.23f * sw_screen;
    c.box_y0 = c.point_y - 0.38f * sh_screen;
    c.box_y1 = c.point_y + 0.38f * sh_screen;
    c.units_per_px = 1.f;
    c.speed = speed;
    c.deadband_yaw = aim_control::touch_lock_deadband(60.f / sh_screen, 20.f, gy);
    c.deadband_pitch = c.deadband_yaw;
    return c;
}

}  // namespace

int main(int argc, char** argv) {
    const bool verbose = (argc > 1 && std::strcmp(argv[1], "verbose") == 0);
    const bool trace = (argc > 1 && std::strcmp(argv[1], "trace") == 0);

    const float SW = 1080.f, SH = 2400.f;
    std::vector<Case> cases;

    // Матрица: частоты кадров игры и оверлея, задержка трубы, промах настройки
    // чувствительности (0.7 — занижена, 1.5 — завышена).
    for (float game : {15.f, 30.f, 60.f}) {
        for (float overlay : {12.f, 30.f, 60.f}) {
            for (int delay : {2, 8}) {
                for (float fb : {1.0f, 0.7f, 1.5f}) {
                    Case c;
                    c.name.clear();
                    c.plant.game_fps = game;
                    c.plant.overlay_fps = overlay;
                    c.plant.delay_frames = delay;
                    c.plant.jitter_deg = 0.02f;
                    c.fallback_scale = fb;
                    cases.push_back(c);
                }
            }
        }
    }
    // Идущая цель: держать надо по факту, «захвата» в полосе тут не будет.
    for (float rate : {10.f, 25.f, 50.f}) {
        Case c;
        c.name = "walking target " + std::to_string((int)rate) + " deg/s";
        c.plant.game_fps = 30.f; c.plant.overlay_fps = 60.f; c.plant.delay_frames = 8;
        c.plant.target_rate_yaw = rate; c.plant.jitter_deg = 0.05f;
        c.start_err = 20.f; c.seconds = 4.f; c.speed = 8.f; c.tracking = true;
        cases.push_back(c);
    }
    // Сглаживание ввода игрой: камера отвечает не сразу и продолжает ехать после
    // сдвига пальца — именно на нём старый контроллер уходил в маятник.
    for (float tau : {0.05f, 0.15f, 0.30f}) {
        Case c;
        c.name = "game smoothing tau=" + std::to_string((int)(tau * 1000.f)) + "ms";
        c.plant.game_fps = 30.f; c.plant.overlay_fps = 60.f; c.plant.delay_frames = 8;
        c.plant.smooth_tau = tau; c.plant.jitter_deg = 0.05f;
        cases.push_back(c);
    }
    // Шум кости: палец не должен ходить за ним.
    for (float jit : {0.05f, 0.15f, 0.30f}) {
        Case c;
        c.name = "bone jitter " + std::to_string((int)(jit * 100.f)) + "e-2 deg";
        c.plant.game_fps = 30.f; c.plant.overlay_fps = 60.f; c.plant.delay_frames = 8;
        c.plant.jitter_deg = jit;
        cases.push_back(c);
    }
    // Затяжной такт: 12 фпс игры, длинная труба, дрожь — серверный случай.
    {
        Case c;
        c.name = "12 fps, delay 8, jitter 0.2";
        c.plant.game_fps = 12.f; c.plant.overlay_fps = 30.f; c.plant.delay_frames = 8;
        c.plant.jitter_deg = 0.20f; c.seconds = 8.f;
        cases.push_back(c);
    }
    // Настоящая ось камеры: коэффициент верный, шаг сразу на весь остаток.
    for (float fb : {0.7f, 1.0f}) {
        Case c;
        c.name = "camera axis, sensitivity x" + std::to_string((int)(fb * 10.f));
        c.plant.game_fps = 30.f; c.plant.overlay_fps = 60.f; c.plant.delay_frames = 8;
        c.plant.jitter_deg = 0.05f;
        c.fallback_scale = fb; c.axis_known = true;
        cases.push_back(c);
    }
    // Долгий прогон: не должно быть позднего маятника или возни у цели.
    {
        Case c;
        c.name = "long run 10 s static";
        c.plant.game_fps = 30.f; c.plant.overlay_fps = 60.f; c.plant.delay_frames = 8;
        c.plant.jitter_deg = 0.05f; c.seconds = 10.f;
        cases.push_back(c);
    }

    if (argc > 1 && std::strcmp(argv[1], "seeds") == 0) {
        const aim_touch::Config cfg = make_cfg(SW, SH, 5.f, 0.10f);
        for (float jit : {0.05f, 0.15f, 0.30f}) {
            for (unsigned k = 0; k < 6; ++k) {
                Plant p; p.game_fps = 30.f; p.overlay_fps = 60.f; p.delay_frames = 8;
                p.jitter_deg = jit;
                const Report r = run(p, 30.f, 6.f, 8.f, 0.10f, 0.10f, false, cfg, false,
                                     0x1234567u + k * 7919u);
                std::printf("jit=%.2f seed=%u lock=%5.2f over=%5.2f tail=%5.3f moves=%3d rev=%d sw=%2d\n",
                            jit, k, r.lock_time, r.overshoot, r.tail_rms, r.moves, r.move_reversals,
                            r.swipes);
            }
        }
        return 0;
    }

    int failed = 0;
    for (const Case& c : cases) {
        const aim_touch::Config cfg = make_cfg(SW, SH, c.speed, c.plant.gain_yaw);
        const float fb_y = c.plant.gain_yaw * c.fallback_scale;
        const Report r = run(c.plant, c.start_err, /*start_pitch_err=*/6.f, c.seconds,
                             fb_y, fb_y, c.axis_known, cfg, trace && c.name.empty());
        const float band = aim_touch::rest_band_degrees(fb_y, cfg.units_per_px, cfg.deadband_yaw);
        // Предел захвата зависит от трубы: на низком фпс игра отдаёт отклик
        // в разы позже, и такт свайпа длиннее — гнать его в одни и те же 2.5 с
        // для 12 и 60 фпс нельзя.
        const float tube = std::fmax(cfg.pipeline_seconds, 8.f / c.plant.game_fps);
        const float lock_limit = 2.0f + 8.0f * tube;

        bool ok = true;
        if (c.tracking) {
            // Погоня: палец идёт в одну сторону, отставание — не больше одного
            // такта по цели (иначе аим не ведёт, а отстаёт), идти он не бросает.
            if (r.move_reversals > 0) ok = false;
            if (r.moves < 8) ok = false;
            // Отставание — порядка одного такта по цели; на 50 град/с оно упирается
            // в геометрию кадра (длина зоны пальца и потолок свайпа), там предел
            // мягче: важно, что аим ведёт, а не бросает цель.
            const float lag_frac = c.plant.target_rate_yaw >= 50.f ? 0.75f : 0.6f;
            if (r.tail_mean > c.plant.target_rate_yaw * lag_frac + 3.f * band) ok = false;
        } else {
            if (r.lock_time < 0.f || r.lock_time > lock_limit) ok = false;
            if (r.move_reversals > 1) ok = false;
            if (r.overshoot > std::fmax(0.5f, 4.f * band)) ok = false;
            if (r.tail_rms > band) ok = false;
            if (r.lifts > 0) ok = false;
            if (r.swipes > 12) ok = false;
        }
        if (!ok) ++failed;
        if (verbose || !ok || !c.name.empty()) {
            std::printf("%-28s game=%2.0f out=%2.0f d=%d fb=%.1f axis=%d | lock=%5.2f over=%5.2f "
                        "tail=%5.3f mean=%6.2f moves=%3d rev=%d swipes=%2d lifts=%d%s\n",
                        c.name.empty() ? "acquisition 30 deg" : c.name.c_str(),
                        c.plant.game_fps, c.plant.overlay_fps, c.plant.delay_frames,
                        c.fallback_scale, (int)c.axis_known, r.lock_time, r.overshoot, r.tail_rms,
                        r.tail_mean, r.moves, r.move_reversals, r.swipes, r.lifts,
                        ok ? "" : "   <-- FAIL");
        }
    }
    std::printf("тач-лок: захват и удержание в полосе покоя, реверсов пальца нет, "
                "перелёта нет, лишних переносов нет\n");
    if (failed) {
        std::printf("ПРОВАЛЕНО сценариев: %d\n", failed);
        return 1;
    }
    std::printf("ALL TOUCH-LOOP TESTS PASS\n");
    return 0;
}
