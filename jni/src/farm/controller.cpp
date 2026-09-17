// farm/controller.cpp — UpdateFarm/UpdateFarmInner: контроллер автофарма.
//
// Модуль разрезан из прежнего монолита jni/src/main.cpp;
// что здесь лежит и кто это зовёт — в шапке farm/controller.h и в docs/CODE_MAP.md.


#include "app/common.h"
#include "aim/controller.h"
#include "app/attach.h"
#include "farm/state.h"
#include "ui/esp_overlay.h"
#include "ui/layout.h"
#include "ui/popover.h"
#include "ui/sheet.h"
#include "farm/controller.h"

// ============================ Автофарм =============================
//
// Полностью на синтетических тачах: три пальца (0 — джойстик движения,
// 1 — камера, 2 — удар). Слой памяти (game.cpp) отдаёт точку прицела в
// градусах от оси выстрела и точку подхода в градусах от оси камеры; здесь —
// только автоматы. Камерный палец пользуется тем же коэффициентом, который
// выучил аимбот, а если своего нет — аккуратно probe'ит фиксированным.
//
// Главный принцип переработки: бот бьёт ПО КРЕСТИКУ. Крестик (hit-streak
// marker) больше не угадывается по именам детей и «прыжкам» трансформа, а
// читается напрямую из памяти игры: у руды это трансформ живого маркера
// OreHitstreaks.MoW, у дерева — вектор TreeHitstreaks.MTQ. Именно до этих
// точек игра меряет дистанцию удара (у дерева — до отрезка MTQ..MTu с радиусом
// 15 см), поэтому попадание в них засчитывается в серию и даёт бонусный
// ресурс. Крестик существует не всегда: игра создаёт его при первом же
// попадании по узлу и гасит через 15 секунд простоя — значит цикл всегда один
// и тот же: удар по корпусу -> появился X -> все следующие удары точно в X.
//
// Фазы кадра:
//   TURN  — доворачиваем камеру (при |walk_yaw| > 72° стик «вперёд» уводит
//           от цели, поэтому сначала поворот, потом шаг);
//   WALK  — держим джойстик к точке подхода, пока не дотянемся до цели;
//   MINE  — стоим, удерживаем прицел на крестике и тапаем удар;
//   узел добыт / потерян / недостижим — берём следующий (чёрный список).

namespace {

// Дальность удара, если орудие в руках не опознано / значение не прочиталось.
// Горизонтальные метры до ТОЧКИ ПРИЦЕЛА; подбиралось на устройстве: топор
// дотягивается до коры примерно с 0.8 м, а по камню удар проходит и с полутора —
// пивот камня часто зарыт, и крестик стоит на склоне в стороне от него.
// *Hold — гистерезис: подошли один раз, не дёргаемся.
constexpr float kReachTreeSpot = 0.80f;
constexpr float kReachOreSpot  = 1.55f;
constexpr float kReachTreeHold = 1.00f;
constexpr float kReachOreHold  = 1.80f;
// Пока крестика нет, подходим к корпусу узла — он большой, этого достаточно,
// чтобы первый удар создал X.
constexpr float kReachTreeBody = 2.60f;
constexpr float kReachOreBody  = 2.40f;

// Живая дальность орудия (tgt.melee_reach = FPMelee.m_MaxReach + hitRadius,
// читается из памяти). Игра засчитывает удар строго внутри этой дистанции,
// меряя 3D-метры ОТ ГЛАЗА до точки попадания (FPMelee.ZkX:
// `if (RaycastHit.distance < m_MaxReach + hitRadius) On_Hit else On_Woosh`),
// поэтому с ней работаем в aim_3d, а не по горизонтали. Запас kReachLiveTight —
// на подачу вперёд за время тапа; kReachLiveHold держит узел в работе до самого
// порога, но не за ним (иначе удары свистят впустую).
constexpr float kReachLiveTight = 0.90f;
constexpr float kReachLiveHold  = 0.98f;
// По корпусу (крестика ещё нет) точка прицела стоит в глубине узла — пивот
// дерева в центре ствола, у камня бывает зарыт, — поэтому к дальности удара
// добавляем запас на радиус узла: первому удару достаточно создать X.
constexpr float kReachBodyAllowance = 1.20f;
// Меньше этого живая дальность недостоверна (в конструкторе FPMelee заглушки
// 0.5/0.1 — префаб их перезаписывает, но и префаб может не успеть примениться).
constexpr float kReachLiveMin = 0.70f;
// Пока у игры нет данных рейкаста прицела (обёртка GKo пуста), удар не
// засчитывается вовсе — FPMelee.ZkX играет один On_Woosh. Лог 14.09.2026:
// валидность луча 97% на aim3d ~0.50 м и 19% на ~0.75 м, а из 48 замахов без
// урона луча не было в 43. Поэтому (а) тап держим, пока луч не появится, но не
// дольше kNoRaySwingAfter — вдруг на этой сборке обёртка просто не читается, и
// (б) стик тем временем поджимает ближе, к kNoRayCloseTo, где луч живёт.
constexpr float kNoRaySwingAfter = 2.5f;   // с без луча — больше не ждём, бьём
constexpr float kNoRayCloseTo    = 0.50f;  // м: докуда поджимаем, пока луча нет
                                           // (ровно тот aim3d, где луч жил в 97%)

// Допуск наведения, градусы. По крестику жёстко: 2° промаха на 1.5 м — это
// 5 см в стороне, а засчитываемая зона у дерева всего 15 см.
constexpr float kAimSpotStart = 1.20f;
constexpr float kAimSpotStop  = 0.35f;
constexpr float kAimBodyStart = 4.00f;
constexpr float kAimBodyStop  = 1.50f;
constexpr float kAimWalkStart = 10.0f;   // на подходе правим только заметный увод
constexpr float kAimWalkStop  = 4.00f;
constexpr float kPitchDeadWalk = 18.0f;  // и только сильно задранную камеру

// Удар — только когда прицел сел на точку (иначе тап уходит туда, где камера
// была в прошлой четверти секунды: это и есть «промахнулся по крестику»).
constexpr float kSwingSpotYaw   = 1.60f;
constexpr float kSwingSpotPitch = 2.00f;
constexpr float kSwingBodyYaw   = 8.00f;

constexpr float kTurnBeforeWalk = 72.f;  // |walk_yaw| больше — сначала доворот
constexpr float kCreepYaw       = 18.f;  // но если почти лицом и очень далеко — шагаем
constexpr float kCreepDist      = 8.f;
constexpr float kArriveWalk     = 0.15f; // дошли до точки подхода
constexpr float kWalkOffHold    = 0.25f; // гистерезис отпускания джойстика, с

constexpr float kTapDownMs = 85.f;       // ритм ударов: веримый быстрый тап,
constexpr float kTapUpMs   = 230.f;      // лишние игра просто ставит в очередь

constexpr float kSettleTime    = 0.70f;  // пауза при смене узла
constexpr float kLostHold      = 0.80f;  // цель может мигнуть на рескане реестра
// Смена узла тоже дебаунсится: новый обязан продержаться «лучшим» несколько
// кадров подряд. Лог 14.09.2026 — узел в 20 м перехватывал цель ровно на один
// кадр 9 раз за 150 с, и каждая смена в обе стороны стоила 0.7 с паузы
// (kSettleTime): 854 кадра простоя. Если цель так и прыгает туда-сюда дольше
// kFlickerGiveUp, переключаемся по-настоящему — иначе бот застыл бы навсегда.
constexpr int   kNodeDebounce  = 3;      // кадров подряд на новом узле
constexpr float kFlickerGiveUp = 1.0f;   // с прыжков — больше не держимся
constexpr float kDepletedFrac  = 0.03f;
constexpr int   kDepletedFrames = 10;    // дебаунс сырого чтения остатка
constexpr float kStuckTime     = 3.00f;  // нет продвижения к точке подхода
constexpr float kEvadeTime     = 1.70f;  // ~0.6 назад + ~1.1 вбок
constexpr int   kEvadeMax      = 4;
constexpr float kBlockedTime   = 0.45f;  // узел перекрыт дольше — пробуем обойти
// Сколько линия удара должна быть чистой подряд, чтобы простить счётчик обходов
// перекрытого узла. Мгновенное прощение (как было) означало бесконечный бюджет:
// «перекрыт» мигает кадр-другой, счётчик обнуляется, обход начинается заново с
// той же стороны — и бот вечно кружит вокруг узла (лог 14.09, камень 86a3c0:
// два «обход #1 (вправо)» подряд и ни одного удара).
constexpr float kBlockedClear  = 1.20f;
constexpr float kGiveUpDrain   = 20.f;   // бьём, а остаток не падает
constexpr float kGiveUpBlind   = 45.f;   // то же, но остаток не читается

// град/px, пока коэффициент не выучен, берётся из чувствительности настроек
// клиента — AimSensitivityGain() (для настройки 2.0 это измеренные по логу
// 14.09.2026 0.10 град/px по yaw и ~0.06 по pitch; прежнее запасное 0.25
// завышало коэффициент в 2.5 раза, и доводка тянулась втрое дольше).

// Камера отвечает не в том же кадре. В том же логе поворот приходил через 2
// кадра (реже 4..6, кадр 8.5 мс), а контроллер за это время успевал дослать ещё
// 9..11 шагов подряд: шаги складывались, камера перелетала крестик, знак ошибки
// менялся, палец швырял её обратно — на экране это и есть «прицел дёргается».
// Отсюда такт с подтверждением (шаг — ответ камеры — следующий шаг): вслепую
// шаги больше не копятся, и даже неверный коэффициент оборачивается одним
// перелётом, а не непрерывной раскачкой.
constexpr float kCamMovedEps   = 0.05f;  // град: настолько камера должна шевельнуться
constexpr float kLookWaitMax   = 0.10f;  // с: дольше ответа не ждём — ввод потерялся
// Полоса правдоподобия коэффициента. Измеренные 0.10 лежат в середине (разброс
// одиночных образцов x2). Полоса нужна не для красоты: шаг пальца считается от
// 1/gain, и значение вне её означало бы либо рывок в двадцать раз сильнее
// нужного (0.0083 в логе — 120 px на градус ошибки), либо полное отсутствие
// реакции (1.97 — палец двигается, камера стоит).
constexpr float kGainLo        = 0.035f; // град/px
constexpr float kGainHi        = 0.350f;
constexpr float kGainMinPx     = 12.f;   // короче свайп — отношение тонет в шуме
constexpr float kAimStepSpot   = 0.42f;  // доля остатка ошибки, закрываемая шагом:
constexpr float kAimStepWalk   = 0.28f;  // по крестику резче, на подходе плавнее
constexpr float kStepSpotFrac  = 0.030f; // доля высоты экрана на один шаг доводки
constexpr float kStepWalkFrac  = 0.075f; // (прежние 0.075 = 81 px = 8° рывком)
constexpr float kPitchDeadSpot = 0.50f;  // град: субградусный pitch в упор не доводим
constexpr float kStepQuantumPx = 1.0f;   // px: шаг мельче кванта цифрователя не доходит

} // namespace

