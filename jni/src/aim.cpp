#include "aim.h"
#include "silent_aim.h"
#include "main.h"                // displayInfo, native_window_screen_*
#include "game.h"
#include "Android_touch/TouchHelperA.h"
#include "aim_learn.h"           // оценка град/px по реакции прицела
#include "app_state.h"
#include "cfg.h"
#include "menu.h"                // g_calibMode
#include "process.h"             // g_esp_attached
#include "ui_util.h"
#include "str.h"               // XS(): строки интерфейса зашифрованы в бинарнике
#include "hud.h"               // ShowToast: мемори-режимы сообщают о поломке тостом
#include "esp_draw.h"            // FrameBoxes, AimFovRadiusPx
#include "manes_aim_source.h"
#include "aim_control.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>
#include <thread>
#include <chrono>
#include <time.h>

static constexpr float kAimTouchDefX = 0.74f;
static constexpr float kAimTouchDefY = 0.50f;

float AimTouchFracX() {
    return (g_state.aim_tx > 0.f && g_state.aim_tx < 1.f) ? g_state.aim_tx : kAimTouchDefX;
}
float AimTouchFracY() {
    return (g_state.aim_ty > 0.f && g_state.aim_ty < 1.f) ? g_state.aim_ty : kAimTouchDefY;
}

// ============================ Aimbot ============================
//
// Drives the game camera with a synthetic "look" finger on the right half of
// the screen. The target is the exact bone world position (head / chest /
// pelvis) projected through the live camera matrices, expressed as a yaw/pitch
// offset from the camera forward axis. The controller is closed-loop: every
// frame it measures how many degrees the crosshair actually moved per pixel
// of finger travel and adapts its gain, so it converges in a handful of
// frames regardless of the in-game sensitivity setting.

struct AimTarget {
    bool  valid = false;
    unsigned long long id = 0;
    float yaw = 0.f, pitch = 0.f;   // degrees from crosshair (+right, +up)
    float sx = 0.f, sy = 0.f;       // screen position (px)
    float dist = 0.f;               // pixel distance from crosshair
    float world_dist = 0.f;
    float world[3] = {};            // точная мировая координата кости для silent-режима
    bool  world_valid = false;
    int   bone = -1;                // слот точки прицела (0 голова, 1 шея, 2 грудь)
    float health = -1.f;            // здоровье цели (для лога и проверки смерти)
    bool  respawning = false;       // PlayerManager.respawning (для лога)
    // Поза выбранной цели — те же габариты скелета, по которым решается
    // «мёртв». В лог они идут вместе с самой целью: иначе, когда игрок
    // жалуется «аим ведёт по трупу», нечем проверить, лежал ли тот труп
    // или его кости ещё стояли.
    int   skel_bones = 0;
    int   death_flag = -1;          // собственный флаг смерти игры
    float skel_up = -1.f, skel_flat = -1.f;
};

static void AimReleaseFinger(bool& fingerDown) {
    if (fingerDown) {
        Touch_Up();
        fingerDown = false;
    }
}

// File-scope so the auto-farm can yield the camera while the aimbot is
// actively pulling onto a player.
bool s_fingerDown = false;
// Аим ведёт цель прямо сейчас (ставится в AimSelectTarget, снимается в AimBegin).
bool g_aimActive = false;
std::atomic<unsigned long long> g_aim_pulse_target{0};
std::atomic<double> g_aim_pulse_at{0.0};
std::atomic<double> g_aim_hold_at{0.0};

// Запасной град/px — из чувствительности, выставленной в настройках клиента.
//
// Игра считает поворот как «накопленный сдвиг касания × m_Sensitivity»
// (MouseLook.ZJo, поле +0x34), поэтому град/px строго пропорционален
// чувствительности, и коэффициент обязан ехать за настройкой игрока.
//
// 0.10 град/px — ИЗМЕРЕННОЕ на устройстве значение (EV «камера: коэффициент …»
// в логе автофарма 14.09.2026: поворот/сдвиг даёт 0.05..0.18, медиана 0.10,
// p10 0.078, p90 0.178). Эталон, к которому оно привязано, — та же настройка
// игрока, при которой шёл замер: 2.00 (поле m_Sensitivity, прочитанное из игры;
// строка аима «чувствительность 2.00» в my_benzware.log 16.09.2026 21:35).
//
// Раньше здесь стояло 5.0 (заводское значение из MouseLook..ctor) — и для игрока
// с настройкой 2.0 запасной коэффициент выходил 0.04 при измеренных 0.10, то
// есть шаг пальца считался в 2.5 раза больше нужного. Петля «палец -> камера ->
// ошибка» при этом расходится (L = k * 0.10 / 0.04 = 1.25 > 1 при k <= 0.5),
// и прицел трясёт: ровно то, на что жаловался игрок. Проверено на стенде
// (tools/aim/sim_loop.cpp, модель петли): с рабочим 0.04 при камере 0.10 захват
// «до 1 град» не наступает вообще, перелёт 5.5..9.8 град, СКЗ хвоста 4.5..6.1;
// с рабочим 0.10 — захват за 1.30 с и хвост 0.00.
//
// Страховка от такой ошибки в эталоне — оценка коэффициента по реакции прицела
// (aim_learn.h): она сходится к истинному значению за ~секунду и работает на
// любом устройстве, даже если настройку в игре поменяли на ходу. Запасное
// значение нужно только на первый такт, пока оценка не готова.
static constexpr float kAimRefSensitivity = 2.0f;  // настройка игрока при замере 0.10 град/px

// Полоса правдоподобия коэффициента (в единицах эталона): ниже 0.04 град/px
// замеров нет, выше 0.25 — тоже (это уже чувствительность 5 и больше).
static constexpr float kAimScaleMin = 0.4f;
static constexpr float kAimScaleMax = 2.5f;

// Во сколько раз настройка игрока отличается от эталонной (2.00 при замере).
// Ровно во столько же раз игра меняет град/px: 2.0 -> 0.10, 5.0 -> 0.25,
// 1.0 -> 0.05. Полоса правдоподобия едет вместе с настройкой — она отмерена при
// 2.00, и зажимать ею значение при чувствительности 5 — это прицел, который
// втрое медленнее, чем позволяет игра.
//
// Значение ДЕРЖИМ и обновляем редко. Чтение может не пройти в отдельном кадре
// (нет локального игрока, мигнул список игроков), а шаг пальца считается как
// err/коэффициент: если значение скачет между прочитанным и запасным, шаг
// меняется в разы от кадра к кадру — это и видно как «аим сильно дергает».
// Поэтому: принятое значение живёт до заметного изменения (25% и больше, то
// есть ручная правка настройки в меню игры) и читать чаще 1.5 с не пробуем.
//
// from_game = true — коэффициент выведен из настройки игры (значит, годится как
// «известный»: по нему видно квант ввода), false — запасное измеренное 0.10.
//
// Монотонные секунды. Нужны ровно для одного: «принятое значение
// чувствительности держим 1.5 с, чаще не читаем».
double MonoNow() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

float AimSensitivityScale(bool& from_game) {
    constexpr double kHoldSeconds = 1.5;    // чаще настройку руками не меняют
    constexpr float  kAdoptRatio  = 1.25f;  // мельче этого — не изменение

    static float  held           = 1.0f;
    static bool   held_from_game = false;
    static double held_at        = 0.0;

    const double now = MonoNow();
    if (held_at > 0.0 && (now - held_at) < kHoldSeconds) {
        from_game = held_from_game;
        return held;
    }
    held_at = now;

    float sensitivity = 0.f;
    if (esp_read_look_sensitivity(sensitivity)) {
        float scale = sensitivity / kAimRefSensitivity;
        if (!std::isfinite(scale) || scale <= 0.f) scale = 1.f;
        if (scale < kAimScaleMin) scale = kAimScaleMin;
        if (scale > kAimScaleMax) scale = kAimScaleMax;
        const bool changed = !(scale < held * kAdoptRatio && scale > held / kAdoptRatio);
        if (!held_from_game || changed) { held = scale; held_from_game = true; }
    } else if (!held_from_game) {
        held = 1.0f;
    }
    // Прочитанное однажды значение переживает осечку чтения: оно верное, а
    // подмена его запасным — это и есть скачок шага.
    from_game = held_from_game;
    return held;
}

