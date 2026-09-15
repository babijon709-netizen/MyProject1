#pragma once
#include <sys/types.h>
#include <vector>

// Skeleton ESP: bone slots and the lines connecting them.
constexpr int ESP_BONE_COUNT      = 22;

struct EspBox {
    unsigned long long id;   // stable per-player identity (PlayerManager*), for target stickiness
    float x1, y1, x2, y2;
    float distance;
    float corners[8][2];
    bool  corner_visible[8];

    // Player name / held weapon (UTF-8, may be empty). Filled from the
    // nicklabel (UI.Text) and FPManager current-weapon chains.
    bool  has_name = false;
    char  name[32] = {};
    bool  has_weapon = false;
    char  weapon[48] = {}; // localized, UTF-8 (Russian names are wider)

    // Team / clan. `ally` is true when this player shares the local player's
    // team name or clan id; `tag` is the clan tag, shown next to the name.
    bool  ally = false;
    bool  has_tag = false;
    char  tag[16] = {};

    // Skeleton (screen-space bone positions), filled when skeleton ESP is enabled.
    bool  has_skeleton;
    bool  bone_valid[ESP_BONE_COUNT];
    float bones[ESP_BONE_COUNT][2];

    // Aim targets (screen-space), always filled when the bone transforms are
    // resolvable — independent of whether skeleton ESP drawing is enabled.
    //   aim_pts[0] = head (skull centre), [1] = neck, [2] = chest (upper spine).
    // aim_valid[i] == false means the corresponding bone could not be read and
    // the caller should fall back to the box estimate.
    bool  aim_valid[3];
    // Where the aim points came from: 0 none, 1 rig bones (exact),
    // 2 KCC head transform (crouch-aware), 3 feet + pose height (estimate).
    int   aim_source;
    bool  crouched;
    float aim_pts[3][2];
    // Angular offset of each aim point from the camera forward axis, in
    // degrees (yaw = +right, pitch = +up). Only meaningful when aim_valid[i].
    float aim_yaw[3];
    float aim_pitch[3];
};

// World markers: ore nodes and animals, drawn as a small labelled pill at the
// object's screen position. Both come from the same networked source
// (Oxide.MineableObject in Mirror's client registry), so one struct covers them.
enum EspMarkerKind { ESP_MARKER_ORE = 0, ESP_MARKER_ANIMAL = 1, ESP_MARKER_LOOT = 2, ESP_MARKER_PICKUP = 3 };
struct EspMarker {
    float x = 0.0F, y = 0.0F;   // screen position (top-centre of the pill)
    float distance = 0.0F;      // metres from the local player
    int   kind = ESP_MARKER_ORE;
    // Ore markers carry their own colour (one per resource); when has_color is
    // false the caller picks the colour for that kind.
    bool  has_color = false;
    // Elite crates: the overlay cycles the label colour through the spectrum.
    bool  rainbow = false;
    unsigned char color_rgb[3] = {255, 255, 255};
    char  name[40] = {};        // localized label (UTF-8), may carry a stack size
};

bool        esp_init(pid_t pid);
void        esp_reset();
void        esp_set_skeleton_enabled(bool enabled);
// Enable the ore / animal / loot / pickup marker scan (all off = no work).
void        esp_set_markers_enabled(bool ore, bool animals, bool loot, bool pickups);
// «Всегда день»: время суток удерживается в полдне (клиентский визуал).
void        esp_set_always_day(bool enabled);
// Markers further away than this (metres) are dropped. Keeps the screen clean
// on open terrain, where the registry easily holds hundreds of nodes.
void        esp_set_marker_max_distance(float metres);
// Markers for the current frame, nearest first. Call after esp_get_boxes():
// it reuses the camera/projection state that call established.
std::vector<EspMarker> esp_get_markers();
// Resolve bones (for aim points) even when skeleton ESP drawing is off.
void        esp_set_aim_bones_enabled(bool enabled);
std::vector<EspBox> esp_get_boxes(int screen_width, int screen_height);

// Players near the local player as of the last esp_get_boxes() call, in all
// directions (360 degrees) — NOT limited to the ones visible on screen.
// Feeds the enemy-counter pill in the overlay.
int         esp_nearby_player_count();

// True while the local player is aiming down sights (ADS) with the current
// weapon. Returns false when the state cannot be read (not attached, no
// weapon, menus, etc.), so "aim only while scoped" fails closed.
bool        esp_local_player_is_aiming();

