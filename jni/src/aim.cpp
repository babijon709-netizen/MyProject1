#include "aim.h"
#include "silent_aim.h"
#include "main.h"                // displayInfo, native_window_screen_*
#include "game.h"
#include "Android_touch/TouchHelperA.h"
#include "aim_learn.h"           // оценка град/px по реакции прицела
#include "app_state.h"
#include "cfg.h"
#include "menu.h"                // g_calibMode
#include "widgets.h"             // g_sheet/g_pop: меню открыто — тач-аим на паузе
#include "process.h"             // g_esp_attached
#include "ui_util.h"
#include "str.h"               // XS(): строки интерфейса зашифрованы в бинарнике
#include "hud.h"               // ShowToast: мемори-режимы сообщают о поломке тостом
#include "esp_draw.h"            // FrameBoxes, AimFovRadiusPx
#include "manes_aim_source.h"
#include "aim_control.h"
#include "farm_joy.h"         // farmjoy::attack_held — кнопка атаки у игры
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>
#include <thread>
#include <chrono>
#include <time.h>

static constexpr float kAimTouchDefX = 0.72f;
static constexpr float kAimTouchDefY = 0.48f;

float AimTouchFracX() {
    return (g_state.aim_tx > 0.f && g_state.aim_tx < 1.f) ? g_state.aim_tx : kAimTouchDefX;
}
float AimTouchFracY() {
    return (g_state.aim_ty > 0.f && g_state.aim_ty < 1.f) ? g_state.aim_ty : kAimTouchDefY;
}

// ============================ Aimbot ============================
//
// Touch drags a synthetic look finger; memory writes camera angles, and silent
// writes the shot axis. Target eligibility and priority are shared below.

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
    float health = -1.f;            // reserved telemetry; not used for target selection
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
// True while an aim mode owns camera or shot-axis control.
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
// Touch steers with screen-space drags after sharing target selection with
// memory. game.cpp projects lead into aim points for both modes.

// Each mode keeps independent target/filter state.
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

struct RankedAimTarget {
    AimTarget target;
    float aim_error = 0.f;
    bool range_valid = false;
    bool sticky = false;
};

static AimPick s_touchPick;

// Per-frame checks for memory and silent modes. Calibration, detached state,
// freecam, and scope-only ADS can pause aiming. The reason is recorded in
// AimMemDiag.inactive:
//   1 disabled              2 game process detached
//   4 zone calibration      6 freecam enabled
//   7 scope-only without ADS  8 invalid screen   9 fire button not held
// ADS state and its source are retained because the inactive path clears the
// rest of the diagnostics.
static int  s_adsState  = -1;   // 1 в прицеле, 0 нет, -1 режим выключен
static int  s_adsSource = 0;    // 1 Aim, 2 isAiming, 0 не подтвердилась
static bool s_adsDebounced = false;
static int s_adsOnStreak = 0, s_adsOffStreak = 0;

// ---- «Только при стрельбе» --------------------------------------------------
// Аим ведёт камеру, пока игрок держит кнопку огня: прицел не уезжает на цель
// заранее и не досаживает после того, как огонь отпущен.
//
// Первый признак — защёлка атаки у хендлера ввода (PlayerInputHandler+0x68,
// farmjoy::attack_held): она единица ровно пока кнопка нажата, и ставит её сама
// игра — и палец на экране, и мышь. Наведение начинается первым же кадром
// нажатия, а не тогда, когда пуля кого-то задела.
//
// Второй признак — отметка попадания игры (PlayerEventHandler.LastHitTime,
// esp_mem_aim_last_shot). КАК ПЕРВЫЙ ПРИЗНАК ОНА НЕГОДНА: пишется при
// попадании, поэтому аим стартует с задержкой на полёт пули (на это и жаловались).
// Здесь она только продлевает окно на kFireHoldSeconds: полуавтомат за один тап
// успевает отпустить кнопку, и без хвоста аим гасил бы наведение между выстрелами.
//
// Окно держится kFireHoldSeconds — столько, чтобы аим не бросал цель между
// патронами очереди (при темпе 0.1 с наш кадр чтения легко попадает в паузу).
// Больше 0.5 с ставить нельзя: аим начнёт дотягивать прицел уже после того, как
// игрок кончил стрелять.
//
// Автофарм — исключение: кнопку на 0x68 в это время держит он, и прицелом на
// фарме рулит сам фарм. Два писателя в одну ось дают маятник, поэтому на фарме
// режим «только при стрельбе» аимбот НЕ ведёт (счётчик остаётся -1: это не наш
// случай, а не «не удалось прочитать»).
//
// Читается раз на кадр в UpdateAim и для всех трёх режимов сразу: у тач-аима
// свой вход (он не проходит AimBegin), а состояние общее.
static constexpr double kFireHoldSeconds = 0.30;
static double s_fireUntil = -1e9;
static float  s_hitMark = 0.f;
static bool   s_hitMarkSeen = false;
static int    s_fireState = -1;      // 1 стреляем, 0 нет, -1 режим выключен
static bool   s_fireGateAllow = true;

