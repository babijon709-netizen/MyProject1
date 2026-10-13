#include "farm.h"
#include "farm_joy.h"          // память-джойстик: движение без тача
#include "cfg.h"
#include "main.h"
#include "game.h"
#include "Android_touch/TouchHelperA.h"
#include "app_state.h"
#include "ui_util.h"
#include "aim.h"                 // g_aimActive, s_fingerDown (владелец камеры)
#include "esp_draw.h"            // FrameBoxes
#include "menu.h"                // g_calibMode
#include "process.h"             // g_esp_attached
#include <cmath>

// Главный принцип: КАМЕРА НАВОДИТСЯ РОВНО В КРЕСТИК, И УДАРЫ ИДУТ ПО НЕМУ.
// Крестик (hit-streak marker) читается напрямую из памяти игры: у руды это
// трансформ живого маркера OreHitstreaks.MoW, у дерева — локальный отрезок
// TreeHitstreaks.MTQ..MTu (серия засчитывается по дистанции от точки удара до
// маркера/отрезка с радиусом 15 см). Попадание в крестик даёт бонусный ресурс,
// поэтому бот ведёт взгляд (накопитель MouseLook) точно в крестик: центр экрана
// и есть точка удара штатного луча игры, так что удар ложится в X сам, а
// расхождение «точка удара -> крестик» остаётся в сантиметрах — заведомо внутри
// этих 15 см. Допуск наведения считаем от дистанции (kSpotHitM), а удары
// открываем только когда камера уже стоит в нём. Подмены удара нет: игра бьёт
// своим лучом по своему узлу.
//
// Прежний механизм (ПИН КРЕСТИКА: фоновый поток телепортировал маркер в точку
// падения удара, а камера держала центр корпуса) больше не используется —
// запись трансформа маркера на живой сборке перестала проходить. Код пина в
// game.cpp цел, но автофарм его не включает; наведение делает ту же работу без
// записи в маркеры. Пока крестика нет, камера ведёт ЦЕНТР КОРПУСА узла: узел
// большой, луч по нему попадает, а первое попадание и создаёт X.
//
// Крестик существует не всегда: игра создаёт его при первом же попадании по
// узлу и гасит через 15 секунд простоя — цикл всегда один и тот же: удар по
// корпусу -> появился X -> дальше камера ведёт X и все удары идут по нему.
//
// Удары отправляются штатным вводом и проходят все игровые проверки
// дистанции/кулдауна по настоящему лучу игры; game_viewmodel скрывает
// локальные позы инструмента на фазе MINE. Сетевую механику попадания
// не подменяем — двигается только маркер бонусной серии.
//
// Фазы кадра:
//   WALK  — идём к точке подхода С ПЕРВОГО ЖЕ кадра: стик давит прямо в
//           сторону цели относительно камеры (полный аналог, 360°), камера
//           доворачивается на узел параллельно, ходьба её не ждёт;
//   MINE  — камера ведёт крестик (до его появления — центр корпуса);
//   узел добыт / потерян / недостижим — берём следующий (чёрный список).
//
// Удар по посторонней геометрии (постройка!) запрещён на нескольких уровнях:
// тап уходит только когда луч игры подтверждённо стоит в САМОМ узле, ритм
// никогда не обгоняет кулдаун орудия (очередь тапов игры выстреливает в
// уведённую камеру), а мёртвый узел снимается с цели до того, как его
// адреса успеет занять чужой объект (см. проверку живости в game_farm.cpp).

bool g_farmActive = false;   // статус для строки в окне автофарма
int  g_farmPhase = 0;        // 0 простой, 2 подход, 3 удар (1 — поворот — больше не используется)
int  g_farmNodes = 0;        // узлов насчитал последний скан реестра
int  g_farmReason = 1;       // причина простоя из esp_farm_debug()
float g_farmTgtDist = 0.f;   // метры до точки прицела
float g_farmReach = 0.f;     // FPMelee.m_MaxReach + hitRadius орудия в руках, м
int  g_farmTgtKind = 0;      // ресурс текущей цели
int  g_farmSpot = 0;         // 0 — крестика нет, иначе его источник (1 руда, 2/3 дерево)
bool g_farmPaused = false;   // Target is visible, but aim or calibration owns input.
int  g_farmStreak = 0;       // hitstreakIndex: сколько подряд попали в крестик
float g_farmHpPct = -1.f;    // здоровье узла в процентах (-1 = не читается)
float g_farmSpotLife = -1.f; // сколько секунд осталось жить крестику (-1 = неизвестно)
float g_farmAttackPeriod = 0.f; // секунд между ударами орудия (0 = неизвестно)
int  g_farmToolHave = 0;     // флаги ToolPurpose орудия в руках (0 = неизвестно)
int  g_farmToolNeed = 0;     // какие умения запросили отброшенные узлы
int  g_farmXp = 0;           // сколько опыта даёт текущий узел (0 = неизвестно)
bool g_farmBlocked = false;  // узел перекрыт: луч игры упёрся ближе точки прицела



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
constexpr float kNoRaySwingAfter = 1.5f;   // с без луча — потом пробный замах
constexpr float kNoRayCloseTo    = 0.50f;  // м: докуда поджимаем, пока луча нет
                                           // (ровно тот aim3d, где луч жил в 97%)

