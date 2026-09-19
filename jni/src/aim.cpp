#include "aim.h"
#include "logfile.h"
#include "main.h"                // displayInfo, native_window_screen_*
#include "game.h"
#include "Android_touch/TouchHelperA.h"
#include "aim_learn.h"           // оценка град/px по реакции прицела
#include "app_state.h"
#include "cfg.h"
#include "menu.h"                // g_buildPrompt, g_calibMode
#include "process.h"             // g_esp_attached
#include "ui_util.h"
#include "str.h"               // XS(): строки интерфейса зашифрованы в бинарнике
#include "hud.h"               // ShowToast: мемори-режимы сообщают о поломке тостом
#include "esp_draw.h"            // FrameBoxes, AimFovRadiusPx
#include "widgets.h"             // g_sheet, g_pop
#include <cmath>
#include <cstdlib>
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
    int   bone = -1;                // слот точки прицела (0 голова, 1 шея, 2 грудь)
    // Мировая точка прицела (с упреждением) — для аима без MouseLook,
    // который вращает камеру прямо к мировой точке.
    float aim_world[3] = {0.f, 0.f, 0.f};
    float health = -1.f;            // здоровье цели (для лога и проверки смерти)
    bool  respawning = false;       // PlayerManager.respawning (для лога)
    // Поза выбранной цели — те же габариты скелета, по которым решается
    // «мёртв». В лог они идут вместе с самой целью: иначе, когда игрок
    // жалуется «аим ведёт по трупу», нечем проверить, лежал ли тот труп
    // или его кости ещё стояли.
    int   skel_bones = 0;
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
// Режимов три (переключатель во вкладке «Аим»): тач ведёт камеру синтетическим
// пальцем, «память» пишет ввод взгляда в память игры, «сайлент» пишет ось
// выстрела. Выбор цели и упреждение у них общие: чем бы прицел ни крутили,
// цель выбирается по одним правилам (круг FOV, приоритет, залипание на цели,
// упреждение по угловой скорости).

// Состояние выбора цели. Держится каждым режимом своим экземпляром: у сайлента
// и у памяти другая петля (нет пальца, который живёт между кадрами), и мешать
// им залипание на цель с тачем незачем.
struct AimPick {
    unsigned long long lastId = 0;      // прилипчивая цель
    int   lastBone    = -1;             // слот точки прицела прошлого такта
    float prevTgtYaw = 0.f, prevTgtPitch = 0.f;
    float prevCamYaw = 0.f, prevCamPitch = 0.f;
    // Скорость цели со сглаживанием — по ней считается упреждение (см. выбор
    // цели): дрожь кости каждый кадр меняет знак и в среднем даёт ноль, а
    // настоящее движение идёт в одну сторону и в среднем остаётся.
    float velYaw = 0.f, velPitch = 0.f;
    bool  havePrev   = false;
    // Цель в этом кадре сменилась. Режиму важно: обученный коэффициент и
    // упреждение по прошлой цели к новой не относятся (тач, например,
    // выбрасывает их при смене). Состояние цели при её пропаже НЕ сбрасывается
    // — решение «цель потеряна» принимает режим (палец ждёт шесть кадров,
    // память пишет перестаёт сразу).
    bool  switched   = false;

    void reset() { lastId = 0; lastBone = -1; havePrev = false; switched = false;
                   velYaw = 0.f; velPitch = 0.f; }
};

// Начало такта: общие проверки и размер экрана. false — в этом кадре аим
// работать не должен (меню, калибровка зон, нет привязки, «только в прицеле»
// без прицела). dt приводится к рабочему диапазону на месте.
// Коды причины «аим молчит» (AimMemDiag.inactive):
//   1 аим выключен          2 нет привязки к процессу   3 открыто меню
//   4 калибровка зон        5 окно сборки               6 включён фрикам
//   7 «только с прицелом», а прицела нет                8 не определён экран
// Признак «в прицеле» и цепочка, которой он прочитан. Запоминаются в AimBegin,
// потому что на пути «аим молчит» диагностика чистится целиком, и иначе в логе
// остался бы ноль — он читается как «цепочка не подтвердилась», хотя проверка
// могла ответить честно и просто «прицела нет».
static int  s_adsState  = -1;   // 1 в прицеле, 0 нет, -1 режим выключен
static int  s_adsSource = 0;    // 1 Aim, 2 isAiming, 3 aimFOV, 0 не подтвердилась