float AimSensitivityGain(bool& from_game) {
    return kAimGainAtRef * AimSensitivityScale(from_game);
}

// ============================ Общее для всех режимов ========================
//
// The three modes are touch (synthetic camera input), memory (write view input
// to game memory), and silent (write the shot axis). Touch and memory share FOV,
// priority, and angle filtering; all modes use the exact bone selected in the menu.
// game.cpp predicts with world velocity and weapon ballistics.

// Состояние выбора цели. Держится каждым режимом своим экземпляром: у сайлента
// и у памяти другая петля (нет пальца, который живёт между кадрами), и мешать
// им залипание на цель с тачем незачем.
struct AimPick {
    unsigned long long lastId = 0;
    int lastBone = -1;
    aim_control::AngleFilter angleFilter{};
    bool filterUsesCamera = false;
    bool filterModeSet = false;
    bool switched = false;

    void reset() {
        lastId = 0;
        lastBone = -1;
        switched = false;
        filterUsesCamera = false;
        filterModeSet = false;
        angleFilter.reset();
    }
};

// Common per-frame checks. Calibration, detached state, freecam, and the
// scope-only condition can pause aiming; regular menu and popover windows do
// not. The reason code is recorded in AimMemDiag.inactive:
//   1 disabled              2 game process detached
//   4 zone calibration      6 freecam enabled
//   7 scope-only without ADS                               8 invalid screen
// ADS state and its source are retained because the inactive path clears the
// rest of the diagnostics.
static int  s_adsState  = -1;   // 1 в прицеле, 0 нет, -1 режим выключен
static int  s_adsSource = 0;    // 1 Aim, 2 isAiming, 0 не подтвердилась
static bool s_adsDebounced = false;
static int s_adsOnStreak = 0, s_adsOffStreak = 0;

static bool AimBegin(float& dt, float& sw, float& sh, int* why = nullptr) {
    g_aimActive = false;
    // Calibration owns the physical touch used to select a screen point.
    // Freecam owns the camera and cannot share its look controller with aim.
    bool active = g_state.aim_touch && g_esp_attached &&
                  g_calibMode == 0 && !g_state.freecam_on;

    // "Только с прицелом": only steer while the local player is ADS.
    if (g_state.aim_scope_only && g_esp_attached) {
        const bool rawAds = esp_local_player_is_aiming();
        s_adsState = rawAds ? 1 : 0;
        s_adsSource = esp_local_aim_source();
        if (rawAds) {
            if (s_adsOnStreak < 2) ++s_adsOnStreak;
            s_adsOffStreak = 0;
            if (s_adsOnStreak >= 2) s_adsDebounced = true;
        } else {
            if (s_adsOffStreak < 2) ++s_adsOffStreak;
            s_adsOnStreak = 0;
            if (s_adsOffStreak >= 2) s_adsDebounced = false;
        }
        if (active && !s_adsDebounced) active = false;
    } else {
        s_adsState = -1;
        s_adsSource = 0;
        s_adsDebounced = false;
        s_adsOnStreak = s_adsOffStreak = 0;
    }
    if (!active) {
        // Причину пишем по первому же невыполненному условию: в логе 15:57
        // «объект=0» шёл в 219 кадрах из 319, и без расшифровки это выглядело
        // как «потерялся MouseLook», хотя проверка объекта даже не запускалась.
        if (why) {
            if (!g_state.aim_touch)                  *why = 1;
            else if (!g_esp_attached)                *why = 2;
            else if (g_calibMode != 0)               *why = 4;
            else if (g_state.freecam_on)             *why = 6;
            else                                     *why = 7;
        }
        return false;
    }

    if (dt <= 0.f || !std::isfinite(dt)) dt = 1.f / 60.f;
    if (dt > 0.1f) dt = 0.1f;

    sw = (float) native_window_screen_x;
    sh = (float) native_window_screen_y;
    if (displayInfo.width > displayInfo.height && displayInfo.width >= 100 && displayInfo.height >= 100) {
        sw = (float) displayInfo.width;  sh = (float) displayInfo.height;
    } else if (displayInfo.height > displayInfo.width && displayInfo.height >= 100 && displayInfo.width >= 100) {
        sw = (float) displayInfo.height; sh = (float) displayInfo.width;
    }
    if (sw < 100.f || sh < 100.f) {
        if (why) *why = 8;
        return false;
    }
    return true;
}

// Select and filter a target angle. A false result means no target; each mode
// decides when to release its lock. degPerPx lets touch set a pixel-based deadband.

static unsigned long s_dead_skipped = 0;   // мёртвых не взяли в цель (для лога)
static unsigned long s_no_skeleton = 0;         // у цели нет скелета вовсе
static unsigned long s_bones_not_projected = 0; // configured bone is unavailable/unprojected
static unsigned long s_ally_skipped = 0;        // союзников не берём никогда
static unsigned long s_offscreen_skipped = 0;   // есть кости, но точка вне экрана
static unsigned long s_out_of_fov = 0;          // точка дальше круга обзора
// Сколько всего игроков приходило на выбор цели за период (для лога): без
// этой цифры «пропущено N» невозможно прочитать — непонятно, много это или
// все, кого видно.
static unsigned long s_seen_boxes = 0;
// Сколько из них — боты (боксы с is_bot): сводка считает их отдельно, чтобы
// «игроков N» читалось честно.
static unsigned long s_seen_bots = 0;
// Боты пропущены, потому что их ЕСП выключено (аим берёт ботов только
// с включёнными маркерами).
static unsigned long s_bot_noesp_skipped = 0;

// ---- поток камеры между тактами -------------------------------------------
// Куда и насколько повернулась камера с прошлого такта. Считается по сдвигу
// ЦЕНТРОВ боксов с совпавшим id: мир стоит, поэтому сдвиг всех боксов разом —
// это чистый поворот камеры (движение одного-двух игроков давится медианой,
// для этого и нужен порог в три совпадения).
//
// Поток — это наблюдение за КАМЕРОЙ, а не за остатком, и на нём держится учёт
// «в полёте» у тач-аима при просадке ФПС: когда игра отвечает на касание раз
// в 5-8 кадров, остаток может стоять или расти и у применённого приказа
// (цель сама ушла), а камера при этом честно крутится. Поглощение, списанное
// по потоку, отличает «приказ в трубе ещё не применён» (камера стоит) от
// «применён» (камера повернулась на столько-то).
// Знаки: поток положителен туда, куда смотрит камера. Камера вправо — боксы
// едут влево (медДкс < 0), поэтому рысканье входит с минусом. Камера вверх —
// боксы едут вниз (медДкс по Y > 0), тангаж входит с плюсом (вверх — больше).
static float s_flowYawDeg = 0.f, s_flowPitchDeg = 0.f;
static bool  s_flowValid = false;
struct AimFlowPrev { unsigned long long id; float cx, cy; };
static std::vector<AimFlowPrev> s_flowPrev;

static void AimUpdateFlow(const std::vector<EspBox>& boxes, float sw, float sh, float camFov) {
    s_flowValid = false;
    s_flowYawDeg = s_flowPitchDeg = 0.f;
    std::vector<AimFlowPrev> cur;
    cur.reserve(boxes.size());
    for (const EspBox& b : boxes) {
        if (b.id == 0) continue;
        if (!std::isfinite(b.x1) || !std::isfinite(b.y1) ||
            !std::isfinite(b.x2) || !std::isfinite(b.y2)) continue;
        cur.push_back({b.id, (b.x1 + b.x2) * 0.5f, (b.y1 + b.y2) * 0.5f});
    }
    if (!s_flowPrev.empty() && !cur.empty()) {
        float dxs[32], dys[32]; int n = 0;
        for (const auto& c : cur) {
            if (n >= 32) break;
            for (const auto& p : s_flowPrev) {
                if (p.id != c.id) continue;
                dxs[n] = c.cx - p.cx; dys[n] = c.cy - p.cy; ++n;
                break;
            }
        }
        if (n >= 3) {
            // Медиана сортировкой вставкой: игроков на экране единицы.
            for (int i = 1; i < n; ++i) {
                const float vx = dxs[i], vy = dys[i];
                int j = i - 1;
                while (j >= 0 && dxs[j] > vx) { dxs[j + 1] = dxs[j]; --j; }
                dxs[j + 1] = vx;
                j = i - 1;
                while (j >= 0 && dys[j] > vy) { dys[j + 1] = dys[j]; --j; }
                dys[j + 1] = vy;
            }
            const float medDx = dxs[n / 2], medDy = dys[n / 2];
            const float fovRad = camFov * (float)M_PI / 180.f;
            // Горизонтальное поле на пиксель — через вертикальное и сторону.
            const float yawPerPx = 2.f * atanf(tanf(fovRad * 0.5f) * (sw / sh)) *
                                   (180.f / (float)M_PI) / sw;
            const float pitchPerPx = camFov / sh;
            float fy = -medDx * yawPerPx, fp = medDy * pitchPerPx;
            if (fy >  30.f) fy =  30.f; else if (fy < -30.f) fy = -30.f;
            if (fp >  30.f) fp =  30.f; else if (fp < -30.f) fp = -30.f;
            if (std::isfinite(fy) && std::isfinite(fp)) {
                s_flowYawDeg = fy; s_flowPitchDeg = fp;
                s_flowValid = true;
            }
        }
    }
    s_flowPrev = std::move(cur);
}