static void AimFireGateUpdate(double now) {
    if (!g_state.aim_fire_only) {
        s_fireGateAllow = true;
        s_fireState = -1;
        s_hitMarkSeen = false;
        s_fireUntil = -1e9;
        return;
    }
    if (g_state.farm_on) {
        s_fireGateAllow = false;
        s_fireState = -1;
        return;
    }
    bool held = false;
    // Прочитали — окно открывает само нажатие. Не прочитали (хендлёр ввода не
    // нашёлся) — остаётся только хвост от отметки попадания: ведём то, что
    // уже началось, и не стартуем вслепую.
    if (farmjoy::attack_held(held) && held) s_fireUntil = now + kFireHoldSeconds;
    float mark = 0.f;
    if (esp_mem_aim_last_shot(mark)) {
        if (!s_hitMarkSeen) {
            // Первая удачная читка только задаёт точку отсчёта: «значение
            // изменилось» без предыдущего значения ничего не значит.
            s_hitMarkSeen = true;
            s_hitMark = mark;
        } else if (mark != s_hitMark) {
            s_hitMark = mark;
            if (now + kFireHoldSeconds > s_fireUntil) s_fireUntil = now + kFireHoldSeconds;
        }
    }
    const bool firing = now <= s_fireUntil;
    s_fireState = firing ? 1 : 0;
    s_fireGateAllow = firing;
}

static bool AimScreenSize(float& sw, float& sh) {
    sw = (float)native_window_screen_x;
    sh = (float)native_window_screen_y;
    if (displayInfo.width > displayInfo.height && displayInfo.width >= 100 && displayInfo.height >= 100) {
        sw = (float)displayInfo.width;  sh = (float)displayInfo.height;
    } else if (displayInfo.height > displayInfo.width && displayInfo.height >= 100 && displayInfo.width >= 100) {
        sw = (float)displayInfo.height;  sh = (float)displayInfo.width;
    }
    return sw >= 100.f && sh >= 100.f;
}

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
    // «Только при стрельбе»: состояние посчитано в UpdateAim (оно общее для
    // всех трёх режимов); без выстрелов аим не ведёт.
    if (active && !s_fireGateAllow) active = false;
    if (!active) {
        // Причину пишем по первому же невыполненному условию: в логе 15:57
        // «объект=0» шёл в 219 кадрах из 319, и без расшифровки это выглядело
        // как «потерялся MouseLook», хотя проверка объекта даже не запускалась.
        if (why) {
            if (!g_state.aim_touch)                  *why = 1;
            else if (!g_esp_attached)                *why = 2;
            else if (g_calibMode != 0)               *why = 4;
            else if (g_state.freecam_on)             *why = 6;
            else if (!s_fireGateAllow)               *why = 9;
            else                                     *why = 7;
        }
        return false;
    }

    if (dt <= 0.f || !std::isfinite(dt)) dt = 1.f / 60.f;
    if (dt > 0.1f) dt = 0.1f;

    if (!AimScreenSize(sw, sh)) {
        if (why) *why = 8;
        return false;
    }
    return true;
}

// Select a target for touch, memory, and silent modes; memory additionally filters its angles.

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