static bool AimBegin(float& dt, float& sw, float& sh, int* why = nullptr) {
    g_aimActive = false;
    const bool menuOpen = g_sheet.visible || (g_pop.visible && !g_pop.closing);
    // Во время калибровки зон (тапом по экрану) аим ничего не трогает: иначе
    // он водил бы камеру прямо под пальцем пользователя.
    // Во фрикаме аим молчит: камера улетела от тела, а точка прицела считается
    // от глаза персонажа — доворачивать камеру по ней было бы некуда.
    bool active = g_state.aim_touch && g_esp_attached && !menuOpen &&
                  g_calibMode == 0 && !g_buildPrompt && !g_state.freecam_on;

    // "Только с прицелом": only steer while the local player is ADS.
    if (g_state.aim_scope_only && g_esp_attached) {
        s_adsState = esp_local_player_is_aiming() ? 1 : 0;
        s_adsSource = esp_local_aim_source();
        if (active && !s_adsState) active = false;
    } else {
        s_adsState = -1; s_adsSource = 0;
    }
    if (!active) {
        // Причину пишем по первому же невыполненному условию: в логе 15:57
        // «объект=0» шёл в 219 кадрах из 319, и без расшифровки это выглядело
        // как «потерялся MouseLook», хотя проверка объекта даже не запускалась.
        if (why) {
            if (!g_state.aim_touch)                  *why = 1;
            else if (!g_esp_attached)                *why = 2;
            else if (menuOpen)                       *why = 3;
            else if (g_calibMode != 0)               *why = 4;
            else if (g_buildPrompt)                  *why = 5;
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

// Выбор цели и упреждение. false — цели нет; сбрасывать ли из-за этого
// прилипание, решает режим (палец ждёт шесть кадров, запись в память
// прекращается сразу). degPerPx наружу — мёртвая зона считается в пикселях.
// Кость, в которую аим ведёт огонь: 0 голова, 1 шея, 2 грудь.
// Шея — просьба 19.09: попадание по кости головы уходит в нижнюю часть
// черепа, а шея даёт крупную и устойчивую зону. Переключателя в меню нет,
// поэтому цель задана константой.
static constexpr int   kAimBoneSlot = 1;

// Множитель упреждения по скорости цели (кадров вперёд). 0 — выключено.
static constexpr float kAimVelocityLead = 0.0f;
static unsigned long s_dead_skipped = 0;   // мёртвых не взяли в цель (для лога)
static unsigned long s_no_skeleton = 0;         // у цели нет скелета вовсе
static unsigned long s_bones_not_projected = 0; // скелет есть, кости не легли на экран
static unsigned long s_ally_skipped = 0;        // союзников не берём никогда
static unsigned long s_offscreen_skipped = 0;   // есть кости, но точка вне экрана
static unsigned long s_out_of_fov = 0;          // точка дальше круга обзора
// Сколько всего игроков приходило на выбор цели за период (для лога): без
// этой цифры «пропущено N» невозможно прочитать — непонятно, много это или
// все, кого видно.
static unsigned long s_seen_boxes = 0;

// why — причина, по которой цель НЕ выбрана (0 когда цель есть). Коды:
//   1 боксов нет вовсе      2 все союзники      3 все мёртвые
//   4 нет скелета           5 кости не легли на экран
//   6 точка вне экрана      7 точка дальше круга обзора
// Без этого кода кадр «аим активен, прицел есть, записей ноль» нечем
// прочесть: в логе 17:06 таких было 32 из 92, и что именно не пустило
// цель — отфильтровали трупа, круг обзора или пустой список — не видно.
static bool AimSelectTarget(float sw, float sh, AimPick& pick, AimTarget& best, float& degPerPx, int* why = nullptr) {
    LogStage(kStageAimSelect);
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
                                    s_ally_skipped + s_offscreen_skipped + s_out_of_fov;
        if (now - s_at >= 5.0 && (total || s_seen_boxes)) {
            s_at = now;
            LogLine("аим: выбор — игроков %lu, пропущено: союзники %lu, мёртвые %lu, без костей %lu (нет скелета %lu, не спроецировались %lu), вне экрана %lu, вне круга %lu",
                    s_seen_boxes, s_ally_skipped, s_dead_skipped,
                    s_no_skeleton + s_bones_not_projected, s_no_skeleton, s_bones_not_projected,
                    s_offscreen_skipped, s_out_of_fov);
            s_seen_boxes = 0; s_ally_skipped = 0; s_dead_skipped = 0;
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

    // Цель — ШЕЯ (просьба 19.09: «пусть аим стреляет в шею, а не в голову»).
    // Слоты: 0 голова, 1 шея, 2 грудь. Голова — это сустав у основания черепа,
    // и попадание в неё «по кости» уходит в нижнюю часть головы; шея даёт
    // более крупную и устойчивую зону. Переключателя в меню нет, поэтому
    // значение задано константой: поменять её — поменяется цель аима.
    const int wantBone = kAimBoneSlot;
    (void)g_state.aim_bone;

    // ---- choose target ----
    float bestScore = 1e18f;
    for (const EspBox& b : boxes) {
        // На союзников аим не работает НИКОГДА, а не только пока в визуалах
        // включён их показ: проверка «g_state.esp_team && b.ally» означала,
        // что со скрытыми союзниками (показ выключен) аим работал по ним
        // как по врагам (вопрос 19.09). Признак своего — из групп/кланов
        // (game.cpp, groups_are_allied), он считается независимо от визуалов.
        ++s_seen_boxes;
        if (b.ally) { ++s_ally_skipped; continue; }
        // Мёртвых не берём: труп ещё несколько секунд остаётся в списке
        // игроков с корректным скелетом, и прицел уезжал на него (19.09).
        if (b.dead) { ++s_dead_skipped; continue; }
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
        if (b.id != 0 && b.id == pick.lastId && pick.lastBone >= 0 && pick.lastBone < 3 &&
            b.aim_valid[pick.lastBone]) {
            usedBone = pick.lastBone;
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
            t.aim_world[0] = b.aim_world[usedBone][0];
            t.aim_world[1] = b.aim_world[usedBone][1];
            t.aim_world[2] = b.aim_world[usedBone][2];
            t.valid = std::isfinite(t.yaw) && std::isfinite(t.pitch) &&
                      std::isfinite(t.sx) && std::isfinite(t.sy);
        } else {
            // По костям цель не нашлась — по просьбе 19.09 («аим полностью по
            // скелету») в цель такие игроки не идут вовсе: запасная точка от
            // габаритов бокса давала прицел выше/ниже настоящей головы.
            // Считаем и РАЗДЕЛЯЕМ причины: «скелета нет вообще» и «скелет есть,
            // но кости не спроецировались» — лечится это по-разному.
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
        const bool sticky = (pick.lastId != 0 && b.id == pick.lastId);
        if (t.sx < -sw || t.sx > sw * 2.f || t.sy < -sh || t.sy > sh * 2.f) { ++s_offscreen_skipped; continue; }
        if (g_state.aim_pos && !sticky && (t.sx < 0.f || t.sy < 0.f || t.sx > sw || t.sy > sh)) { ++s_offscreen_skipped; continue; }
        float dx = t.sx - crossX, dy = t.sy - crossY;
        t.dist = sqrtf(dx * dx + dy * dy);
        t.world_dist = b.distance;
        // The target we are already pulling to may briefly leave the FOV
        // circle (overshoot while the gain is still being learned) — keep it.
        if (t.dist > (sticky ? fovR * 2.f : fovR)) { ++s_out_of_fov; continue; }

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

    // Lead a moving target: the game applies our step next frame, by
    // which time the target has moved on. Use the target's angular velocity
    // relative to the camera (with the camera's own rotation removed) and
    // aim one frame ahead. Reset on target switch.
    //
    // Камера здесь та же, что и в обучении коэффициента (esp_aim_camera_angles):
    // вычитать отстающий базис из матрицы вида из угловой скорости цели нельзя —
    // в поправку попадал бы его запаздывающий поворот, и прицел уезжал бы в
    // сторону. Нет настоящей оси — нет и упреждения: так вёл цель эталонный аим.
    {
        float cy = 0.f, cp = 0.f;
        // Мусорный угол камеры (чтение сорвалось) не годится даже в
        // нормализацию: упреждение по нему уводит прицел, а раньше из-за него
        // ещё и зависал цикл приведения угла.
        bool haveC = esp_aim_camera_angles(cy, cp) && std::isfinite(cy) && std::isfinite(cp);
        if (best.id == pick.lastId && pick.havePrev && haveC) {
            const float dCamYaw = WrapDeg180(cy - pick.prevCamYaw);
            float dCamPitch = cp - pick.prevCamPitch;
            // world-space angular motion of the target = change in offset + camera rotation
            float vYaw = (best.yaw - pick.prevTgtYaw) + dCamYaw;
            float vPitch = (best.pitch - pick.prevTgtPitch) + dCamPitch;
            if (std::isfinite(vYaw) && std::isfinite(vPitch) && fabsf(vYaw) < 10.f && fabsf(vPitch) < 10.f) {
                pick.prevTgtYaw = best.yaw; pick.prevTgtPitch = best.pitch;
                // Скорость сглаживаем, прежде чем упреждать по ней. Раньше
                // упреждение брало сырой прирост за кадр, а он у дрожащей кости
                // каждый кадр другого знака: прицел получал рывок в полный
                // размах дрожи и ещё столько же вперёд — то есть дрожь
                // удваивалась. Это и была жалоба «мемори трясётся»: магнит
                // держал цель, но вокруг неё камера ходила.
                const float kv = 0.35f;
                pick.velYaw   += (vYaw   - pick.velYaw)   * kv;
                pick.velPitch += (vPitch - pick.velPitch) * kv;
                // Below this the "motion" is bone animation jitter (breathing,
                // sway), which at long range is larger than the head itself.
                // Extrapolating it would double the error, so only lead real
                // movement. Aim more than a frame ahead for fast movers so the
                // crosshair stays on a laterally running target (the controller
                // smoothing otherwise makes it trail behind).
                const float leadMin = degPerPx * 2.f;
                float vMag = sqrtf(pick.velYaw * pick.velYaw + pick.velPitch * pick.velPitch);
                if (vMag > leadMin) {
                    // Забегаем чуть больше чем на кадр вперёд (игра применит
                    // шаг в своём следующем кадре). По сглаженной скорости это
                    // безопасно: дрожь в неё не проходит, а постоянное
                    // движение проходит целиком. Множитель больше 1.1 только
                    // мерил: при 2.0 цель идёт с остатком 0.98° против 0.70°,
                    // то есть прицел уезжает вперёд, а не отстаёт.
                    // Упреждение по скорости ВЫКЛЮЧЕНО (было до 1.5 кадра
                    // вперёд). Именно оно и выглядело как «аим пытается
                    // запредиктить движение игрока» (жалоба 19.09): точка
                    // уезжала вперёд по ходу цели, а когда цель меняла
                    // направление, прицел оказывался не там. Задержку петли
                    // теперь гасит окно «в полёте» в контроллере — второе
                    // упреждение только раскачивает. Константа оставлена:
                    // вернуть — одна правка числа (было 1.1).
                    float k = kAimVelocityLead * (1.f - leadMin / vMag);
                    if (k > 1.5f) k = 1.5f;
                    best.yaw   += pick.velYaw   * k;
                    best.pitch += pick.velPitch * k;
                }
            } else {
                // Скорость сорвалась (мусор) — и сглаженную обнуляем, чтобы
                // одиночный выброс не тянул прицел дальше.
                pick.velYaw = pick.velPitch = 0.f;
                pick.prevTgtYaw = best.yaw; pick.prevTgtPitch = best.pitch;
            }
        } else {
            pick.prevTgtYaw = best.yaw; pick.prevTgtPitch = best.pitch;
        }
        if (haveC) { pick.prevCamYaw = cy; pick.prevCamPitch = cp; pick.havePrev = true; }
        else pick.havePrev = false;
    }
    // Цель взята — с этого такта камера (или ось выстрела) наша: автофарм ей
    // больше не командует.
    g_aimActive = true;
    // Состояние выбранной цели — чтобы по логу проверить признак смерти:
    // игрок жаловался, что аим ведёт по мёртвым, а по respawning (0x208) в
    // логе 14:55 не сработало ни разу. Печатаем здоровье и оба флага.
    if (best.id != pick.lastId)
        LogLine("аим: цель 0x%llx здоровье=%.1f respawning=%d дистанция=%.1f м костей=%d высота=%.2f м ширина=%.2f м",
                (unsigned long long)best.id, (double)best.health,
                (int)best.respawning, (double)best.world_dist,
                (int)best.skel_bones, (double)best.skel_up, (double)best.skel_flat);
    pick.switched = (best.id != pick.lastId);
    pick.lastId = best.id; pick.lastBone = best.bone;
    return true;
}

// ============================ Тач-аим ======================================
static void UpdateAimTouch(float dt) {
    LogStage(kStageAimSelect);
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
    // Доля шага при захвате цели: 0.3 сразу после захвата, 1.0 через 0.35 с
    // (см. «мягкий старт» ниже — им гасится маятник первых двух секунд).
    static float s_acquire = 1.f;
    static int   s_holdFrames = 0;
    static AimPick pick;

    float sw = 0.f, sh = 0.f;
    if (!AimBegin(dt, sw, sh)) {
        AimReleaseFinger(s_fingerDown);
        s_haveLast = false; pick.reset();
        s_lostFrames = 0; s_holdFrames = 0;
        s_trackYaw.reset(); s_trackPitch.reset();
        return;
    }

    // Точка, из которой аим водит палец: выбирается в меню тапом по экрану
    // (вкладка «Аим», см. g_calibMode == 3). Пока не задана — прежняя позиция
    // справа по центру, поэтому поведение по умолчанию не меняется.
    const float ptX = AimTouchFracX() * sw, ptY = AimTouchFracY() * sh;

    AimTarget best;
    float degPerPx = 0.f;
    if (!AimSelectTarget(sw, sh, pick, best, degPerPx)) {
        // Keep the finger down briefly so a momentary read failure does not
        // register as a tap (tap-to-shoot in some layouts) or reset momentum.
        if (++s_lostFrames > 6) {
            AimReleaseFinger(s_fingerDown);
            s_haveLast = false; pick.reset();
            s_trackYaw.reset(); s_trackPitch.reset();
        }
        return;
    }
    s_lostFrames = 0;
    if (pick.switched) {
        s_haveLast = false;                      // do not learn gain across a target switch
        s_trackYaw.reset(); s_trackPitch.reset();  // и оценку коэффициента тоже
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
    // Окно в восемь тактов. Касание идёт через систему ввода Android: от
    // нашего Touch_Move до поворота камеры проходит до 100 мс, то есть шесть-
    // восемь кадров. Учитывали четыре — остаток недоучитывался, и первые
    // секунды после захвата цели аим раскачивался (жалоба 19.09): крупные шаги
    // на подходе уходят «в полёт», камера добирает их все разом и проскакивает.
    static float s_flightYaw[8] = {0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f};
    static float s_flightPitch[8] = {0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f};
    const bool  useFlight = !haveCam;
    float flightYaw = 0.f, flightPitch = 0.f;
    for (int i = 0; i < 8; ++i) { flightYaw += s_flightYaw[i]; flightPitch += s_flightPitch[i]; }
    auto shiftFlight = [&]() {
        for (int i = 0; i < 7; ++i) { s_flightYaw[i] = s_flightYaw[i + 1]; s_flightPitch[i] = s_flightPitch[i + 1]; }
        s_flightYaw[7] = 0.f; s_flightPitch[7] = 0.f;
    };
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
        shiftFlight();
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
        s_acquire = 0.f;
        // Палец опустился заново: окно замера начинается с нуля, иначе первая
        // же пара «сдвиг пальца -> отклик» посчитает бросок пальца к точке.
        s_trackYaw.reset(); s_trackPitch.reset();
        for (int i = 0; i < 8; ++i) { s_flightYaw[i] = 0.f; s_flightPitch[i] = 0.f; }
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
    // 0.5 даёт |z| = 0.71 — качели гаснут за 2-3 такта. Снизил до 0.45: на
    // устройстве задержка отработки касания плавает (2-4 кадра), и запас по
    // устойчивости здесь не лишний — «почти пропал, но не до конца» (19.09).
    // На скорость захвата это почти не влияет: при большой ошибке шаг всё
    // равно упирается в потолок maxStep.
    const float kStable = 0.45f;
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

    // Мягкий старт при захвате.
    //
    // Жалоба 19.09: «тач-аим ходит маятником первые две секунды». Механика
    // такая: в момент захвата ошибка велика, и контроллер сразу выдаёт шаг под
    // потолок (0.15*sh пикселей — 162 px на этом экране). Касание идёт до
    // камеры через систему ввода Android 2—8 кадров, и за это время мы
    // успеваем заказать ещё столько же. Камера приходит сразу на несколько
    // шагов и перелетает; ошибка меняет знак; дальше петля ходит, пока окно
    // «в полёте» не выровняется — эти две секунды и есть.
    //
    // Поэтому первые 0.35 с после захвата (и после нового касания) шаг режем
    // до трети и плавно отдаём полный. Перелёта тогда нет вовсе, а наведение
    // всё равно занимает считанные кадры: при большой ошибке шаг и в треть
    // размера упирается в потолок.
    if (pick.switched) s_acquire = 0.f;
    s_acquire += dt / 0.35f;
    if (s_acquire > 1.f) s_acquire = 1.f;
    k *= 0.3f + 0.7f * s_acquire;

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
        shiftFlight();
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
        for (int i = 0; i < 8; ++i) { s_flightYaw[i] = 0.f; s_flightPitch[i] = 0.f; }
        s_haveLast = false;
        // Перенос пальца — это не шаг прицела: замер по нему покажет мусор.
        s_trackYaw.reset(); s_trackPitch.reset();
        return;
    }
    s_fx = nx; s_fy = ny;
    s_pendDx += dx; s_pendDy += dy;   // ждёт отработки камерой (ack-такт)
    shiftFlight(); s_flightYaw[7] = dx;
    s_flightPitch[7] = -dy;
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
        LogLine("аим: %s кадр=%lu объект=%d отклик=%d записей=%d отказов=%d расхождение=%.1f молчит=%d прицел=%d источник=%d нетцели=%d нацели=%d",
                aim_mode_name(), s_frames, (int)d.params, (int)d.responded,
                d.writes, d.fails, (double)d.mismatch, d.inactive, d.ads, d.ads_source,
                d.no_target, d.on_target);
}

// ====================== Мемори-аим: «память» ===============================
//
// Камера ведётся записью ЛОКАЛЬНОГО ПОВОРОТА её собственного Transform —
// MouseLook не участвует (переделка 19.09: объект MouseLook терялся — адрес
// локального игрока ездил в кадре, объект умирал на респавне, выученный класс
// оказывался чужим, — и каждый такой сбой молчал аимом «теряет MouseLook и
// не нацеливается»). Математика и адрес — в game.cpp, esp_mem_aim_point_at.
//
// Почему эта схема убирает целый класс «аим качает / потерял объект»:
//   * объекта терять нечего: запись идёт в камеру, а камера есть, пока идёт
//     игра; слот решается с нуля каждый такт и подтверждается матрицей вида;
//   * ошибка всегда меряется от ТЕКУЩЕЙ позы камеры: запись, которую игра
//     перебила своим вводом, компенсируется следующим же тактом;
//   * нет второго места, где хранится угол, — «углы разошлись с экраном»
//     исчезает в принципе.
//
// Контроллер: остаток — угол между осью камеры и мировой точкой цели
// (дрожь кости и упреждение уже в самой точке). Шаг = min(остаток, потолок),
// потолок — из настройки «сила». Мёртвая зона (0.15°) — в функции записи.

// Потолок поворота за такт, градусы на единицу силы. Сила 1..10 -> 8..80:
// единица — плавное ведение, за бегущей целью всё равно успевает (8° за
// такт при 60 fps = 480°/s), десятка — почти снап: цель захватывается за
// один-два такта.
static constexpr float kMemStepPerStr = 8.f;

static AimMemDiag s_memDiag;

const AimMemDiag& AimMemoryDiag() { return s_memDiag; }

static void UpdateAimMemory(float dt) {
    static AimPick pick;
    static float  s_noCamTime = 0.f;   // сколько не было подтверждённого слота
    static float  s_lastErr = -1.f;    // ошибка на прошлом такте (град)
    static bool   s_toasted = false;

    float sw = 0.f, sh = 0.f;
    int why = 0;
    if (!AimBegin(dt, sw, sh, &why)) {
        s_memDiag = AimMemDiag{};
        s_memDiag.inactive = why;   // печатается в той же строке «объект=…»
        s_memDiag.ads = s_adsState; s_memDiag.ads_source = s_adsSource;
        pick.reset();
        s_noCamTime = 0.f; s_toasted = false; s_lastErr = -1.f;
        return;
    }
    s_memDiag.inactive = 0;
    s_memDiag.ads = s_adsState; s_memDiag.ads_source = s_adsSource;

    AimTarget best;
    float degPerPx = 0.f;
    int noTgtWhy = 0;
    if (!AimSelectTarget(sw, sh, pick, best, degPerPx, &noTgtWhy)) {
        s_memDiag.no_target = noTgtWhy;
        pick.reset();
        // Цели нет: камеру никуда не ведём (иначе ушла бы к последней
        // точке). Отклик/ошибку сбрасываем, чтобы «ведёт» не светил.
        s_memDiag.params = false;
        s_memDiag.responded = false;
        s_noCamTime = 0.f;
        s_lastErr = -1.f;
        return;
    }
    s_memDiag.no_target = 0;

    // Сила (1..10) — потолок шага за такт (см. kMemStepPerStr).
    float sm = g_state.gun_str;
    if (!(sm >= 1.f)) sm = 1.f;
    if (sm > 10.f) sm = 10.f;
    const float maxStep = kMemStepPerStr * sm;

    // Камера -> мировая точка цели. Слот, родитель, мёртвая зона и запись —
    // в game.cpp; сюда приходят только результат и текущая ошибка.
    LogStage(kStageAimWrite);
    float err = -1.f;
    float applied = 0.f;
    const bool ok = esp_mem_aim_point_at(best.aim_world[0], best.aim_world[1],
                                         best.aim_world[2], maxStep, err, applied);
    if (!ok && err < 0.f) {
        // Подтверждённого слота камеры нет (сбой чтения, пересборка мира,
        // ещё нет камеры). Аим молчит, но причина в логе и тосте — не
        // «потерял объект» навсегда, а «в этом такте не подтвердился».
        s_memDiag.params = false;
        s_memDiag.responded = false;
        s_noCamTime += dt;
        if (s_noCamTime > 1.5f && !s_toasted) {
            s_toasted = true;
            ShowToast(XS("Мемори-аим: не найдена камера"));
        }
        s_lastErr = -1.f;
        return;
    }
    s_memDiag.params = true;
    s_noCamTime = 0.f;
    s_toasted = false;
    s_memDiag.mismatch = err;   // текущий остаток — печатается в строке лога
    if (applied > 0.f) {
        ++s_memDiag.writes;
        // «Камера ответила»: после нашей записи ошибка не выросла (запись
        // дошла, цель не убежала дальше шага).
        if (s_lastErr < 0.f || err <= s_lastErr + 0.05f) s_memDiag.responded = true;
    } else if (err >= 0.f && err < 0.5f) {
        s_memDiag.responded = true;   // уже в прицеле — считать ответом
    }
    if (err >= 0.f && err < 1.0f) ++s_memDiag.on_target;
    if (err >= 0.f) s_lastErr = err;
}

// Диспетчер: какой режим ведёт камеру в этом кадре.
void UpdateAim(float dt) {
    ++s_frames;
    const int mode = g_state.aim_mode;
    AimLogTick(dt);
    if (mode == AIM_MODE_MEMORY) UpdateAimMemory(dt);
    else                         UpdateAimTouch(dt);
}

// ============================ Автофарм =============================
//
// Полностью на синтетических тачах: три пальца (0 — джойстик движения,
// 1 — камера, 2 — удар). Слой памяти (game.cpp) отдаёт точку прицела в
// градусах от оси выстрела и точку подхода в градусах от оси камеры; здесь —
// только автоматы. Камерный палец пользуется тем же коэффициентом, который
// выучил аимбот, а если своего нет — аккуратно probe'ит фиксированным.
//