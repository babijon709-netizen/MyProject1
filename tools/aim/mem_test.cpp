// Стенд мемори-режимов аима (запуск: sh tools/aim/run.sh).
//
// НАСТОЯЩИЙ код из jni/src/aim.cpp — выбор цели, «память» и «сайлент» (их
// вырезает run.sh в ctrl.inc) — вокруг заглушек игры: MouseLook, камера, ось
// выстрела. NDK не нужен: обычная сборка g++, поэтому гонять можно после
// каждой правки, не дожидаясь устройства.
//
// Зачем. Запись в память игры снаружи не видна: по экрану не отличить «поле
// перезаписано игрой раньше, чем прочитано» от «адрес не тот». Стенд
// проверяет то, что видно только в числах:
//   * «память»: петля «записали -> камера повернулась -> остаток меньше»
//     сходится и НЕ раскачивается (перелёт цели — это «прицел дёргается»);
//     коэффициент град/ед подтверждается замером по отклику камеры;
//   * «память» при потерянных записях (игра перезаписывает поле раньше, чем
//     читает): ввод не копится в никуда, камера не улетает;
//   * «память» без оси камеры (штатный режим на устройстве из логов
//     14-16.09.2026: cam_st = 0): работает запасная оценка коэффициента;
//   * Object не найден -> записи нет ВОВСЕ, на тач не падаем (выбор
//     пользователя: сломанный мемори-аим молчит, а не притворяется тачем);
//   * «сайлент»: ось выстрела доворачивается в цель, остаётся единичной и не
//     уезжает дальше предела; когда игра перезаписала ось своей — отклонение
//     начинается с нуля, а не копилось.
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "xp.inc"

// ---- заглушка языка (XS() шифрует строку, перевод здесь не нужен) ----
namespace lang { inline const char* text(const char* ru) { return ru; } }

#include "app_state.h"     // настоящий AppState (настройки аима)
#include "game.h"          // настоящий EspBox и объявления esp_*
#include "aim.h"           // AimMode, AimMemDiag
#include "aim_learn.h"     // настоящий GainTrack
#include "widgets.h"       // Sheet/Popover (меню открыто — аим молчит)

// ====================== заглушки оверлея и игры ===========================
static const float kRad2Deg = 57.29577951f;

struct { int width = 2460, height = 1080, orientation = 0; } displayInfo;
static int native_window_screen_x = 2460, native_window_screen_y = 1080;
static bool g_esp_attached = true;
static int  g_calibMode = 0;
static bool g_buildPrompt = false;
AppState   g_state;
InputState g_input;
bool       g_aimActive = false;   // аим ведёт цель (автофарм уступает камеру)
Sheet      g_sheet;
Popover    g_pop;
// Тач-режим в стенд не входит (его проверяет tools/touch), но диспетчер его
// зовёт — здесь это пустышка.
static void UpdateAimTouch(float) { }

static int g_toasts = 0;
void ShowToast(const char*, float) { ++g_toasts; }
void ShowToast(const char*) { ++g_toasts; }

// Тач-режим в стенд не входит (его проверяет tools/touch), но диспетчер его
// зовёт — для стенда это пустышка.
// ---- «игра» ----
struct Game {
    // MouseLook: m_Invert, m_Sensitivity, гироскоп, ввод взгляда, углы.
    bool  haveLook = true;
    float sens = 2.0f;              // градусов на единицу ввода
    bool  invert = false;
    bool  gyro = false;
    float lookIn[2] = {0.f, 0.f};   // +0x88: игра перезаписывает его каждый кадр
    float yaw = 0.f, pitch = 0.f;   // накопленные углы камеры
    // Ось выстрела: MouseLook.Update кладёт сюда forward камеры.
    bool  haveAxis = true;
    float ax = 0.f, ay = 0.f, az = 1.f;
    bool  holdOn = false;           // писатель-доминатор включён
    bool  holderLoses = false;      // игра перезаписывает ось раньше, чем писатель
    // Когда игра читает ввод взгляда: 1 — каждый кадр (повезло), 3 — каждый
    // третий (ZJX затёр поле до чтения).
    int   acceptEvery = 1;
    int   gameFrames = 0;
    int   consumed = 0;             // сколько раз ввод дошёл до камеры

