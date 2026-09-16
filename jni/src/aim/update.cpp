// aim/update.cpp — UpdateAim: один такт аимбота.
//
// Модуль разрезан из прежнего монолита jni/src/main.cpp;
// что здесь лежит и кто это зовёт — в шапке aim/update.h и в docs/CODE_MAP.md.


#include "app/common.h"
#include "aim/controller.h"
#include "aim/memory.h"
#include "esp/aim_mem.h"
#include "app/attach.h"
#include "ui/esp_overlay.h"
#include "ui/layout.h"
#include "ui/popover.h"
#include "ui/sheet.h"
#include "aim/update.h"

void UpdateAim(float dt) {
    static float s_fx = 0.f, s_fy = 0.f;         // finger position (px)
    static float s_lastCamYaw = 0.f, s_lastCamPitch = 0.f; // absolute camera angles
    static bool  s_haveLast = false;
    static float s_gainYaw = 0.f, s_gainPitch = 0.f; // deg per px, learned
    // Оценка коэффициента по реакции прицела (см. aim_learn.h): нужна там, где
    // настоящей оси камеры нет и выучить его из поворота нечем.
    static aim::GainTrack s_trackYaw, s_trackPitch;
    // Ввод, который камера ещё не отработала (низкий FPS игры): накопленный
    // сдвиг пальца с момента последнего НАБЛЮДАЕМОГО поворота камеры.
    static float s_pendDx = 0.f, s_pendDy = 0.f;
    static float s_pendTime = 0.f;
    // Был ли поворот камеры виден в ПРОШЛОМ кадре. Нужен такту с подтверждением:
    // поза камеры читается до того, как игра применит наш ввод, и строгое
    // «камера не двинулась в этом кадре» заставляло ждать лишний такт на каждом
    // шаге — ведение становилось в несколько раз медленнее.
    static bool  s_camMovedPrev = false;
    static unsigned long long s_lastId = 0;        // sticky target
    static int s_lastBone = -1;                    // слот точки прицела прошлого такта
    static int   s_lostFrames = 0;
    static int   s_holdFrames = 0;

    const bool menuOpen = g_sheet.visible || (g_pop.visible && !g_pop.closing);
    // Во время калибровки зон (тапом по экрану) аим отпускает палец и ничего не
    // трогает: иначе он водил бы камеру прямо под пальцем пользователя.
    bool active = g_state.aim_touch && g_esp_attached && !menuOpen &&
                  g_calibMode == 0 && !g_buildPrompt;

    // "Только с прицелом": only steer while the local player is ADS.
    if (active && g_state.aim_scope_only && !esp_local_player_is_aiming())
        active = false;

    if (dt <= 0.f || !std::isfinite(dt)) dt = 1.f / 60.f;
    if (dt > 0.1f) dt = 0.1f;

    // ---- Режим: «Тач» (палец) или «Мемори» (запись поворота в память) -----
    // Меняется во вкладке «Аим». Обе петли держат собственное состояние, поэтому
    // на смене режима старое сбрасывается целиком: иначе в новый режим приехал
    // бы незавершённый шаг пальца, а результат прежнего самотеста — устаревшие
    // адреса чужого мира. Самотест при этом запускается заново, и его состояние
    // («подбираю… / готово / нельзя») видно в меню словами.
    const bool memoryMode = (g_state.aim_mode == 1);
    static bool s_memModePrev = false;
    if (memoryMode != s_memModePrev) {
        s_memModePrev = memoryMode;
        AimReleaseFinger(s_fingerDown);
        s_haveLast = false; s_lastId = 0; s_lastBone = -1;
        s_pendDx = s_pendDy = 0.f; s_pendTime = 0.f;
        s_trackYaw.reset(); s_trackPitch.reset();
        if (memoryMode) esp_mem_aim_reset();
    }

    // Самотест и сторож дорожки записи живут отдельно от наведения — их надо
    // крутить и когда цели нет (иначе «подбираю…» застынет навсегда), но только
    // когда аим вообще активен: самотест делает пробные довороты, и в меню или
    // под пальцем игрока их быть не должно.
    //
    // Отказ устройства (MEM_AIM_UNSUPPORTED) самотест больше не трогает: там
    // работает запасная тач-ветка, а её камера — это уже чужое движение, на
    // котором ни один замер не сойдётся. Перепроверка будет на смене режима или
    // на перепривязке (esp_reset -> esp_mem_aim_reset).
    int memState = esp_mem_aim_state();
    if (active && memoryMode && memState != MEM_AIM_UNSUPPORTED) esp_mem_aim_tick(dt);
    // Самотест мог закончиться прямо в этом такте (нашёл дорожку или сдался) —
    // решение о довороте принимаем по свежему состоянию, а не по вчерашнему.
    if (memoryMode) memState = esp_mem_aim_state();

    if (!active) {
        AimReleaseFinger(s_fingerDown);
        s_haveLast = false; s_lastId = 0; s_lastBone = -1;
        s_lostFrames = 0; s_holdFrames = 0;
        s_trackYaw.reset(); s_trackPitch.reset();
        return;
    }

    float sw = (float) native_window_screen_x;
    float sh = (float) native_window_screen_y;
    if (displayInfo.width > displayInfo.height && displayInfo.width >= 100 && displayInfo.height >= 100) {
        sw = (float) displayInfo.width;  sh = (float) displayInfo.height;
    } else if (displayInfo.height > displayInfo.width && displayInfo.height >= 100 && displayInfo.width >= 100) {
        sw = (float) displayInfo.height; sh = (float) displayInfo.width;
    }
    if (sw < 100.f || sh < 100.f) { AimReleaseFinger(s_fingerDown); return; }

    // Точка, из которой аим водит палец: выбирается в меню тапом по экрану
    // (вкладка «Аим», см. g_calibMode == 3). Пока не задана — прежняя позиция
    // справа по центру, поэтому поведение по умолчанию не меняется.
    const float ptX = AimTouchFracX() * sw, ptY = AimTouchFracY() * sh;

    const std::vector<EspBox>& boxes = FrameBoxes(sw, sh);

    const float crossX = sw * 0.5f, crossY = sh * 0.5f;
    const float fovR = AimFovRadiusPx(sw, sh);
    float camFov = esp_camera_fov_deg();
    if (!(camFov > 1.f && camFov < 179.f)) camFov = 60.f;
    // degrees per pixel at the screen centre (vertical axis)
    const float degPerPx = camFov / sh;

    const int wantBone = (g_state.aim_bone < 0 || g_state.aim_bone > 2) ? 0 : g_state.aim_bone;

    // ---- choose target ----
    AimTarget best;
    float bestScore = 1e18f;
    for (const EspBox& b : boxes) {
        // Never pull onto a team mate / clan mate while that ESP category is on.
        if (g_state.esp_team && b.ally) continue;
        AimTarget t;
        // Exact bone, with graceful fallback to the next-best bone.
        // Slots: 0 head, 1 neck, 2 chest. Never fall back below the chest.
        static const int order[3][3] = {{0, 1, 2}, {1, 0, 2}, {2, 1, 0}};
        int usedBone = -1;
        // Прилипчивость точки прицела. Слоты головы, шеи и груди разнесены на
        // 0.2-0.3 м (это до полуградуса на дистанции), а читаются они не каждый
        // кадр: раньше слот перескакивал между ними из кадра в кадр, и прицел
        // мелко трясло в стороны. Пока цель та же и слот читается — держим его;
        // цепочка запасных слотов работает заново только когда он пропал.
        if (b.id != 0 && b.id == s_lastId && s_lastBone >= 0 && s_lastBone < 3 &&
            b.aim_valid[s_lastBone]) {
            usedBone = s_lastBone;
        } else {
            for (int k = 0; k < 3; ++k) {
                int bi = order[wantBone][k];
                if (b.aim_valid[bi]) { usedBone = bi; break; }
            }
        }
        if (usedBone >= 0) {
            t.bone = usedBone;
            t.yaw = b.aim_yaw[usedBone];  t.pitch = b.aim_pitch[usedBone];
            t.sx  = b.aim_pts[usedBone][0]; t.sy = b.aim_pts[usedBone][1];
            t.valid = std::isfinite(t.yaw) && std::isfinite(t.pitch) &&
                      std::isfinite(t.sx) && std::isfinite(t.sy);
        } else {
            // Box estimate (nothing else resolved): derive angles from pixels.
            // The box itself is already crouch-aware (KCC pose height).
            if (!std::isfinite(b.x1) || !std::isfinite(b.y1) || !std::isfinite(b.x2) || !std::isfinite(b.y2)) continue;
            float h = b.y2 - b.y1;
            if (h < 4.f || h > sh * 4.f) continue;
            const float frac = (wantBone == 0) ? 0.07f : (wantBone == 1) ? 0.15f : 0.30f;
            t.sx = (b.x1 + b.x2) * 0.5f;
            t.sy = b.y1 + frac * h;
            float ex = t.sx - crossX, ey = t.sy - crossY;
            t.yaw = atanf(ex / (sh * 0.5f) * tanf(camFov * 0.5f * (float)M_PI / 180.f)) * 180.f / (float)M_PI;
            t.pitch = -atanf(ey / (sh * 0.5f) * tanf(camFov * 0.5f * (float)M_PI / 180.f)) * 180.f / (float)M_PI;
            t.valid = true;
        }
        if (!t.valid) continue;
        t.id = b.id;
        const bool sticky = (s_lastId != 0 && b.id == s_lastId);
        if (t.sx < -sw || t.sx > sw * 2.f || t.sy < -sh || t.sy > sh * 2.f) continue;
        if (g_state.aim_pos && !sticky && (t.sx < 0.f || t.sy < 0.f || t.sx > sw || t.sy > sh)) continue;
        float dx = t.sx - crossX, dy = t.sy - crossY;
        t.dist = sqrtf(dx * dx + dy * dy);
        t.world_dist = b.distance;
        // The target we are already pulling to may briefly leave the FOV
        // circle (overshoot while the gain is still being learned) — keep it.
        if (t.dist > (sticky ? fovR * 2.f : fovR)) continue;

        // Target priority (see "Приоритет цели"):
        //   0 balanced  — crosshair distance and range, both normalised
        //                 (FOV radius, 120 m) and summed;
        //   1 crosshair — purely the pixel distance from the crosshair;
        //   2 range     — purely the world distance, crosshair only breaks ties.
        // Whatever the mode, the current target gets a strong preference so the
        // aim does not flip between two players standing next to each other.
        const float pixelTerm = t.dist / (fovR > 1.f ? fovR : 1.f);
        const bool  haveRange = (t.world_dist >= 0.f) && std::isfinite(t.world_dist);
        const float rangeTerm = haveRange ? (t.world_dist / 120.f) : 1.f;
        float score;
        switch (g_state.aim_priority) {
            case 1:  score = pixelTerm; break;
            case 2:  score = rangeTerm * 4.f + pixelTerm * 0.05f; break;
            default: score = pixelTerm + rangeTerm * 0.8f; break;
        }
        if (sticky) score *= 0.35f;
        if (score < bestScore) { bestScore = score; best = t; }
    }

    if (!best.valid) {
        // В мемори-режиме палец не нужен вовсе: отпускаем сразу, чтобы
        // оставшийся от прошлого режима тач не крутил камеру.
        if (memoryMode) AimReleaseFinger(s_fingerDown);
        // Keep the finger down briefly so a momentary read failure does not
        // register as a tap (tap-to-shoot in some layouts) or reset momentum.
        if (++s_lostFrames > 6) {
            AimReleaseFinger(s_fingerDown);
            s_haveLast = false; s_lastId = 0; s_lastBone = -1;
            s_trackYaw.reset(); s_trackPitch.reset();
        }
        return;
    }
    s_lostFrames = 0;
    if (best.id != s_lastId) {
        s_haveLast = false;                      // do not learn gain across a target switch
        s_trackYaw.reset(); s_trackPitch.reset();  // и оценку коэффициента тоже
    }

    // Lead a moving target: the game applies our finger delta next frame, by
    // which time the target has moved on. Use the target's angular velocity
    // relative to the camera (with the camera's own rotation removed) and
    // aim one frame ahead. Reset on target switch.
    //
    // Камера здесь та же, что и в обучении коэффициента (esp_aim_camera_angles):
    // вычитать отстающий базис из матрицы вида из угловой скорости цели нельзя —
    // в поправку попадал бы его запаздывающий поворот, и прицел уезжал бы в
    // сторону. Нет настоящей оси — нет и упреждения: так вёл цель эталонный аим.
    static float s_prevTgtYaw = 0.f, s_prevTgtPitch = 0.f, s_prevCamYawT = 0.f, s_prevCamPitchT = 0.f;
    static bool  s_havePrevTgt = false;
    {
        float cy = 0.f, cp = 0.f;
        bool haveC = esp_aim_camera_angles(cy, cp);
        if (best.id == s_lastId && s_havePrevTgt && haveC) {
            float dCamYaw = cy - s_prevCamYawT;
            while (dCamYaw > 180.f) dCamYaw -= 360.f;
            while (dCamYaw < -180.f) dCamYaw += 360.f;
            float dCamPitch = cp - s_prevCamPitchT;
            // world-space angular motion of the target = change in offset + camera rotation
            float vYaw = (best.yaw - s_prevTgtYaw) + dCamYaw;
            float vPitch = (best.pitch - s_prevTgtPitch) + dCamPitch;
            if (std::isfinite(vYaw) && std::isfinite(vPitch) && fabsf(vYaw) < 10.f && fabsf(vPitch) < 10.f) {
                s_prevTgtYaw = best.yaw; s_prevTgtPitch = best.pitch;
                // Below this the "motion" is bone animation jitter (breathing,
                // sway), which at long range is larger than the head itself.
                // Extrapolating it would double the error, so only lead real
                // movement. Aim more than a frame ahead for fast movers so the
                // crosshair stays on a laterally running target (the controller
                // smoothing otherwise makes it trail behind).
                // В тач-режиме порог — два кванта пальца (ниже движение цели
                // не отличить от дрожания). В мемори-режиме кванта нет: там
                // порог задан прямо в градусах.
                const float leadMin = memoryMode ? 0.15f : degPerPx * 2.f;
                float vMag = sqrtf(vYaw * vYaw + vPitch * vPitch);
                if (vMag > leadMin) {
                    float k = 1.1f * (1.f - leadMin / vMag);
                    if (k > 1.5f) k = 1.5f;
                    best.yaw += vYaw * k;
                    best.pitch += vPitch * k;
                }
            } else {
                s_prevTgtYaw = best.yaw; s_prevTgtPitch = best.pitch;
            }
        } else {
            s_prevTgtYaw = best.yaw; s_prevTgtPitch = best.pitch;
        }
        if (haveC) { s_prevCamYawT = cy; s_prevCamPitchT = cp; s_havePrevTgt = true; }
        else s_havePrevTgt = false;
    }
    // ---- Режим «Мемори»: палец не участвует вовсе -------------------------
    // Прицел доводится записью поворота в память игры (esp/aim_mem.cpp), поэтому
    // ниже — ни тач-зон, ни кванта ввода, ни обучения град/px: контроллер берёт
    // остаток ошибки (тот же самый, что у тач-режима, вместе с упреждением) и
    // считает новые абсолютные углы. Стенд арифметики — tools/aim/run_mem.sh.
    if (memoryMode) {
        if (memState == MEM_AIM_READY) {
            AimReleaseFinger(s_fingerDown);
            s_lastId = best.id; s_lastBone = best.bone;
            s_haveLast = false;              // коэффициент тач-режима не ведём
            s_pendDx = s_pendDy = 0.f; s_pendTime = 0.f;
            float yaw_now = 0.f, pitch_now = 0.f;
            if (esp_mem_aim_read_angles(yaw_now, pitch_now)) {
                float yaw_new = 0.f, pitch_new = 0.f;
                if (AimMemoryStep(best, dt, g_state.gun_str, best.world_dist,
                                  yaw_now, pitch_now, yaw_new, pitch_new))
                    esp_mem_aim_apply(yaw_new, pitch_new);
            }
            // Углы пропали — сторож в esp_mem_aim_tick() объявит запись
            // потерянной и запустит самотест заново; до тех пор не пишем.
            return;
        }
        // Идёт самотест. Палец отпущен и камера не трогается: пробный доворот
        // меряется по настоящему повороту прицела, и наше же движение его
        // сломало бы. Самотест ограничен по времени (esp/aim_mem.cpp).
        if (memState != MEM_AIM_UNSUPPORTED) {
            AimReleaseFinger(s_fingerDown);
            return;
        }
        // Дорожки записи на этом устройстве нет — ведём тач-веткой ниже, чтобы
        // аим вообще не остался мёртвым. В меню при этом написано, что режим
        // «Мемори» не поддержан и работает тач.
    }

    s_lastId = best.id; s_lastBone = best.bone;

    // ---- оценка коэффициента по реакции прицела ----
    // Скармливаем тот остаток, по которому контроллер и будет считать шаг (после
    // упреждения), и позицию пальца до этого шага. Коэффициент считается по
    // корреляции «сдвинул палец -> изменилась ошибка», поэтому движение цели и
    // мигание кости на него влияют как шум, а не как сдвиг.
    s_trackYaw.observe(best.yaw, s_fx);
    s_trackPitch.observe(best.pitch, s_fy);

    // ---- learn finger gain (deg per px) from the previous frame ----
    // Measured from the camera's own rotation, so a moving target does not
    // pollute the estimate.
    //
    // Углы берём у esp_aim_camera_angles — только настоящая ось (поза камеры или
    // ось выстрела). Базис из матрицы вида, который фарм получает по
    // esp_camera_angles, отстаёт на кадр: выученный по нему коэффициент скакал
    // 0.072 -> 0.347 -> -0.072 (лог 15.09.2026, со сменой знака) и слал палец на
    // 160 px в край экрана. Нет настоящей оси — аим идёт запасным коэффициентом
    // без обучения, ровно как в сборке, где он вёл идеально.
    float camYaw = 0.f, camPitch = 0.f;
    const bool haveCam = esp_aim_camera_angles(camYaw, camPitch);
    float camYawDelta = 0.f, camPitchDelta = 0.f;
    bool camMoved = false;
    if (haveCam && s_haveLast) {
        camYawDelta = camYaw - s_lastCamYaw;
        while (camYawDelta > 180.f) camYawDelta -= 360.f;
        while (camYawDelta < -180.f) camYawDelta += 360.f;
        camPitchDelta = camPitch - s_lastCamPitch;
        camMoved = fabsf(camYawDelta) > 0.02f || fabsf(camPitchDelta) > 0.02f;
    }
    if (haveCam && s_haveLast && s_fingerDown && camMoved) {
        // Signed gains: a negative value simply means the game inverts that
        // axis (e.g. "invert Y" enabled) and the controller follows suit.
        // Adopt the measurement outright when it disagrees strongly with the
        // current estimate (sensitivity changed / first sample), else smooth.
        auto learn = [](float& gain, float measured) {
            float m = fabsf(measured);
            if (!std::isfinite(measured) || m < 0.005f || m > 2.0f) return;
            if (gain == 0.f || (measured > 0.f) != (gain > 0.f) ||
                m > fabsf(gain) * 1.3f || m < fabsf(gain) * 0.7f) gain = measured;
            else gain = gain * 0.7f + measured * 0.3f;
        };
        // Учимся на НАКОПЛЕННОМ сдвиге пальца с прошлого поворота камеры:
        // при низком FPS игра проглатывает несколько наших движений и
        // поворачивается на их сумму — деление на один последний сдвиг
        // завышало гейн и раскачивало прицел.
        if (fabsf(s_pendDx) >= 1.f) learn(s_gainYaw, camYawDelta / s_pendDx);
        if (fabsf(s_pendDy) >= 1.f) learn(s_gainPitch, -camPitchDelta / s_pendDy);
        s_pendDx = s_pendDy = 0.f;
        s_pendTime = 0.f;
    }
    if (haveCam) { s_lastCamYaw = camYaw; s_lastCamPitch = camPitch; s_haveLast = true; }
    else { s_haveLast = false; s_pendDx = s_pendDy = 0.f; s_pendTime = 0.f; }

    // ---- чувствительность пальца: град поворота камеры на пиксель экрана ----
    // Шаг считается как err / gain, поэтому ЗАВЫШЕННЫЙ запасной коэффициент это
    // ровно «аим стал медленным»: палец уходит на gain_истинный/gain_запасной
    // часть нужных пикселей, и остаток ошибки закрывается не за 1-2 кадра, а
    // экспонентой. Измерено по накопленному сдвигу пальца в логе автофарма
    // (14.09.2026, 35 свайпов, медиана 0.10 град/px по yaw; p10 0.078, p90 0.178)
    // — то же самое число выучил камерный контроллер фарма.
    // Прежнее запасное 0.35 было завышено в 3.5 раза: в логе 16.09.2026 при
    // exp = 0 остаток падал как 5.47 -> 0.35 град за 0.2 с (12-13 кадров), то
    // есть в 3.5 раза медленнее, чем позволяет игра, — при 0.10 хватает 1-2
    // кадров. Учиться на устройстве аиму не из чего: поза камеры и ось выстрела
    // не читаются (cam_st = 0 во всех строках аима), а базис из матрицы вида
    // отстаёт на кадр и выучивал коэффициент со сменой знака (0.072 -> 0.347 ->
    // -0.072) — «аим дёргается». Пока настоящей оси нет, запасное значение и
    // есть рабочий коэффициент.
    //
    // А запасной берётся из настройки игрока (AimSensitivityGain): игра умножает
    // накопленный сдвиг касания ровно на m_Sensitivity, поэтому град/px растёт
    // вместе с ней. Зашитое число работало бы только на одной чувствительности:
    // на высокой аим не доводил бы цель, на низкой — перелетал.
    bool gainFromSens = false;
    const float probeGainYaw   = AimSensitivityGain(gainFromSens);
    const float probeGainPitch = probeGainYaw;
    const bool  learned = (s_gainYaw != 0.f);
    // Порядок предпочтения коэффициента: оценка по реакции прицела (aim_learn.h)
    // — она измеряется на этой самой петле; затем выученный по повороту камеры;
    // затем значение из настройки чувствительности.
    // Рабочий коэффициент = значение из настройки чувствительности (это
    // множитель самой игры), подтянутое выученным — но только В ОДНУ СТОРОНУ и в
    // узкой полосе. По порядку важности:
    //
    // 1. Знак берём у значения из настройки и НЕ переворачиваем по замеру. В
    //    логе 15.09.2026 выученное уходило 0.072 -> 0.347 -> -0.072: это шум
    //    замера (палец 160 px, смена знака ошибки), а не инверсия оси — с
    //    положительным коэффициентом аим цель держит (остаток тангажа
    //    0.03/-0.85 град в логе 16.09.2026 21:35). С отрицательным он шагает в
    //    другую сторону от цели, ошибка растёт, палец уходит в край зоны: это и
    //    видно как «аим дёргается».
    // 2. Занижать нельзя. Шаг пальца считается как ошибка/коэффициент, поэтому
    //    заниженный коэффициент — это шаг больше нужного, а камера отвечает не
    //    сразу: петля err(t+1) = err(t) - L*err(t-1) при L = k*gain_истинный/
    //    gain_рабочий > 1 расходится (|z| = sqrt(L)). Ровно это давал эталон
    //    5.0 при настройке 2.00: рабочий 0.04 против измеренных 0.10 — на стенде
    //    цель не захватывается вовсе, СКЗ остатка 6.6 град, палец ездит по зоне.
    //    Поэтому нижняя граница подтяжки — сама настройка.
    // 3. Завышать можно в пределах 1.4x и плавно: завышенный коэффициент даёт шаг
    //    меньше нужного, то есть аим просто медленнее — безопасная сторона.
    constexpr float kGainTrimMax   = 1.4f;
    constexpr float kGainTrimStep  = 0.15f;   // ~10 тактов на новую подтяжку
    static float s_trimYaw = 1.f, s_trimPitch = 1.f;
    auto trimTo = [](float trim, float measured, float base) {
        float want = 1.f;
        if (fabsf(measured) > 1e-4f && fabsf(base) > 1e-4f) {
            want = fabsf(measured) / fabsf(base);
            if (want < 1.f) want = 1.f;
            if (want > kGainTrimMax) want = kGainTrimMax;
        }
        return trim + (want - trim) * kGainTrimStep;
    };
    const bool lsYaw   = s_trackYaw.ready();
    const bool lsPitch = s_trackPitch.ready();
    s_trimYaw   = trimTo(s_trimYaw,   lsYaw   ? s_trackYaw.gain()   : (learned ? s_gainYaw   : 0.f), probeGainYaw);
    s_trimPitch = trimTo(s_trimPitch, lsPitch ? s_trackPitch.gain() : s_gainPitch,                    probeGainPitch);
    const float gy = probeGainYaw   * s_trimYaw;
    const float gp = probeGainPitch * s_trimPitch;

    // Такт с подтверждением: если наш прошлый сдвиг ещё не отразился в
    // камере (игра не отрендерила кадр — низкий FPS), НЕ шлём новую
    // коррекцию: ошибка на экране устаревшая, и вторая поправка по ней —
    // это двойная коррекция, тот самый перелёт-раскачка. Держим палец на
    // месте и ждём реакции камеры (таймаут на случай проглоченного ввода).
    //
    // Ответ принимаем и за прошлый кадр: поза камеры читается до того, как игра
    // успевает применить наш ввод, и без этого запаса шаг ждал лишний такт.
    // Настоящий обрыв ввода всё равно ловится: ответа нет ни в этом кадре, ни в
    // прошлом — значит игра шаг не отработала.
    //
    // Такт работает только когда углы есть (cam_st != 0). Без настоящей оси
    // s_pendDx обнуляется каждый кадр (см. ветку haveCam выше), поэтому шаг
    // никогда не «ждёт ответа» — иначе аим, у которого углов нет вовсе, стоял бы
    // по 0.25 с на каждом шаге.
    // Ось камеры может быть «живой» по чтению и при этом не отражать наши шаги
    // (замершая поза). Тогда подтверждения не будет никогда, и каждый шаг ждал бы
    // свой таймаут: аим двигался бы рывками. Считаем таймауты подряд — после
    // третьего выключаем такт на 5 с и работаем шагами по ошибке (он устойчив и
    // без подтверждения: доля ошибки за такт ограничена половиной).
    static int   s_ackTimeouts = 0;
    static float s_ackDisabled = 0.f;
    const bool camAnswered = camMoved || s_camMovedPrev;
    s_camMovedPrev = camMoved;
    if (s_ackDisabled > 0.f) s_ackDisabled -= dt;
    const bool ackGate = (s_ackDisabled <= 0.f);
    if (ackGate && s_fingerDown && !camAnswered &&
        (fabsf(s_pendDx) >= 1.f || fabsf(s_pendDy) >= 1.f)) {
        s_pendTime += dt;
        if (s_pendTime < 0.25f) {
            Touch_Move(s_fx, s_fy);   // держим тач живым, ничего не двигаем
            return;
        }
        s_pendDx = s_pendDy = 0.f;    // ввод потерялся — продолжаем
        s_pendTime = 0.f;
        if (++s_ackTimeouts >= 3) { s_ackTimeouts = 0; s_ackDisabled = 5.f; }
    } else if (camAnswered) {
        s_ackTimeouts = 0;
    }

    // ---- «шаги в полёте» ---------------------------------------------------
    // Игра отрабатывает наш шаг не в этом такте: поза камеры отстаёт на два
    // кадра (замер в контроллере фарма — тот же палец и тот же экран). Когда
    // настоящей оси камеры нет (esp_aim_camera_angles -> false, штатный режим на
    // этом устройстве), такт с подтверждением выше не работает вовсе: s_pendDx
    // обнуляется каждый кадр, и контроллер видит ещё не уменьшившийся остаток.
    // Он шлёт следующую такую же поправку — и к моменту отклика камера
    // поворачивается вдвое больше нужного: рывок через цель и обратно.
    // Поэтому шаг считаем по остатку, каким он станет, когда камера покажет
    // заказанное за два последних такта. Стенд (та же арифметика, задержка два
    // такта): перелёт 4.8 -> 0.4 град, ход пальца при удержании цели
    // 718 -> 457 px, захват с 60 град вдвое быстрее.
    // Шаги берём по их очереди, а не по отклику ошибки: отклик сдвигается и
    // движением цели, и тогда поправка «съедала» настоящий остаток (прогон
    // стенда: остаток ошибки 10 град при шаге 1 px — прицел вставал).
    static float s_flightYaw[2] = {0.f, 0.f}, s_flightPitch[2] = {0.f, 0.f};
    const bool  useFlight = !haveCam;
    const float flightYaw   = s_flightYaw[0] + s_flightYaw[1];
    const float flightPitch = s_flightPitch[0] + s_flightPitch[1];
    const float errYaw   = useFlight ? (best.yaw   - flightYaw   * gy) : best.yaw;
    const float errPitch = useFlight ? (best.pitch + flightPitch * gp) : best.pitch;

    // ---- input quantum ----
    // The finger can only rest on the digitizer grid, so the camera can only
    // be steered in steps of (gain / units-per-pixel) degrees. At long range
    // one such step can exceed the size of a head; the controller therefore
    // has to settle on the NEAREST reachable position and hold still there,
    // never hunt back and forth across the target.
    float unitsPerPx = Touch_DeviceUnitsPerPixel();
    if (!(unitsPerPx >= 0.25f && unitsPerPx <= 16.f)) unitsPerPx = 1.f;
    const float gridPx = 1.f / unitsPerPx;               // screen px per device unit
    // «Известный» коэффициент — это и выученный, и взятый из чувствительности игры:
    // второй не догадка, а множитель самой игры, и по нему видно квант ввода. Без
    // этого при чувствительности выше эталонной шаг цифровера больше мёртвой зоны,
    // и контроллер бесконечно дёргает цель то влево, то вправо — «сильно дергает».
    const bool  gainKnownYaw   = (s_gainYaw != 0.f)   || gainFromSens || lsYaw;
    const bool  gainKnownPitch = (s_gainPitch != 0.f) || gainFromSens || lsPitch;
    const float qYaw   = gainKnownYaw   ? fabsf(gy) * gridPx : 0.f; // deg per device unit
    const float qPitch = gainKnownPitch ? fabsf(gp) * gridPx : 0.f;

    // ---- dead zone ----
    // Target: ~3 cm at the target's range (well inside a head), but never
    // tighter than what the input grid can actually reach (0.55 step), and
    // never wider than 1.5 screen px.
    float deadBase = degPerPx * 1.5f;
    if (best.world_dist > 1.f && std::isfinite(best.world_dist)) {
        float d = atanf(0.03f / best.world_dist) * 180.f / (float)M_PI;
        if (d < deadBase) deadBase = d;
    }
    // Пол мёртвой зоны — ДВА кванта ввода, а не половина. Точка прицела (кость
    // головы) читается каждый кадр чуть по-разному: дрожь в 1-2 кванта — это
    // нормальный шум замера, а не движение цели. С полом в половину кванта
    // контроллер выходил за мёртвую зону на этот шум и гнал палец то влево, то
    // вправо с частотой кадров («трясёт в разные стороны»). Стенд с шумом замера
    // +-0.25 град: реверсов пальца 12 -> 2 при мёртвой зоне 0.2 град (два
    // кванта) и 6 при 0.15.
    float deadYaw = deadBase, deadPitch = deadBase;
    if (gainKnownYaw   && deadYaw   < qYaw   * 2.0f) deadYaw   = qYaw   * 2.0f;
    if (gainKnownPitch && deadPitch < qPitch * 2.0f) deadPitch = qPitch * 2.0f;
    if (fabsf(errYaw) < deadYaw && fabsf(errPitch) < deadPitch) {
        s_pendDx = s_pendDy = 0.f; s_pendTime = 0.f;
        // Заказанное за прошлые такты камера дорабатывает и без нас.
        s_flightYaw[0] = s_flightYaw[1];     s_flightYaw[1] = 0.f;
        s_flightPitch[0] = s_flightPitch[1]; s_flightPitch[1] = 0.f;
        if (s_fingerDown) Touch_Move(s_fx, s_fy); // hold still, keep the touch alive
        return;
    }

    auto snapGrid = [&](float v) { return roundf(v * unitsPerPx) / unitsPerPx; };

    if (!s_fingerDown) {
        s_fx = snapGrid(ptX);
        s_fy = snapGrid(ptY);
        Touch_Down(s_fx, s_fy);
        s_fingerDown = true;
        s_pendDx = s_pendDy = 0.f; s_pendTime = 0.f;
        s_holdFrames = 0;
        // Палец опустился заново: окно замера начинается с нуля, иначе первая
        // же пара «сдвиг пальца -> отклик» посчитает бросок пальца к точке.
        s_trackYaw.reset(); s_trackPitch.reset();
        s_flightYaw[0] = s_flightYaw[1] = 0.f;
        s_flightPitch[0] = s_flightPitch[1] = 0.f;
        return; // let the game register the touch before moving it
    }
    if (s_holdFrames < 1) { ++s_holdFrames; Touch_Move(s_fx, s_fy); return; }

    // ---- controller ----
    // Speed slider keeps the old direction (higher = faster):
    //   1 = gentle (~25% of the remaining error per frame), 10 = snap.
    float sm = g_state.gun_str;
    if (!(sm >= 1.f)) sm = 1.f;
    if (sm > 10.f) sm = 10.f;
    float frac = 0.25f + (sm - 1.f) / 9.f * 0.75f;   // 0.25 .. 1.00 per frame @60fps
    // Frame-rate independent: convert per-frame fraction to a rate.
    float k = 1.f - powf(1.f - frac, dt * 60.f);
    // ...но не больше половины видимой ошибки за такт. Камера отвечает не в
    // этом такте: поза отстаёт от касания на 2 кадра (замер в контроллере
    // фарма, тот же палец и тот же экран), поэтому при k -> 1 петля идёт по
    // err(t+1) = err(t) - k*err(t-1), то есть z^2 - z + k = 0: корни
    // |z| = sqrt(k) = 0.993 при k = 0.986, угол ~80 градусов — колебания с
    // периодом 4.5 такта (0.36 с) не затухают. В логе 16.09.2026 это и видно:
    // |sent| * 0.10 / |err| = 0.982 (медиана, 167 строк), смена знака ошибки в
    // 45% пар соседних строк, шаги до потолка 54 px — «аим дёргается».
    // Половина ошибки за такт даёт |z| <= 0.71: хвост гаснет за 2-3 такта.
    // Захват цели при этом не замедляется — при ошибке больше 5 градусов шаг
    // всё равно упирается в потолок maxStep (0.05*sh = 54 px на этом экране),
    // и скорость наведения задаёт он. Слайдер ниже середины продолжает
    // работать: доля считается от устойчивого предела пропорционально frac.
    const float kStable = 0.5f;
    const float fracMid = 0.25f + (5.f - 1.f) / 9.f * 0.75f;   // слайдер 5
    if (k > kStable) k = kStable;
    if (frac < fracMid) k *= frac / fracMid;
    if (k > 1.f) k = 1.f;
    if (k < 0.05f) k = 0.05f;

    // Гаситель «качелей»: остаток по любой из осей перескочил через ноль и уже
    // набрал заметную величину обратного знака — это перелёт от нашей поправки. На несколько
    // тактов режем шаг, чтобы вместо дрожи подойти спокойно; восстанавливаемся
    // плавно (около четырёх тактов). Стенд: реверсы пальца на подходе 6 -> 4,
    // перелёт не растёт.
    static float s_flipDamp = 1.f;
    static float s_prevCtlYaw = 0.f, s_prevCtlPitch = 0.f;
    static bool  s_haveCtlErr = false;
    const bool flipYaw = s_haveCtlErr && s_prevCtlYaw * errYaw < 0.f &&
                         fabsf(errYaw) > fabsf(s_prevCtlYaw) * 0.35f;
    const bool flipPitch = s_haveCtlErr && s_prevCtlPitch * errPitch < 0.f &&
                           fabsf(errPitch) > fabsf(s_prevCtlPitch) * 0.35f;
    if (flipYaw || flipPitch) {
        s_flipDamp = 0.45f;
    } else if (s_flipDamp < 1.f) {
        s_flipDamp += (1.f - s_flipDamp) * (dt * 2.5f);
        if (s_flipDamp > 1.f) s_flipDamp = 1.f;
    }
    s_prevCtlYaw = errYaw; s_prevCtlPitch = errPitch; s_haveCtlErr = true;
    k *= s_flipDamp;

    float dx =  errYaw   * k / gy;
    float dy = -errPitch * k / gp;

    // Final approach: within a few input steps of the target, stop smoothing
    // and jump straight to the nearest reachable grid position. Smoothing
    // here would either creep for many frames or, once rounded, overshoot
    // and oscillate by a full step around the head.
    if (gainKnownYaw && fabsf(errYaw) < qYaw * 3.f)
        dx = roundf((errYaw / gy) * unitsPerPx) / unitsPerPx;
    if (gainKnownPitch && fabsf(errPitch) < qPitch * 3.f)
        dy = roundf((-errPitch / gp) * unitsPerPx) / unitsPerPx;

    // Шаг не больше оставшейся ошибки. Иначе собственный шаг перелетает цель, и
    // следующий шаг считается уже по ошибке другого знака — то самое «дёргается»
    // (камера отрабатывает шаг не сразу, поэтому перелёт виден глазом).
    if (fabsf(dx * gy) > fabsf(errYaw)) dx = errYaw / gy;
    if (fabsf(dy * gp) > fabsf(errPitch)) dy = -errPitch / gp;

    // Clamp per-frame travel so a bad gain estimate never slingshots.
    // Потолок шага поднимаем только коэффициенту, выученному по повороту камеры
    // (это прямое измерение град/px). Оценка по реакции прицела и значение из
    // настройки работают с прежним потолком: большой шаг по непроверенному
    // коэффициенту — это рывок через пол-экрана.
    const float maxStep = learned ? sh * 0.15f : sh * 0.05f;
    if (dx >  maxStep) dx =  maxStep;
    if (dx < -maxStep) dx = -maxStep;
    if (dy >  maxStep) dy =  maxStep;
    if (dy < -maxStep) dy = -maxStep;

    // Move in whole device units so the applied delta is exactly what we
    // measure next frame (gain learning) and the finger never accumulates a
    // hidden sub-unit remainder that later pops out as an unplanned step.
    float nx = snapGrid(s_fx + dx), ny = snapGrid(s_fy + dy);
    dx = nx - s_fx; dy = ny - s_fy;
    if (dx == 0.f && dy == 0.f) {
        s_pendDx = s_pendDy = 0.f; s_pendTime = 0.f;
        s_flightYaw[0] = s_flightYaw[1];     s_flightYaw[1] = 0.f;
        s_flightPitch[0] = s_flightPitch[1]; s_flightPitch[1] = 0.f;
        Touch_Move(s_fx, s_fy);
        return;
    }

    // Keep the finger in the look area around the chosen point. If it drifts to
    // an edge, lift and re-place it on the point instead of getting stuck.
    // Размеры области — те же, что были у жёсткой правой полосы (56..97% по X
    // и 12..88% по Y при точке 74%/50%), но привязаны к выбранной точке, чтобы
    // она могла стоять в любой части экрана. За экран область не выпускаем.
    float minX = ptX - 0.18f * sw, maxX = ptX + 0.23f * sw;
    float minY = ptY - 0.38f * sh, maxY = ptY + 0.38f * sh;
    if (minX < sw * 0.02f) minX = sw * 0.02f;
    if (maxX > sw * 0.98f) maxX = sw * 0.98f;
    if (minY < sh * 0.02f) minY = sh * 0.02f;
    if (maxY > sh * 0.98f) maxY = sh * 0.98f;
    if (nx < minX || nx > maxX || ny < minY || ny > maxY) {
        Touch_Up();
        s_fingerDown = false;
        s_fx = snapGrid(ptX); s_fy = snapGrid(ptY);
        s_pendDx = s_pendDy = 0.f; s_pendTime = 0.f;
        // Палец отпущен — заказанное игра не отработает вовсе.
        s_flightYaw[0] = s_flightYaw[1] = 0.f;
        s_flightPitch[0] = s_flightPitch[1] = 0.f;
        s_haveLast = false;
        // Перенос пальца — это не шаг прицела: замер по нему покажет мусор.
        s_trackYaw.reset(); s_trackPitch.reset();
        return;
    }
    s_fx = nx; s_fy = ny;
    s_pendDx += dx; s_pendDy += dy;   // ждёт отработки камерой (ack-такт)
    s_flightYaw[0] = s_flightYaw[1];     s_flightYaw[1] = dx;
    s_flightPitch[0] = s_flightPitch[1]; s_flightPitch[1] = dy;
    Touch_Move(s_fx, s_fy);
}