static void UpdateFarmInner(float dt);

void UpdateFarm(float dt) {
    UpdateFarmInner(dt);
}

static void UpdateFarmInner(float dt) {
    static bool  s_moveDown = false;  // палец 0: джойстик движения
    static bool  s_lookDown = false;  // палец 1: камера
    static bool  s_tapDown  = false;  // палец 2: удары
    static float s_lookX = 0.f, s_lookY = 0.f;
    static int   s_lookHold = 0;
    static int   s_tapTimer = 0;      // мс до следующего переключения пальца 2
    static float s_gainYaw = 0.f;     // град/px, учим по собственным свайпам
    static float s_lastCamYaw = 0.f, s_lastCamPitch = 0.f;
    static bool  s_haveLast = false;
    // Сдвиг пальца, посланный с прошлого ответа камеры (накопленно): игра
    // проглатывает часть движений при низком FPS и поворачивается на их сумму,
    // поэтому делить можно только на сумму, а не на последний шаг.
    static float s_gainPendDx = 0.f, s_gainPendDy = 0.f;
    static float s_lookWait = 0.f;    // сколько ждём ответа камеры на посланный шаг
    static float s_gainS1 = 0.f, s_gainS2 = 0.f, s_gainS3 = 0.f; // образцы
    static int   s_gainN = 0;         // сколько образцов принято
    static unsigned long long s_nodeId = 0;
    static float s_settle = 0.f;      // пауза между целями (пальцы подняты)
    static float s_lostTime = 0.f;    // цель пропала: ждём, вдруг вернётся
    static float s_mineTime = 0.f;    // сколько бьём этот узел без прогресса
    static float s_fracStart = -1.f;  // остаток на входе (для контроля добычи)
    static float s_healthStart = -1.f;// здоровье узла на входе (m_CurrentHealth)
    static float s_blockedTime = 0.f; // сколько подряд узел перекрыт
    static float s_clearTime = 0.f;   // сколько линия удара чистая (дебаунс прощения обходов)
    static int   s_bestStreak = 0;    // максимум серии по крестику на этом узле
    static int   s_depletedFrames = 0;
    static float s_lastWalk = 1e9f;   // лучшая дистанция до точки подхода
    static float s_stuckTime = 0.f;
    static float s_evadeTime = 0.f;   // > 0: идёт манёвр обхода препятствия
    static float s_evadeDir = 1.f;    // +1 вправо, -1 влево
    static int   s_evadeCount = 0;
    static int   s_evadeWhy = 0;      // 0 нет, 1 застрял на подходе, 2 узел перекрыт
    static float s_walkOffTime = 0.f; // гистерезис отпускания джойстика
    static float s_stickPx = 0.f, s_stickPy = 0.f; // сглаженная позиция стика
    static float s_noRayTime = 0.f;   // сколько подряд у игры нет луча прицела
    static unsigned long long s_candId = 0; // узел-кандидат на смену цели
    static int   s_candFrames = 0;    // сколько кадров подряд он лучший
    static float s_flickerTime = 0.f; // сколько держимся прежнего, с

    auto releaseAll = [&]() {
        if (s_moveDown) { Touch_Up_N(0); s_moveDown = false; }
        if (s_lookDown) { Touch_Up_N(1); s_lookDown = false; }
        if (s_tapDown)  { Touch_Up_N(2); s_tapDown = false; }
        s_haveLast = false;
        s_lookHold = 0;
    };
    auto resetNode = [&]() {
        s_nodeId = 0;
        s_mineTime = 0.f; s_fracStart = -1.f; s_bestStreak = 0;
        s_healthStart = -1.f; s_blockedTime = 0.f; s_clearTime = 0.f;
        s_depletedFrames = 0;
        s_lastWalk = 1e9f; s_stuckTime = 0.f;
        s_evadeTime = 0.f; s_evadeCount = 0; s_evadeWhy = 0;
        s_walkOffTime = 0.f; s_noRayTime = 0.f;
        s_candId = 0; s_candFrames = 0; s_flickerTime = 0.f;
    };

    if (dt <= 0.f || !std::isfinite(dt)) dt = 1.f / 60.f;
    if (dt > 0.1f) dt = 0.1f;

    unsigned mask = 0;
    if (g_state.farm_on) {
        if (g_state.farm_wood)   mask |= 1u;
        if (g_state.farm_stone)  mask |= 2u;
        if (g_state.farm_metal)  mask |= 4u;
        if (g_state.farm_sulfur) mask |= 8u;
    }
    esp_farm_set_resources(mask);
    esp_farm_set_range(g_state.farm_range);

    const bool menuBlocked = g_sheet.visible || (g_pop.visible && !g_pop.closing);
    if (mask == 0 || !g_esp_attached) {
        releaseAll();
        g_farmActive = false; g_farmPhase = 0; g_farmPaused = false;
        g_farmHpPct = -1.f; g_farmSpotLife = -1.f; g_farmBlocked = false;
        g_farmSpot = 0; g_farmStreak = 0;
        resetNode(); s_settle = 0.f; s_lostTime = 0.f;
        return;
    }
    // Водим пальцами только когда ничто не мешает: аимбот владеет камерой,
    // пока ведёт игрока (фарм уступает полностью), открытое меню и калибровка
    // зон — тоже пауза. А вот цель читаем всегда: иначе строка статуса в окне
    // автофарма показывала бы «простой» ровно тогда, когда на неё смотрят
    // (само это окно и ставит бота на паузу).
    // Аимбот владеет камерой, пока ведёт игрока — и пальцем, и записью в память
    // (AimIsDriving, см. aim/controller.h). Без второй части автофарм продолжал
    // бы водить камеру своим «глазным» пальцем одновременно с мемори-аимом: со
    // стороны это выглядело как «в режиме «Мемори» аим использует палец»,
    // а по факту камеру тянули двое в разные стороны.
    const bool driving = !menuBlocked && !AimIsDriving() && g_calibMode == 0 && !g_buildPrompt;

    float sw = (float)native_window_screen_x;
    float sh = (float)native_window_screen_y;
    if (displayInfo.width > displayInfo.height && displayInfo.width >= 100 && displayInfo.height >= 100) {
        sw = (float)displayInfo.width;  sh = (float)displayInfo.height;
    } else if (displayInfo.height > displayInfo.width && displayInfo.height >= 100 && displayInfo.width >= 100) {
        sw = (float)displayInfo.height; sh = (float)displayInfo.width;
    }
    if (sw < 100.f || sh < 100.f) {
        releaseAll(); g_farmActive = false; return;
    }

    // Цель меряется относительно состояния камеры, которое публикует
    // esp_get_boxes(); убедимся, что снимок этого кадра есть, даже если ни
    // ESP-оверлей, ни аимбот его в этом кадре не запрашивали.
    FrameBoxes(sw, sh);

    FarmTarget tgt;
    const bool haveTgt = esp_farm_get_target(tgt) && tgt.valid;
    esp_farm_debug(g_farmNodes, g_farmReason);
    if (!haveTgt) {
        // Рабочая цель может исчезнуть на пару кадров (рескан реестра, сбой
        // чтения посреди апдейта) и тут же вернуться. Мгновенный сброс всего
        // давал топтание «стоп-шаг-стоп»: на короткое время просто замораживаем
        // ввод, а по-настоящему сбрасываемся, только если цель не вернулась.
        if (s_nodeId != 0 && s_lostTime < kLostHold) {
            s_lostTime += dt;
            if (s_tapDown) { Touch_Up_N(2); s_tapDown = false; } // вслепую не машем
            if (s_lookDown) { Touch_Up_N(1); s_lookDown = false; s_haveLast = false; }
            return; // палец движения оставляем, где был
        }
        releaseAll();
        g_farmActive = false; g_farmPhase = 0; g_farmPaused = false;
        g_farmHpPct = -1.f; g_farmSpotLife = -1.f; g_farmBlocked = false;
        g_farmSpot = 0; g_farmStreak = 0;
        resetNode(); s_settle = 0.f; s_lostTime = 0.f;
        return;
    }
    s_lostTime = 0.f;
    g_farmActive = true;
    g_farmPaused = !driving;
    g_farmTgtDist = tgt.aim_dist;
    g_farmReach = tgt.melee_reach;
    g_farmTgtKind = tgt.kind;
    g_farmSpot    = tgt.has_spot ? tgt.spot_source : (tgt.ext_found ? 0 : -1);
    g_farmStreak  = tgt.streak;
    g_farmHpPct   = (tgt.node_health >= 0.f && tgt.node_health_max > 0.f)
                  ? 100.f * tgt.node_health / tgt.node_health_max : -1.f;
    g_farmSpotLife = tgt.has_spot ? tgt.spot_life : -1.f;
    g_farmAttackPeriod = tgt.attack_period;
    g_farmXp = tgt.node_experience;
    g_farmBlocked = tgt.ray_blocked;
    {   // умения орудия: своё поле у FPTool, у узлов — своё требование
        int have = 0, need = 0;
        esp_farm_tool_info(have, need);
        g_farmToolHave = have ? have : tgt.tool_purposes;
        g_farmToolNeed = need;
    }

    // Есть ли у игры данные рейкаста прицела. FPMelee.ZkX берёт distance из
    // GKo (RaycastData/AimRaycast); пока обёртка пуста, удар не засчитывается
    // вовсе — играет один On_Woosh. Лог 14.09.2026: 43 из 48 замахов без урона
    // шли вообще без луча, а из 39 результативных луч был в 37.
    const bool noRay = tgt.at_spot && !tgt.ray_valid;
    s_noRayTime = noRay ? (s_noRayTime + dt) : 0.f;

    if (!driving) {
        // Меню открыто / камеру ведёт аимбот / идёт калибровка зон: статус
        // живой, ввод стоит. Таймеры подхода и добычи не крутятся, иначе бот
        // «застревал» и сдавался на узле за то время, пока пользователь
        // смотрел в настройки.
        releaseAll();
        g_farmPhase = 0;
        s_walkOffTime = 0.f;
        s_haveLast = false;
        return;
    }

    if (tgt.id == s_nodeId) {
        // Цель на месте — кандидат сбрасывается: считать надо кадры ПОДРЯД,
        // иначе редкие однокадровые мигания на один и тот же чужой узел
        // накопились бы и переключение всё равно случилось (проверено стендом:
        // на 3-м мигании s_candFrames дорастал до kNodeDebounce).
        s_candId = 0; s_candFrames = 0; s_flickerTime = 0.f;
    } else {
        // Дебаунс: один кадр на новом узле — это ещё не смена цели (рескан
        // реестра легко роняет рабочую цель на кадр). Держим прежний узел тем
        // же способом, что и при пропаже цели: ввод заморожен, ничего не
        // сбрасывается, иначе каждое мигание стоило бы 0.7 с паузы.
        if (tgt.id == s_candId) ++s_candFrames;
        else { s_candId = tgt.id; s_candFrames = 1; s_flickerTime = 0.f; }
        if (s_nodeId != 0 && s_candFrames < kNodeDebounce && s_flickerTime < kFlickerGiveUp) {
            s_flickerTime += dt;
            if (s_tapDown)  { Touch_Up_N(2); s_tapDown = false; }   // вслепую не машем
            if (s_lookDown) { Touch_Up_N(1); s_lookDown = false; s_haveLast = false; }
            return;   // палец движения оставляем, где был
        }
        s_candId = 0; s_candFrames = 0; s_flickerTime = 0.f;
        // Смена узла: поднять все пальцы и постоять. Иначе старые вводные
        // движения/камеры ещё несколько кадров проигрываются против новой цели
        // — та самая бешеная тряска сразу после того, как узел добыт.
        const bool hadNode = s_nodeId != 0;
        resetNode();
        s_nodeId = tgt.id;
        s_lastWalk = tgt.walk_dist;
        s_fracStart = tgt.fraction;
        s_healthStart = tgt.node_health;
        s_blockedTime = 0.f;
        if (hadNode) { releaseAll(); s_settle = kSettleTime; }
    }

    // Уже добытый узел уходит в чёрный список на месте, а не обходится кругами:
    // подборщик вернулся бы к нему, только когда в радиусе ничего нет, а танцы
    // вокруг пустого пня никому не нужны. С дебаунсом: fraction — сырое чтение,
    // и один мусорный кадр (значение посреди апдейта, сбой чтения) раньше
    // бросал наполовину срубленное дерево — считается только устойчивое «пусто».
    if (tgt.fraction >= 0.f && tgt.fraction < kDepletedFrac) {
        if (++s_depletedFrames >= kDepletedFrames) {
            esp_farm_blacklist(tgt.id, 120.f);
            releaseAll();
            resetNode();
            s_settle = kSettleTime;
            g_farmPhase = 0;
            return;
        }
    } else {
        s_depletedFrames = 0;
    }

    // Пауза между целями: пальцы подняты, камера стоит, следующая цель
    // начинается с чистого листа.
    if (s_settle > 0.f) {
        s_settle -= dt;
        releaseAll();
        return;
    }

    // Экранная метка ровно на той точке, по которой бот бьёт: зелёная — по
    // крестику, оранжевая — по корпусу (крестика ещё нет или он за стволом).
    // Заодно и диагностика: если метка не на том объекте, чинить надо подбор
    // цели, а не контроллер.
    if (tgt.on_screen) {
        auto* fg = ImGui::GetForegroundDrawList();
        const ImU32 mc = tgt.at_spot ? IM_COL32(80, 255, 120, 235) : IM_COL32(255, 200, 60, 230);
        const float r = tgt.at_spot ? 13.f : 17.f;
        fg->AddCircle({tgt.sx, tgt.sy}, r, mc, 24, 3.f);
        fg->AddLine({tgt.sx - r * 1.6f, tgt.sy}, {tgt.sx - r * 0.5f, tgt.sy}, mc, 3.f);
        fg->AddLine({tgt.sx + r * 0.5f, tgt.sy}, {tgt.sx + r * 1.6f, tgt.sy}, mc, 3.f);
        fg->AddLine({tgt.sx, tgt.sy - r * 1.6f}, {tgt.sx, tgt.sy - r * 0.5f}, mc, 3.f);
        fg->AddLine({tgt.sx, tgt.sy + r * 0.5f}, {tgt.sx, tgt.sy + r * 1.6f}, mc, 3.f);
        if (tgt.at_spot)
            fg->AddCircleFilled({tgt.sx, tgt.sy}, 2.5f, mc, 12);
    }

    bool gainFromSensFarm = false;
    const float gainScale = AimSensitivityScale(gainFromSensFarm);
    const float gainLo    = kGainLo * gainScale;
    const float gainHi    = kGainHi * gainScale;

    // ---- коэффициент камеры: такт с подтверждением ---------------------------
    // Учимся на накопленном сдвиге пальца между двумя ответами камеры, а не на
    // отношении одного кадра: поза камеры отстаёт от касания на 2 кадра, и
    // прежнее «dyaw последнего кадра / dx последнего кадра» давало то ноль, то
    // двойную норму. В логе 14.09 коэффициент из-за этого сбрасывался 571 раз за
    // 122 с и гулял 0.0083..1.97 град/px — при 0.0083 палец слал 120 px на градус
    // ошибки (рывок на 8° за кадр), при 1.97 не двигал камеру вовсе. Один и тот
    // же свайп 81 px измерялся как -2.39°, -13.21° и -17.02°.
    float camYaw = 0.f, camPitch = 0.f;
    const bool haveCam = esp_camera_angles(camYaw, camPitch);
    float camYawDelta = 0.f;
    bool camMoved = false;
    if (haveCam && s_haveLast) {
        camYawDelta = camYaw - s_lastCamYaw;
        while (camYawDelta > 180.f) camYawDelta -= 360.f;
        while (camYawDelta < -180.f) camYawDelta += 360.f;
        camMoved = fabsf(camYawDelta) > kCamMovedEps ||
                   fabsf(camPitch - s_lastCamPitch) > kCamMovedEps;
    }
    if (camMoved) {
        // Ходьба образец не портит: меряем поворот самой камеры, а джойстик её не
        // вращает (вращается только поза цели относительно камеры, а она в
        // отношение не входит). Знак требуем тот же, что у свайпа, — кроме самого
        // первого образца: отрицательный коэффициент означает инверсию оси в
        // настройках игры, и поймать её надо сразу. Переворот знака по слабому
        // отклику не принимаем: так в обучатель попало бы чужое движение камеры
        // (отдача, палец игрока).
        const bool sameSign = (camYawDelta * s_gainPendDx > 0.f);
        const bool strongPull = fabsf(camYawDelta) >= 0.5f;
        if (fabsf(s_gainPendDx) >= kGainMinPx && std::isfinite(camYawDelta) &&
            (sameSign || (s_gainYaw == 0.f && strongPull))) {
            const float measured = camYawDelta / s_gainPendDx;
            const float m = fabsf(measured);
            if (m >= gainLo && m <= gainHi) {
                // Медиана последних трёх образцов: одиночный мусорный кадр больше
                // не может развернуть палец в двадцать раз сильнее нужного.
                s_gainS3 = s_gainS2; s_gainS2 = s_gainS1; s_gainS1 = measured;
                if (s_gainN < 1000) ++s_gainN;
                float med;
                if (s_gainN <= 1) {
                    med = s_gainS1;
                } else if (s_gainN == 2) {
                    med = 0.5f * (s_gainS1 + s_gainS2);
                } else {
                    float a = s_gainS1, b = s_gainS2, c = s_gainS3;
                    if (a > b) std::swap(a, b);
                    if (b > c) std::swap(b, c);
                    if (a > b) std::swap(a, b);
                    med = b;
                }
                const float prev = s_gainYaw;
                const float next = (prev == 0.f) ? med : prev * 0.6f + med * 0.4f;
                s_gainYaw = next;
            }
        }
        s_gainPendDx = s_gainPendDy = 0.f;   // ответ получен — копим с нуля
    }
    if (haveCam) {
        s_lastCamYaw = camYaw; s_lastCamPitch = camPitch; s_haveLast = true;
    } else {
        s_haveLast = false;
        s_gainPendDx = s_gainPendDy = 0.f;   // поза пропала — накопление мусорное
        s_lookWait = 0.f;
    }
    // Полосу правдоподобия держим и на запасном значении: шаг пальца считается от
    // 1/gain, и вылет за полосу — это либо перелёт через цель, либо «камера не
    // слушается», то есть ровно то, что игрок видит как дёрганье. Знак сохраняем:
    // отрицательный коэффициент означает инверсию оси в настройках игры.
    float gain = (s_gainYaw != 0.f) ? s_gainYaw : (kAimGainAtRef * gainScale);
    if (!std::isfinite(gain) || gain == 0.f) gain = kAimGainAtRef * gainScale;
    const float gainMag = fabsf(gain);
    if (gainMag < gainLo)      gain = (gain < 0.f) ? -gainLo : gainLo;
    else if (gainMag > gainHi) gain = (gain < 0.f) ? -gainHi : gainHi;
    const float invGain = 1.f / gain;

    // ---- фаза ---------------------------------------------------------------
    // Дотянулись ли до точки прицела. По крестику порог заметно ближе, чем по
    // корпусу: удар по X засчитывается только в упор.
    const bool isTree = (tgt.kind == 0);
    const bool atSpot = tgt.at_spot;
    const bool wasMining = (g_farmPhase == 3);
    // meleeTight — дистанция, с которой удар реально достаёт точку прицела.
    // По крестику она же решает «достали ли» (с гистерезисом meleeHold, чтобы
    // не выпускать узел из-за полуметрового дрожания дистанции). По корпусу
    // фаза удара начинается раньше — узел большой, — НО стик продолжает
    // поджимать вперёд вплоть до meleeTight: иначе топор до коры не дотянется,
    // первый удар не пройдёт и крестик так и не появится.
    // Живая дальность орудия (из памяти игры) важнее эмпирических порогов:
    // это ровно то число, с которым игра сравнивает дистанцию удара.
    const bool liveReach = (tgt.melee_reach >= kReachLiveMin);
    // Меряем в той же метрике, в которой задан порог: с живой дальностью —
    // 3D-метры от глаза (как считает игра), без неё — прежние горизонтальные.
    // Исключение — tgt.ray_self: луч игры упёрся в сам узел раньше нашей точки
    // прицела (у камня прицел стоит на пивоте ВНУТРИ породы, log 14.09: aim_3d
    // 1.97..2.33 м при луче 0.51..0.77 м). Игра сравнивает с дальностью удара
    // именно distance своего луча (FPMelee.ZkX), поэтому и мы берём её: на
    // крупном камне aim_3d не влезал ни в один порог, и бот не доходил до фазы
    // удара даже с живым попаданием луча в породу. Для дерева ветка не
    // срабатывает: там прицел стоит на коре (ray_distance ≈ aim_3d), и
    // ray_self ставится только если старая формула вообще заподозрила стену.
    const bool raySelfCloser = tgt.ray_self && tgt.ray_valid &&
                               tgt.ray_distance > 0.f && tgt.ray_distance < tgt.aim_3d;
    const float distNow = liveReach ? (raySelfCloser ? tgt.ray_distance : tgt.aim_3d)
                                    : tgt.aim_dist;
    float meleeTight, meleeHold, meleeBody;
    if (liveReach) {
        meleeTight = tgt.melee_reach * kReachLiveTight;
        meleeHold  = tgt.melee_reach * kReachLiveHold;
        meleeBody  = meleeTight + kReachBodyAllowance;
    } else {
        meleeTight = isTree ? kReachTreeSpot : kReachOreSpot;
        meleeHold  = isTree ? kReachTreeHold : kReachOreHold;
        meleeBody  = isTree ? kReachTreeBody : kReachOreBody;
    }
    const float reachNow = atSpot ? (wasMining ? meleeHold : meleeTight) : meleeBody;
    const bool inReach = distNow <= reachNow;

    // Доводка камеры с гистерезисом: свайп начинается, когда ошибка явно
    // снаружи, и заканчивается глубоко внутри — иначе камера дёргается
    // влево-вправо вокруг центра.
    float startDeg, stopDeg;
    if (inReach) {
        startDeg = atSpot ? kAimSpotStart : kAimBodyStart;
        stopDeg  = atSpot ? kAimSpotStop  : kAimBodyStop;
    } else {
        startDeg = kAimWalkStart;
        stopDeg  = kAimWalkStop;
    }
    // Мёртвая зона по pitch в упор не нулевая: в логе 14.09 ошибка по pitch
    // держалась 0.4..0.7 град в 57% кадров доводки, палец не отпускал касание и
    // слал по 1..2 px за кадр — крестик заметно дрожал. Полградуса на 0.8 м это
    // 7 мм, в разы меньше засчитываемой зоны крестика (15 см).
    const float pitchDead = inReach ? kPitchDeadSpot : kPitchDeadWalk;
    const float pitchErr = fmaxf(fabsf(tgt.pitch) - pitchDead, 0.f);
    const float errDeg = fmaxf(fabsf(tgt.yaw), pitchErr);

    // В пределах удара — всегда MINE: прицел доводится тем же пальцем, а удар
    // удар запускается только когда «прицел сел». Вне пределов — идём, но если точка
    // подхода сильно в стороне, сначала доворачиваем камеру (стик «вперёд»
    // смотрит по камере, и на yaw 90° он уводил бота вдоль узла).
    const int phase = inReach ? 3 : ((fabsf(tgt.walk_yaw) <= kTurnBeforeWalk) ? 2 : 1);
    g_farmPhase = phase;

    // ---- палец 1: камера ------------------------------------------------------
    {
        const float wantYawPx = tgt.yaw * invGain;
        float wantPitchPx = 0.f;
        if (fabsf(tgt.pitch) > pitchDead) wantPitchPx = -tgt.pitch * fabsf(invGain);

        // Пропорциональный шаг: закрываем часть остатка ошибки (по крестику
        // больше — там точность важнее скорости). Предел шага по крестику урезан
        // с 0.075 до 0.030 высоты экрана: 81 px при измеренных 0.10 град/px это
        // рывок на 8° за кадр — ровно то, что выглядит как дёрганье прицела, даже
        // когда коэффициент верный.
        const bool fineStep = (phase == 3 && atSpot);
        const float maxStep = sh * (fineStep ? kStepSpotFrac : kStepWalkFrac);
        const float kAim = fineStep ? kAimStepSpot : kAimStepWalk;

        // Квант ввода: позиция пальца пишется в цифрователь целыми пикселями,
        // поэтому шаг меньше пикселя до камеры не доходит вовсе. Прежний
        // контроллер в таком случае держал касание опущенным и слал по 0.8 px за
        // кадр вечно (в логе стенда — 12 кадров подряд на ошибке 0.5°): палец не
        // отпускается, крестик дрожит. Ошибка в один квант считается севшей.
        const bool belowQuantum =
            fmaxf(fabsf(wantYawPx), fabsf(wantPitchPx)) * kAim < kStepQuantumPx;
        const bool needTurn = !belowQuantum &&
                              (s_lookDown ? (errDeg > stopDeg) : (errDeg > startDeg));
        // Ждём ли ещё ответа камеры на посланный шаг.
        const bool awaitingCam = haveCam && !camMoved && s_lookWait < kLookWaitMax &&
                                 (fabsf(s_gainPendDx) >= 1.f || fabsf(s_gainPendDy) >= 1.f);
        if (!needTurn) {
            if (s_lookDown) { Touch_Up_N(1); s_lookDown = false; }
            s_gainPendDx = s_gainPendDy = 0.f;
            s_lookWait = 0.f;
        } else if (!s_lookDown) {
            s_lookX = sw * 0.74f; s_lookY = sh * 0.42f;
            Touch_Down_N(1, s_lookX, s_lookY);
            s_lookDown = true;
            s_lookHold = 0;
            s_gainPendDx = s_gainPendDy = 0.f;
            s_lookWait = 0.f;
        } else if (s_lookHold < 1) {
            ++s_lookHold;               // даём игре зарегистрировать касание
            Touch_Down_N(1, s_lookX, s_lookY);
        } else if (awaitingCam) {
            // Такт с подтверждением: прошлый сдвиг ещё не отразился в позе камеры,
            // значит ошибка на экране устаревшая. Держим палец на месте и ждём —
            // иначе 9..11 шагов подряд складываются и камера перелетает крестик
            // (в логе 14.09 именно так: ответ через 2 кадра, досыл 9..11 кадров).
            s_lookWait += dt;
            Touch_Down_N(1, s_lookX, s_lookY);   // тач остаётся живым, сдвига нет
        } else {
            if (s_lookWait >= kLookWaitMax) {
                s_gainPendDx = s_gainPendDy = 0.f;  // ввод потерялся — шлём заново
                s_lookWait = 0.f;
            }
            float dx = wantYawPx * kAim;
            if (dx >  maxStep) dx =  maxStep;
            if (dx < -maxStep) dx = -maxStep;
            float dy = wantPitchPx * kAim;
            const float maxStepY = maxStep * 0.5f;
            if (dy >  maxStepY) dy =  maxStepY;
            if (dy < -maxStepY) dy = -maxStepY;
            const float nx = s_lookX + dx, ny = s_lookY + dy;
            // Край экрана: lift и перенос в центр, а не drag за границу.
            if (nx < sw * 0.56f || nx > sw * 0.97f || ny < sh * 0.12f || ny > sh * 0.88f) {
                // Палец дошёл до края: поднимаем и переносим в центр.
                Touch_Up_N(1); s_lookDown = false; s_haveLast = false;
                s_gainPendDx = s_gainPendDy = 0.f;
                s_lookWait = 0.f;
            } else {
                s_lookX = nx; s_lookY = ny;
                Touch_Down_N(1, s_lookX, s_lookY);
                s_gainPendDx += dx; s_gainPendDy += dy;
                s_lookWait = 0.f;
            }
        }
    }

    // ---- палец 0: джойстик движения -------------------------------------------
    {
        // В фазе удара всё ещё поджимаем вперёд, пока до точки прицела дальше
        // meleeTight: у тонких деревьев пивот в центре ствола, и остановка в
        // паре метров оставляла удар коротким. Гистерезис — жмём, пока дальше
        // +0.1..0.25 м, отпускаем только внутри (без хлопанья на границе,
        // которое выглядело как топтание на месте).
        const float pressAt = s_moveDown
            ? meleeTight
            : meleeTight + (liveReach ? 0.12f : (isTree ? 0.08f : 0.25f));
        bool wantWalk =
            (phase == 2 && tgt.walk_dist > kArriveWalk) ||
            (phase == 1 && fabsf(tgt.walk_yaw) < kCreepYaw && tgt.walk_dist > kCreepDist) ||
            (phase == 3 && distNow > pressAt) ||
            // Нет луча игры — прицел мимо меша: поджимаем вплотную, там ствол
            // перекрывает уход декали вбок и луч появляется (97% на 0.5 м).
            // Только пока ждём луч: после kNoRaySwingAfter поведение прежнее,
            // чтобы на сборке с нечитаемым GKo бот не тёрся о ствол вечно.
            (phase == 3 && noRay && s_noRayTime < kNoRaySwingAfter &&
             distNow > kNoRayCloseTo);
        if (s_evadeTime > 0.f) wantWalk = true;  // манёвр ведёт стик сам

        // Гистерезис отпускания: фаза мигает на кадр-другой вокруг порогов
        // (шум дистанции/угла), и каждое мигание раньше поднимало и снова
        // ставило палец движения — видимое «дёрганье джойстика» на подходе.
        // Теперь палец поднимается, только если ходьба не нужна четверть
        // секунды подряд; на удары это не влияет (палец 2 независим).
        if (wantWalk) s_walkOffTime = 0.f;
        else if (s_moveDown) {
            s_walkOffTime += dt;
            if (s_walkOffTime < kWalkOffHold) wantWalk = true; // держим, гасим дёрганье
        }

        if (wantWalk) {
            // Центр виртуального стика и толчок вперёд, чуть подруливающий к
            // цели, чтобы мелкую ошибку угла не приходилось править камерой.
            const float cx = (g_state.farm_joy_x >= 0.f) ? sw * g_state.farm_joy_x : sw * 0.165f;
            const float cy = (g_state.farm_joy_y >= 0.f) ? sh * g_state.farm_joy_y : sh * 0.70f;
            const float r  = sh * 0.16f;
            float px, py;
            if (s_evadeTime > 0.f) {
                // Обход препятствия: коротко назад, затем жёстко вбок и чуть
                // вперёд — скользим вокруг камня/стены, в которые упирается
                // прямолинейная ходьба.
                s_evadeTime -= dt;
                if (s_evadeTime > kEvadeTime - 0.6f) { px = cx; py = cy + r * 0.9f; }
                else                                 { px = cx + r * 0.95f * s_evadeDir; py = cy - r * 0.35f; }
                if (s_evadeTime <= 0.f) { s_evadeTime = 0.f; s_stuckTime = 0.f; s_lastWalk = 1e9f; }
            } else {
                // Мёртвая зона: пара градусов дрожания yaw не должна рулить
                // вовсе — знак у почти нулевой ошибки меняется каждый кадр, и
                // стик от этого хлопал влево-вправо.
                float steerYaw = tgt.walk_yaw;
                const float yawDead = (phase == 3) ? 3.f : ((tgt.walk_dist > 3.f) ? 10.f : 4.f);
                if (fabsf(steerYaw) < yawDead) steerYaw = 0.f;
                if (phase == 3) {
                    // Финальный подвод: мягко вперёд, руль пропорционален ошибке
                    // наведения (без скачков sign()).
                    float s3 = tgt.yaw / 45.f;
                    if (s3 >  1.f) s3 =  1.f;
                    if (s3 < -1.f) s3 = -1.f;
                    px = cx + r * 0.35f * s3;
                    py = cy - r * 0.75f;
                } else {
                    float steer = steerYaw / 70.f;
                    if (steer >  0.6f) steer =  0.6f;
                    if (steer < -0.6f) steer = -0.6f;
                    px = cx + r * steer;
                    py = cy - r * sqrtf(1.f - steer * steer);
                }
                if (s_walkOffTime > 0.f) { px = cx; py = cy; }  // доезжаем к центру
            }
            if (!s_moveDown) {
                Touch_Down_N(0, cx, cy);       // сначала ставим в центр стика
                s_moveDown = true;
                s_stickPx = cx; s_stickPy = cy;
            } else {
                // Гладим стик к нужному отклонению вместо телепорта: некоторые
                // устройства/сборки принимают резкий скачок за боковой флик, и
                // бота уносило с линии подхода.
                const float k = 1.f - expf(-14.f * dt);   // ~90% пути за 0.16 с
                s_stickPx += (px - s_stickPx) * k;
                s_stickPy += (py - s_stickPy) * k;
                Touch_Down_N(0, s_stickPx, s_stickPy);
            }
        } else if (s_moveDown) {
            Touch_Up_N(0); s_moveDown = false;
        }
    }

    // ---- перекрыт ли узел ------------------------------------------------------
    // Луч, которым игра сама проверяет удар (RaycastManager -> активности
    // PlayerEventHandler -> GKo.RaycastHit), упёрся заметно ближе нашей точки
    // прицела: между нами и крестиком камень, забор или склон. Удар в такую
    // точку уходит в перекрытие, так что не машем — отходим тем же манёвром,
    // что и при застревании, и только исчерпав попытки сдаём узел. Дебаунс
    // обязателен: на неуспевшем сесть прицеле луч смотрит мимо узла и даёт
    // короткое ложное «перекрыто».
    if (phase == 3 && tgt.ray_blocked) {
        s_blockedTime += dt;
        if (s_blockedTime > kBlockedTime && s_evadeTime <= 0.f) {
            s_blockedTime = 0.f;
            if (s_evadeCount < kEvadeMax) {
                s_evadeWhy = 2;
                s_evadeTime = kEvadeTime;
                s_evadeDir = (s_evadeCount % 2 == 0) ? 1.f : -1.f;
                ++s_evadeCount;
            } else {
                esp_farm_blacklist(tgt.id, 60.f);
                releaseAll();
                resetNode();
                s_settle = kSettleTime;
            }
        }
    } else {
        // Дебаунс в обе стороны. Таймер перекрытия не обнуляется мгновенно, а
        // тает вдвое медленнее, чем копится: мигание луча (прицел ещё едет, X
        // вот-вот появится) больше не сбрасывает подготовку обхода. Счётчик
        // обходов прощается только после kBlockedClear чистой линии удара —
        // иначе следующая перекрытая точка начнётся сразу с отказа от узла, но
        // и бесконечного круга, как раньше, тоже нет: бюджет kEvadeMax теперь
        // действительно заканчивается и узел уходит в чёрный список.
        s_blockedTime = fmaxf(0.f, s_blockedTime - dt * 2.f);
        if (s_evadeTime <= 0.f && s_evadeWhy == 2) {
            s_clearTime += dt;
            if (s_clearTime > kBlockedClear) { s_evadeWhy = 0; s_evadeCount = 0; s_clearTime = 0.f; }
        } else {
            s_clearTime = 0.f;
        }
    }

    // ---- палец 2: удары --------------------------------------------------------
    if (phase == 3) {
        s_mineTime += dt;
        // Держим удар, пока прицел ещё едет на точку: тап посреди свайпа
        // прилетает туда, где камера была, — это и есть «не попал по крестику».
        // По корпусу допуск свободный (узел огромный), по крестику — жёсткий.
        const bool aimSettled = atSpot
            ? (fabsf(tgt.yaw) <= kSwingSpotYaw && fabsf(tgt.pitch) <= kSwingSpotPitch)
            : (fabsf(tgt.yaw) <= kSwingBodyYaw);
        // Ритм ударов — из самого орудия: FPMelee.m_TimeBetweenAttacks +
        // pauseAfterAttack (у каждого инструмента свои, сериализованы в
        // префабе). Тап раньше срока игра ставит в очередь, и он вылетает уже
        // в уведённую камеру, поэтому держимся чуть медленнее кулдауна;
        // живого значения нет — остаёмся на подобранных миллисекундах.
        float tapDownMs = kTapDownMs, tapUpMs = kTapUpMs;
        if (tgt.attack_period > 0.05f && tgt.attack_period < 5.f) {
            const float periodMs = fminf(fmaxf(tgt.attack_period * 1020.f, 250.f), 2000.f);
            tapDownMs = fminf(fmaxf(periodMs * 0.10f, 40.f), 120.f);
            tapUpMs   = fmaxf(periodMs - tapDownMs, 60.f);
        }
        // Перекрытый узел не бьём: ждём, пока манёвр выведет на линию удара.
        // Так же держим тап, пока у игры вообще нет луча прицела: без данных
        // GKo FPMelee.ZkX не может засчитать удар и играет один On_Woosh —
        // ровно те «удары в воздух», которые видно со стороны. Ждём не вечно
        // (kNoRaySwingAfter), а стик всё это время поджимает ближе к стволу.
        const bool swingBlocked = tgt.ray_blocked || (noRay && s_noRayTime < kNoRaySwingAfter);
        s_tapTimer -= (int)roundf(dt * 1000.f);
        if (s_tapTimer <= 0 && (!aimSettled || swingBlocked) && !s_tapDown) {
            s_tapTimer = 0;              // ждём камеру/обход, не теряя такт
        } else if (s_tapTimer <= 0) {
            if (!s_tapDown) {
                // Кнопка огня: откалиброванная зона, иначе правая половина
                // экрана подальше от камерного пальца.
                const float fx = (g_state.farm_fire_x >= 0.f) ? sw * g_state.farm_fire_x : sw * 0.88f;
                const float fy = (g_state.farm_fire_y >= 0.f) ? sh * g_state.farm_fire_y : sh * 0.66f;
                Touch_Down_N(2, fx, fy);
                s_tapDown = true;
                s_tapTimer = (int)tapDownMs;
            } else {
                Touch_Up_N(2);
                s_tapDown = false;
                s_tapTimer = (int)tapUpMs;
            }
        }
    } else {
        if (s_tapDown) { Touch_Up_N(2); s_tapDown = false; }
        s_tapTimer = 0;
    }

    // ---- watchdog'и ------------------------------------------------------------
    if (phase == 1 || phase == 2) {
        s_mineTime = 0.f;
        // Нет продвижения к точке подхода — упёрлись в препятствие. Сначала
        // пробуем обойти (назад + вбок, чередуя стороны), и только когда
        // манёвры не помогают, узел уходит в чёрный список. Меряем именно
        // дистанцию до точки подхода: на последних метрах дистанция до самого
        // узла почти не меняется, и по ней watchdog срабатывал ложно.
        if (tgt.walk_dist < s_lastWalk - 0.25f) {
            s_lastWalk = tgt.walk_dist;
            s_stuckTime = 0.f;
        } else if (s_evadeTime <= 0.f) {
            s_stuckTime += dt;
            if (s_stuckTime > kStuckTime) {
                if (s_evadeCount < kEvadeMax) {
                    s_evadeWhy = 1;
                    s_evadeTime = kEvadeTime;
                    s_evadeDir = (s_evadeCount % 2 == 0) ? 1.f : -1.f;
                    ++s_evadeCount;
                    s_stuckTime = 0.f;
                } else {
                    esp_farm_blacklist(tgt.id, 30.f);
                    releaseAll();
                    resetNode();
                    s_settle = kSettleTime;
                }
            }
        }
    } else {
        // Дошли — препятствий на подходе больше нет, бюджет обходов обнуляем.
        // НО манёвр из-за ПЕРЕКРЫТОГО узла (s_evadeWhy == 2) не трогаем: он
        // начинается в этой же фазе, и обнуление здесь означало, что обход не
        // отрабатывал ни одного кадра — счётчик сбрасывался, обход запускался
        // заново и бот вечно танцевал на месте, не доходя до отказа от узла.
        // Контроль прогресса ниже при этом работает в обоих случаях.
        if (s_evadeWhy != 2) { s_evadeCount = 0; s_evadeTime = 0.f; s_evadeWhy = 0; }
        // Машем, а узел не убывает: стоим на волосок дальше нужного (финальный
        // подвод это лечит) или в руке не инструмент. Прогресс теперь видим по
        // двум независимым признакам — падение остатка И рост серии попаданий по
        // крестику (streak растёт только когда игра засчитала удар в X, так что
        // это самый честный сигнал, что фарм действительно работает).
        bool progress = false;
        // Здоровье узла — самый тонкий признак: m_CurrentHealth падает уже от
        // первого дошедшего удара, тогда как fractionRemaining сдвигается на
        // проценты лишь спустя десятки попаданий (а у некоторых узлов не
        // сдвигается вовсе, и watchdog сдавал живой узел).
        if (tgt.node_health >= 0.f) {
            if (s_healthStart < 0.f) s_healthStart = tgt.node_health;
            else if (tgt.node_health < s_healthStart - 0.01f) { s_healthStart = tgt.node_health; progress = true; }
        }
        if (tgt.fraction >= 0.f) {
            if (s_fracStart < 0.f) s_fracStart = tgt.fraction;
            else if (tgt.fraction < s_fracStart - 0.01f) { s_fracStart = tgt.fraction; progress = true; }
        }
        if (tgt.streak > s_bestStreak) { s_bestStreak = tgt.streak; progress = true; }
        if (progress) {
            s_mineTime = 0.f;
        } else {
            // Сдаёмся, только когда прогресс есть чем мерить (здоровье или
            // остаток ЧИТАЮТСЯ) и он явно не двигается долго. Когда не читается
            // ничего, прежний 14-секундный таймер бросал вполне живые узлы на
            // середине — «перестаёт добивать узел».
            const float giveUp = (tgt.node_health >= 0.f || tgt.fraction >= 0.f)
                               ? kGiveUpDrain : kGiveUpBlind;
            if (s_mineTime > giveUp) {
                esp_farm_blacklist(tgt.id, 60.f);
                releaseAll();
                resetNode();
                s_settle = kSettleTime;
            }
        }
    }
}