// ---- Автофарм ---------------------------------------------------------------
// Слой памяти только находит узел и говорит, куда смотреть и куда идти; сам
// подход и удары делает контроллер на синтетических тачах в main.cpp.
//
// «Крестик» (hit-streak marker) — объект, который игра сама создаёт на ресурсе
// после первого попадания и уничтожает через 15 секунд простоя. Его координаты
// читаются напрямую из экстеншенов узла (OreHitstreaks.MoW / TreeHitstreaks.MTQ,
// см. game_offsets.h) — именно до них игра меряет дистанцию удара, поэтому
// попадание в крестик засчитывается в серию и даёт бонусный ресурс.
struct FarmTarget {
    bool  valid = false;
    unsigned long long id = 0;    // NetworkIdentity (для чёрного списка)
    int   kind = 0;               // 0 дерево, 1 камень, 2 металл, 3 сера
    float fraction = -1.f;        // остаток ресурса 0..1, -1 = не читается

    // Точка прицела: крестик, пока он жив и с нашей стороны; иначе корпус узла
    // (первый же удар по корпусу создаёт крестик — так задумано игрой).
    float yaw = 0.f, pitch = 0.f; // градусы от оси выстрела (+вправо, +вверх)
    float aim_dist = 0.f;         // метры по горизонтали до точки прицела
    float aim_3d = 0.f;           // 3D-метры от глаза/оси выстрела до точки прицела
                                  // (ровно то, что игра сравнивает с дальностью удара)
    bool  at_spot = false;        // точка прицела — крестик (не корпус)

    // Дальность удара текущего орудия, прочитанная из игры: FPMelee.m_MaxReach +
    // FPMelee.hitRadius — те самые 3D-метры от камеры, внутри которых
    // FPMelee.ZkX засчитывает удар (снаружи играет On_Woosh). 0 = неизвестно
    // (в руках не ближнее орудие либо чтение не удалось) — тогда контроллер
    // работает по своим эмпирическим порогам.
    float melee_reach = 0.f;
    float melee_ray = 0.f;        // RaycastManager.m_RayLength — длина луча взаимодействия

    // Состояние крестика.
    bool  ext_found = false;      // найден ли сам экстеншен крестика на узле
    bool  has_spot = false;       // X прочитан из памяти игры
    bool  spot_front = false;     // X с нашей стороны узла (иначе он за стволом)
    int   spot_source = 0;        // 1 маркер руды, 2 точка дерева MTQ, 3 декаль дерева
    int   streak = 0;             // hitstreakIndex / MTz: сколько подряд попали в X

    // Куда идти: точка подхода перед крестиком (или перед узлом, если X нет).
    float walk_yaw = 0.f;         // градусы от оси камеры (+вправо)
    float walk_dist = 0.f;        // метры до точки подхода
    float node_dist = 0.f;        // метры по горизонтали до самого узла

    // Орудие в руках и состояние узла — значения из самой игры (дамп 62a8534:
    // FPTool.m_ToolPurposes, FPMelee.m_TimeBetweenAttacks/pauseAfterAttack,
    // MineableObject.m_CurrentHealth/m_MaxHealth/m_Experience).
    int   tool_purposes = 0;      // флаги ToolPurpose (1 дерево, 2 камень, 4 животные); 0 = неизвестно
    float attack_period = 0.f;    // секунд между ударами этого орудия; 0 = неизвестно
    float node_health = -1.f;     // текущее здоровье цели
    float node_health_max = -1.f; // его же максимум (для процентов)
    int   node_experience = 0;    // сколько опыта даёт узел