    void frame() {
        ++gameFrames;
        const float ux = lookIn[0], uy = lookIn[1];
        lookIn[0] = lookIn[1] = 0.f;          // ZJX кладёт сюда сдвиг касания
        if (haveLook && (gameFrames % acceptEvery) == 0) {
            if (ux != 0.f || uy != 0.f) ++consumed;
            float vx = ux, vy = invert ? uy : -uy;   // fnmul + fcsel по m_Invert
            yaw += vx * sens; pitch += vy * sens;
        }
        float fx = 0.f, fy = 0.f, fz = 1.f;
        anglesTo(yaw, pitch, fx, fy, fz);
        if (!holdOn || holderLoses) { ax = fx; ay = fy; az = fz; }
    }
    static void anglesTo(float y, float p, float& x, float& yy, float& z) {
        const float cy = cosf(y / kRad2Deg), sy = sinf(y / kRad2Deg);
        const float cp = cosf(p / kRad2Deg), sp = sinf(p / kRad2Deg);
        x = cp * sy; yy = sp; z = cp * cy;
    }
};
static Game g_game;

// ---- ESP: один бокс, ошибка считается от настоящего базиса ----
static std::vector<EspBox> g_boxes;
static float g_targetYaw = 10.f, g_targetPitch = 5.f;   // куда смотрит цель
static bool  g_haveCamAxis = true;
static float g_pxPerDeg = 18.f;

const std::vector<EspBox>& FrameBoxes(float, float) { return g_boxes; }
float AimFovRadiusPx(float, float) { return 1e6f; }    // круг FOV в стенде не мешает

// Ошибка прицела: от базиса base к направлению на цель (цель стоит в мире
// неподвижно, angle = g_targetYaw/Pitch от нулевого направления).
static void errFrom(float baseYaw, float basePitch, float& eYaw, float& ePitch) {
    float tx = 0.f, ty = 0.f, tz = 1.f;
    Game::anglesTo(g_targetYaw, g_targetPitch, tx, ty, tz);
    float fx = 0.f, fy = 0.f, fz = 1.f;
    Game::anglesTo(baseYaw, basePitch, fx, fy, fz);
    // right = normalize(cross(world_up, fwd)), up = cross(fwd, right)
    float rx = fz, ry = 0.f, rz = -fx;
    const float rl = sqrtf(rx * rx + rz * rz) > 1e-6f ? sqrtf(rx * rx + rz * rz) : 1.f;
    rx /= rl; rz /= rl;
    const float ux = fy * rz - fz * ry, uy = fz * rx - fx * rz, uz = fx * ry - fy * rx;
    const float df = tx * fx + ty * fy + tz * fz;
    const float dr = tx * rx + ty * ry + tz * rz;
    const float du = tx * ux + ty * uy + tz * uz;
    eYaw = atan2f(dr, df) * kRad2Deg;
    ePitch = atan2f(du, sqrtf(df * df + dr * dr)) * kRad2Deg;
}

// Снимок ESP вызывается перед каждым тактом аима.
static void snapshot() {
    float baseYaw = g_game.yaw, basePitch = g_game.pitch;
    // Сайлент крутит не камеру, а ось выстрела, и ESP (как aim_angles_for)
    // отсчитывает ошибку от настоящей оси выстрела.
    if (g_state.aim_mode == AIM_MODE_SILENT) {
        baseYaw = atan2f(g_game.ax, g_game.az) * kRad2Deg;
        basePitch = atan2f(g_game.ay, sqrtf(g_game.ax * g_game.ax + g_game.az * g_game.az)) * kRad2Deg;
    }
    float eYaw = 0.f, ePitch = 0.f;
    errFrom(baseYaw, basePitch, eYaw, ePitch);
    EspBox b{};
    b.id = 1;
    b.ally = false;
    b.distance = 25.f;
    b.aim_valid[0] = true;
    b.aim_yaw[0] = eYaw; b.aim_pitch[0] = ePitch;
    b.aim_pts[0][0] = 1230.f + eYaw * g_pxPerDeg;
    b.aim_pts[0][1] = 540.f - ePitch * g_pxPerDeg;
    g_boxes.assign(1, b);
}

