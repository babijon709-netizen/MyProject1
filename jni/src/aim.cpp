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
#include "aim_touch_core.h"      // свайповый драйвер тач-аима
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
// Тач-путь поток больше не читает: учёт трубы целиком в aim_touch_core.h, и
// заказ там списывается по отклику остатка, а не по потоку камеры (потока в
// штатном режиме на устройстве всё равно нет). Функция оставлена для
// диагностики (AimMemoryDiag-подобные сводки) и на случай, когда ось камеры
// станет читаемой везде.
[[maybe_unused]] static bool aim_camera_flow(float& yawDeg, float& pitchDeg) {
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
    // Накопленный сдвиг пальца с момента последнего НАБЛЮДАЕМОГО поворота камеры:
    // из него выучивается коэффициент град/px (см. learn ниже).
    static float s_pendDx = 0.f, s_pendDy = 0.f;
    // Кадры без цели подряд: палец держим недолго, чтобы мигание чтения не
    // сочлось тапом (выстрел) и жест не сорвался.
    static int   s_lostFrames = 0;
    // Весь такт прицела: свайповый драйвер (jni/src/aim_touch_core.h).
    static aim_touch::Lock s_lock;
    static float s_trimYaw = 1.f, s_trimPitch = 1.f;
    static bool s_wasActive = false;
    static AimPick pick;

    auto resetRuntime = [&]() {
        AimReleaseFinger(s_fingerDown);
        s_fx = s_fy = 0.0f;
        s_lastCamYaw = s_lastCamPitch = 0.0f;
        s_haveLast = false;
        s_gainYaw = s_gainPitch = 0.0f;
        s_trackYaw.reset(); s_trackPitch.reset();
        s_pendDx = s_pendDy = 0.0f;
        s_lostFrames = 0;
        s_lock.reset();
        s_trimYaw = s_trimPitch = 1.0f;
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
    // Такт оверлея и такт игры не совпадают: команда ждёт отклика до восьми
    // медленных кадров игры. Из этой оценки собирается окно трубы для драйвера
    // свайпов (см. ниже), по нему же считался и прежний такт с подтверждением.
    const float responseWindow = aim_control::touch_response_window(dt);

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
            s_pendDx = s_pendDy = 0.f;
            s_lock.reset();
        }
        return;
    }
    s_lostFrames = 0;
    if (pick.switched) {
        s_haveLast = false;                      // do not learn gain across a target switch
        s_trackYaw.reset(); s_trackPitch.reset();  // и оценку коэффициента тоже
        s_pendDx = s_pendDy = 0.f;
        s_lock.reset();
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
    }
    if (haveCam) { s_lastCamYaw = camYaw; s_lastCamPitch = camPitch; s_haveLast = true; }
    else {
        s_haveLast = false;
        s_pendDx = s_pendDy = 0.f;
    }

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
    // ---- controller -------------------------------------------------------
    // Весь такт ведёт aim_touch::Lock: шаг прицела выдаётся ЦЕЛЫМ свайпом,
    // следующий — только когда предыдущий отработал (отклик по остатку) или
    // истекло окно трубы. Прежний контроллер выдавал долю остатка каждый такт
    // оверлея, и шаги складывались в трубе касания: на 12-15 фпс игры её окно
    // доходит до 0.5-0.7 с, поэтому к моменту отклика камера получала сумму
    // шагов больше остатка — маятник первых секунд наведения. Из окружения
    // драйверу нужны только зона пальца, квант цифровера, коэффициент и
    // тайминги трубы — всё это собирается здесь.
    float sm = g_state.gun_str;
    if (!(sm >= 1.f)) sm = 1.f;
    if (sm > 10.f) sm = 10.f;

    float minX = ptX - 0.18f * sw, maxX = ptX + 0.23f * sw;
    float minY = ptY - 0.38f * sh, maxY = ptY + 0.38f * sh;
    if (minX < sw * 0.02f) minX = sw * 0.02f;
    if (maxX > sw * 0.98f) maxX = sw * 0.98f;
    if (minY < sh * 0.02f) minY = sh * 0.02f;
    if (maxY > sh * 0.98f) maxY = sh * 0.98f;

    // Остаток — прямо из выбранной кости: учёт трубы ведёт сам драйвер, вычитать
    // из остатка «заказанное в полёте» здесь больше не нужно (в этом и была
    // причина маятника: двойной учёт одного и того же заказа).
    const float errYaw = best.yaw, errPitch = best.pitch;

    aim_touch::Config acfg;
    acfg.point_x = ptX; acfg.point_y = ptY;
    acfg.box_x0 = minX; acfg.box_x1 = maxX;
    acfg.box_y0 = minY; acfg.box_y1 = maxY;
    acfg.units_per_px = unitsPerPx;
    acfg.speed = sm;
    acfg.deadband_yaw = deadYaw;
    acfg.deadband_pitch = deadPitch;
    // Окно трубы: пока заказ не откликнулся, второй свайп не выдаётся (см.
    // tube_window в ядре). Берём прежнюю оценку отклика от такта оверлея —
    // переоценить окно безопасно (аим чуть медленнее), недооценить нельзя:
    // это второй свайп поверх недоехавшего, то есть перелёт.
    acfg.pipeline_seconds = responseWindow;
    acfg.cycle_seconds = 0.20f;

    aim_touch::Gains gains;
    // «Известен» здесь — значит по оси есть рабочее число, по которому можно
    // считать шаг: и настройка чувствительности, и выученный коэффициент, и
    // оценка. Ноль в коэффициенте оставил бы ось без шага вовсе (прежний код в
    // этом случае всё равно вёл палец по запасному значению).
    gains.yaw_known = (fabsf(gy) > 1e-4f);
    gains.pitch_known = (fabsf(gp) > 1e-4f);
    // «Подтверждён» — коэффициент измерен на этой петле (по повороту камеры или
    // оценщиком по реакции прицела), а не взят из настройки: по подтверждённому
    // шаг идёт на весь остаток сразу, по настройке — с запасом. Настройка даёт
    // знак и порядок величины: её множитель игра применяет к сдвигу касания.
    gains.yaw_verified = (s_gainYaw != 0.f) && aim_touch::gain_trusted(s_gainYaw, gy);
    gains.pitch_verified = (s_gainPitch != 0.f) && aim_touch::gain_trusted(s_gainPitch, gp);
    gains.yaw = gy;
    gains.pitch = gp;

    const aim_touch::Outcome act = s_lock.step(errYaw, errPitch, dt, acfg, gains);
    switch (act.action) {
        case aim_touch::Action::Place:
            s_fx = act.x; s_fy = act.y;
            Touch_Down(s_fx, s_fy);
            s_fingerDown = true;
            // Перенос пальца — не команда прицелу: из накопленного сдвига его
            // надо исключить, иначе обучение коэффициента посчитает его ходом.
            s_pendDx = s_pendDy = 0.f;
            s_trackYaw.reset(); s_trackPitch.reset();
            s_haveLast = false;
            break;
        case aim_touch::Action::Move: {
            const float dx = act.x - s_fx, dy = act.y - s_fy;
            s_fx = act.x; s_fy = act.y;
            s_pendDx += dx; s_pendDy += dy;
            Touch_Move(s_fx, s_fy);
            break;
        }
        case aim_touch::Action::Hold:
            // Палец стоит, но тач жив: иначе игра сочтёт удержание за тап
            // (выстрел) и снимет жест.
            if (s_fingerDown) Touch_Move(s_fx, s_fy);
            break;
        case aim_touch::Action::Lift:
            AimReleaseFinger(s_fingerDown);
            s_pendDx = s_pendDy = 0.f;
            // Отпустили — заказанное игра уже не отработает.
            s_trackYaw.reset(); s_trackPitch.reset();
            s_haveLast = false;
            break;
        case aim_touch::Action::None:
            break;
    }
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