#include "aim_touch_geometry.h"
#include "aim.h"
#include "silent_aim.h"
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
    float world[3] = {};
    bool world_valid = false;
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
// Действует только в ТАЧ-режиме: там камера догоняет кость через трубу
// касания, и цель за это время уходит. Мемори-аиму упреждение не нужно и
// выключено: лок каждый такт пишет в память абсолютный угол самой кости —
// упреждать нечего, а сдвиг точки вперёд по ходу цели выглядел бы как
// «прицел убегает от головы» (жалоба 19.09). 0.055 — просьба 24.09; 0.070 — просьба 26.09; 0.100 — просьба 26.09.
static constexpr float kAimVelocityLead = 0.100f;
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
        if (b.id == 0 || b.aim_only) continue;
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

// Поток этого такта (в градусах). Ложь — оценить не вышло: меньше трёх боксов
// совпало по id (дуэль 1 на 1, первая пара кадров, все боксы мусорные).
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
                                    s_ally_skipped + s_offscreen_skipped + s_out_of_fov +
                                    s_bot_noesp_skipped;
        if (now - s_at >= 5.0 && (total || s_seen_boxes)) {
            s_at = now;
            LogLine("аим: выбор — игроков %lu (ботов %lu, из них без есп %lu), пропущено: союзники %lu, мёртвые %lu, без костей %lu (нет скелета %lu, не спроецировались %lu), вне экрана %lu, вне круга %lu",
                    s_seen_boxes, s_seen_bots, s_bot_noesp_skipped, s_ally_skipped, s_dead_skipped,
                    s_no_skeleton + s_bones_not_projected, s_no_skeleton, s_bones_not_projected,
                    s_offscreen_skipped, s_out_of_fov);
            // Which source can currently prove a player is an ally: local
            // PlayerManager selection, group strings, live TeamData identity,
            // and the exact roster-read stage.
            int dteam = 0, dclan = 0, dtag = 0, local_ok = 0, data_ok = 0;
            int rval = 0, rcnt = 0, rstage = ESP_ALLY_ROSTER_NOT_CHECKED;
            int states_valid = 0, states_ids = 0, states_entries = -1;
            int states_array = -1, states_parsed = 0;
            unsigned long long states_uid_checks = 0, states_uid_matches = 0;
            char rteam[32] = {};
            esp_get_ally_diag(&dteam, &dclan, &dtag, &local_ok, &data_ok,
                              &rval, &rcnt, &rstage,
                              &states_valid, &states_ids, &states_entries,
                              &states_array, &states_parsed,
                              &states_uid_checks, &states_uid_matches,
                              rteam, sizeof(rteam));
            const char* stage_name = "неизвестно";
            switch (rstage) {
                case ESP_ALLY_ROSTER_NOT_CHECKED: stage_name = "не проверялся"; break;
                case ESP_ALLY_ROSTER_NO_LOCAL_PLAYER: stage_name = "нет локального игрока"; break;
                case ESP_ALLY_ROSTER_NO_TEAM_COMPONENT: stage_name = "нет team-компонента"; break;
                case ESP_ALLY_ROSTER_NO_TEAM_DATA: stage_name = "нет team-data"; break;
                case ESP_ALLY_ROSTER_NO_MEMBER_LIST: stage_name = "нет member-list"; break;
                case ESP_ALLY_ROSTER_COLLECTION_UNREADABLE: stage_name = "collection не читается"; break;
                case ESP_ALLY_ROSTER_EMPTY: stage_name = "пустой список"; break;
                case ESP_ALLY_ROSTER_MEMBERS: stage_name = "участники прочитаны"; break;
                case ESP_ALLY_ROSTER_MEMBER_ENTRIES_UNREADABLE: stage_name = "поля участников не читаются"; break;
            }
            LogLine("аим: союзники — локальный игрок %s; группа: team=%d clan=%d tag=%d симв.; team-data %s; TeamData roster %s (%s, %d), команда «%s»; TeammateStates %s (IDs=%d, entries=%d, array=%d, parse=%s, UID checks=%llu matches=%llu)",
                    local_ok ? "подтверждён" : "не подтверждён", dteam, dclan, dtag,
                    data_ok ? "доступен" : "нет", rval ? "прочитан" : "нет",
                    stage_name, rcnt, rteam,
                    states_valid ? "доступен" : "нет", states_ids,
                    states_entries, states_array, states_parsed ? "ok" : "ошибка/не проверен",
                    states_uid_checks, states_uid_matches);
            char ally_details[1536] = {};
            esp_get_ally_diag_details(ally_details, sizeof(ally_details));
            if (ally_details[0])
                LogLine("аим: союзники — поля и указатели: %s", ally_details);
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

    // Цель — ШЕЯ (просьба 19.09: «пусть аим стреляет в шею, а не в голову»).
    // Слоты: 0 голова, 1 шея, 2 грудь. Голова — это сустав у основания черепа,
    // и попадание в неё «по кости» уходит в нижнюю часть головы; шея даёт
    // более крупную и устойчивую зону. Переключателя в меню нет, поэтому
    // значение задано константой: поменять её — поменяется цель аима.
    int wantBone = g_state.aim_bone;
    if (wantBone < 0 || wantBone > 2) wantBone = 1;

    // ---- choose target ----
    float bestScore = 1e18f;
    for (const EspBox& b : boxes) {
        // На союзников аим не работает НИКОГДА, а не только пока в визуалах
        // включён их показ: проверка «g_state.esp_team && b.ally» означала,
        // что со скрытыми союзниками (показ выключен) аим работал по ним
        // как по врагам (вопрос 19.09). Признак своего — из групп/кланов
        // (game.cpp, team_membership::groups_are_allied), он считается независимо от визуалов.
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
        // Exact bone, with graceful fallback to the next-best bone.
        // Slots: 0 head, 1 neck, 2 chest. Never fall back below the chest.
        static const int order[3][3] = {{0, 1, 2}, {1, 0, 2}, {2, 1, 0}};
        int usedBone = -1;
        const bool sticky = (pick.lastId != 0 && b.id == pick.lastId);
        // At point-blank range a projected head/neck can be far from the screen
        // centre even though the player is directly in front of the camera.
        // Keep the normal user FOV at range, but give targets within 2.5 m the
        // same bounded capture cone as near-plane-only boxes.
        const bool closeRange = b.aim_only ||
            (std::isfinite(b.distance) && b.distance >= 0.f && b.distance < 2.5f);
        const float localFov = closeRange ? std::max(fovR, sh * 0.35f) : fovR;
        auto boneInCone = [&](int bi) {
            if (!b.aim_valid[bi]) return false;
            const float sx = b.aim_pts[bi][0], sy = b.aim_pts[bi][1];
            if (!std::isfinite(sx) || !std::isfinite(sy)) return false;
            if (b.aim_only && (sx < 0.f || sx > sw || sy < 0.f || sy > sh)) return false;
            if (g_state.aim_pos && !sticky && (sx < 0.f || sx > sw || sy < 0.f || sy > sh)) return false;
            const float dx = sx - crossX, dy = sy - crossY;
            return dx * dx + dy * dy <= localFov * localFov * (sticky ? 4.f : 1.f);
        };
        // Прилипчивость точки прицела. Слоты головы, шеи и груди разнесены на
        // 0.2-0.3 м (это до полуградуса на дистанции), а читаются они не каждый
        // кадр: раньше слот перескакивал между ними из кадра в кадр, и прицел
        // мелко трясло в стороны. Пока цель та же и слот читается — держим его;
        // цепочка запасных слотов работает заново только когда он пропал.
        if (b.id != 0 && b.id == pick.lastId && pick.lastBone >= 0 && pick.lastBone < 3 &&
            boneInCone(pick.lastBone)) {
            usedBone = pick.lastBone;
        } else {
            // Near the camera the neck can be off-screen even when the chest
            // is in the reticle. Do not discard the whole player based on the
            // preferred bone: pick the first real bone within the FOV.
            for (int k = 0; k < 3; ++k) {
                const int bi = order[wantBone][k];
                if (boneInCone(bi)) { usedBone = bi; break; }
            }
            if (usedBone < 0 && !b.aim_only) {
                for (int k = 0; k < 3; ++k) {
                    const int bi = order[wantBone][k];
                    if (b.aim_valid[bi]) { usedBone = bi; break; }
                }
            }
        }
        if (usedBone >= 0) {
            t.bone = usedBone;
            t.world_valid = b.aim_world_valid[usedBone];
            for (int i = 0; i < 3; ++i) t.world[i] = b.aim_world[usedBone][i];
            t.yaw = b.aim_yaw[usedBone];  t.pitch = b.aim_pitch[usedBone];
            t.sx  = b.aim_pts[usedBone][0]; t.sy = b.aim_pts[usedBone][1];
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
        if (!t.valid || (g_state.aim_mode == AIM_MODE_SILENT && !t.world_valid)) continue;
        t.id = b.id;
        t.health = b.health; t.respawning = b.respawning;
        t.skel_bones = b.skel_bones; t.skel_up = b.skel_up; t.skel_flat = b.skel_flat;
        t.death_flag = b.death_flag;
        if (t.sx < -sw || t.sx > sw * 2.f || t.sy < -sh || t.sy > sh * 2.f) { ++s_offscreen_skipped; continue; }
        if (g_state.aim_pos && !sticky && (t.sx < 0.f || t.sy < 0.f || t.sx > sw || t.sy > sh)) { ++s_offscreen_skipped; continue; }
        float dx = t.sx - crossX, dy = t.sy - crossY;
        t.dist = sqrtf(dx * dx + dy * dy);
        t.world_dist = b.distance;
        // The target we are already pulling to may briefly leave the FOV
        // circle (overshoot while the gain is still being learned) — keep it.
        if (t.dist > (sticky ? localFov * 2.f : localFov)) { ++s_out_of_fov; continue; }

        // Target priority (see "Приоритет цели"):
        //   0 balanced  — crosshair distance and range, both normalised
        //                 (FOV radius, 120 m) and summed;
        //   1 crosshair — purely the pixel distance from the crosshair;
        //   2 range     — purely the world distance, crosshair only breaks ties.
        // Whatever the mode, the current target gets a strong preference so the
        // aim does not flip between two players standing next to each other.
        const float pixelTerm = t.dist / (localFov > 1.f ? localFov : 1.f);
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
    //
    // Упреждение действует только в ТАЧ-режиме: камера догоняет кость через
    // трубу касания, и цель за это время уходит. Мемори-лок пишет абсолютный
    // угол самой кости каждый такт — ему упреждать нечего (просьба 24.09:
    // «есть ли упреждение в мемори аиме, оно там не нужно»).
    if (g_state.aim_mode == AIM_MODE_TOUCH) {
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
                    // История: было до 1.5 кадра вперёд — выглядело как «аим
                    // пытается запредиктить движение игрока» (жалоба 19.09):
                    // точка уезжала вперёд по ходу цели, а когда цель меняла
                    // направление, прицел оказывался не там. Поэтому упреждение
                    // долго стояло на нуле. Просьба 24.09 — вернуть упреждение
                    // в тач и поднять его до 0.055 кадра (26.09: 0.070): оно компенсирует трубу
                    // касания и уход цели за время догонки. Кэп 1.5 кадра
                    // оставлен, чтобы не уезжать далеко вперёд по ходу цели.
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
        LogLine("аим: цель 0x%llx здоровье=%.1f respawning=%d смерть=%d дистанция=%.1f м костей=%d высота=%.2f м ширина=%.2f м",
                (unsigned long long)best.id, (double)best.health,
                (int)best.respawning, (int)best.death_flag, (double)best.world_dist,
                (int)best.skel_bones, (double)best.skel_up, (double)best.skel_flat);
    pick.switched = (best.id != pick.lastId);
    pick.lastId = best.id; pick.lastBone = best.bone;
    return true;
}

// ---- диагностика тач-аима -----------------------------------------------
// Без цифр спорить о «маятнике» бессмысленно: в логе должно быть видно, сколько
// тактов реально дали шаг, сколько реверсов знака ошибки, сколько тактов
// пропущено из-за того, что игра не нарисовала новый кадр, и какой сейчас
// коэффициент град/px (главный источник перелётов, если он завышен).
struct TouchDiag {
    int ticks = 0, steps = 0, flips = 0, skipNoFrame = 0, skipLongFrame = 0;
    float errSum = 0.f, errMax = 0.f, stepSum = 0.f;
    float errAvg = 0.f, stepAvg = 0.f;
    float gainYaw = 0.f, gainPitch = 0.f, trim = 1.f;
    bool  learned = false;
};
static TouchDiag s_tdiag;
// Амплитуда, на которой контроллер разворачивается (предельный цикл): по ней
// расширяется мёртвая зона, чтобы аим не «охотился» внутри собственной тряски.
static float s_flipMag = 0.f;
float AimTouchFlipMag() { return s_flipMag; }
// Наружу отдаём простыми полями: срез этого файла собирается ещё и стендом
// tools/aim, и тип туда не попадает.
void AimTouchDiagFetch(int* ticks, int* steps, int* flips, int* skipNoFrame, int* skipLong,
                       float* errAvg, float* errMax, float* stepAvg,
                       float* gy, float* gp, float* trim, int* learned) {
    if (ticks) *ticks = s_tdiag.ticks;
    if (steps) *steps = s_tdiag.steps;
    if (flips) *flips = s_tdiag.flips;
    if (skipNoFrame) *skipNoFrame = s_tdiag.skipNoFrame;
    if (skipLong) *skipLong = s_tdiag.skipLongFrame;
    if (errAvg) *errAvg = s_tdiag.ticks ? s_tdiag.errSum / (float)s_tdiag.ticks : 0.f;
    if (errMax) *errMax = s_tdiag.errMax;
    if (stepAvg) *stepAvg = s_tdiag.steps ? s_tdiag.stepSum / (float)s_tdiag.steps : 0.f;
    if (gy) *gy = s_tdiag.gainYaw;
    if (gp) *gp = s_tdiag.gainPitch;
    if (trim) *trim = s_tdiag.trim;
    if (learned) *learned = s_tdiag.learned ? 1 : 0;
    s_tdiag = TouchDiag{};
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
    static int   s_holdFrames = 0;
    // Приказы «в полёте» (см. «шаг в полёте» ниже): окно последних приказов и
    // оценка по поглощению остатка. Объявлены вверху, потому что чистятся в
    // том числе на путях ДО их использования (нет цели, смена цели).
    struct FlightOrd { float yawDeg, pitchDeg, age; };
    static std::vector<FlightOrd> s_flight;
    static float s_absorbYaw = 0.f, s_absorbPitch = 0.f; // заказано, не отработано (град)
    static int   s_holdNoFrame = 0;   // тактов подряд без нового кадра игры
    static int   s_skipLongStreak = 0; // рваных кадров подряд (см. ниже)
    static double s_lastStepTime = 0.0;  // когда выдали прошлый заказ
    ++s_tdiag.ticks;
    static float s_stallYaw = 0.f, s_stallPitch = 0.f;
    static float s_prevRawYaw = 0.f, s_prevRawPitch = 0.f;
    static bool  s_havePrevRaw = false;
    // Счётчики однонаправленного остатка в полосе дрожи (см. константы выше).
    static int s_streakYaw = 0, s_streakPitch = 0;
    static AimPick pick;

    float sw = 0.f, sh = 0.f;
    if (!AimBegin(dt, sw, sh)) {
        AimReleaseFinger(s_fingerDown);
        s_haveLast = false; pick.reset();
        s_lostFrames = 0; s_holdFrames = 0;
        s_trackYaw.reset(); s_trackPitch.reset();
        s_flight.clear();
        s_absorbYaw = s_absorbPitch = 0.f;
        s_stallYaw = s_stallPitch = 0.f;
        s_havePrevRaw = false;
        s_streakYaw = s_streakPitch = 0;
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
    // ---- сторож молчания ----------------------------------------------------
    // Ни одна из страховок ниже (ожидание отклика камеры, учёт «в полёте»,
    // темп подачи, пропуск рваного кадра) не имеет права выключить аим
    // насовсем: каждая из них рассчитана на единичные такты, но при неудачном
    // сочетании (нет оси камеры, низкий ФПС, заказ без отклика) они молчат
    // одна за другой — и получается «аим вообще не работает». Если цель есть,
    // а шага не было дольше kAimSilenceMax, следующий такт проходит все
    // задержки насквозь и работает по сырому остатку. Мёртвая зона при этом
    // соблюдается: если прицел уже на цели, шаг не нужен и сторож не сработает.
    static constexpr double kAimSilenceMax = 0.35;   // с без единого шага
    const bool forceStep = (MonoNow() - s_lastStepTime) > kAimSilenceMax;

    static int   s_ackTimeouts = 0;
    static float s_ackDisabled = 0.f;
    const bool camAnswered = camMoved || s_camMovedPrev;
    s_camMovedPrev = camMoved;
    if (s_ackDisabled > 0.f) s_ackDisabled -= dt;
    const bool ackGate = (s_ackDisabled <= 0.f);
    if (ackGate && s_fingerDown && !camAnswered && !forceStep &&
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
    //   2. ОКНО приказов моложе kTouchFlightLife: ограничивает оценку сверху
    //      на ведении цели (цель убегает, а поток не посчитался — остаток не
    //      падает, и поглощение само не списывается никогда).
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
    //   * у цели (полоса 0.45°, вблизи 1.2°) шаг выдаётся только при
    //     устойчивом однонаправленном остатке — дрожь кости гоняла палец с
    //     частотой кадров (110 реверсов на стенде);
    //   * доля шага спадает вместе с остатком — поздние прилёты длинной
    //     трубы не перелетают кость.
    static constexpr float kTouchFlightLife = 0.13f;  // жизнь приказа в окне, с
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
    // Полоса дрожи на средней/дальней дистанции. Вблизи она расширяется до
    // 1.2° (см. jitterGate): одна и та же анимация кости занимает больше
    // градусов, но устойчивое смещение и в расширенной полосе проходит.
    static constexpr float kTouchJitterBand  = 0.45f;
    static constexpr int   kTouchDriftStreak = 3;
    const bool  useFlight = !haveCam;
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
                       [](const FlightOrd& o) { return o.age >= kTouchFlightLife; }),
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
        if (forceStep) {
            // Сторож молчания: заказы, из-за которых остаток «съеден», уже
            // давно не отвечают — считать их летящими больше нельзя, иначе
            // сторож пробьёт задержки и всё равно получит нулевую ошибку.
            s_flight.clear();
            s_absorbYaw = s_absorbPitch = 0.f;
            s_stallYaw = s_stallPitch = 0.f;
        } else {
            errYaw   -= inflYaw;
            errPitch -= inflPitch;
        }
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

    // ---- dead zone ----
    // Angular size of a few centimetres grows sharply inside 3 m. Keep the
    // old long-range floor, but stop chasing every animation twitch at range.
    float deadYaw = AimTouchDeadDeg(degPerPx, best.world_dist,
                                     gainKnownYaw ? qYaw : 0.f);
    float deadPitch = AimTouchDeadDeg(degPerPx, best.world_dist,
                                       gainKnownPitch ? qPitch : 0.f);
    // Предельный цикл. Лог 26.09 22:50: ошибка уже мелкая (0.5-1°), гейн
    // стабилен, но шаг выдаётся почти на каждом такте и знак ошибки меняется
    // 4 раза в секунду — контроллер «охотится» внутри собственной амплитуды и
    // трясёт картинку. Запоминаем, на какой величине ошибки происходят
    // реверсы, и расширяем мёртвую зону до этой амплитуды: мельче неё аим
    // просто не дёргается, крупные поправки идут как раньше.
    {
        const float amp = AimTouchFlipMag();
        if (amp > 0.f) {
            // 0.35 амплитуды и потолок 0.6°: прошлая версия (0.7 и 2.5°) душила
            // и сами поправки — аим стал вялым, а качка просто замедлилась.
            float grow = amp * 0.35f;
            if (grow > 0.6f) grow = 0.6f;
            if (grow > deadYaw) deadYaw = grow;
            if (grow > deadPitch) deadPitch = grow;
        }
    }
    if (fabsf(errYaw) < deadYaw && fabsf(errPitch) < deadPitch) {
        s_pendDx = s_pendDy = 0.f; s_pendTime = 0.f;
        // Заказанное за прошлые такты камера дорабатывает и без нас: окно
        // стареет само, приказы молча истекают по kTouchFlightLife.
        if (s_fingerDown) Touch_Move(s_fx, s_fy); // hold still, keep the touch alive
        return;
    }

    // ---- полоса дрожи у цели ----
    // Остаток внутри полосы (но вне мёртвой зоны) — это либо дрожь кости,
    // либо медленный дрейф. Шаг выдаётся только при УСТОЙЧИВОМ остатке в одну
    // сторону: чередующаяся анимация не проходит, настоящий дрейф проходит
    // целиком после нескольких тактов. Вблизи дрожание той же кости занимает
    // больше градусов, поэтому там расширяем только эту фильтруемую полосу;
    // dead-zone остаётся малой и устойчивое смещение всё равно исправляется.
    const float touchJitterBand =
        (std::isfinite(best.world_dist) && best.world_dist > 0.f && best.world_dist < 2.5f)
            ? 1.2f : kTouchJitterBand;
    auto jitterGate = [touchJitterBand](float eff, int& streak) {
        if (eff == 0.f) { streak = 0; return 0.f; }
        if (fabsf(eff) >= touchJitterBand) { streak = 0; return eff; }
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

    // ---- темп подачи: не чаще 30 заказов в секунду ----
    // Прошлая попытка ждала, пока «шевельнётся камера», и почти не срабатывала:
    // камера двигается и сама (ходьба, отдача), так что условие было истинным
    // на каждом такте (лог 27.09 00:08: пропущено 1-3 из 15). Поэтому теперь
    // ограничение по ЧАСАМ: игра рисует 30-40 кадров в секунду, и чаще всё
    // равно некуда — каждый лишний заказ между её кадрами удваивает петлевое
    // усиление и раскачивает прицел. Шаг при этом считается от полной ошибки,
    // так что скорость наведения не падает: закрываем больше за один заказ.
    {
        const double now_s = MonoNow();
        if (!forceStep && now_s - s_lastStepTime < 0.030) {
            ++s_tdiag.skipNoFrame;
            if (s_fingerDown) Touch_Move(s_fx, s_fy);
            return;
        }
    }

    // ---- такт без нового кадра игры ----
    // Наш оверлей идёт 60 раз в секунду, а игра рисует 30-40: примерно каждый
    // второй такт видит ТУ ЖЕ позу камеры. Для контроллера это выглядит как
    // «поправка не подействовала» — и он заказывает ещё столько же. Камера
    // приходит на сумму и перелетает; ошибка меняет знак — маятник влево-
    // вправо. Пока есть неотработанный заказ и камера не шевельнулась,
    // держим палец и ждём отклика.
    // ВАЖНО: признак camMoved считается ТОЛЬКО когда читается настоящая ось
    // камеры (haveCam). На этом устройстве её нет (cam_st = 0 во всех строках
    // лога), и camMoved там false ВСЕГДА — значит условие ниже выполнялось на
    // каждом такте, где висел хоть какой-то заказ, а заказ без потока камеры
    // списывается только по падению остатка или по таймауту в 1 с. На практике
    // это давало 8 пропущенных тактов после каждого шага и шаг раз в секунду:
    // жалоба 27.09 «тач-аим вообще перестал работать». Поэтому ждать отклика
    // камеры имеет смысл только там, где этот отклик вообще виден.
    if (!forceStep && haveCam && !camMoved && (s_absorbYaw != 0.f || s_absorbPitch != 0.f)) {
        ++s_holdNoFrame;
        if (s_holdNoFrame <= 8) {          // но не залипаем навсегда
            ++s_tdiag.skipNoFrame;
            if (s_fingerDown) Touch_Move(s_fx, s_fy);
            return;
        }
    } else {
        s_holdNoFrame = 0;
    }

    // ---- рваный кадр ----
    // Модель задержки у контроллера жёсткая: касание доезжает до камеры за
    // 2-4 кадра. Когда кадр вместо 17 мс занял 100+ мс (в логе 26.09 были
    // пики до 960 мс из-за самого чита), «в полёте» и поза игрока устарели на
    // целую вечность, и шаг, посчитанный по такой ошибке, гарантированно
    // перелетает — это и есть маятник после каждого фриза. На таком кадре
    // палец держим на месте и ждём нормального такта: потеря — один кадр,
    // выигрыш — отсутствие перелёта.
    // Порог 0.045 с — это 22 кадра оверлея в секунду: на телефоне под нагрузкой
    // (ЕСП + подбор оффсетов) такт легко идёт дольше, и аим замолкал НАВСЕГДА,
    // а не «на фризе» (жалоба 27.09). Пропускаем только действительно рваный
    // кадр и не больше двух подряд: дальше ведём цель даже по устаревшей позе —
    // медленный аим лучше молчащего.
    if (!forceStep && dt > 0.090f && s_skipLongStreak < 2) {
        ++s_skipLongStreak;
        ++s_tdiag.skipLongFrame;
        s_streakYaw = s_streakPitch = 0;
        if (s_fingerDown) Touch_Move(s_fx, s_fy);
        return;
    }
    s_skipLongStreak = 0;

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
        clearFlight();
        return; // let the game register the touch before moving it
    }
    if (s_holdFrames < 1) { ++s_holdFrames; Touch_Move(s_fx, s_fy); return; }

    // ---- контроллер: жёсткий лок, как у режима «Память» --------------------
    // Приказ считается от ПОЛНОГО остатка (err уже за минусом летящих в трубе
    // касания ускорений — см. учёт «в полёте» выше), как мемори пишет
    // абсолютный угол кости: камера закрывает расстояние до кости за 1-2
    // приказа вместо экспоненциального догона. Палец переводит пиксели в
    // градусы строго линейно (игра умножает накопленный сдвиг на
    // m_Sensitivity), поэтому полный приказ — это точный поворот на угол
    // остатка, а не «рывок неизвестно насколько».
    //
    // Бывшие ограничители доли ошибки (потолок 0.55/такт, спад доли у цели,
    // мягкий старт в треть шага после захвата) убраны (просьба 27.09: «тач
    // должен цепляться сразу, как память»): они и давали вялое «ползёт за
    // костью» и заметное отставание на бегущих целях. Устойчивость теперь
    // держат: вычёт летящих приказов, мёртвая зона, порог устойчивого дрейфа
    // в полосе дрожи и гасители реверсов ниже — они срабатывают только когда
    // петля реально качнулась через ноль.
    //
    // Слайдер «Скорость»: 4 и выше (умолчание 5) — полный лок; 1-3 оставляют
    // мягкое ведение долей ошибки для тех, кто хочет «тихий» аим.
    float sm = g_state.gun_str;
    if (!(sm >= 1.f)) sm = 1.f;
    if (sm > 10.f) sm = 10.f;
    float k = 1.f;
    if (sm < 4.f) {
        const float frac = 0.20f + (sm - 1.f) * 0.10f;   // 0.20..0.40 за кадр @60fps
        k = 1.f - powf(1.f - frac, dt * 60.f);
        if (k < 0.05f) k = 0.05f;
    }

    // Гаситель «качелей»: остаток по любой из осей перескочил через ноль и уже
    // набрал заметную величину обратного знака — это перелёт от нашей поправки. На несколько
    // тактов режем шаг, чтобы вместо дрожи подойти спокойно; восстанавливаемся
    // плавно (около четырёх тактов). Стенд: реверсы пальца на подходе 6 -> 4,
    // перелёт не растёт.
    static float s_flipDamp = 1.f;
    static float s_prevCtlYaw = 0.f, s_prevCtlPitch = 0.f;
    static bool  s_haveCtlErr = false;
    // Стойкий трим: одиночный гаситель (0.45 на реверс, восстановление за
    // четыре такта) качели гасит, но НЕ лечит устойчивую раскачку — в логе
    // 26.09 22:30 реверсы шли по 1-2 на каждые 15 тактов подряд. Копим их:
    // каждый реверс срезает шаг ещё на 15%, спокойные такты возвращают по 1.5%.
    // Нижний предел 0.35 — дальше наведение станет заметно вялым.
    static float s_trim = 1.f;
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
    if (flipYaw || flipPitch) {
        ++s_tdiag.flips;
        s_trim *= 0.9f;
        if (s_trim < 0.6f) s_trim = 0.6f;
        const float mag = (fabsf(errYaw) > fabsf(errPitch)) ? fabsf(errYaw) : fabsf(errPitch);
        s_flipMag = (s_flipMag <= 0.f) ? mag : (s_flipMag * 0.7f + mag * 0.3f);
    } else {
        // Забываем амплитуду медленно: одиночный спокойный такт не должен
        // возвращать охоту.
        if (s_flipMag > 0.f) {
            s_flipMag -= dt * 0.6f;
            if (s_flipMag < 0.f) s_flipMag = 0.f;
        }
        s_trim += 0.02f;       // назад к полной силе за ~0.3 с
        if (s_trim > 1.f) s_trim = 1.f;
    }
    k *= s_trim;
    s_tdiag.trim = s_trim;
    {
        const float mag = (fabsf(errYaw) > fabsf(errPitch)) ? fabsf(errYaw) : fabsf(errPitch);
        s_tdiag.errSum += mag;
        if (mag > s_tdiag.errMax) s_tdiag.errMax = mag;
        s_tdiag.gainYaw = gy; s_tdiag.gainPitch = gp; s_tdiag.learned = learned;
    }
    s_prevCtlYaw = errYaw; s_prevCtlPitch = errPitch; s_haveCtlErr = true;
    k *= s_flipDamp;

    float dx =  errYaw   * k / gy;
    float dy = -errPitch * k / gp;

    // Do not send the whole remaining error on the last 2-3 digitizer
    // units: wait for each one to land instead of alternating across a bone.
    if (gainKnownYaw)
        dx = AimTouchFinalGridStep(errYaw, gy, qYaw, unitsPerPx, deadYaw, dx);
    if (gainKnownPitch)
        dy = AimTouchFinalGridStep(-errPitch, gp, qPitch, unitsPerPx, deadPitch, dy);
    // A noisy axis already inside its own zone must not follow the other axis.
    if (fabsf(errYaw) < deadYaw) dx = 0.f;
    if (fabsf(errPitch) < deadPitch) dy = 0.f;

    // Шаг не больше оставшейся ошибки. Иначе собственный шаг перелетает цель, и
    // следующий шаг считается уже по ошибке другого знака — то самое «дёргается»
    // (камера отрабатывает шаг не сразу, поэтому перелёт виден глазом).
    if (fabsf(dx * gy) > fabsf(errYaw)) dx = errYaw / gy;
    if (fabsf(dy * gp) > fabsf(errPitch)) dy = -errPitch / gp;

    // Clamp per-frame travel so a bad gain estimate never slingshots.
    // Жёсткому локу нужен полный ход пальца: при ИЗМЕРЕННОМ коэффициенте
    // (выучен по повороту камеры или оценке по реакции прицела) палец может
    // пройти до половины высоты экрана за один приказ — больше и не нужно,
    // дальше круга FOV всё равно никого нет. По коэффициенту из настройки
    // без измерения ошибка возможна в 2 раза по модулю, поэтому там —
    // прежняя осторожная полоса.
    const float maxStep = (learned || lsYaw || lsPitch) ? sh * 0.50f : sh * 0.05f;
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
    ++s_tdiag.steps;
    s_lastStepTime = MonoNow();
    s_tdiag.stepSum += (fabsf(dx) > fabsf(dy)) ? fabsf(dx) : fabsf(dy);
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
        case AIM_MODE_SILENT: return "сайлент";
        default:              return "тач";
    }
}

// Сколько раз ось выстрелаdobили в конце кадра между двумя строками лога

void AimTouchDiagFetch(int*, int*, int*, int*, int*, float*, float*, float*, float*, float*, float*, int*);

static void AimLogTick(float dt) {
    s_logTime += dt;
    if (s_logTime < 0.25f) return;
    s_logTime = 0.f;
    const AimMemDiag& d = AimMemoryDiag();
    if (g_state.aim_mode == AIM_MODE_SILENT) {
        const auto s = silent_aim::stats();
        LogLine("сайлент DLP2: активен=%d ось=%d записей=%llu отказов=%llu гвард=%llu молчит=%d нетцели=%d",
                (int)s.active, (int)s.axis, (unsigned long long)s.writes,
                (unsigned long long)s.failures, (unsigned long long)s.guard_failures,
                d.inactive, d.no_target);
    }
    if (g_state.aim_mode == AIM_MODE_TOUCH) {
        int ticks = 0, steps = 0, flips = 0, skipNoFrame = 0, skipLong = 0, learned = 0;
        float errAvg = 0.f, errMax = 0.f, stepAvg = 0.f, gy = 0.f, gp = 0.f;
        float trim = 1.f;
        AimTouchDiagFetch(&ticks, &steps, &flips, &skipNoFrame, &skipLong,
                          &errAvg, &errMax, &stepAvg, &gy, &gp, &trim, &learned);
        // Пишем только когда есть о чём: иначе четыре пустые строки в секунду
        // забивают журнал (лог 26.09 21:32 — сплошные «шагов 0»).
        if (ticks && (steps || flips || skipNoFrame || skipLong))
            LogLine("тач-аим: тактов %d, шагов %d, реверсов %d, пропущено без кадра игры %d, "
                    "рваных кадров %d, |ошибка| сред %.2f° макс %.2f°, шаг сред %.1f px, "
                    "трим %.2f, гейн yaw %.4f pitch %.4f град/px%s",
                    ticks, steps, flips, skipNoFrame, skipLong,
                    (double)errAvg, (double)errMax, (double)stepAvg, (double)trim,
                    (double)gy, (double)gp, learned ? " (выучен)" : " (из настройки)");
    }
    if (g_state.aim_mode == AIM_MODE_MEMORY)
        LogLine("аим: %s кадр=%lu объект=%d отклик=%d записей=%d отказов=%d расхождение=%.1f молчит=%d прицел=%d источник=%d нетцели=%d нацели=%d чтение=%d",
                aim_mode_name(), s_frames, (int)d.params, (int)d.responded,
                d.writes, d.fails, (double)d.mismatch, d.inactive, d.ads, d.ads_source,
                d.no_target, d.on_target, d.read_fail);
}

// ====================== Мемори-аим: «память» ===============================
//
// ЖЁСТКИЙ ЛОК кости абсолютным углом. Запись идёт в накопитель MouseLook —
// тот самый, из которого игра каждый кадр разворачивает поворот камеры:
//
//     angles = clamp(angles + input * m_Sensitivity)      (MouseLook.ZJo)
//
// Накопитель уже в градусах, целевое направление — база фактического луча
// выстрела (см. esp_aim_projection_basis) плюс остаток до кости. Это абсолютная
// цель, не прибавка к прошлой команде; ход к ней ограничен скоростью 120°/с,
// чтобы «Память» не дёргала камеру резкой записью. Повторная запись остаётся
// идемпотентной, поэтому задержка применения не складывает несколько шагов.
// Если игра переписала накопитель своим вводом, следующий такт снова считает
// шаг от фактически прочитанного значения, а не накапливает потерянную команду.
//
// Дрожь кости гасится без уменьшения шага. Шаг — ВСЕГДА полный остаток (лок
// бьёт ровно в кость: доля у цели давала видимое отставание от идущей кости,
// жалоба 23.09 «мемори отстаёт от головы»). Дрожь отсекает пара фильтров:
// мёртвая зона (остаток внутри неё — камера стоит) и порог устойчивого
// однонаправленного дрейфа в полосе у цели — дрожь знак чередует и порог не
// набирает, настоящее движение знак держит и проходит целиком
// (см. mem_axis_off; стенды tools/aim/lock_sim.cpp и mem_test: строгая
// дрожь не двигает камеру вовсе, медленный дрейф 3°/с сходится без следа).
//
// Знаки накопителя (замер в логах): [0] rotationX = минус тангаж (вверх —
// меньше), [1] rotationY = рысканье (вправо — больше).

// Мёртвая зона, град. Внутри неё камера стоит: пока остаток меньше зоны,
// кость «дышит» (анимация двигает точку на сантиметры), и лок дёргал бы
// камеру на каждый вдох. 0.12° — ~5 см на 25 м, внутри зоны поражения головы.
static constexpr float kMemDeadDeg  = 0.12f;
// Полоса «дрожь кости» и порог дрейфа в ней. Выше полосы шаг ВСЕГДА полный:
// лок бьёт ровно в кость без вычета мёртвой зоны и без долей — прежняя доля
// 0.4 в полосе 0.6° давала видимое отставание: на идущей цели лок полз к
// кости со скоростью ~2°/с, а кость уходила на 5-30°/с (жалоба «мемори
// отстаёт от головы»). Внутри полосы полный шаг выдаётся только при
// УСТОЙЧИВОМ однонаправленном дрейфе (дрожь кости знак чередует, настоящее
// движение — нет). Дрожь ±0.15..0.4° при чередовании знака ни разу не
// набирает порога — камера стоит (проверено стендом).
static constexpr float kMemJitterBand = 0.45f;
static constexpr int   kMemDriftStreak = 3;
// ACK по накопителю: расхождение с нашей записью, в пределах которого игра
// запись приняла (свой ввод игрок между тактами не добавлял). Только для
// диагностики («отклик» в меню) — жёсткому локу подтверждать нечего: он
// пишет абсолютный угол каждый такт.
static constexpr float kMemAckEps = 0.10f;
// Slew-limit large absolute writes. The game consumes this accumulator as an
// angle, so a bounded step still lands on the exact bone without snapping the
// view in one update. dt scaling keeps the rate stable across frame rates.
static constexpr float kMemTurnRateDegPerSec = 120.f;
static constexpr float kMemTurnStepMaxDeg = 12.f;

static AimMemDiag s_memDiag;

const AimMemDiag& AimMemoryDiag() { return s_memDiag; }

// Остаток -> шаг к кости. Возврат: полный остаток (лок ровно в кость) или 0
// (камера на цели / кость дрожит — стоим). Счётчик однонаправленного дрейфа
// свой у каждой оси.
static float mem_axis_off(float err, int& streak) {
    if (fabsf(err) < kMemDeadDeg) { streak = 0; return 0.f; }
    if (fabsf(err) >= kMemJitterBand) return err;   // настоящее движение — полный лок
    // Дрожь-зона: полный шаг только при устойчивом дрейфе в одну сторону.
    const int s = (err > 0.f) ? 1 : -1;
    streak = (streak * s > 0) ? streak + s : s;
    if (streak * s >= kMemDriftStreak) return err;
    return 0.f;
}

static void UpdateAimMemory(float dt) {
    static AimPick pick;
    static float s_lastCamYaw = 0.f, s_lastCamPitch = 0.f;
    static bool  s_haveLast = false;
    // Последняя запись в накопитель (в тех же завёрнутых координатах, что
    // вернёт чтение): по совпадению видно, приняла ли игра запись.
    static float s_lastWriteX = 0.f, s_lastWriteY = 0.f;
    static bool  s_haveLastWrite = false;
    // Счётчики однонаправленного дрейфа (см. mem_axis_off): по своему на ось.
    static int s_streakYaw = 0, s_streakPitch = 0;

    float sw = 0.f, sh = 0.f;
    int why = 0;
    if (!AimBegin(dt, sw, sh, &why)) {
        s_memDiag = AimMemDiag{};
        s_memDiag.inactive = why;   // печатается в той же строке «объект=…»
        s_memDiag.ads = s_adsState; s_memDiag.ads_source = s_adsSource;
        pick.reset(); s_haveLast = false; s_haveLastWrite = false;
        s_streakYaw = s_streakPitch = 0;
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
        // Писать некуда: объекта нет (нет игрока, респавн, смена мира). На тач
        // НЕ падаем — режим либо работает, либо молчит.
        return;
    }
    s_memDiag.gyro = gyro;

    // Текущий накопитель углов камеры. Запасная запись (без базы углов)
    // считается от него; абсолютной базе он не нужен, но прочитать его всё
    // равно необходимо: это самая дешёвая проверка, что объект жив.
    float accX = 0.f, accY = 0.f;
    LogStage(kStageAimRead);
    if (!esp_mem_aim_read_angles(accX, accY)) {
        // Самый молчаливый путь: цель есть, писать нечем, а ни один счётчик
        // не растёт. Без отметки здесь кадр не отличить от «цели не было».
        ++s_memDiag.read_fail;
        pick.reset(); s_haveLast = false; s_haveLastWrite = false;
        s_streakYaw = s_streakPitch = 0;
        return;
    }

    AimTarget best;
    float degPerPx = 0.f;
    int noTgtWhy = 0;
    if (!AimSelectTarget(sw, sh, pick, best, degPerPx, &noTgtWhy)) {
        s_memDiag.no_target = noTgtWhy;
        pick.reset(); s_haveLast = false; s_haveLastWrite = false;
        s_streakYaw = s_streakPitch = 0;
        return;
    }
    s_memDiag.no_target = 0;
    if (pick.switched) {
        // Другая цель: подтверждение прошлой записи и дрейф по прежней кости
        // к ней не относятся.
        s_haveLastWrite = false;
        s_streakYaw = s_streakPitch = 0;
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
                             fabsf(WrapDeg180(accX - camPitch));
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

    // ---- абсолютная цель с ограничением скорости ----------------------------
    // Сетпоинт по каждой оси — ПОЛНЫЙ остаток до кости (без вычета мёртвой зоны
    // и без долей); ограничение по скорости ниже сглаживает большой разворот,
    // но не меняет конечный угол. Ноль — камера на цели или кость дрожит:
    // писать нечего (см. mem_axis_off).
    const float offYaw   = mem_axis_off(best.yaw,   s_streakYaw);
    const float offPitch = mem_axis_off(best.pitch, s_streakPitch);
    if (offYaw == 0.f && offPitch == 0.f) {
        s_haveLastWrite = false;   // подтверждения больше нет — записи нет
        ++s_memDiag.on_target;
        return;
    }

    // Абсолютное направление кости: база — ТА ЖЕ камера, от которой посчитан
    // остаток (иначе лок встанет на расхождение источников, см.
    // esp_aim_projection_basis). Ось, по которой шага нет, пишем из
    // накопителя как есть: база в этот такт может отставать от него на кадр
    // (граница фаз), и запись «база + 0» дёрнула бы камеру назад. Запасной
    // путь без базы — инкремент от накопителя: редок (без матрицы вида не
    // строится сам бокс) и потому мирится с задержкой видимости, от которой
    // абсолютная запись свободна.
    float baseYaw = 0.f, basePitch = 0.f;
    const bool haveBasis = esp_aim_projection_basis(baseYaw, basePitch) &&
                           std::isfinite(baseYaw) && std::isfinite(basePitch);
    const float maxTurn = std::fmin(kMemTurnRateDegPerSec * dt, kMemTurnStepMaxDeg);
    auto limitTurn = [maxTurn](float step) {
        if (step > maxTurn) return maxTurn;
        if (step < -maxTurn) return -maxTurn;
        return step;
    };
    float writeX = accX, writeY = accY;
    if (haveBasis) {
        if (offYaw != 0.f) {
            const float targetYaw = WrapDeg180(baseYaw + offYaw);
            const float deltaYaw = WrapDeg180(targetYaw - accY);
            writeY = WrapDeg180(accY + limitTurn(deltaYaw));
        }
        if (offPitch != 0.f) {
            const float currentPitch = -accX;
            const float targetPitch = basePitch + offPitch;
            writeX = WrapDeg180(-(currentPitch + limitTurn(targetPitch - currentPitch)));
        }
    } else {
        // Without a projection basis the residual is relative to the current
        // accumulator; limit the incremental request but preserve the same axis
        // signs as the exact absolute-write path above.
        if (offYaw != 0.f) writeY = WrapDeg180(accY + limitTurn(offYaw));
        if (offPitch != 0.f) writeX = WrapDeg180(accX - limitTurn(offPitch));
    }

    // Градусы -> накопитель игры: X — «наклон вниз» (вверх — меньше),
    // Y — рысканье (вправо — больше).
    LogStage(kStageAimWrite);
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


// ====================== Сайлент (DLP2 adapter) =============================
// Сайлент ставит ось точно в кость, но пуля всё равно уходит в конус разброса
// оружия: в логе 26.09 23:29 конус был 0..20, и на дистанции это гарантирует
// мимо. Разброс живёт в данных (DispersionConfig.min/max), их обнуляет блок
// «Оружие» — поэтому, пока сайлент активен, просим тот же механизм выключить
// разброс. Патчей кода нет: те же записи в объектные данные с сохранением и
// возвратом оригиналов.
static std::atomic<bool> g_silent_no_spread{false};
static std::atomic<unsigned long long> g_silent_no_spread_ms{0};
// Флаг «липкий»: цель теряется и находится по нескольку раз в секунду, а
// каждое переключение — это запись в память игры и возврат оригиналов.
// Держим полторы секунды после последней цели, чтобы не молотить запись.
bool esp_silent_wants_no_spread() {
    if (g_silent_no_spread.load(std::memory_order_relaxed)) return true;
    const unsigned long long until = g_silent_no_spread_ms.load(std::memory_order_relaxed);
    if (!until) return false;
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const unsigned long long now = (unsigned long long)ts.tv_sec * 1000ull +
                                   (unsigned long long)(ts.tv_nsec / 1000000);
    return now < until;
}
static void silent_no_spread_set(bool on) {
    if (on) {
        struct timespec ts{};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        const unsigned long long now = (unsigned long long)ts.tv_sec * 1000ull +
                                       (unsigned long long)(ts.tv_nsec / 1000000);
        g_silent_no_spread_ms.store(now + 1500ull, std::memory_order_relaxed);
    }
    g_silent_no_spread.store(on, std::memory_order_relaxed);
}

static AimPick s_silentPick;
static void UpdateAimSilent(float dt) {
    float sw = 0.f, sh = 0.f;
    int why = 0;
    if (!AimBegin(dt, sw, sh, &why)) {
        silent_aim::clear(); s_silentPick.reset();
        silent_no_spread_set(false);
        s_memDiag = AimMemDiag{}; s_memDiag.inactive = why;
        return;
    }
    s_memDiag.inactive = 0;
    s_memDiag.ads = s_adsState; s_memDiag.ads_source = s_adsSource;
    AimTarget best;
    float degPerPx = 0.f;
    if (!AimSelectTarget(sw, sh, s_silentPick, best, degPerPx, &why)) {
        silent_aim::clear(); s_silentPick.reset();
        silent_no_spread_set(false);
        s_memDiag.no_target = why;
        return;
    }
    s_memDiag.no_target = 0;
    // Цель есть и ось публикуется — держим разброс нулевым.
    silent_no_spread_set(true);
    if (!esp_silent_publish(best.id, best.world)) {
        g_aimActive = false;
        ++s_memDiag.fails;
    }
    s_memDiag.axis = silent_aim::stats().axis;

    // Замер промаха: как только игра отметила НОВЫЙ выстрел, сравниваем точку
    // попадания с точкой, куда целились. Жалоба «сайлент не попадает, особенно
    // вдали» без этих цифр не разбирается: видно будет и промах в метрах, и
    // дистанцию, и ушла ли пуля в сторону или не долетела.
    {
        static float s_last_shot_time = -1.f;
        float shot = 0.f;
        if (esp_mem_aim_last_shot(shot) && shot != s_last_shot_time) {
            const bool first = s_last_shot_time < 0.f;
            s_last_shot_time = shot;
            float hit[3] = {0.f, 0.f, 0.f};
            static float s_prev_hit[3] = {0.f, 0.f, 0.f};
            if (!first && best.world_valid && esp_aim_last_hit_point(hit)) {
                const float dx = hit[0] - best.world[0];
                const float dy = hit[1] - best.world[1];
                const float dz = hit[2] - best.world[2];
                const float miss = sqrtf(dx * dx + dy * dy + dz * dz);
                // Точка попадания не изменилась с прошлого выстрела — значит
                // пуля не попала НИКУДА: игра обновляет её только по факту
                // попадания. Лог 26.09 22:51: дальше 25 м так было у всех
                // четырёх выстрелов подряд.
                const bool zero = fabsf(hit[0]) < 0.001f && fabsf(hit[1]) < 0.001f &&
                                  fabsf(hit[2]) < 0.001f;
                const bool same = zero ||
                                  (fabsf(hit[0] - s_prev_hit[0]) < 0.001f &&
                                   fabsf(hit[1] - s_prev_hit[1]) < 0.001f &&
                                   fabsf(hit[2] - s_prev_hit[2]) < 0.001f);
                s_prev_hit[0] = hit[0]; s_prev_hit[1] = hit[1]; s_prev_hit[2] = hit[2];
                float wmax = 0.f, wbase = 0.f, wlo = 0.f, whi = 0.f;
                const bool have_w = esp_weapon_ballistics(wmax, wbase, wlo, whi);
                if (same) {
                    LogLine("сайлент выстрел: ПОПАДАНИЯ НЕТ, цель на %.1f м; оружие: дальность %.0f м, "
                            "базовый разброс %.3f, конус %.3f..%.3f%s",
                            (double)best.world_dist, (double)wmax, (double)wbase,
                            (double)wlo, (double)whi,
                            have_w ? "" : " (баллистику прочитать не удалось)");
                } else {
                    LogLine("сайлент выстрел: промах %.2f м (цель на %.1f м), оружие бьёт на %.0f м, "
                            "конус %.3f..%.3f; цель [%.1f %.1f %.1f], попадание [%.1f %.1f %.1f]",
                            (double)miss, (double)best.world_dist, (double)wmax,
                            (double)wlo, (double)whi,
                            (double)best.world[0], (double)best.world[1], (double)best.world[2],
                            (double)hit[0], (double)hit[1], (double)hit[2]);
                }
            }
        }
    }
}

void AimEndFrame() {
    if (g_state.aim_mode != AIM_MODE_SILENT) {
        silent_aim::clear();
        silent_no_spread_set(false);
        return;
    }
    // UI runs after UpdateAim: honour a just-opened menu or disabled toggle
    // before another frame, without publishing a second/stale command.
    const bool wasActive = g_aimActive;
    float dt = 1.f / 60.f, sw = 0.f, sh = 0.f;
    if (!AimBegin(dt, sw, sh)) silent_aim::clear();
    else g_aimActive = wasActive;
}

// Диспетчер: какой режим ведёт камеру в этом кадре.
void UpdateAim(float dt) {
    ++s_frames;
    const int mode = g_state.aim_mode;
    AimLogTick(dt);
    if (mode != AIM_MODE_SILENT) {
        silent_aim::clear(); s_silentPick.reset();
        silent_no_spread_set(false);
    }
    if (mode != AIM_MODE_TOUCH) AimReleaseFinger(s_fingerDown);
    if (mode == AIM_MODE_SILENT) UpdateAimSilent(dt);
    else if (mode == AIM_MODE_MEMORY) UpdateAimMemory(dt);
    else UpdateAimTouch(dt);
}

// ============================ Автофарм =============================
//
// Полностью на синтетических тачах: три пальца (0 — джойстик движения,
// 1 — камера, 2 — удар). Слой памяти (game.cpp) отдаёт точку прицела в
// градусах от оси выстрела и точку подхода в градусах от оси камеры; здесь —
// только автоматы. Камерный палец пользуется тем же коэффициентом, который
// выучил аимбот, а если своего нет — аккуратно probe'ит фиксированным.
//