// Стенд мемори-режимов аима (запуск: sh tools/aim/run.sh).
//
// НАСТОЯЩИЙ код из jni/src/aim.cpp — выбор цели, «память» и «сайлент» (их
// вырезает run.sh в ctrl.inc) — вокруг заглушек игры: MouseLook с накопителем
// углов, камера, ось выстрела. NDK не нужен: обычная сборка g++, поэтому
// гонять можно после каждой правки, не дожидаясь устройства.
//
// Зачем. Запись в память игры снаружи не видна: по экрану не отличить «поле
// перезаписано игрой» от «адрес не тот». Стенд проверяет то, что видно только
// в числах:
//   * «память»: петля «записали в накопитель -> камера повернулась -> остаток
//     меньше» сходится и НЕ раскачивается (перелёт — это «прицел дёргается»);
//   * «память» работает и без оси камеры (накопитель — сам по себе угол,
//     выучивать чувствительность не нужно);
//   * объекта нет -> записи нет ВОВСЕ, на тач не падаем (выбор пользователя);
//   * «сайлент»: ось выстрела доворачивается в цель, остаётся единичной и не
//     уезжает дальше предела; когда игра перезаписала ось своей — отклонение
//     начинается с нуля, а не копилось.
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
#include <cstdarg>
#include <future>
#include <chrono>

#include "xp.inc"

// ---- заглушка лога (в стенде пишет в stderr, только если задан AIM_LOG) ----
#include "logfile.h"
static bool g_log_echo = getenv("AIM_LOG") != nullptr;
void LogOpen() {}
void LogClose() {}
void LogStage(int) {}
void LogFrameBeat(unsigned long) {}
void LogWatchdogStart() {}
void LogLine(const char* fmt, ...) {
    if (!g_log_echo) return;
    va_list a; va_start(a, fmt);
    fprintf(stderr, "  лог: "); vfprintf(stderr, fmt, a); fputc('\n', stderr);
    va_end(a);
}

// ---- заглушка языка (XS() шифрует строку, перевод здесь не нужен) ----
namespace lang { inline const char* text(const char* ru) { return ru; } }

#include "app_state.h"     // настоящий AppState (настройки аима)
#include "game.h"          // настоящий EspBox и объявления esp_*
#include "aim.h"           // AimMode, AimMemDiag
#include "aim_learn.h"     // настоящий GainTrack
#include "widgets.h"       // Sheet/Popover (меню открыто — аим молчит)

// ---- заглушки оверлея ----
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

// ====================== «игра» ==============================================
static bool g_firing = false;       // стенд: «игрок стреляет»

struct Game {
    // ---- MouseLook ----
    bool  haveLook = true;
    // m_Sensitivity: нужен только проверкой «объект тот»; переводить единицы
    // ввода в градусы им больше не нужно — накопитель уже в градусах.
    float sens = 2.0f;
    bool  invert = false, gyro = false;
    // Накопитель углов (+0x4C rotationX / +0x50 rotationY). Игра каждый кадр
    // делает angles = clamp(angles + input * m_Sensitivity) и разворачивает
    // результат в localRotation камеры (MouseLook.ZJo).
    float accX = 0.f, accY = 0.f;
    static constexpr float kPitchLimit = 85.f;      // предел обзора игры

    // ---- ось выстрела (PlayerEventHandler.LookDirection) ----
    bool  haveAxis = true;
    float shotTime = 0.f;           // метка выстрела (FPHitscan.LastLocalHitTime)
    float ax = 0.f, ay = 0.f, az = 1.f;
    int   gameFrames = 0;

    // Камера: rotationX — это «наклон вниз», то есть возвышение с минусом.
    float camYaw()   const { return accY; }
    float camPitch() const { return -accX; }

    static void anglesTo(float yaw, float pitch, float& x, float& y, float& z) {
        const float cy = cosf(yaw / kRad2Deg), sy = sinf(yaw / kRad2Deg);
        const float cp = cosf(pitch / kRad2Deg), sp = sinf(pitch / kRad2Deg);
        x = cp * sy; y = sp; z = cp * cy;
    }