// ---- esp_* из game.h ----
float esp_camera_fov_deg() { return 60.f; }
bool esp_local_player_is_aiming() { return true; }
bool esp_aim_camera_angles(float& yaw, float& pitch) {
    if (!g_haveCamAxis) return false;
    yaw = g_game.yaw; pitch = g_game.pitch;
    return true;
}
bool esp_mem_aim_look_params(float& dpu, bool& inv, bool& gyr) {
    if (!g_game.haveLook) return false;
    dpu = g_game.sens; inv = g_game.invert; gyr = g_game.gyro;
    return true;
}
bool esp_mem_aim_write_look(float ux, float uy) {
    if (!g_game.haveLook) return false;
    g_game.lookIn[0] = ux; g_game.lookIn[1] = uy;
    return true;
}
bool esp_mem_aim_read_fire_dir(float& x, float& y, float& z) {
    if (!g_game.haveAxis) return false;
    const float len = sqrtf(g_game.ax * g_game.ax + g_game.ay * g_game.ay + g_game.az * g_game.az);
    if (!(len > 0.5f && len < 2.0f)) return false;
    x = g_game.ax; y = g_game.ay; z = g_game.az;
    return true;
}
bool esp_mem_aim_write_fire_dir(float x, float y, float z) {
    if (!g_game.haveAxis) return false;
    const float len = sqrtf(x * x + y * y + z * z);
    if (!(len > 0.001f)) return false;
    g_game.ax = x / len; g_game.ay = y / len; g_game.az = z / len;
    return true;
}
void esp_mem_aim_hold_fire_dir(bool on) { g_game.holdOn = on; }

// Вырезанный настоящий код режимов (см. run.sh): он должен видеть объявления
// окружения (g_state, FrameBoxes, esp_*), поэтому включается после заглушек.
#include "ctrl.inc"

// ====================== прогон ============================================
struct Result {
    int    frames = 0;
    float  errYaw = 0.f, errPitch = 0.f;    // остаток на последнем кадре
    float  maxAbsYaw = 0.f;                 // максимум |поворот камеры|
    int    signFlips = 0;                   // смен знака остатка (раскачка)
    int    writes = 0, fails = 0, toasts = 0;
};

// Прогон: dt оверлея 80 мс (12 fps, как на устройстве), игра — 60 fps.
static Result run(int mode, int frames, bool measureAxis = true) {
    g_state.aim_mode = mode;
    Result r;
    float prevYawErr = 0.f; bool havePrev = false;
    r.writes = AimMemoryDiag().writes; r.fails = AimMemoryDiag().fails;
    const int toasts0 = g_toasts;
    for (int i = 0; i < frames; ++i) {
        snapshot();
        UpdateAim(0.08f);
        for (int f = 0; f < 5; ++f) g_game.frame();   // 5 кадров игры на кадр оверлея
        float eYaw = 0.f, ePitch = 0.f;
        if (measureAxis) {
            float baseYaw = g_game.yaw, basePitch = g_game.pitch;
            if (mode == AIM_MODE_SILENT) {
                baseYaw = atan2f(g_game.ax, g_game.az) * kRad2Deg;
                basePitch = atan2f(g_game.ay, sqrtf(g_game.ax * g_game.ax + g_game.az * g_game.az)) * kRad2Deg;
            }
            errFrom(baseYaw, basePitch, eYaw, ePitch);
            if (havePrev && prevYawErr * eYaw < 0.f) ++r.signFlips;
            prevYawErr = eYaw; havePrev = true;
        }
        const float ay = fabsf(g_game.yaw);
        if (ay > r.maxAbsYaw) r.maxAbsYaw = ay;
        ++r.frames;
    }
    r.errYaw = fabsf(prevYawErr);
    r.writes = AimMemoryDiag().writes - r.writes;
    r.fails = AimMemoryDiag().fails - r.fails;
    r.toasts = g_toasts - toasts0;
    return r;
}