// Допуск наведения камеры, градусы. Камера ведёт ту же точку, по которой бьёт:
// крестик, когда он есть (tgt.look_spot), иначе центр корпуса узла. Корпусу
// хватает широкого допуска — он большой; крестик это 15 см, которыми игра
// меряет серию, поэтому допуск по нему считаем от дистанции (kSpotHitM ниже).
constexpr float kAimBodyStart = 4.00f;
constexpr float kAimBodyStop  = 1.50f;
constexpr float kAimWalkStart = 6.00f;   // на подходе правим только заметный увод
constexpr float kAimWalkStop  = 2.50f;   // (камеру ведём сразу: стик больше не
                                         //  ждёт доворота, см. фазу WALK)
constexpr float kPitchDeadWalk = 18.0f;  // и только сильно задранную камеру

// Наведение на крестик: kSpotHitM метров промаха на текущей дистанции,
// переведённые в градусы. Игра засчитывает серию, если её луч попал не дальше
// 0.15 м от крестика, поэтому берём 0.09 м — с запасом на шум позы камеры
// (камера читается из памяти, «покачивание» look-root в расчёт не входит) и на
// смещение декали от коры. Ниже kSpotTolMin не опускаем: это уже мельче шага
// записи накопителя (kMemDeadDeg/kMemAckEps), доводиться бессмысленно; выше
// kSpotTolMax — «по площади», а не по крестику.
constexpr float kSpotHitM   = 0.09f;     // м: допуск промаха по крестику
constexpr float kSpotTolMin = 0.30f;     // град
constexpr float kSpotTolMax = 6.00f;     // град
// Сколько ждём доводку в допуск, прежде чем махнуть рукой на крестик: стоим в
// упор, а камера не садится (запись в накопитель не проходит). Тогда бьём по
// корпусу — урон по узлу идёт, серия просто не растёт; иначе watchdog сдал бы
// живой узел в чёрный список.
constexpr float kSpotStuckTime = 1.20f;  // с

// Замах по корпусу (крестика ещё нет) — только когда камера реально смотрит на
// узел: удар уходит по лучу игры, и тот должен попасть в узел. Допуск широкий:
// узел большой, а точку попадания задаёт сам луч.
constexpr float kSwingBodyYaw   = 8.00f;

constexpr float kArriveWalk     = 0.15f; // дошли до точки подхода
constexpr float kWalkOffHold    = 0.25f; // гистерезис отпускания джойстика, с

// Ритм ударов, когда ритм орудия не прочитался из памяти. Безопасный такт
// 600 мс: держим кнопку атаки в памяти ~60 мс и отпускаем. Прежний повод
// держаться медленнее кулдауна — очередь тапов, которую игра разгребала
// позже, уже в уведённую камеру, — с памятью пропал: преждевременный Try()
// игра просто отбрасывает. Такт оставлен как есть, чтобы не долбить ввод
// каждый кадр и не менять привычное поведение бота.
constexpr float kSwingHoldMs = 60.f;     // зажатие атаки (память),
constexpr float kSwingIdleMs = 540.f;    // пауза до следующего (600 мс цикл)