    // Луч прицела: результат тех же лучей RaycastManager, из которых игра берёт
    // distance для засчёта удара (Gum.RaycastData/AimRaycast -> GKo.RaycastHit).
    // ray_blocked — луч упёрся заметно раньше нашей точки прицела, то есть узел
    // перекрыт и удар уйдёт в перекрытие: махать в такой ситуации впустую.
    bool  ray_valid = false;
    float ray_distance = 0.f;
    bool  ray_blocked = false;
    // Луч прицела упёрся в САМ узел (его коллайдер/GameObject), просто раньше
    // точки прицела: у камня прицел стоит внутри породы. Это не перекрытие —
    // игра засчитывает удар по distance этого же луча, поэтому ray_blocked при
    // ray_self снимается. Флаг наружу отдаём, чтобы контроллер писал в лог,
    // когда именно сработала эта ветка (иначе её не отличить от «луча нет»).
    bool  ray_self = false;
    // Какая проверка признала попадание «своим»: 0 — не признала (или проверка не
    // запускалась), 1 — коллайдер крестика руды (OREHS_COLLIDER), 2 — GameObject
    // узла совпал с GameObject попадания, 3 — попадание внутрь поддерева деталей
    // узла, 4 — руда без крестика, у которой луч остановился в пределах
    // дальности удара (страховка: пивот руды внутри породы, прицел по нему луч
    // не подтверждает, а удар по такому лучу игра засчитывает). Нужна в логе:
    // по ней видно, работает ли послабление на устройстве.
    int   ray_self_why = 0;
    // Якорь точки удара руды (MineableObject.LXX, 0xC8): мировая позиция и
    // дистанция от неё до точки попадания луча. Пишется только для руды и только
    // на кадрах, где луч выглядел перекрытым, — это замер «стоит ли якорь на
    // поверхности камня» (0.0x м) или это мусор (десятки метров).
    bool  ore_anchor_valid = false;
    float ore_anchor_x = 0.f, ore_anchor_y = 0.f, ore_anchor_z = 0.f;
    float ore_anchor_to_ray = -1.f;
    // Куда луч игры упёрся на самом деле (RaycastHit.m_Point) и нормаль
    // поверхности там (m_Normal). ray_point_valid = false, когда координаты не
    // прочитались. По расхождению с точкой прицела видно, стоит ли прицел на
    // коллайдере: у крестика на дереве декаль сдвинута на 0.25 м от коры, и
    // прицел «в воздух» рядом со стволом оставляет луч без попадания — удар
    // при этом не засчитывается вовсе (FPMelee.ZkX играет On_Woosh).
    bool  ray_point_valid = false;
    float ray_px = 0.f, ray_py = 0.f, ray_pz = 0.f;
    float ray_nx = 0.f, ray_ny = 0.f, ray_nz = 0.f;

    // Сколько секунд осталось жить крестику (руда — из 15, дерево — из своего
    // lifetime). -1 = неизвестно; 0 = вот-вот потухнет.
    float spot_life = -1.f;

    // Проекция точки прицела на экран — для метки «куда бьёт бот».
    bool  on_screen = false;
    float sx = 0.f, sy = 0.f;

    // Мировые координаты точки прицела и пивота узла. В лог автофарма они
    // идут только в строках EV, но без них геометрию удара не разобрать:
    // насколько прицел отстоит от ствола, попал ли луч игры в ту же точку.
    float aim_x = 0.f, aim_y = 0.f, aim_z = 0.f;
    float node_x = 0.f, node_y = 0.f, node_z = 0.f;
};
// Профиль одного вызова esp_farm_get_target: где именно ушёл кадр автофарма и
// сколько syscall'ов чтения/записи память игры на это потребовала. Нужен для
// разбора лага «фарм съел кадр»: сама стадия в логе видна, а её состав — нет.
// total_ms включает всё, что делает вызов (в т.ч. участки без собственного
// таймера: чёрный список, позиции узлов); суммы участков к нему не обязаны
// сходиться в точности — подтаймеры нужны, чтобы найти виновника, а не свести
// баланс.
struct FarmTargetDiag {
    float total_ms = 0.f;    // весь вызов
    float scan_ms  = 0.f;    // шаг скана реестра (farm_scan_tick)
    float reach_ms = 0.f;    // чтение орудия в руках (read_local_melee_reach)
    float loop_ms  = 0.f;    // перебор узлов и выбор лучшего
    float spot_ms  = 0.f;    // экстеншен крестика + его точка (farm_read_spot)
    int   reads    = 0;      // syscall'ов чтения за вызов
    double read_ms = 0.0;    // сколько в них просуммировано
    // Состояние скана: идёт ли проход сейчас, сколько в нём всего записей и
    // сколько осталось, сколько условных единиц бюджета истрачено за кадр и
    // сколько записей пришлось классифицировать всерьёз (не по кешу).
    int   scan_running = 0;
    int   scan_total   = 0;
    int   scan_left    = 0;
    int   scan_units   = 0;
    int   scan_new     = 0;
    int   entities     = 0;  // узлов в рабочем списке
    int   neg_cache    = 0;  // записей в отрицательном кеше
    int   blacklisted  = 0;  // узлов в чёрном списке
    int   spot_source  = 0;  // источник крестика в этом кадре (0 = нет)
};