static void resetGame() {
    g_game = Game{};
    g_state = AppState{};
    g_state.aim_touch = true;
    g_state.gun_str = 5.f;
    g_state.gun_fov = 60.f;
    g_boxes.clear();
    g_haveCamAxis = true;
    g_toasts = 0;
    s_memDiag = AimMemDiag{};
}

static int g_fail = 0;
static void check(bool ok, const char* what) {
    printf("  %-58s %s\n", what, ok ? "ок" : "ПРОВАЛ");
    if (!ok) g_fail = 1;
}

int main() {
    printf("--- мемори-режимы аима (стенд на настоящем коде aim.cpp)\n");

    // A. «Память»: камера отвечает на каждую запись.
    {
        resetGame();
        g_targetYaw = 10.f; g_targetPitch = 5.f;
        Result r = run(AIM_MODE_MEMORY, 120);
        printf("ПАМЯТЬ отвечает: кадров %d, остаток %.2f°, макс |поворот| %.1f°, "
               "смен знака %d, записей %d\n",
               r.frames, r.errYaw, r.maxAbsYaw, r.signFlips, r.writes);
        check(r.errYaw < 0.5f, "сошлось к цели (остаток < 0.5°)");
        check(r.signFlips <= 3, "без раскачки (смен знака не больше 3)");
        check(AimMemoryDiag().responded, "камера ответила (коэффициент подтверждён)");
        const float gain = AimMemoryDiag().deg_per_unit;
        check(gain > g_game.sens * 0.5f && gain < g_game.sens * 2.f,
              "коэффициент град/ед подтверждён замером");
    }

    // A2. «Память» с инвертированной вертикалью в настройках игры: знак
    //     вертикали переворачивает сама игра (fnmul + fcsel в MouseLook.ZJo).
    {
        resetGame();
        g_game.invert = true;
        g_targetYaw = 10.f; g_targetPitch = 5.f;
        Result r = run(AIM_MODE_MEMORY, 120);
        printf("ПАМЯТЬ инверсия Y: кадров %d, остаток %.2f°, поворот %.1f/%.1f°\n",
               r.frames, r.errYaw, g_game.yaw, g_game.pitch);
        check(r.errYaw < 0.5f, "сошлось при m_Invert (вертикаль не перевёрнута)");
        check(g_game.pitch > 4.f && g_game.pitch < 6.f, "тангаж дошёл до цели, а не в другую сторону");
    }

    // B. «Память» при потерянных записях: игру перезаписывает поле раньше,
    //    чем читает (принимается каждый третий кадр).
    {
        resetGame();
        g_game.acceptEvery = 3;
        Result r = run(AIM_MODE_MEMORY, 240);
        printf("ПАМЯТЬ 1/3 записей: кадров %d, остаток %.2f°, макс |поворот| %.1f°, "
               "смен знака %d, дошло до камеры %d\n",
               r.frames, r.errYaw, r.maxAbsYaw, r.signFlips, g_game.consumed);
        check(r.errYaw < 1.0f, "сошлось и при потерянных записях");
        check(r.maxAbsYaw < 20.f, "камера не улетела (ввод не копился в никуда)");
    }

    // C. «Память» без оси камеры: остаётся запасная оценка коэффициента
    //    по реакции прицела (cam_st = 0 в логе устройства).
    {
        resetGame();
        g_haveCamAxis = false;
        Result r = run(AIM_MODE_MEMORY, 240);
        printf("ПАМЯТЬ без оси камеры: кадров %d, остаток %.2f°, макс |поворот| %.1f°, "
               "смен знака %d\n", r.frames, r.errYaw, r.maxAbsYaw, r.signFlips);
        check(r.errYaw < 1.5f, "ведёт и без настоящей оси камеры");
        check(r.maxAbsYaw < 20.f, "без оси камеры не уезжает");
        check(!AimMemoryDiag().responded, "без оси камеры замер не подтверждён (честно)");
    }

    // D. MouseLook не найден: пишем ноль раз и НЕ падаем на тач.
    {
        resetGame();
        g_game.haveLook = false;
        Result r = run(AIM_MODE_MEMORY, 60);
        printf("ПАМЯТЬ без MouseLook: записей %d, отказов %d, поворот камеры %.2f°, "
               "тостов %d\n", r.writes, r.fails, g_game.yaw, r.toasts);
        check(r.writes == 0, "записей нет вовсе (не падаем на тач)");
        check(fabsf(g_game.yaw) < 1e-3f, "камера не двигалась");
        check(!AimMemoryDiag().params, "диагностика: params = нет");
        check(r.toasts >= 1, "пользователю сказали, почему режим молчит");
    }

    // E. «Сайлент»: ось выстрела доворачивается, камера стоит.
    {
        resetGame();
        g_targetYaw = 8.f; g_targetPitch = 4.f;
        Result r = run(AIM_MODE_SILENT, 120);
        const float axisLen = sqrtf(g_game.ax * g_game.ax + g_game.ay * g_game.ay + g_game.az * g_game.az);
        printf("САЙЛЕНТ: кадров %d, остаток %.2f°, отклонение %.1f°, длина оси %.4f, "
               "поворот камеры %.2f°\n",
               r.frames, r.errYaw, AimMemoryDiag().dev, axisLen, g_game.yaw);
        check(r.errYaw < 0.5f, "ось выстрела смотрит в цель");
        check(fabsf(axisLen - 1.f) < 1e-3f, "ось осталась единичной");
        check(fabsf(g_game.yaw) < 1e-3f, "камера не двигалась (silent)");
        check(AimMemoryDiag().dev < 15.f, "отклонение в пределах нормы");
    }

    // F. «Сайлент» с целью за пределами отклонения: ось упирается в предел,
    //    а не уезжает куда попало.
    {
        resetGame();
        g_targetYaw = 60.f; g_targetPitch = 0.f;
        Result r = run(AIM_MODE_SILENT, 120);
        printf("САЙЛЕНТ цель 60°: отклонение %.1f°, остаток %.1f°, длина оси %.4f\n",
               AimMemoryDiag().dev, r.errYaw,
               sqrtf(g_game.ax * g_game.ax + g_game.ay * g_game.ay + g_game.az * g_game.az));
        check(AimMemoryDiag().dev <= 35.5f, "отклонение зажато пределом (35°)");
        check(AimMemoryDiag().dev > 30.f, "ось всё же довёрнута к цели");
        check(std::isfinite(g_game.ax) && std::isfinite(g_game.ay) && std::isfinite(g_game.az),
              "ось не испортилась");
    }

    // G. «Сайлент», когда игра каждый кадр перезаписывает ось своей
    //    (писатель проигрывает гонку): отклонение не копится.
    {
        resetGame();
        g_targetYaw = 8.f; g_targetPitch = 4.f;
        g_game.holderLoses = true;
        Result r = run(AIM_MODE_SILENT, 120);
        printf("САЙЛЕНТ ось игры каждый кадр: отклонение %.1f°, остаток %.1f°\n",
               AimMemoryDiag().dev, r.errYaw);
        check(AimMemoryDiag().dev <= 35.5f, "отклонение не накопилось за предел");
        check(std::isfinite(g_game.ay), "ось не испортилась");
    }

    // H. «Сайлент» без оси выстрела: записи нет, ось отдана игре.
    {
        resetGame();
        g_game.haveAxis = false;
        Result r = run(AIM_MODE_SILENT, 60);
        printf("САЙЛЕНТ без оси: записей %d, отказов %d, писатель %s, тостов %d\n",
               r.writes, r.fails, g_game.holdOn ? "включён" : "выключен", r.toasts);
        check(r.writes == 0, "записей нет");
        check(!g_game.holdOn, "писатель оси выключен");
        check(r.toasts >= 1, "пользователю сказали, почему режим молчит");
    }

    // I. Выключенный аим ничего не пишет ни в одном режиме.
    {
        resetGame();
        g_state.aim_touch = false;
        Result r = run(AIM_MODE_SILENT, 30);
        check(r.writes == 0 && !g_game.holdOn, "с выключенным аимом ось отдана игре");
    }

    printf(g_fail ? "--- ЕСТЬ ПРОВАЛЫ\n" : "--- все проверки пройдены\n");
    return g_fail;
}