constexpr float kSettleTime    = 0.25f;  // пауза при смене узла (пальцы подняты)
constexpr float kLostHold      = 0.60f;  // цель может мигнуть на рескане реестра
// Смена узла тоже дебаунсится: новый обязан продержаться «лучшим» несколько
// кадров подряд. Лог 14.09.2026 — узел в 20 м перехватывал цель ровно на один
// кадр 9 раз за 150 с, и каждая смена в обе стороны стоила 0.7 с паузы
// (kSettleTime): 854 кадра простоя. Если цель так и прыгает туда-сюда дольше
// kFlickerGiveUp, переключаемся по-настоящему — иначе бот застыл бы навсегда.
constexpr int   kNodeDebounce  = 3;      // кадров подряд на новом узле
constexpr float kFlickerGiveUp = 1.0f;   // с прыжков — больше не держимся
constexpr float kDepletedFrac  = 0.03f;
constexpr int   kDepletedFrames = 10;    // дебаунс сырого чтения остатка
constexpr float kStuckTime     = 2.20f;  // нет продвижения к точке подхода
constexpr float kEvadeTime     = 1.70f;  // ~0.6 назад + ~1.1 вбок
constexpr int   kEvadeMax      = 4;
constexpr float kBlockedTime   = 0.30f;  // узел перекрыт дольше — пробуем обойти
// Сколько линия удара должна быть чистой подряд, чтобы простить счётчик обходов
// перекрытого узла. Мгновенное прощение (как было) означало бесконечный бюджет:
// «перекрыт» мигает кадр-другой, счётчик обнуляется, обход начинается заново с
// той же стороны — и бот вечно кружит вокруг узла (лог 14.09, камень 86a3c0:
// два «обход #1 (вправо)» подряд и ни одного удара).
constexpr float kBlockedClear  = 1.20f;
constexpr float kGiveUpDrain   = 12.f;   // бьём, а остаток не падает
constexpr float kGiveUpBlind   = 25.f;   // то же, но остаток не читается

// Камера ведётся через накопитель MouseLook (как мемори-аим): пишем абсолютный
// угол, игра разворачивает камеру в этом же кадре. Следующий шаг шлём только
// после того, как накопитель вернул записанное (игра приняла), либо истёк таймаут.
// Так шаги не складываются и камера не перелетает узел.
constexpr float kLookWaitMax   = 0.10f;  // с: дольше ответа не ждём — запись потеряна
constexpr float kMemAckEps     = 0.10f;  // град: допуск «накопитель = наша запись»
constexpr float kAimStepWalk   = 0.45f;  // доля остатка ошибки, закрываемая шагом
constexpr float kMemStepWalk   = 15.0f;  // град: предел одного шага на подходе
constexpr float kMemStepBody   = 4.0f;   // град: предел шага, когда узел в прицеле
constexpr float kMemDeadDeg    = 0.05f;  // град: мельче — не шлём
constexpr float kPitchDeadSpot = 0.50f;  // град: субградусный pitch в упор не доводим

} // namespace

static void UpdateFarmInner(float dt);

void UpdateFarm(float dt) {
    UpdateFarmInner(dt);
}