// why — причина, по которой цель НЕ выбрана (0 когда цель есть). Коды:
//   1 боксов нет вовсе      2 все союзники      3 все мёртвые
//   4 нет скелета           5 кости не легли на экран
//   6 точка вне экрана      7 точка дальше круга обзора
// Без этого кода кадр «аим активен, прицел есть, записей ноль» нечем
// прочесть: в логе 17:06 таких было 32 из 92, и что именно не пустило
// цель — отфильтровали трупа, круг обзора или пустой список — не видно.
static bool AimSelectTarget(float sw, float sh, float dt, AimPick& pick, AimTarget& best,
                            int* why = nullptr) {
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
    // Read the current menu/config selection on every target-selection pass.
    // Slots are [0] head, [1] neck, and [2] chest.
    int wantBone = g_state.aim_bone;
    if (wantBone < 0 || wantBone > 2) wantBone = 1;

    // ---- choose target ----
    static std::vector<RankedAimTarget> candidates;
    candidates.clear();
    if (candidates.capacity() < boxes.size()) candidates.reserve(boxes.size());
    bool haveNearestRange = false;
    float nearestRange = 0.f;
    const bool cameraAim = g_state.aim_mode == AIM_MODE_MEMORY ||
                           g_state.aim_mode == AIM_MODE_TOUCH;
    const float fovRadiusDeg = manes_aim_source::fov_radius_degrees(cfg::aim::fov);
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
        const float angularDistance = cameraAim
            ? manes_aim_source::angular_distance_degrees(t.yaw, t.pitch)
            : 0.f;
        // Touch and memory use the same configured angular FOV; silent keeps
        // the projected screen-space circle used for shot-axis selection.
        const bool inFov = cameraAim
            ? manes_aim_source::inside_angular_fov(
                  t.yaw, t.pitch, cfg::aim::fov, sticky ? fovRadiusDeg : 0.f)
            : t.dist <= (sticky ? fovR * 2.f : fovR);
        if (!inFov) { ++s_out_of_fov; continue; }

        const float pixelTerm = t.dist / (fovR > 1.f ? fovR : 1.f);
        const float angleTerm = angularDistance /
            (fovRadiusDeg > 1.f ? fovRadiusDeg : 1.f);
        const float aimError = cameraAim ? angleTerm : pixelTerm;
        const bool haveRange = t.world_dist > 0.1f && std::isfinite(t.world_dist);
        candidates.push_back({t, aimError, haveRange, sticky});
        if (haveRange && (!haveNearestRange || t.world_dist < nearestRange)) {
            nearestRange = t.world_dist;
            haveNearestRange = true;
        }
    }

    bool haveBest = false;
    float bestScore = 1e18f;
    for (const RankedAimTarget& candidate : candidates) {
        // Unknown distances cannot outrank a known near target. The range
        // window also prevents a distant crosshair-perfect target from taking
        // priority over a much closer valid one.
        if (haveNearestRange && !candidate.range_valid) continue;
        if (haveNearestRange && !aim_control::target_within_range_window(
                candidate.target.world_dist, nearestRange)) continue;
        const float score = aim_control::target_priority_score(
            g_state.aim_priority, candidate.aim_error,
            candidate.target.world_dist, candidate.range_valid, candidate.sticky);
        if (!haveBest || score < bestScore) {
            bestScore = score;
            best = candidate.target;
            haveBest = true;
        }
    }

    if (!haveBest) {
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
    if (g_state.aim_mode == AIM_MODE_MEMORY) {
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
        float timeConstant = 0.050f;
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
    static float s_fx = 0.f, s_fy = 0.f;

    const bool menuOpen = g_sheet.visible || (g_pop.visible && !g_pop.closing);
    bool active = g_state.aim_touch && g_esp_attached && !menuOpen &&
                  g_calibMode == 0 && !g_state.freecam_on;
    if (active && g_state.aim_scope_only && !esp_local_player_is_aiming())
        active = false;
    if (active && !s_fireGateAllow)   // «Только при стрельбе» (см. AimFireGateUpdate)
        active = false;

    if (dt <= 0.f || !std::isfinite(dt)) dt = 1.f / 60.f;
    if (dt > 0.1f) dt = 0.1f;

    if (!active) {
        AimReleaseFinger(s_fingerDown);
        g_aimActive = false;
        s_touchPick.reset();
        return;
    }

    float sw = (float)native_window_screen_x;
    float sh = (float)native_window_screen_y;
    if (displayInfo.width > displayInfo.height && displayInfo.width >= 100 && displayInfo.height >= 100) {
        sw = (float)displayInfo.width;
        sh = (float)displayInfo.height;
    } else if (displayInfo.height > displayInfo.width && displayInfo.height >= 100 && displayInfo.width >= 100) {
        sw = (float)displayInfo.height;
        sh = (float)displayInfo.width;
    }
    if (sw < 100.f || sh < 100.f) {
        AimReleaseFinger(s_fingerDown);
        g_aimActive = false;
        s_touchPick.reset();
        return;
    }

    const float ptX = AimTouchFracX() * sw;
    const float ptY = AimTouchFracY() * sh;
    AimTarget best;
    if (!AimSelectTarget(sw, sh, dt, s_touchPick, best)) {
        g_aimActive = false;
        s_touchPick.reset();
        AimReleaseFinger(s_fingerDown);
        return;
    }

    const float crossX = sw * 0.5f, crossY = sh * 0.5f;
    const float ex = best.sx - crossX, ey = best.sy - crossY;
    if (fabsf(ex) < 2.5f && fabsf(ey) < 2.5f) return;

    if (!s_fingerDown) {
        s_fx = ptX;
        s_fy = ptY;
        Touch_Down(s_fx, s_fy);
        s_fingerDown = true;
    }

    float sm = g_state.gun_str;
    if (!(sm >= 1.f)) sm = 1.f;
    if (sm > 10.f) sm = 10.f;
    const float rate = 4.f + (sm - 1.f) * (24.f / 9.f);
    float k = 1.f - expf(-rate * dt);
    if (k > 0.85f) k = 0.85f;

    float maxStep = (2500.f + (sm - 1.f) * 500.f) * dt;
    if (maxStep > 150.f) maxStep = 150.f;
    float dx = ex * k, dy = ey * k;
    if (dx > maxStep) dx = maxStep;
    if (dx < -maxStep) dx = -maxStep;
    if (dy > maxStep) dy = maxStep;
    if (dy < -maxStep) dy = -maxStep;

    // Shift the archived touch zone with the configured base point.
    const float xOffset = ptX - 0.72f * sw;
    const float yOffset = ptY - 0.48f * sh;
    float xMin = 0.55f * sw + xOffset, xMax = 0.95f * sw + xOffset;
    float yMin = 0.20f * sh + yOffset, yMax = 0.80f * sh + yOffset;
    xMin = std::max(xMin, 0.02f * sw);
    xMax = std::min(xMax, 0.98f * sw);
    yMin = std::max(yMin, 0.02f * sh);
    yMax = std::min(yMax, 0.98f * sh);

    s_fx += dx;
    s_fy += dy;
    if (s_fx < xMin || s_fx > xMax || s_fy < yMin || s_fy > yMax) {
        AimReleaseFinger(s_fingerDown);
        return;
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
        s_memDiag.fire = s_fireState;
        pick.reset(); s_missFrames = 0; s_haveLast = false; s_haveLastWrite = false;
        return;
    }
    s_memDiag.inactive = 0;
    s_memDiag.ads = s_adsState; s_memDiag.ads_source = s_adsSource;
    s_memDiag.fire = s_fireState;

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
    int noTgtWhy = 0;
    if (!AimSelectTarget(sw, sh, dt, pick, best, &noTgtWhy)) {
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
    if (!AimSelectTarget(sw, sh, dt, pick, best) || !best.world_valid) {
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
    // Гейт «только при стрельбе» считается ДО диспетчера и один раз на кадр:
    // он общий для всех трёх режимов, а после смены режима меню могло успеть
    // снять тумблер — тогда в этом же кадре аим уже не ведёт.
    AimFireGateUpdate(MonoNow());
    static int previousMode = -1;
    if (mode != previousMode) {
        if (previousMode == AIM_MODE_TOUCH) {
            AimReleaseFinger(s_fingerDown);
            g_aimActive = false;
        }
        s_touchPick.reset();
        previousMode = mode;
    }
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