    // Кадр игры: развернуть накопитель в камеру и положить свою ось выстрела.
    void frame() {
        ++gameFrames;
        // ZJo: клампит накопитель и оборачивает рысканье. Ввода касания в
        // стенде нет, поэтому прибавлять нечего.
        if (accX >  kPitchLimit) accX =  kPitchLimit;
        if (accX < -kPitchLimit) accX = -kPitchLimit;
        while (accY >  180.f) accY -= 360.f;
        while (accY < -180.f) accY += 360.f;
        float fx = 0.f, fy = 0.f, fz = 1.f;
        anglesTo(camYaw(), camPitch(), fx, fy, fz);
        // MouseLook.Update кладёт в ось выстрела forward камеры — каждый свой
        // кадр. Наша запись живёт до следующего кадра игры: это и есть та
        // гонка, ради которой ось повторяют в конце кадра оверлея.
        ax = fx; ay = fy; az = fz;
        if (g_firing) shotTime += 2.f;   // стрельба: метка растёт
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

// Углы оси выстрела (сайлент крутит не камеру, а её).
static void axisAngles(float& yaw, float& pitch) {
    yaw = atan2f(g_game.ax, g_game.az) * kRad2Deg;
    pitch = atan2f(g_game.ay, sqrtf(g_game.ax * g_game.ax + g_game.az * g_game.az)) * kRad2Deg;
}

// Снимок ESP вызывается перед каждым тактом аима.
static void snapshot() {
    float baseYaw = g_game.camYaw(), basePitch = g_game.camPitch();
    // Сайлент крутит не камеру, а ось выстрела, и ESP (как aim_angles_for)
    // отсчитывает ошибку от настоящей оси выстрела.
    if (g_state.aim_mode == AIM_MODE_SILENT) axisAngles(baseYaw, basePitch);
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
// Муссор вместо угла камеры: 0 — камера честная, иначе отдаём это значение.
// Так проверяется, что кадр не залипнет (см. сценарий «мусорный угол»).
float g_garbageCam = 0.f;
bool esp_aim_camera_angles(float& yaw, float& pitch) {
    if (!g_haveCamAxis) return false;
    if (g_garbageCam != 0.f) { yaw = g_garbageCam; pitch = g_garbageCam; return true; }
    yaw = g_game.camYaw(); pitch = g_game.camPitch();
    return true;
}
bool esp_mem_aim_look_params(float& dpu, bool& inv, bool& gyr) {
    if (!g_game.haveLook) return false;
    dpu = g_game.sens; inv = g_game.invert; gyr = g_game.gyro;
    return true;
}
bool esp_mem_aim_read_angles(float& x, float& y) {
    if (!g_game.haveLook) return false;
    x = g_game.accX; y = g_game.accY;
    return true;
}
bool esp_mem_aim_write_angles(float x, float y) {
    if (!g_game.haveLook) return false;
    if (!std::isfinite(x) || !std::isfinite(y)) return false;
    if (fabsf(x) > 360.f || fabsf(y) > 360.f) return false;
    g_game.accX = x; g_game.accY = y;
    return true;
}
bool esp_mem_aim_read_fire_dir(float& x, float& y, float& z) {
    if (!g_game.haveAxis) return false;
    const float len = sqrtf(g_game.ax * g_game.ax + g_game.ay * g_game.ay + g_game.az * g_game.az);
    if (!(len > 0.5f && len < 2.0f)) return false;
    x = g_game.ax; y = g_game.ay; z = g_game.az;
    return true;
}
bool esp_mem_aim_last_shot(float& time_sec) { time_sec = g_game.shotTime; return true; }

bool esp_mem_aim_write_fire_dir(float x, float y, float z) {
    if (!g_game.haveAxis) return false;
    const float len = sqrtf(x * x + y * y + z * z);
    if (!(len > 0.001f)) return false;
    g_game.ax = x / len; g_game.ay = y / len; g_game.az = z / len;
    return true;
}

// WrapDeg180() — общая обёртка угла, её зовёт вырезанный код ниже.
#include "ui_util.h"

// Вырезанный настоящий код режимов (см. run.sh): он должен видеть объявления
// окружения (g_state, FrameBoxes, esp_*), поэтому включается после заглушек.
#include "ctrl.inc"

// ====================== прогон ============================================
// Прогон: dt оверлея 80 мс (12 fps, как на устройстве), игра — 60 fps.
// Остаток мерим дважды: сразу после такта аима (то, что видит наша запись) и
// после кадров игры (то, что достанется выстрелу, если игра перезаписала ось).
struct Result {
    int    frames = 0;
    float  errAfterAim = 0.f;               // остаток по нашей записи
    float  errAfterGame = 0.f;              // остаток после кадров игры
    float  maxAbsYaw = 0.f;
    int    signFlips = 0;
    int    writes = 0, fails = 0, toasts = 0;
};

static Result run(int mode, int frames) {
    g_state.aim_mode = mode;
    Result r;
    float prevYawErr = 0.f; bool havePrev = false;
    r.writes = AimMemoryDiag().writes; r.fails = AimMemoryDiag().fails;
    const int toasts0 = g_toasts;
    for (int i = 0; i < frames; ++i) {
        snapshot();
        UpdateAim(0.08f);
        AimEndFrame();                       // повтор оси перед концом кадра
        float eYaw = 0.f, ePitch = 0.f;
        float baseYaw = g_game.camYaw(), basePitch = g_game.camPitch();
        if (mode == AIM_MODE_SILENT) axisAngles(baseYaw, basePitch);
        errFrom(baseYaw, basePitch, eYaw, ePitch);
        r.errAfterAim = fabsf(eYaw);

        for (int f = 0; f < 5; ++f) g_game.frame();   // 5 кадров игры на кадр оверлея
        baseYaw = g_game.camYaw(); basePitch = g_game.camPitch();
        if (mode == AIM_MODE_SILENT) axisAngles(baseYaw, basePitch);
        errFrom(baseYaw, basePitch, eYaw, ePitch);
        if (havePrev && prevYawErr * eYaw < 0.f) ++r.signFlips;
        prevYawErr = eYaw; havePrev = true;
        const float ay = fabsf(g_game.camYaw());
        if (ay > r.maxAbsYaw) r.maxAbsYaw = ay;
        ++r.frames;
    }
    r.errAfterGame = fabsf(prevYawErr);
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
    g_garbageCam = 0.f;
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

    // A. «Память»: накопитель углов -> камера.
    {
        resetGame();
        g_targetYaw = 10.f; g_targetPitch = 5.f;
        Result r = run(AIM_MODE_MEMORY, 60);
        printf("ПАМЯТЬ: кадров %d, остаток %.2f°, поворот камеры %.1f/%.1f°, "
               "смен знака %d, записей %d\n",
               r.frames, r.errAfterAim, g_game.camYaw(), g_game.camPitch(), r.signFlips, r.writes);
        check(r.errAfterAim < 0.5f, "сошлось к цели (остаток < 0.5°)");
        check(r.signFlips <= 3, "без раскачки (смен знака не больше 3)");
        check(AimMemoryDiag().responded, "камера ответила (запись доходит)");
        check(fabsf(g_game.camPitch() - 5.f) < 0.6f, "тангаж дошёл до цели (знак не перевёрнут)");
        check(fabsf(g_game.camYaw() - 10.f) < 0.6f, "рысканье дошло до цели");
    }

    // B. «Память» без оси камеры: накопитель — сам по себе угол, поэтому
    //    работать это обязано и там, где выучивать чувствительность нечем.
    {
        resetGame();
        g_haveCamAxis = false;
        Result r = run(AIM_MODE_MEMORY, 60);
        printf("ПАМЯТЬ без оси камеры: остаток %.2f°, поворот %.1f/%.1f°, записей %d\n",
               r.errAfterAim, g_game.camYaw(), g_game.camPitch(), r.writes);
        check(r.errAfterAim < 0.5f, "ведёт и без настоящей оси камеры");
        check(!AimMemoryDiag().responded, "без оси камеры отклик не подтверждён (честно)");
    }

    // C. MouseLook не найден: пишем ноль раз и НЕ падаем на тач.
    {
        resetGame();
        g_game.haveLook = false;
        Result r = run(AIM_MODE_MEMORY, 30);
        printf("ПАМЯТЬ без MouseLook: записей %d, поворот камеры %.2f°, тостов %d\n",
               r.writes, g_game.camYaw(), r.toasts);
        check(r.writes == 0, "записей нет вовсе (не падаем на тач)");
        check(fabsf(g_game.camYaw()) < 1e-3f, "камера не двигалась");
        check(!AimMemoryDiag().params, "диагностика: params = нет");
        check(r.toasts >= 1, "пользователю сказали, почему режим молчит");
    }

    // D. «Сайлент»: ось выстрела доворачивается, камера стоит.
    {
        resetGame();
        g_targetYaw = 8.f; g_targetPitch = 4.f;
        Result r = run(AIM_MODE_SILENT, 60);
        const float axisLen = sqrtf(g_game.ax * g_game.ax + g_game.ay * g_game.ay + g_game.az * g_game.az);
        printf("САЙЛЕНТ: кадров %d, остаток %.2f°, отклонение %.1f°, длина оси %.4f, "
               "поворот камеры %.2f°\n",
               r.frames, r.errAfterAim, AimMemoryDiag().dev, axisLen, g_game.camYaw());
        check(r.errAfterAim < 0.5f, "ось выстрела смотрит в цель");
        check(fabsf(axisLen - 1.f) < 1e-3f, "ось осталась единичной");
        check(r.errAfterGame > 1.f, "кадр игры ось себе вернул (гонка смоделирована)");
        check(fabsf(g_game.camYaw()) < 1e-3f, "камера не двигалась (silent)");
        check(AimMemoryDiag().dev < 15.f, "отклонение в пределах нормы");
    }

    // E. «Сайлент» с целью за пределами отклонения: ось упирается в предел.
    {
        resetGame();
        g_targetYaw = 60.f; g_targetPitch = 0.f;
        Result r = run(AIM_MODE_SILENT, 60);
        printf("САЙЛЕНТ цель 60°: отклонение %.1f°, остаток %.1f°, длина оси %.4f\n",
               AimMemoryDiag().dev, r.errAfterAim,
               sqrtf(g_game.ax * g_game.ax + g_game.ay * g_game.ay + g_game.az * g_game.az));
        check(AimMemoryDiag().dev <= 35.5f, "отклонение зажато пределом (35°)");
        check(AimMemoryDiag().dev > 30.f, "ось всё же довёрнута к цели");
        check(std::isfinite(g_game.ax) && std::isfinite(g_game.ay) && std::isfinite(g_game.az),
              "ось не испортилась");
    }

    // F. «Сайлент» долго: отклонение не копится, кадр за кадром ось свежая.
    {
        resetGame();
        g_targetYaw = 8.f; g_targetPitch = 4.f;
        Result r = run(AIM_MODE_SILENT, 300);
        printf("САЙЛЕНТ 300 кадров: отклонение %.1f°, остаток %.2f°, смен знака %d\n",
               AimMemoryDiag().dev, r.errAfterAim, r.signFlips);
        check(AimMemoryDiag().dev <= 35.5f, "отклонение не накопилось за предел");
        check(r.errAfterAim < 0.5f, "ось каждый кадр доворачивается заново");
        check(std::isfinite(g_game.ay), "ось не испортилась");
    }

    // G. «Сайлент» без оси выстрела: записи нет, ось отдана игре.
    {
        resetGame();
        g_game.haveAxis = false;
        Result r = run(AIM_MODE_SILENT, 30);
        printf("САЙЛЕНТ без оси: записей %d, отказов %d, тостов %d\n", r.writes, r.fails, r.toasts);
        check(r.writes == 0, "записей нет");
        check(fabsf(g_game.camYaw()) < 1e-3f, "камера не двигалась");
        check(r.toasts >= 1, "пользователю сказали, почему режим молчит");
    }

    // H. Выключенный аим ничего не пишет ни в одном режиме.
    {
        resetGame();
        g_state.aim_touch = false;
        Result r = run(AIM_MODE_SILENT, 30);
        check(r.writes == 0, "с выключенным аимом ось отдана игре");
    }

    // I. Мусорный угол камеры не вешает кадр.
    //
    // С устройства пришёл лог, где оверлей встал на 255 с ровно в стадии выбора
    // цели: приведение угла делалось циклом «пока больше 180 — вычесть 360», а
    // из памяти пришло мусорное значение (от 1e38 вычитание 360 ничего не
    // меняет — цикл бесконечен). Игра при этом продолжала работать, потому что
    // залип только поток отрисовки. Кадр прогоняется в отдельном потоке с
    // таймаутом: если аим залипнет, стенд скажет «ПРОВАЛ», а не повиснет сам.
    {
        resetGame();
        g_targetYaw = 8.f; g_targetPitch = 4.f;
        run(AIM_MODE_SILENT, 5);                 // чтобы упреждение развернулось

        const float junk[3] = { 3.4e38f, INFINITY, -INFINITY };
        int hung = 0;
        for (float j : junk) {
            g_garbageCam = j;
            auto fut = std::async(std::launch::async, [] {
                snapshot(); UpdateAim(0.08f); AimEndFrame();
            });
            if (fut.wait_for(std::chrono::seconds(3)) != std::future_status::ready) {
                ++hung;
                printf("    залип на угле %g\n", j);
                fut.wait();  // иначе деструктор future всё равно дождётся
            }
        }
        g_garbageCam = 0.f;
        check(hung == 0, "мусорный угол камеры не вешает кадр (3 значения)");
    }

    // J. Обёртка угла: одна формула вместо цикла.
    {
        check(fabsf(WrapDeg180(190.f) - (-170.f)) < 0.01f, "190° -> -170°");
        check(fabsf(WrapDeg180(-190.f) - 170.f) < 0.01f,   "-190° -> 170°");
        check(fabsf(WrapDeg180(360.f)) < 0.01f,            "360° -> 0°");
        check(fabsf(WrapDeg180(0.f)) < 0.01f,              "0° остаётся 0°");
        check(WrapDeg180(180.f) == 180.f,                  "180° не переворачивается");
        const float big = WrapDeg180(3.4e38f);
        check(std::isfinite(big) && fabsf(big) <= 180.f,   "мусор 1e38 -> конечный угол");
        check(WrapDeg180(INFINITY) == 0.f,                 "бесконечность -> 0");
        check(WrapDeg180(-INFINITY) == 0.f,                "минус бесконечность -> 0");
        check(WrapDeg180(NAN) == 0.f,                      "не число -> 0");
    }

    printf(g_fail ? "--- ЕСТЬ ПРОВАЛЫ\n" : "--- все проверки пройдены\n");
    return g_fail;
}