static void UpdateFarmInner(float dt) {
    static bool  s_moveDown = false;  // стик движения ведём (память, не тач)
    static bool  s_camTurn = false;   // камера ведётся памятью: идёт доводка
    static bool  s_swingOn  = false;  // кнопка атаки зажата памятью (удары)
    static float s_memWX = 0.f, s_memWY = 0.f; // последняя запись в накопитель
    static bool  s_memPend = false;   // запись ждёт подтверждения накопителем
    static int   s_swingTimer = 0;      // мс до следующего зажатия/отпускания удара
    static float s_memWait = 0.f;     // сколько ждём подтверждения записи, с
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
    static float s_joyX = 0.f, s_joyY = 0.f;         // сглаженный вектор стика
    static float s_noRayTime = 0.f;   // сколько подряд у игры нет луча прицела
    static int   s_blindSwings = 0;   // замахов вслепую (без луча) уже ушло
    static float s_blindHp = -1.f;    // здоровье узла перед последним слепым
    static unsigned long long s_candId = 0; // узел-кандидат на смену цели
    static int   s_candFrames = 0;    // сколько кадров подряд он лучший
    static float s_flickerTime = 0.f; // сколько держимся прежнего, с
    static float s_spotAimStuck = 0.f;// сколько не можем довестись в допуск по X

    auto releaseAll = [&]() {
        if (s_moveDown) { farmjoy::release(); s_moveDown = false; }
        s_camTurn = false; s_memPend = false;
        if (s_swingOn)  { farmjoy::attack(false); s_swingOn = false; }
        s_memWait = 0.f;
    };
    auto resetNode = [&]() {
        s_nodeId = 0;
        s_mineTime = 0.f; s_fracStart = -1.f; s_bestStreak = 0;
        s_healthStart = -1.f; s_blockedTime = 0.f; s_clearTime = 0.f;
        s_depletedFrames = 0;
        s_lastWalk = 1e9f; s_stuckTime = 0.f;
        s_evadeTime = 0.f; s_evadeCount = 0; s_evadeWhy = 0;
        s_walkOffTime = 0.f; s_noRayTime = 0.f;
        s_blindSwings = 0; s_blindHp = -1.f;
        s_candId = 0; s_candFrames = 0; s_flickerTime = 0.f;
        s_spotAimStuck = 0.f;
    };

    if (dt <= 0.f || !std::isfinite(dt)) dt = 1.f / 60.f;
    if (dt > 0.1f) dt = 0.1f;
    // Память-джойстик: кэш цели движения и его периодическая перепроверка.
    farmjoy::tick(dt);

    unsigned mask = 0;
    if (g_state.farm_on) {
        if (g_state.farm_wood)   mask |= 1u;
        if (g_state.farm_stone)  mask |= 2u;
        if (g_state.farm_metal)  mask |= 4u;
        if (g_state.farm_sulfur) mask |= 8u;
    }
    esp_farm_set_resources(mask);
    esp_farm_set_range(g_state.farm_range);

    if (mask == 0 || !g_esp_attached) {
        releaseAll();
        g_farmActive = false; g_farmPhase = 0; g_farmPaused = false;
        g_farmHpPct = -1.f; g_farmSpotLife = -1.f; g_farmBlocked = false;
        g_farmSpot = 0; g_farmStreak = 0;
        resetNode(); s_settle = 0.f; s_lostTime = 0.f;
        return;
    }
    // Aim owns the camera while tracking a player, and calibration owns the
    // physical touch used to select a control zone. Menu and popover windows
    // do not pause farming, so opening its status/settings cannot stop it.
    const bool driving = !s_fingerDown && !g_aimActive && g_calibMode == 0;

    float sw = (float)native_window_screen_x;
    float sh = (float)native_window_screen_y;
    if (displayInfo.width > displayInfo.height && displayInfo.width >= 100 && displayInfo.height >= 100) {
        sw = (float)displayInfo.width;  sh = (float)displayInfo.height;
    } else if (displayInfo.height > displayInfo.width && displayInfo.height >= 100 && displayInfo.width >= 100) {
        sw = (float)displayInfo.height; sh = (float)displayInfo.width;
    }
    if (sw < 100.f || sh < 100.f) {
        g_farmActive = false;
        releaseAll(); return;
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
            if (s_swingOn) { farmjoy::attack(false); s_swingOn = false; } // вслепую не машем
            s_camTurn = false; s_memPend = false; s_memWait = 0.f;
            return; // палец движения оставляем, где был
        }
        g_farmActive = false;
        releaseAll();
        g_farmPhase = 0; g_farmPaused = false;
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
    // шли вообще без луча, а из 39 результативных луч был в 37. Отсутствие
    // луча не привязано к крестику: по корпусу узла луч нужен ровно так же.
    const bool noRay = !tgt.ray_valid;
    s_noRayTime = noRay ? (s_noRayTime + dt) : 0.f;

    if (!driving) {
        // Меню открыто / камеру ведёт аимбот / идёт калибровка зон: статус
        // живой, ввод стоит. Таймеры подхода и добычи не крутятся, иначе бот
        // «застревал» и сдавался на узле за то время, пока пользователь
        // смотрел в настройки.
        releaseAll();
        g_farmPhase = 0;
        s_walkOffTime = 0.f;
        s_camTurn = false; s_memPend = false; s_memWait = 0.f;
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
            if (s_swingOn)  { farmjoy::attack(false); s_swingOn = false; }   // вслепую не машем
            s_camTurn = false; s_memPend = false; s_memWait = 0.f;
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
        const ImVec4& fc = tgt.at_spot ? cfg::esp::farm_spot_col : cfg::esp::farm_body_col;
        const ImU32 mc = IM_COL32((int)(fc.x*255),(int)(fc.y*255),(int)(fc.z*255), tgt.at_spot ? 235 : 230);
        const float r = tgt.at_spot ? 13.f : 17.f;
        fg->AddCircle({tgt.sx, tgt.sy}, r, mc, 24, 3.f);
        fg->AddLine({tgt.sx - r * 1.6f, tgt.sy}, {tgt.sx - r * 0.5f, tgt.sy}, mc, 3.f);
        fg->AddLine({tgt.sx + r * 0.5f, tgt.sy}, {tgt.sx + r * 1.6f, tgt.sy}, mc, 3.f);
        fg->AddLine({tgt.sx, tgt.sy - r * 1.6f}, {tgt.sx, tgt.sy - r * 0.5f}, mc, 3.f);
        fg->AddLine({tgt.sx, tgt.sy + r * 0.5f}, {tgt.sx, tgt.sy + r * 1.6f}, mc, 3.f);
        if (tgt.at_spot)
            fg->AddCircleFilled({tgt.sx, tgt.sy}, 2.5f, mc, 12);
    }

    // ---- камера: накопитель MouseLook, без тача -------------------------------
    // Знаки те же, что у мемори-аима: yaw +вправо -> накопитель Y, pitch +вверх ->
    // накопитель X с минусом (вверх — меньше). Накопитель уже в градусах, коэффициента
    // чувствительности не нужно: абсолютная запись сама задаёт поворот.
    float accX = 0.f, accY = 0.f;
    const bool haveAcc = esp_mem_aim_read_angles(accX, accY);
    // Запись подтверждена, если накопитель вернул ровно то, что мы писали.
    const bool acked = s_memPend && haveAcc &&
                       fabsf(WrapDeg180(accY - s_memWY)) < kMemAckEps &&
                       fabsf(WrapDeg180(accX - s_memWX)) < kMemAckEps;

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

    // Допуск наведения по крестику: сколько градусов промаха даёт kSpotHitM
    // метров на текущем удалении. Меряем от глаза (aim_3d) — угловая ошибка
    // превращается в промах именно через эту дистанцию.
    const bool lookSpot = tgt.look_spot;
    float spotTol = kSpotTolMax;
    {
        const float eye = (std::isfinite(tgt.aim_3d) && tgt.aim_3d > 0.05f)
                        ? tgt.aim_3d : tgt.aim_dist;
        if (std::isfinite(eye) && eye > 0.05f)
            spotTol = atan2f(kSpotHitM, eye) * 57.29577951f;
        spotTol = fminf(fmaxf(spotTol, kSpotTolMin), kSpotTolMax);
    }
    // Доводка камеры с гистерезисом: доводить начинаем, когда ошибка явно
    // снаружи, и прекращаем глубоко внутри — иначе камера дёргается
    // влево-вправо вокруг центра. По крестику обе границы жёсткие: промах
    // больше spotTol — это уже мимо X (серия не растёт).
    float startDeg, stopDeg;
    if (lookSpot) {
        stopDeg  = spotTol;
        startDeg = fmaxf(spotTol * 1.5f, stopDeg + 0.25f);
    } else if (inReach) {
        startDeg = kAimBodyStart;
        stopDeg  = kAimBodyStop;
    } else {
        startDeg = kAimWalkStart;
        stopDeg  = kAimWalkStop;
    }
    // Мёртвая зона по pitch: по крестику она равна допуску (иначе субградусная
    // ошибка по X остаётся недоведённой и удар уходит на сантиметры мимо), по
    // корпусу в упор половина градуса, а на подходе правим только сильно
    // задранную камеру.
    const float pitchDead = lookSpot ? spotTol
                                     : (inReach ? kPitchDeadSpot : kPitchDeadWalk);
    const float steerYaw = tgt.yaw;
    const float steerPitch = tgt.pitch;
    const float pitchErr = fmaxf(fabsf(steerPitch) - pitchDead, 0.f);
    const float errDeg = fmaxf(fabsf(steerYaw), pitchErr);

    // В пределах удара — всегда MINE: камера доводится записью в накопитель
    // прямо в крестик, и удар уходит по нему штатным лучом игры.
    // Вне пределов — идём. Отдельной
    // фазы поворота больше нет: стик давит прямо в сторону цели относительно
    // камеры (полный аналог, 360°), так что шаг к узлу начинается с первого же
    // кадра, а камера доворачивается параллельно — ходьба её не ждёт.
    const int phase = inReach ? 3 : 2;
    g_farmPhase = phase;

    // Сел ли прицел на ТОЧКУ УДАРА. По крестику ждём и yaw, и pitch внутри
    // допуска: иначе удар ляжет мимо X и серия не вырастет (ради неё всё и
    // делается). По корпусу допуск широкий — узел большой, а первое попадание
    // лишь создаёт крестик.
    const bool aimOnSpot = fabsf(tgt.yaw) <= spotTol && fabsf(tgt.pitch) <= spotTol;
    if (phase == 3 && lookSpot && !aimOnSpot) s_spotAimStuck += dt;
    else s_spotAimStuck = 0.f;
    // Страховка: довестись не удаётся долго — бьём по корпусу (урон пройдёт,
    // серия не вырастет), но узел в работе.
    const bool aimSettled = lookSpot
        ? (aimOnSpot || (s_spotAimStuck > kSpotStuckTime &&
                         fabsf(tgt.yaw) <= kSwingBodyYaw))
        : (fabsf(tgt.yaw) <= kSwingBodyYaw);

    // ---- камера: пишем накопитель MouseLook ----------------------------------
    {
        const float maxStep = inReach ? kMemStepBody : kMemStepWalk;
        const float wantYaw   = steerYaw;
        const float wantPitch = (fabsf(steerPitch) > pitchDead) ? steerPitch : 0.f;
        const bool needTurn = haveAcc && (s_camTurn ? (errDeg > stopDeg) : (errDeg > startDeg));

        if (!needTurn) {
            s_camTurn = false; s_memPend = false; s_memWait = 0.f;
        } else {
            s_camTurn = true;
            if (s_memPend && !acked && s_memWait < kLookWaitMax) {
                s_memWait += dt;                  // ответа ждём, второй шаг не шлём
            } else {
                float offYaw   = wantYaw   * kAimStepWalk;
                float offPitch = wantPitch * kAimStepWalk;
                offYaw   = fminf(fmaxf(offYaw,   -maxStep), maxStep);
                offPitch = fminf(fmaxf(offPitch, -maxStep), maxStep);
                if (!std::isfinite(offYaw))   offYaw = 0.f;
                if (!std::isfinite(offPitch)) offPitch = 0.f;
                if (fabsf(offYaw) < kMemDeadDeg && fabsf(offPitch) < kMemDeadDeg) {
                    s_memPend = false; s_memWait = 0.f;
                } else {
                    const float writeY = WrapDeg180(accY + offYaw);
                    const float writeX = WrapDeg180(accX - offPitch);
                    if (esp_mem_aim_write_angles(writeX, writeY)) {
                        s_memWX = writeX; s_memWY = writeY;
                        s_memPend = true; s_memWait = 0.f;
                    } else {
                        s_memPend = false; s_memWait = 0.f;
                    }
                }
            }
        }
    }

    // Обратная связь памяти-джойстика: пока кандидат ведёт стик, дистанция до
    // точки подхода обязана сокращаться — по этому признаку модуль и выбирает
    // рабочий слой ввода (см. farm_joy.h). «Чистой» ходьбой считаем только
    // фазу подхода с курсом вперёд: на подводе (фаза 3) стик жмёт в упор, там
    // сближение меряется иначе, а манёвр обхода ведёт стик сам.
    {
        const bool walkClean = (phase == 2) && (s_evadeTime <= 0.f) &&
                               (fabsf(tgt.walk_yaw) <= 60.f) && (tgt.walk_dist > 0.f);
        farmjoy::observe(tgt.walk_dist, walkClean);
    }

    // ---- стик: движение через ПАМЯТЬ игры, без тача ----------------------------
    // Вектор стика (x — вправо, y — вперёд, магнитуда 0..1) уходит в память
    // игры (farm_joy.cpp: UI-стик / ввод хендлера / канал MovementInput),
    // синтетический тач по зоне джойстика больше не используется вовсе.
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
            // Направление на цель задаёт сам вектор стика, камера лишь доводит
            // прицел до точки удара.
            float wantX = 0.f, wantY = 0.f;
            if (s_evadeTime > 0.f) {
                // Обход препятствия: коротко назад, затем жёстко вбок и чуть
                // вперёд — скользим вокруг камня/стены, в которые упирается
                // прямолинейная ходьба.
                s_evadeTime -= dt;
                if (s_evadeTime > kEvadeTime - 0.6f) { wantX = 0.f;                  wantY = -0.90f; }
                else                                 { wantX = 0.95f * s_evadeDir;   wantY =  0.35f; }
                if (s_evadeTime <= 0.f) { s_evadeTime = 0.f; s_stuckTime = 0.f; s_lastWalk = 1e9f; }
            } else {
                // Мёртвая зона: пара градусов дрожания yaw не должна рулить
                // вовсе — знак у почти нулевой ошибки меняется каждый кадр, и
                // стик от этого хлопал влево-вправо.
                float steerYaw = tgt.walk_yaw;
                const float yawDead = (phase == 3) ? 3.f : ((tgt.walk_dist > 3.f) ? 8.f : 3.f);
                if (fabsf(steerYaw) < yawDead) steerYaw = 0.f;
                if (phase == 3) {
                    // Финальный подвод: мягко вперёд, руль пропорционален ошибке
                    // наведения (без скачков sign()).
                    float s3 = tgt.yaw / 45.f;
                    if (s3 >  1.f) s3 =  1.f;
                    if (s3 < -1.f) s3 = -1.f;
                    wantX = 0.35f * s3;
                    wantY = 0.75f;
                } else {
                    // Полный аналог, 360°: стик давит ПРЯМО в сторону точки
                    // подхода относительно камеры — идём к цели с первого же
                    // кадра, не стоя на месте, пока камера доворачивается.
                    // Прежняя схема («вперёд + руль ±0.6», а при |yaw| > 72°
                    // сперва доворот камеры) стоила секунды на каждый узел:
                    // при цели сбоку/сзади бот топтался или уходил по дуге.
                    // Магнитуда полная: край стика игра сама мапит в бег, а
                    // камера тем временем сводит yaw к нулю, и направление
                    // само становится «вперёд» (самый быстрый ход).
                    const float a = steerYaw * 0.017453293f;
                    wantX = sinf(a);
                    wantY = cosf(a);
                }
                if (s_walkOffTime > 0.f) { wantX = 0.f; wantY = 0.f; }  // доезжаем к центру
            }
            // Гладим ТОЛЬКО направление, а к краю стик встаёт с первого кадра.
            // Раньше сглаживался весь вектор, и при смене курса (камера доводит
            // прицел, вектор стика едет за ней) его магнитуда висела на 0.9
            // вместо 1.0 — а магнитуда стика это доля скорости хода: на 0.9
            // персонаж идёт медленнее полного шага. Бег включается не
            // отклонением, а кнопкой бега (её ведёт отдельно farmjoy::sprint),
            // поэтому отклонение держим полным. Угол по-прежнему ведём 26/с
            // (90% пути за ~90 мс), так что «дёрганья» руля нет.
            const float wantLen = sqrtf(wantX * wantX + wantY * wantY);
            if (wantLen > 1e-4f) {
                float ax = wantX / wantLen, ay = wantY / wantLen;
                const float curLen = sqrtf(s_joyX * s_joyX + s_joyY * s_joyY);
                if (curLen > 1e-3f) {
                    const float k = 1.f - expf(-26.f * dt);
                    const float cx = s_joyX / curLen + (ax - s_joyX / curLen) * k;
                    const float cy = s_joyY / curLen + (ay - s_joyY / curLen) * k;
                    const float cl = sqrtf(cx * cx + cy * cy);
                    if (cl > 1e-4f) { ax = cx / cl; ay = cy / cl; }
                }
                const float mag = (wantLen > 1.f) ? 1.f : wantLen;
                s_joyX = ax * mag;
                s_joyY = ay * mag;
            } else {
                s_joyX = 0.f;
                s_joyY = 0.f;
            }
            farmjoy::command(s_joyX, s_joyY);
            // Бег. Игра включает его только по кнопке бега (InputAction
            // LD+0x48), а не по величине стика: без взведённой кнопки бот
            // идёт пешком при любой оттяжке (см. farm_joy.h). Модуль сам
            // снимет кнопку в release().
            farmjoy::sprint(true);
            s_moveDown = true;
        } else if (s_moveDown) {
            farmjoy::release();
            s_joyX = 0.f; s_joyY = 0.f;
            s_moveDown = false;
        }
    }

    // ---- перекрыт ли узел ------------------------------------------------------
    // Луч, которым игра сама проверяет удар (RaycastManager -> активности
    // PlayerEventHandler -> GKo.RaycastHit), упёрся в ЧУЖУЮ геометрию:
    // заметно ближе нашей точки прицела (ray_blocked — камень, забор, склон)
    // или в посторонний объект на линии удара вовсе без учёта дистанции
    // (ray_foreign — то самое «упёрся в постройку носом»). Удар в такую линию
    // ломает чужое, так что не машем — отходим манёвром и, если не помогает,
    // сдаём узел. Учитывается только при севшем прицеле: пока камера едет к
    // узлу, луч цепляет случайные объекты и «перекрыто» выходит ложным.
    // Наведение тут не помогает: луч игры всё равно упирается в чужую
    // геометрию, так что ждём манёвра.
    if (phase == 3 && aimSettled &&
        (tgt.ray_blocked || tgt.ray_foreign)) {
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

    // ---- удары (память, без тача) ----------------------------------------------
    if (phase == 3) {
        s_mineTime += dt;
        // Удар — память, а не палец: держим байт «атака зажата» у хендлера
        // ввода (farmjoy::attack(true)), и игра в каждом кадре сама зовёт
        // AttackContinuously.Try() -> FPManager.OnTry_Attack(true) ->
        // оружие.TryAttackContinuously(). Бьёт оно так часто, как позволяет
        // его же кулдаун (FPMelee.m_NextUseTime), поэтому ритм ниже — только
        // скважность удержания: короткое зажатие и пауза. Такт взят из
        // орудия (m_TimeBetweenAttacks + pauseAfterAttack), живого значения
        // нет — безопасные 600 мс.
        float holdMs = kSwingHoldMs, idleMs = kSwingIdleMs;
        if (tgt.attack_period > 0.05f && tgt.attack_period < 5.f) {
            const float periodMs = fminf(fmaxf(tgt.attack_period * 1020.f, 250.f), 2000.f);
            holdMs = fminf(fmaxf(periodMs * 0.10f, 40.f), 120.f);
            idleMs = fmaxf(periodMs - holdMs, 60.f);
        }
        // Узел при смерти: новый замах не начинаем. Последний удар мог уже
        // убить узел — его GameObject вот-вот освободится, и чтение крестика
        // (экстеншен/маркер) начнёт отдавать чужую память (аллокатор отдаёт
        // адреса тут же, вплоть до объектов постройки). Дебаунс остатка
        // (kDepletedFrames) сменит цель сам; наша задача — не махать вслепую
        // по мёртвому узлу и не считать его крестик своим.
        const bool nodeDying =
            (tgt.node_health >= 0.f && tgt.node_health_max > 0.f &&
             tgt.node_health <= 0.02f * tgt.node_health_max);
        // Можно ли бить по лучу игры:
        //   ray_self — можно: игра сама подтверждает, что линия удара стоит
        //     в САМОМ узле (поддерево трансформов / коллайдер крестика);
        //   ray_foreign (луч в посторонний объект) — нельзя НИКОГДА, на
        //     какой бы дистанции он ни стоял: удар уйдёт в чужую геометрию.
        //     Прежняя проверка смотрела только «чужое ближе точки прицела на
        //     0.6 м» — упёршись в постройку носом, бот продолжал махать;
        //   луча нет вовсе — после kNoRaySwingAfter разрешаем ОДИН пробный
        //     замах (вдруг на этой сборке обёртка GKo не читается у нас, а у
        //     игры она жива), и дальше только пока слепые замахи дают прогресс:
        //     здоровье узла падает. Не падает — прекращаем: они уходят
        //     неизвестно куда, в том числе в постройку.
        const bool blindSwing =
            noRay && atSpot && s_noRayTime >= kNoRaySwingAfter &&
            (s_blindSwings == 0 ||
             (tgt.node_health >= 0.f && tgt.node_health < s_blindHp - 0.01f));
        const bool swingByRay = tgt.ray_self || blindSwing;
        const bool swingBlocked = !swingByRay || tgt.ray_blocked;
        s_swingTimer -= (int)roundf(dt * 1000.f);
        if (s_swingTimer <= 0 && (!aimSettled || swingBlocked || nodeDying) && !s_swingOn) {
            s_swingTimer = 0;              // ждём камеру/обход/узел, не теряя такт
        } else if (s_swingTimer <= 0) {
            if (!s_swingOn) {
                // Зажимаем кнопку атаки в памяти хендлера ввода — тот же путь,
                // которым идёт зажатая кнопка атаки на экране. Экрана и зоны
                // огня для удара больше не нужно.
                farmjoy::attack(true);
                s_swingOn = true;
                s_swingTimer = (int)holdMs;
                if (!tgt.ray_valid) {      // замах вслепую — под контроль
                    s_blindHp = tgt.node_health;
                    ++s_blindSwings;
                } else {
                    s_blindSwings = 0;     // луч жив — слепой режим кончился
                }
            } else {
                farmjoy::attack(false);
                s_swingOn = false;
                s_swingTimer = (int)idleMs;
            }
        }
    } else {
        if (s_swingOn) { farmjoy::attack(false); s_swingOn = false; }
        s_swingTimer = 0;
        s_blindSwings = 0;    // фаза ушла — счётчик слепых замахов обнуляем
        s_blindHp = -1.f;
    }

    // ---- watchdog'и ------------------------------------------------------------
    if (phase == 2) {
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