// Поток этого такта (в градусах). Ложь — меньше трёх боксов совпало по id.
static bool aim_camera_flow(float& yawDeg, float& pitchDeg) {
    yawDeg = s_flowYawDeg; pitchDeg = s_flowPitchDeg;
    return s_flowValid;
}

// why — причина, по которой цель НЕ выбрана (0 когда цель есть). Коды:
//   1 боксов нет вовсе      2 все союзники      3 все мёртвые
//   4 нет скелета           5 кости не легли на экран
//   6 точка вне экрана      7 точка дальше круга обзора
// Без этого кода кадр «аим активен, прицел есть, записей ноль» нечем
// прочесть: в логе 17:06 таких было 32 из 92, и что именно не пустило
// цель — отфильтровали трупа, круг обзора или пустой список — не видно.
static bool AimSelectTarget(float sw, float sh, float dt, AimPick& pick, AimTarget& best,
                            float& degPerPx, int* why = nullptr) {
    if (why) *why = 0;
    const unsigned long was_ally = s_ally_skipped, was_dead = s_dead_skipped;
    const unsigned long was_nosk = s_no_skeleton,  was_noproj = s_bones_not_projected;
    const unsigned long was_off  = s_offscreen_skipped, was_fov = s_out_of_fov;
    const unsigned long was_seen = s_seen_boxes;
    const std::vector<EspBox>& boxes = FrameBoxes(sw, sh);

    // Сводку пропусков печатаем БЕЗ условий и ДО выбора. В сборке 829a1dc
    // строка «мёртвых N» жила внутри ветки «кости не нашлись», и когда
    // мёртвыми оказались вообще все игроки, в логе не появилось НИ ОДНОЙ
    // строки: причину жалобы «аим вообще не нацеливается на игроков»
    // (19.09) стало нечем подтвердить. Счётчик, который молчит именно
    // тогда, когда сломалось, — не диагностика.
    {
        static double s_at = -1e9;
        const double now = (double)clock() / CLOCKS_PER_SEC;
        const unsigned long total = s_no_skeleton + s_bones_not_projected + s_dead_skipped +
                                    s_ally_skipped + s_offscreen_skipped + s_out_of_fov +
                                    s_bot_noesp_skipped;
        if (now - s_at >= 5.0 && (total || s_seen_boxes)) {
            s_at = now;
            s_seen_boxes = 0; s_seen_bots = 0; s_bot_noesp_skipped = 0;
            s_ally_skipped = 0; s_dead_skipped = 0;
            s_no_skeleton = 0; s_bones_not_projected = 0;
            s_offscreen_skipped = 0; s_out_of_fov = 0;
        }
    }

    const float crossX = sw * 0.5f, crossY = sh * 0.5f;
    const float fovR = AimFovRadiusPx(sw, sh);
    float camFov = esp_camera_fov_deg();
    if (!(camFov > 1.f && camFov < 179.f)) camFov = 60.f;
    // degrees per pixel at the screen centre (vertical axis)
    degPerPx = camFov / sh;

    // Поток камеры этого такта: считаем по всем боксам ДО фильтров цели —
    // камера крутит и союзников, и трупы, и для оценки поворота они годятся.
    AimUpdateFlow(boxes, sw, sh, camFov);

    // Read the current menu/config selection on every target-selection pass.
    // Slots are [0] head, [1] neck, and [2] chest.
    int wantBone = g_state.aim_bone;
    if (wantBone < 0 || wantBone > 2) wantBone = 1;

    // ---- choose target ----
    float bestScore = 1e18f;
    for (const EspBox& b : boxes) {
        // На союзников аим не работает НИКОГДА, а не только пока в визуалах
        // включён их показ: проверка «g_state.esp_team && b.ally» означала,
        // что со скрытыми союзниками (показ выключен) аим работал по ним
        // как по врагам (вопрос 19.09). Признак своего — из групп/кланов
        // (game.cpp, groups_are_allied), он считается независимо от визуалов.
        ++s_seen_boxes;
        if (b.is_bot) ++s_seen_bots;
        // Боты — цели, только пока включено их ЕСП: выключил «Боты» —
        // аим их не трогает. Действует в обоих режимах (выбор цели общий).
        if (b.is_bot && !g_state.esp_bots) { ++s_bot_noesp_skipped; continue; }
        if (b.ally) { ++s_ally_skipped; continue; }
        // Мёртвых не берём: труп ещё несколько секунд остаётся в списке
        // игроков с корректным скелетом, и прицел уезжал на него (19.09).
        if (b.dead) { ++s_dead_skipped; continue; }
        AimTarget t;
        const bool holdCurrentTarget = g_state.aim_mode == AIM_MODE_MEMORY ||
                                       g_state.aim_mode == AIM_MODE_TOUCH;
        const int usedBone = aim_control::configured_bone_slot(wantBone, b.aim_valid);
        if (usedBone >= 0) {
            t.bone = usedBone;
            t.yaw = b.aim_yaw[usedBone];  t.pitch = b.aim_pitch[usedBone];
            t.sx  = b.aim_pts[usedBone][0]; t.sy = b.aim_pts[usedBone][1];
            t.valid = std::isfinite(t.yaw) && std::isfinite(t.pitch) &&
                      std::isfinite(t.sx) && std::isfinite(t.sy);
            t.world_valid = b.aim_world_valid[usedBone];
            for (int i = 0; i < 3; ++i) {
                t.world[i] = b.aim_world[usedBone][i];
                t.world_valid = t.world_valid && std::isfinite(t.world[i]);
            }
        } else {
            // Do not substitute another joint or a box estimate when the
            // configured bone is unavailable.
            if (b.has_skeleton) ++s_bones_not_projected; else ++s_no_skeleton;
            continue;
        }
        if (false) {
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
        t.health = b.health; t.respawning = b.respawning;
        t.skel_bones = b.skel_bones; t.skel_up = b.skel_up; t.skel_flat = b.skel_flat;
        t.death_flag = b.death_flag;
        const bool sticky = (pick.lastId != 0 && b.id == pick.lastId);
        if (t.sx < -sw || t.sx > sw * 2.f || t.sy < -sh || t.sy > sh * 2.f) { ++s_offscreen_skipped; continue; }
        if (g_state.aim_pos && !sticky && (t.sx < 0.f || t.sy < 0.f || t.sx > sw || t.sy > sh)) { ++s_offscreen_skipped; continue; }
        float dx = t.sx - crossX, dy = t.sy - crossY;
        t.dist = sqrtf(dx * dx + dy * dy);
        t.world_dist = b.distance;
        const bool memoryAim = g_state.aim_mode == AIM_MODE_MEMORY;
        const float fovRadiusDeg = manes_aim_source::fov_radius_degrees(cfg::aim::fov);
        const float angularDistance = memoryAim
            ? manes_aim_source::angular_distance_degrees(t.yaw, t.pitch)
            : 0.f;
        // The source memory path selects by angular distance; the touch path
        // keeps its screen-pixel circle because it steers through a touch drag.
        const bool inFov = memoryAim
            ? manes_aim_source::inside_angular_fov(
                  t.yaw, t.pitch, cfg::aim::fov, sticky ? fovRadiusDeg : 0.f)
            : t.dist <= (sticky ? fovR * 2.f : fovR);
        if (!inFov) { ++s_out_of_fov; continue; }

        // Memory aim ranks the same camera-relative angle used by the source;
        // touch aim ranks the projected pixel distance used by its drag path.
        const float pixelTerm = t.dist / (fovR > 1.f ? fovR : 1.f);
        const float angleTerm = angularDistance /
            (fovRadiusDeg > 1.f ? fovRadiusDeg : 1.f);
        const float aimTerm = memoryAim ? angleTerm : pixelTerm;
        const bool  haveRange = (t.world_dist >= 0.f) && std::isfinite(t.world_dist);
        const float rangeTerm = haveRange ? (t.world_dist / 120.f) : 1.f;
        float score;
        switch (g_state.aim_priority) {
            case 1:  score = aimTerm; break;
            case 2:  score = rangeTerm * 4.f + aimTerm * 0.05f; break;
            default: score = aimTerm + rangeTerm * 0.8f; break;
        }
        // Keep the current target until its exact bone becomes invalid or it
        // leaves the release FOV. This prevents nearby candidates from stealing
        // the lock during ADS/FOV changes.
        if (holdCurrentTarget && sticky) score = -1.f;
        else if (sticky) score *= 0.35f;
        if (score < bestScore) { bestScore = score; best = t; }
    }

    if (!best.valid) {
        if (why) {
            // Берём ту причину, которая в ЭТОМ кадре отсеяла больше всех:
            // счётчики накопительные, а нужен ответ про конкретный кадр.
            const unsigned long d[] = {
                s_seen_boxes - was_seen, s_ally_skipped - was_ally,
                s_dead_skipped - was_dead, s_no_skeleton - was_nosk,
                s_bones_not_projected - was_noproj, s_offscreen_skipped - was_off,
                s_out_of_fov - was_fov
            };
            // «Боксов нет» — только когда на выбор не пришёл НИ ОДИН игрок;
            // иначе причина та, которой отсеяно больше всего.
            int code = 1; unsigned long top = 0;
            for (int i = 1; i < 7; ++i) if (d[i] > top) { top = d[i]; code = i + 1; }
            if (d[0] > 0 && top == 0) code = 1;
            if (d[0] == 0) code = 1;
            *why = code;
        }
        return false;
    }

    const bool targetChanged = best.id != pick.lastId;
    const bool selectionChanged = targetChanged || best.bone != pick.lastBone;
    if (selectionChanged) pick.angleFilter.reset();
    if (g_state.aim_mode == AIM_MODE_MEMORY || g_state.aim_mode == AIM_MODE_TOUCH) {
        float filteredYaw = best.yaw;
        float filteredPitch = best.pitch;
        float cameraYaw = 0.0f, cameraPitch = 0.0f;
        const bool haveCamera = esp_aim_camera_angles(cameraYaw, cameraPitch) &&
                                std::isfinite(cameraYaw) && std::isfinite(cameraPitch);
        if (!pick.filterModeSet || pick.filterUsesCamera != haveCamera) {
            pick.angleFilter.reset();
            pick.filterUsesCamera = haveCamera;
            pick.filterModeSet = true;
        }
        float timeConstant = g_state.aim_mode == AIM_MODE_MEMORY ? 0.050f : 0.045f;
        if (best.world_dist > 0.0f && best.world_dist < 12.0f) timeConstant *= 0.75f;
        if (haveCamera) {
            filteredYaw = WrapDeg180(cameraYaw + filteredYaw);
            filteredPitch += cameraPitch;
            pick.angleFilter.update(filteredYaw, filteredPitch, dt, timeConstant);
            best.yaw = WrapDeg180(filteredYaw - cameraYaw);
            best.pitch = filteredPitch - cameraPitch;
        } else {
            pick.angleFilter.update(filteredYaw, filteredPitch, dt, timeConstant);
            best.yaw = filteredYaw;
            best.pitch = filteredPitch;
        }
    }
    // Цель взята — с этого такта камера (или ось выстрела) наша: автофарм ей
    // больше не командует.
    g_aimActive = true;
    // Состояние выбранной цели — чтобы по логу проверить признак смерти:
    // игрок жаловался, что аим ведёт по мёртвым, а по respawning (0x208) в
    // логе 14:55 не сработало ни разу. Печатаем здоровье и оба флага.
    pick.switched = selectionChanged;
    const double now = MonoNow();
    if (targetChanged) {
        g_aim_pulse_target.store(best.id, std::memory_order_relaxed);
        g_aim_pulse_at.store(now, std::memory_order_relaxed);
    }
    g_aim_hold_at.store(now, std::memory_order_relaxed);
    pick.lastId = best.id; pick.lastBone = best.bone;
    return true;
}

// ============================ Тач-аим ======================================
static void UpdateAimTouch(float dt) {
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
    static int   s_lostFrames = 0;
    // Доля шага при захвате цели: 0.3 сразу после захвата, 1.0 через 0.2 с
    // (см. «мягкий старт» ниже — страховка первого шага при наведении).
    static float s_acquire = 1.f;
    static int   s_holdFrames = 0;
    // Приказы «в полёте» (см. «шаг в полёте» ниже): окно последних приказов и
    // оценка по поглощению остатка. Объявлены вверху, потому что чистятся в
    // том числе на путях ДО их использования (нет цели, смена цели).
    struct FlightOrd { float yawDeg, pitchDeg, age; };
    static std::vector<FlightOrd> s_flight;
    static float s_absorbYaw = 0.f, s_absorbPitch = 0.f; // заказано, не отработано (град)
    static float s_stallYaw = 0.f, s_stallPitch = 0.f;
    static float s_prevRawYaw = 0.f, s_prevRawPitch = 0.f;
    static bool  s_havePrevRaw = false;
    // Счётчики однонаправленного остатка в полосе дрожи (см. константы выше).
    static int s_streakYaw = 0, s_streakPitch = 0;
    static int s_ackTimeouts = 0;
    static float s_ackDisabled = 0.f;
    static float s_trimYaw = 1.f, s_trimPitch = 1.f;
    static float s_flipDamp = 1.f;
    static float s_prevCtlYaw = 0.f, s_prevCtlPitch = 0.f;
    static bool s_haveCtlErr = false;
    static bool s_wasActive = false;
    static AimPick pick;

    auto resetRuntime = [&]() {
        AimReleaseFinger(s_fingerDown);
        s_fx = s_fy = 0.0f;
        s_lastCamYaw = s_lastCamPitch = 0.0f;
        s_haveLast = false;
        s_gainYaw = s_gainPitch = 0.0f;
        s_trackYaw.reset(); s_trackPitch.reset();
        s_pendDx = s_pendDy = s_pendTime = 0.0f;
        s_camMovedPrev = false;
        s_lostFrames = s_holdFrames = 0;
        s_acquire = 0.0f;
        s_flight.clear();
        s_absorbYaw = s_absorbPitch = 0.0f;
        s_stallYaw = s_stallPitch = 0.0f;
        s_prevRawYaw = s_prevRawPitch = 0.0f;
        s_havePrevRaw = false;
        s_streakYaw = s_streakPitch = 0;
        s_ackTimeouts = 0;
        s_ackDisabled = 0.0f;
        s_trimYaw = s_trimPitch = 1.0f;
        s_flipDamp = 1.0f;
        s_prevCtlYaw = s_prevCtlPitch = 0.0f;
        s_haveCtlErr = false;
        pick.reset();
    };

    float sw = 0.f, sh = 0.f;
    if (!AimBegin(dt, sw, sh)) {
        resetRuntime();
        s_wasActive = false;
        return;
    }
    if (!s_wasActive) {
        resetRuntime();
        s_wasActive = true;
    }
    // Overlay cadence can exceed the game's touch-processing cadence by a lot.
    // Keep commands pending long enough for up to eight slow game frames.
    const float responseWindow = aim_control::touch_response_window(dt);
    const float ackWait = responseWindow;

    // Точка, из которой аим водит палец: выбирается в меню тапом по экрану
    // (вкладка «Аим», см. g_calibMode == 3). Пока не задана — прежняя позиция
    // справа по центру, поэтому поведение по умолчанию не меняется.
    const float ptX = AimTouchFracX() * sw, ptY = AimTouchFracY() * sh;

    AimTarget best;
    float degPerPx = 0.f;
    if (!AimSelectTarget(sw, sh, dt, pick, best, degPerPx)) {
        // Keep the finger down briefly so a momentary read failure does not
        // register as a tap (tap-to-shoot in some layouts) or reset momentum.
        if (++s_lostFrames > 6) {
            AimReleaseFinger(s_fingerDown);
            s_haveLast = false; pick.reset();
            s_trackYaw.reset(); s_trackPitch.reset();
            // Палец отпущен — заказанное игра уже не отработает. Обнуляем
            // полёт, иначе возврат цели начнётся со старой оценкой.
            s_flight.clear();
            s_absorbYaw = s_absorbPitch = 0.f;
            s_stallYaw = s_stallPitch = 0.f;
            s_havePrevRaw = false;
            s_streakYaw = s_streakPitch = 0;
        }
        return;
    }
    s_lostFrames = 0;
    if (pick.switched) {
        s_haveLast = false;                      // do not learn gain across a target switch
        s_trackYaw.reset(); s_trackPitch.reset();  // и оценку коэффициента тоже
        // Заказы «в полёте» и их поглощение принадлежат прошлой цели.
        s_flight.clear();
        s_absorbYaw = s_absorbPitch = 0.f;
        s_stallYaw = s_stallPitch = 0.f;
        s_havePrevRaw = false;
        s_streakYaw = s_streakPitch = 0;
    }

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
        camYawDelta = WrapDeg180(camYaw - s_lastCamYaw);
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
    constexpr float kGainTrimStep  = 0.25f;   // ~6 тактов на новую подтяжку
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
    const bool camAnswered = camMoved || s_camMovedPrev;
    s_camMovedPrev = camMoved;
    if (s_ackDisabled > 0.f) s_ackDisabled -= dt;
    const bool ackGate = (s_ackDisabled <= 0.f);
    if (ackGate && s_fingerDown && !camAnswered &&
        (fabsf(s_pendDx) >= 1.f || fabsf(s_pendDy) >= 1.f)) {
        s_pendTime += dt;
        if (s_pendTime < ackWait) {
            Touch_Move(s_fx, s_fy);   // держим тач живым, ничего не двигаем
            return;
        }
        s_pendDx = s_pendDy = 0.f;    // ввод потерялся — продолжаем
        s_pendTime = 0.f;
        if (++s_ackTimeouts >= 3) { s_ackTimeouts = 0; s_ackDisabled = 5.f; }
    } else if (camAnswered) {
        s_ackTimeouts = 0;
    }

    // ---- «шаг в полёте» -----------------------------------------------------
    // Игра отрабатывает наш шаг не в этом такте: касание идёт через систему
    // ввода Android до 100 мс (шесть-восемь кадров игры). Когда настоящей оси
    // камеры нет (esp_aim_camera_angles -> false, штатный режим на этом
    // устройстве), такт с подтверждением выше не работает вовсе: контроллер
    // видит ещё не уменьшившийся остаток и шлёт ту же поправку повторно — к
    // моменту отклика камера поворачивается вдвое больше нужного: рывок через
    // цель и обратно, маятник первых секунд наведения.
    //
    // Учёт «в полёте» поэтому считается двумя независимыми оценками, а какая
    // из них правит — зависит от остатка (стенд tools/aim/lock_sim.cpp, сетка
    // оверлей 12-60 fps × задержка касания 2-8 кадров игры, захват 30°):
    //
    //   1. ПОГЛОЩЕНИЕ: заказ копится в градусах, а списывается по факту
    //      отработки. Основной источник — ПОТОК КАМЕРЫ (см. aim_camera_flow):
    //      камера повернулась на Х градусов — ровно Х заказанного она съела,
    //      независимо от того, куда за это время ушла цель. Запасной (когда
    //      боксов меньше трёх) — падение остатка: камера отработала приказ
    //      тогда, когда остаток стал меньше. Ни предположений о задержке, ни
    //      поедания уже отработанных приказов: сколько съедено, столько списано.
    //   2. Flight orders younger than responseWindow cap the estimate while
    //      tracking a moving target when camera feedback is unavailable.
    //
    // Выбор оценки по величине остатка. Раньше бралась просто меньшая по
    // величине из двух, и на просадке ФПС (жалоба 23.09, третий заход: «при
    // посадке ФПС тач-аим ходит маятником») это давало маятник: приказ живёт
    // в трубе дольше жизни окна, окно пустеет, меньшая оценка — ноль,
    // контроллер заказывает заново поверх летящего, по прилёте обоих камера
    // перелетает цель; плюс таймаут «потерялся» в 0.4 с списывал заказ,
    // который при 12-15 фпс ещё летит (до 0.7 с). Теперь на захвате
    // (|остаток| > 2.5°) правит поглощение — оно списывается по потоку камеры
    // при любой задержке трубы, — а на ведении у цели правит минимум с окном,
    // чтобы след за целью не растягивался.
    //
    // Защита знака: остаток перескочил ноль — весь висящий заказ обнуляется
    // сразу (камера дальше цели, летящий приказ толкнёт ещё дальше), а новый
    // приказ против хода не выдаётся вовсе. Довычитывать приказы, которых уже
    // нет, или заказывать против хода камеры — это и есть маятник.
    //
    // Четвёртый заход по маятнику (жалоба 24.09: «всё равно маятник первые
    // 1-2 секунды при наводке») добавил три правила, найденные стендом:
    //   * поглощение клэмпится к ТЕКУЩЕМУ остатку — цель сближается сама, и
    //     заказ по остатку прошлых тактов перелетал кость;
    //   * у цели (полоса 0.45°) шаг выдаётся только при устойчивом
    //     однонаправленном остатке — дрожь кости гоняла палец с частотой
    //     кадров (110 реверсов на стенде);
    //   * доля шага спадает вместе с остатком — поздние прилёты длинной
    //     трубы не перелетают кость.
    // Заказ без отклика — потерялся. При просадке ФПС игры труба касания
    // растягивается на 2-8 кадров ИГРЫ: на 12-15 фпс это до 0.5-0.7 с, а
    // прежние 0.4 с списывали заказ, который ещё ЛЕТИТ — контроллер считал
    // его потерянным, заказывал заново, и по прилёте обоих камера перелетала
    // цель (жалоба 23.09: «при посадке ФПС тач-аим ходит маятником»).
    // Порог обязан перекрывать худшую задержку трубы с запасом.
    static constexpr float kTouchStallTime  = 1.00f;  // заказ без отклика — потерялся
    // Граница «захват/ведение» для выбора оценки в полёте, град: при большом
    // остатке берём поглощение, при малом — минимум с окном (см. ниже).
    static constexpr float kTouchInflightBand = 2.5f;
    // Полоса «дрожь кости» у цели и порог устойчивого дрейфа в ней — та же
    // пара, что в мемори-локе: дрожь знак чередует и порог не набирает
    // (камера стоит), настоящее медленное движение знак держит и проходит.
    // Без порога контроллер гонял палец за дрожью с частотой кадров —
    // 110 реверсов на стенде, видимый маятник у цели.
    static constexpr float kTouchJitterBand  = 0.45f;
    static constexpr int   kTouchDriftStreak = 3;
    // Доля шага у цели падает вместе с остатком (до минимума): при длинной
    // трубе касания поздние прилёты заказанного не перелетают кость. На
    // стенде перелёт на дрожи ±0.4° падает с 0.9° до 0.4°, реверсов 0.
    static constexpr float kTouchRampBand = 2.5f;   // град, с чего начинается спад
    static constexpr float kTouchRampMin  = 0.40f;  // пол: ниже шаг станет меньше пикселя и прицел залипнет
    // If camera feedback timed out, use the latency queue even when an angle
    // source exists; otherwise the disabled ACK gate would resend every frame.
    const bool useFlight = !haveCam || s_ackDisabled > 0.0f;
    auto clearFlight = [&]() {
        s_flight.clear();
        s_absorbYaw = s_absorbPitch = 0.f;
        s_stallYaw = s_stallPitch = 0.f;
        s_havePrevRaw = false;
        s_streakYaw = s_streakPitch = 0;
    };
    float errYaw = best.yaw, errPitch = best.pitch;
    bool s_flowOkTick = false;
    if (useFlight) {
        // Поглощение списывается двумя способами, по возможности:
        //
        //   1. ПОТОК КАМЕРЫ (основной): камера повернулась на столько-то —
        //      ровно столько заказа она отработала, в какую бы сторону ни шёл
        //      остаток. На просадке ФПС это единственный честный признак:
        //      остаток может стоять и даже расти у ПРИМЕНЁННОГО приказа
        //      (цель сама ушла за те 2-8 кадров игры, пока приказ летел), и
        //      списание по падению остатка там молчит или ест не то.
        //   2. ПАДЕНИЕ ОСТАТКА (запасной, когда поток не посчитался — дуэль
        //      1 на 1, меньше трёх боксов на экране): прежний способ, на
        //      захвате вклад цели мал против шагов и он работает.
        if (s_havePrevRaw) {
            float flowYaw = 0.f, flowPitch = 0.f;
            const bool flowOk = aim_camera_flow(flowYaw, flowPitch);
            s_flowOkTick = flowOk;
            auto drainFlow = [](float& absorb, float flow) {
                if (absorb > 0.f && flow > 0.f)
                    absorb = (flow >= absorb) ? 0.f : absorb - flow;
                else if (absorb < 0.f && flow < 0.f)
                    absorb = (-flow >= -absorb) ? 0.f : absorb - flow;
            };
            if (flowOk) {
                const float wasYaw = s_absorbYaw, wasPitch = s_absorbPitch;
                drainFlow(s_absorbYaw, flowYaw);
                drainFlow(s_absorbPitch, flowPitch);
                if (s_absorbYaw   != wasYaw)   s_stallYaw   = 0.f;
                if (s_absorbPitch != wasPitch) s_stallPitch = 0.f;
            } else {
                // Камера повернулась в сторону заказа — остаток упал.
                const float dYaw   = s_prevRawYaw   - best.yaw;
                const float dPitch = s_prevRawPitch - best.pitch;
                if (dYaw > 0.f && s_absorbYaw > 0.f) {
                    s_absorbYaw = (dYaw >= s_absorbYaw) ? 0.f : s_absorbYaw - dYaw;
                    s_stallYaw = 0.f;
                } else if (dYaw < 0.f && s_absorbYaw < 0.f) {
                    s_absorbYaw = (-dYaw >= -s_absorbYaw) ? 0.f : s_absorbYaw - dYaw;
                    s_stallYaw = 0.f;
                }
                if (dPitch > 0.f && s_absorbPitch > 0.f) {
                    s_absorbPitch = (dPitch >= s_absorbPitch) ? 0.f : s_absorbPitch - dPitch;
                    s_stallPitch = 0.f;
                } else if (dPitch < 0.f && s_absorbPitch < 0.f) {
                    s_absorbPitch = (-dPitch >= -s_absorbPitch) ? 0.f : s_absorbPitch - dPitch;
                    s_stallPitch = 0.f;
                }
            }
            // Остаток перескочил через ноль — камера уже дальше цели: заказ,
            // который ещё висит в трубе, толкнёт её ещё дальше. Обнуляем не
            // дожидаясь прилёта (это и есть гашение перелёта на захвате).
            if (s_prevRawYaw * best.yaw < 0.f)         s_absorbYaw = 0.f;
            if (s_prevRawPitch * best.pitch < 0.f)     s_absorbPitch = 0.f;
        }
        s_prevRawYaw = best.yaw; s_prevRawPitch = best.pitch; s_havePrevRaw = true;
        // Заказ давно не откликается — ввод потерялся (палец перехвачен игрой,
        // экран перекрыт). Обнуляем, иначе оценка вечно ела бы остаток.
        if (s_absorbYaw != 0.f) { s_stallYaw += dt; if (s_stallYaw > kTouchStallTime) s_absorbYaw = 0.f; }
        if (s_absorbPitch != 0.f) { s_stallPitch += dt; if (s_stallPitch > kTouchStallTime) s_absorbPitch = 0.f; }
        // Заказ не может превышать ТЕКУЩИЙ остаток: цель сближается и сама —
        // если держать заказ по остатку прошлых тактов, по прилёте камера
        // проскакивает кость (на стенде перелёт 2.3° на идущей навстречу
        // цели, с клэмпом меньше, реверсов 0). Клэмп работает только когда
        // виден поток камеры: без потока уменьшение остатка — единственный
        // сигнал отработки, и клэмп списывал бы его дважды (на стенде шлейф
        // ведения в дуэли 1 на 1 рос с 5.6° до 63°).
        if (s_flowOkTick) {
            if (s_absorbYaw > 0.f && s_absorbYaw > best.yaw)
                s_absorbYaw = (best.yaw > 0.f) ? best.yaw : 0.f;
            if (s_absorbYaw < 0.f && s_absorbYaw < best.yaw)
                s_absorbYaw = (best.yaw < 0.f) ? best.yaw : 0.f;
            if (s_absorbPitch > 0.f && s_absorbPitch > best.pitch)
                s_absorbPitch = (best.pitch > 0.f) ? best.pitch : 0.f;
            if (s_absorbPitch < 0.f && s_absorbPitch < best.pitch)
                s_absorbPitch = (best.pitch < 0.f) ? best.pitch : 0.f;
        }
        // Окно приказов.
        for (auto& o : s_flight) o.age += dt;
        s_flight.erase(std::remove_if(s_flight.begin(), s_flight.end(),
                       [responseWindow](const FlightOrd& o) { return o.age >= responseWindow; }),
                       s_flight.end());
        float winYaw = 0.f, winPitch = 0.f;
        for (const auto& o : s_flight) { winYaw += o.yawDeg; winPitch += o.pitchDeg; }
        const auto minmag = [](float a, float b) {
            return fabsf(a) < fabsf(b) ? a : b;
        };
        // Что вычитается из остатка. Когда виден ПОТОК КАМЕРЫ: большой
        // остаток — это ЗАХВАТ, приказы идут один за другим в одну сторону,
        // и правильно списывать их по факту (поглощение, оно сливается по
        // потоку) какая бы ни была задержка трубы. Окно тут только вредило:
        // на просадке ФПС приказы живут в трубе дольше окна, оно пустеет, и
        // контроллер заказывал заново поверх летящих — маятник. Малый
        // остаток — ВЕДЕНИЕ у цели: минимум с окном держит след коротким.
        // Когда потока НЕТ (дуэль 1 на 1, меньше трёх боксов): поглощению
        // сливаться нечем, кроме падения остатка, и на ведении убегающей
        // цели оно не сливается вовсе (на стенде шлейф рос без границы) —
        // поэтому оценка ВСЕГДА ограничивается окном, как в первом гибриде.
        const bool noFlow = !s_flowOkTick;
        const float inflYaw   = (!noFlow && fabsf(best.yaw) > kTouchInflightBand)
                              ? s_absorbYaw   : minmag(s_absorbYaw,   winYaw);
        const float inflPitch = (!noFlow && fabsf(best.pitch) > kTouchInflightBand)
                              ? s_absorbPitch : minmag(s_absorbPitch, winPitch);
        errYaw   -= inflYaw;
        errPitch -= inflPitch;
        // Защита знака: в полёте уже не меньше остатка — стоим, ждём отклика.
        if (best.yaw * errYaw < 0.f)     errYaw = 0.f;
        if (best.pitch * errPitch < 0.f) errPitch = 0.f;
    } else {
        // Есть настоящая ось — работает такт с подтверждением, а не полёт.
        // Прошлый сырой остаток забываем: вернётся режим без оси — первый же
        // его такт не должен списывать заказ по разнице через пропасть.
        s_havePrevRaw = false;
    }

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

    // Stop inside the tighter of half a screen pixel and a 5 mm target error.
    // Quantization sets the true lower limit: the nearest representable input is
    // at most half one touch step away from the bone.
    const float deadYaw = aim_control::touch_lock_deadband(
        degPerPx, best.world_dist, gainKnownYaw ? qYaw : 0.0f);
    const float deadPitch = aim_control::touch_lock_deadband(
        degPerPx, best.world_dist, gainKnownPitch ? qPitch : 0.0f);
    if (fabsf(errYaw) < deadYaw && fabsf(errPitch) < deadPitch) {
        s_pendDx = s_pendDy = 0.f; s_pendTime = 0.f;
        // Orders from previous frames continue resolving without new input;
        // each expires after the adaptive responseWindow.
        if (s_fingerDown) Touch_Move(s_fx, s_fy); // hold still, keep the touch alive
        return;
    }

    // ---- полоса дрожи у цели ----
    // Остаток внутри полосы (но вне мёртвой зоны) — это либо дрожь кости,
    // либо медленный дрейф. Шаг выдаётся только при УСТОЙЧИВОМ остатке в одну
    // сторону (порог тактов): дрожь знак чередует и не проходит вовсе,
    // настоящий дрейф проходит целиком. Та же пара фильтров, что в
    // мемори-локе — оба режима у цели ведут себя одинаково спокойно.
    auto jitterGate = [&](float eff, int& streak) {
        if (eff == 0.f) { streak = 0; return 0.f; }
        if (fabsf(eff) >= kTouchJitterBand) { streak = 0; return eff; }
        const int s = (eff > 0.f) ? 1 : -1;
        streak = (streak * s > 0) ? streak + s : s;
        return (streak * s >= kTouchDriftStreak) ? eff : 0.f;
    };
    errYaw   = jitterGate(errYaw,   s_streakYaw);
    errPitch = jitterGate(errPitch, s_streakPitch);
    if (errYaw == 0.f && errPitch == 0.f) {
        // Дрожь: палец стоит, камера не ходит за костью.
        if (s_fingerDown) Touch_Move(s_fx, s_fy);
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
        s_acquire = 0.f;
        // Палец опустился заново: окно замера начинается с нуля, иначе первая
        // же пара «сдвиг пальца -> отклик» посчитает бросок пальца к точке.
        s_trackYaw.reset(); s_trackPitch.reset();
        clearFlight();
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
    // Frame-rate independent source smoothing fraction, adapted to the current dt.
    float k = manes_aim_source::frame_gain(frac, dt);
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
    // The device still showed a fast pendulum at 0.45; use a more conservative
    // fraction so delayed touch responses cannot reverse the next command.
    const float kStable = 0.30f;
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

    // Soft-start a new lock. Previously, the first frame could use the old
    // pixel cap (0.15*sh, about 162 px here). Android touch can take 2-8 frames
    // to reach the camera, so commands accumulated and caused an overshoot
    // before the in-flight window caught up. Ramp the first 0.2 s from one-third
    // to full strength; the in-flight accounting handles the remaining delay.
    if (pick.switched) s_acquire = 0.f;
    s_acquire += dt / 0.20f;
    if (s_acquire > 1.f) s_acquire = 1.f;
    k *= 0.3f + 0.7f * s_acquire;

    // Спад доли шага у цели: чем меньше остаток, тем меньшую его долю
    // заказываем за такт. Иначе заказы, сделанные на последних градусах,
    // прилетают по длинной трубе уже ПОСЛЕ того, как остаток закрыт, и
    // камера перелетает кость (захват «маятником» первых секунд). На
    // скорости захвата почти не сказывается: большой остаток заказывает
    // полный потолок шага.
    {
        const float maxEff = (fabsf(errYaw) > fabsf(errPitch)) ? fabsf(errYaw) : fabsf(errPitch);
        float ramp = maxEff / kTouchRampBand;
        if (ramp > 1.f) ramp = 1.f;
        if (ramp < kTouchRampMin) ramp = kTouchRampMin;
        k *= ramp;
    }

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

    // Bound each command in camera degrees as well as screen pixels. Learned
    // sensitivity must not multiply the first ADS correction into a large jump.
    const float maxAnglePerFrame = 1.0f + 0.65f * sm;
    const float maxYawStep = aim_control::touch_step_limit_pixels(sh, gy, maxAnglePerFrame);
    const float maxPitchStep = aim_control::touch_step_limit_pixels(sh, gp, maxAnglePerFrame);
    if (dx >  maxYawStep) dx =  maxYawStep;
    if (dx < -maxYawStep) dx = -maxYawStep;
    if (dy >  maxPitchStep) dy =  maxPitchStep;
    if (dy < -maxPitchStep) dy = -maxPitchStep;

    // Move in whole device units so the applied delta is exactly what we
    // measure next frame (gain learning) and the finger never accumulates a
    // hidden sub-unit remainder that later pops out as an unplanned step.
    float nx = snapGrid(s_fx + dx), ny = snapGrid(s_fy + dy);
    dx = nx - s_fx; dy = ny - s_fy;
    if (dx == 0.f && dy == 0.f) {
        s_pendDx = s_pendDy = 0.f; s_pendTime = 0.f;
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
        clearFlight();
        s_haveLast = false;
        // Перенос пальца — это не шаг прицела: замер по нему покажет мусор.
        s_trackYaw.reset(); s_trackPitch.reset();
        return;
    }
    s_fx = nx; s_fy = ny;
    s_pendDx += dx; s_pendDy += dy;   // ждёт отработки камерой (ack-такт)
    if (useFlight) {
        // Заказ — «в полёт»: в градусах остатка. Рысканье: шаг dx поворачивает
        // камеру на dx*gy к цели (остаток упадёт на столько); тангаж: сдвиг dy
        // вниз уменьшает возвышение, то есть остаток упадёт на -dy*gp.
        const float ordYaw = dx * gy, ordPitch = -dy * gp;
        s_flight.push_back({ordYaw, ordPitch, 0.f});
        s_absorbYaw   += ordYaw;   s_stallYaw   = 0.f;
        s_absorbPitch += ordPitch; s_stallPitch = 0.f;
    }
    Touch_Move(s_fx, s_fy);
}

// ---- диагностика режимов: по 4 строки в секунду в лог (Загрузки) ----------
// Главное, что должно быть видно: выигрывает ли запись гонку у игры. Поэтому
// в сайленте печатается то, что лежало в оси ДО нашей записи, — если там наше
// вчерашнее значение, значит игра ось в этом кадре не переписывала.
static unsigned long s_frames = 0;
static float         s_logTime = 0.f;

static const char* aim_mode_name() {
    switch (g_state.aim_mode) {
        case AIM_MODE_MEMORY: return "память";
        default:              return "тач";
    }
}

// Сколько раз ось выстрелаdobили в конце кадра между двумя строками лога

static void AimLogTick(float dt) {
    s_logTime += dt;
    if (s_logTime < 0.25f) return;
    s_logTime = 0.f;
    const AimMemDiag& d = AimMemoryDiag();
    if (g_state.aim_mode == AIM_MODE_MEMORY)
        ;
}

// ====================== Мемори-аим: «память» ===============================
//
// Memory-aim работает напрямую через накопитель MouseLook. Новый шаг всегда
// направлен к выбранной кости, но ограничен и уменьшается вместе с остатком,
// чтобы лок не дёргался у цели.
//
// Знаки накопителя (замер в логах): [0] rotationX = минус тангаж (вверх —
// меньше), [1] rotationY = рысканье (вправо — больше).

// Плавный жёсткий лок: контроллер держит выбранную кость, но не кладёт в
// MouseLook полный шум одного снимка рига. Мёртвая зона меньше одного пикселя
// на обычном FOV; за ней шаг плавно ограничен скоростью. Поэтому лок остаётся
// на кости, а микроколебания анимации/чтения не превращаются в дёрганье камеры.
// ACK по накопителю: расхождение с нашей записью, в пределах которого игра
// приняла значение. Это только диагностика отклика в меню.
static constexpr float kMemAckEps = 0.10f;

static AimMemDiag s_memDiag;

const AimMemDiag& AimMemoryDiag() { return s_memDiag; }

// Convert remaining error into a smooth lock step. Use a small deadband near
// the bone; farther away, acquisition speed depends on sensitivity and range.
static float mem_axis_step(float err, float dt, float range) {
    return aim_control::memory_step(err, dt, g_state.gun_str, range);
}

static void UpdateAimMemory(float dt) {
    static AimPick pick;
    static int s_missFrames = 0;
    static float s_lastCamYaw = 0.f, s_lastCamPitch = 0.f;
    static bool  s_haveLast = false;
    // Последняя запись в накопитель (в тех же завёрнутых координатах, что
    // вернёт чтение): по совпадению видно, приняла ли игра запись.
    static float s_lastWriteX = 0.f, s_lastWriteY = 0.f;
    static bool  s_haveLastWrite = false;
    float sw = 0.f, sh = 0.f;
    int why = 0;
    if (!AimBegin(dt, sw, sh, &why)) {
        s_memDiag = AimMemDiag{};
        s_memDiag.inactive = why;   // печатается в той же строке «объект=…»
        s_memDiag.ads = s_adsState; s_memDiag.ads_source = s_adsSource;
        pick.reset(); s_missFrames = 0; s_haveLast = false; s_haveLastWrite = false;
        return;
    }
    s_memDiag.inactive = 0;
    s_memDiag.ads = s_adsState; s_memDiag.ads_source = s_adsSource;

    // MouseLook нужен не ради чувствительности (накопитель уже в градусах), а
    // как проверка «объект тот»: m_Sensitivity в правдоподобных пределах —
    // значит это действительно MouseLook локального игрока.
    float sens = 0.f; bool invert = false, gyro = false;
    const bool haveParams = esp_mem_aim_look_params(sens, invert, gyro);
    s_memDiag.params = haveParams;
    if (!haveParams) {
        if (++s_missFrames > 6) { pick.reset(); s_haveLast = false; }
        s_haveLastWrite = false;
        return;
    }
    s_memDiag.gyro = gyro;

    // Текущий накопитель углов камеры. Запасная запись (без базы углов)
    // считается от него; абсолютной базе он не нужен, но прочитать его всё
    // равно необходимо: это самая дешёвая проверка, что объект жив.
    float accX = 0.f, accY = 0.f;
    if (!esp_mem_aim_read_angles(accX, accY)) {
        // Самый молчаливый путь: цель есть, писать нечем, а ни один счётчик
        // не растёт. Без отметки здесь кадр не отличить от «цели не было».
        ++s_memDiag.read_fail;
        if (++s_missFrames > 6) { pick.reset(); s_haveLast = false; }
        s_haveLastWrite = false;
        return;
    }

    AimTarget best;
    float degPerPx = 0.f;
    int noTgtWhy = 0;
    if (!AimSelectTarget(sw, sh, dt, pick, best, degPerPx, &noTgtWhy)) {
        s_memDiag.no_target = noTgtWhy;
        if (++s_missFrames > 6) { pick.reset(); s_haveLast = false; }
        s_haveLastWrite = false;
        return;
    }
    s_missFrames = 0;
    s_memDiag.no_target = 0;
    if (pick.switched) {
        // Другая цель: подтверждение прошлой записи и дрейф по прежней кости
        // к ней не относятся.
        s_haveLastWrite = false;
    }

    // ---- ACK по накопителю --------------------------------------------------
    // Мы пишем абсолютные углы. Если к этому такту в накопителе лежит ровно
    // записанное (в пределах kMemAckEps), игра запись приняла: свой ввод
    // игрок между нашими тактами не добавлял, чужой MouseLook значение не
    // перезаписал. Диагностике это даёт «отклик» и там, где оси камеры нет
    // вовсе (штатный режим этого устройства).
    if (s_haveLastWrite) {
        const bool consumed =
            fabsf(WrapDeg180(accY - s_lastWriteY)) < kMemAckEps &&
            fabsf(WrapDeg180(accX - s_lastWriteX)) < kMemAckEps;
        if (consumed) s_memDiag.responded = true;
    }

    // ---- камера: расхождение и отклик ---------------------------------------
    // Накопитель и камера должны совпадать: в конце каждого ZJo игра кладёт в
    // накопитель фактические углы камеры. Разошлись — это чужой MouseLook.
    float camYaw = 0.f, camPitch = 0.f;
    const bool haveCam = esp_aim_camera_angles(camYaw, camPitch);
    if (haveCam) {
        s_memDiag.mismatch = fabsf(WrapDeg180(accY - camYaw)) +
                             fabsf(WrapDeg180(accX + camPitch));
        // Камера задвигалась в такт записи и накопитель не расходится —
        // разворачивается именно наша запись (дополнительно к ACK).
        if (s_haveLast && s_memDiag.mismatch < 1.0f &&
            (fabsf(WrapDeg180(camYaw - s_lastCamYaw)) > 0.02f ||
             fabsf(camPitch - s_lastCamPitch) > 0.02f))
            s_memDiag.responded = true;
        s_lastCamYaw = camYaw; s_lastCamPitch = camPitch; s_haveLast = true;
    } else {
        s_haveLast = false;
    }

    // ---- плавный жёсткий лок --------------------------------------------------
    const float offYaw   = mem_axis_step(best.yaw, dt, best.world_dist);
    const float offPitch = mem_axis_step(best.pitch, dt, best.world_dist);
    if (offYaw == 0.f && offPitch == 0.f) {
        s_haveLastWrite = false;   // подтверждения больше нет — записи нет
        ++s_memDiag.on_target;
        return;
    }

    // Write from the camera orientation rather than from a pending accumulator
    // value. Repeated overlay ticks then overwrite the same absolute command
    // until the slower game frame consumes it, instead of stacking stale steps.
    float cameraYaw = 0.0f, cameraPitch = 0.0f;
    const bool haveCameraReference = esp_camera_angles(cameraYaw, cameraPitch) &&
                                     std::isfinite(cameraYaw) && std::isfinite(cameraPitch);
    float writeY = 0.0f, writeX = 0.0f;
    aim_control::memory_command_angles(accY, accX, haveCameraReference,
                                       cameraYaw, cameraPitch, offYaw, offPitch,
                                       writeY, writeX);

    // Градусы -> накопитель игры: X — «наклон вниз» (вверх — меньше),
    // Y — рысканье (вправо — больше).
    if (!esp_mem_aim_write_angles(writeX, writeY)) {
        ++s_memDiag.fails;
        return;
    }
    ++s_memDiag.writes;
    // Запомнить записанное — в обёрнутом виде: чтение возвращает обёрнутые
    // углы, и сравнение на следующем такте обязано быть в тех же координатах.
    s_lastWriteX = writeX; s_lastWriteY = writeY;
    s_haveLastWrite = true;
}


// ============================ Сайлент-аим ====================================
static void UpdateAimSilent(float dt) {
    static AimPick pick;
    float sw = 0.f, sh = 0.f;
    if (!AimBegin(dt, sw, sh)) {
        pick.reset();
        silent_aim::clear();
        return;
    }

    AimTarget best;
    float degPerPx = 0.f;
    if (!AimSelectTarget(sw, sh, dt, pick, best, degPerPx) || !best.world_valid) {
        g_aimActive = false;
        pick.reset();
        silent_aim::clear();
        return;
    }

    // Writer принимает только точную мировую координату кости: экранная точка
    // и поправки touch/memory-режимов не должны менять направление выстрела.
    if (!esp_silent_publish(best.id, best.world)) {
        g_aimActive = false;
        silent_aim::clear();
    }
}

// Сюда управление приходит после меню и фрикама. Если в середине кадра режим
// сменился, новый кадр не оставит writer с последним направлением до lease timeout.
void AimEndFrame() {
    const bool keep = g_state.aim_touch && g_state.aim_mode == AIM_MODE_SILENT &&
                      g_aimActive && !g_state.freecam_on;
    if (!keep) silent_aim::clear();
}

// Диспетчер: какой режим ведёт камеру в этом кадре.
void UpdateAim(float dt) {
    ++s_frames;
    const int mode = g_state.aim_mode;
    AimLogTick(dt);
    if (mode == AIM_MODE_MEMORY) UpdateAimMemory(dt);
    else if (mode == AIM_MODE_SILENT) UpdateAimSilent(dt);
    else                              UpdateAimTouch(dt);
}

// ============================ Автофарм =============================
//
// Синтетические тачи: два пальца (0 — джойстик движения, 2 — удар). Камера
// ведётся через накопитель MouseLook, тем же путём, что мемори-аим: абсолютная
// запись угла, без пальца и без коэффициента град/px. Слой памяти (game.cpp)
// отдаёт точку прицела в градусах от оси камеры; здесь — только автоматы.
//