// Диагностика обращений к памяти игры: сколько syscall'ов и сколько мс в них
// ушло с момента последнего сброса. Сбрасывается раз в кадр хозяином цикла.
void        esp_io_meter_reset();
void        esp_io_meter(int& read_calls, double& read_ms, int& write_calls, double& write_ms);
void        esp_farm_target_diag(FarmTargetDiag& out);
// Какие ресурсы добывать: bit0 дерево, bit1 камень, bit2 металл, bit3 сера.
// 0 выключает скан целиком (никакой лишней работы в кадре).
void        esp_farm_set_resources(unsigned mask);
// Радиус поиска узлов в метрах (зажимается в 10..300).
void        esp_farm_set_range(float meters);
// Ближайший подходящий узел по состоянию последнего esp_get_boxes() (нужна
// его камера).
bool        esp_farm_get_target(FarmTarget& out);
// Сдаться на узле (недостижим / застряли) на `seconds`.
void        esp_farm_blacklist(unsigned long long id, float seconds);
// Почему последний esp_farm_get_target() ничего не вернул + сколько узлов
// насчитал последний скан реестра. Причины: 0 ок, 1 фарм выключен,
// 2 кадр не опубликован (нет камеры/позиции), 3 в реестре нет подходящих
// узлов, 4 узлы есть, но все вне радиуса / в чёрном списке, 5 поза камеры
// не читается (не посчитать углы).
void        esp_farm_debug(int& nodes_cached, int& idle_reason);
// Что умеет орудие в руках и какие умения запросили отброшенные узлы (флаги
// ToolPurpose). need != 0 при reason == 6 — «ближайший узел нечем взять».
void        esp_farm_tool_info(int& purposes_have, int& purposes_need);
// Сырые поля сегмента крестика у дерева (SPOT_A = точка на коре, SPOT_B = конец
// сегмента) и причина, по которой SPOT_A не стал точкой прицела:
// 0 — принят, 1 — не конечен, 2 — не прошёл проверку «точка на узле»,
// 3 — не читали вовсе (экстеншен не тот / крестика нет). len = |A-B|, -1 если
// не считалась. Нужны в логе автофарма: по ним видно, пусты ли поля в памяти
// или значения есть, но их отвергает проверка.
void        esp_farm_spot_raw(float& ax, float& ay, float& az, float& bx, float& by, float& bz,
                              int& why, float& len);
// X-ray: камера не рисует всё ближе `meters` (запись near clip plane).
// 0 выключает и восстанавливает исходное значение. Диапазон 0..50 м.
void        esp_set_xray(float meters);

// Vertical field of view (degrees) of the game camera as last read by
// esp_get_boxes(). 0 if unknown.
float       esp_camera_fov_deg();

// Absolute camera orientation (degrees; yaw around world up, pitch +up) as of
// the last esp_get_boxes(). Returns false if the camera pose is unknown.
// Источники для аимбота — поза камеры и ось выстрела (как было в сборке без
// правок автофарма): по этим углам аимбот учит чувствительность и ждёт ответа
// игры на свой шаг, а базис из матрицы вида отстаёт на кадр и ломает это
// ожидание. Автофарм использует esp_camera_angles_farm().
bool        esp_camera_angles(float& yaw_deg, float& pitch_deg);
// Те же углы, но с третьим источником — базисом из матрицы вида кадра. Нужен
// автофарму на устройствах, где поза камеры не читается вовсе; отстаёт на кадр.
bool        esp_camera_angles_farm(float& yaw_deg, float& pitch_deg);
// Diagnostic: bit 0 camera pose known, bit 1 pose derived from the view
// matrix, bit 2 firing reference in use. See esp_camera_state() in game.cpp.
int         esp_camera_state();

// Позиция глаза локального игрока в мире — та же точка, от которой меряются
// углы прицела (PlayerEventHandler.LookDirection из KCC, а если её нет — поза
// камеры). Логу автофарма нужна, чтобы сравнивать команды стика с тем, куда
// персонаж пошёл на самом деле. false = поза в этом кадре не прочитана.
bool        esp_local_eye_position(float& x, float& y, float& z);
