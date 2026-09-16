// farm_target.cpp — Автофарм: выбор узла и точка удара.
//
// Модуль разрезан из прежнего монолита jni/src/game.cpp;
// что здесь лежит и кто это зовёт — в шапке farm_target.h и в docs/CODE_MAP.md.

#include "esp/common.h"
#include "esp/camera.h"
#include "esp/farm_scan.h"
#include "esp/frame.h"
#include "esp/game_patch.h"
#include "esp/markers.h"
#include "esp/math.h"
#include "esp/melee.h"
#include "esp/mem.h"
#include "esp/names.h"
#include "esp/skeleton_names.h"
#include "farm_target.h"

// Мировая точка крестика и откуда она взялась (для диагностики в меню).
// source: 1 — трансформ маркера руды, 2 — точка на коре дерева (MTQ, либо
// ближайшая к глазу точка отрезка MTQ..MTu — именно до отрезка игра меряет
// попадание), 3 — трансформ декаля дерева (запасной путь; декаль смещён от
// коры на 0.25 м по нормали, поэтому он второй).
// life_left — сколько секунд крестику осталось жить (0 = уже потух, -1 = не
// прочиталось).
// Сколько секунд жизни осталось у крестика: у руды возраст (lHG) сравнивается
// с 15.0 прямо в OreHitstreaksMarker.Update, у дерева — свой lifetime, который
// HitMarkerItem.Update набирает в lzr. Мусорное значение отдаёт -1 («не
// знаю»), и тогда вызывающий код ведёт себя как раньше.
// Возвращает остаток жизни в секундах; 0.0F — ровно «потух» (вызывающий код
// сравнивает с нулём), -1.0F — «не прочиталось» (тогда верим полю маркера, как
// раньше, чтобы сбой чтения не лишал бота крестика).
static float farm_marker_life_left(uint64_t marker, uint64_t age_offset, float lifetime) {
    float age = 0.0F;
    if (!rd_exact(marker + age_offset, age) || !std::isfinite(age) || age < 0.0F) return -1.0F;
    if (!(lifetime > 0.0F)) return -1.0F;
    const float left = lifetime - age;
    return left > 0.0F ? left : 0.0F;
}

// Диагностика крестика для лога автофарма: что реально лежит в полях сегмента
// дерева (SPOT_A = точка на коре, SPOT_B = конец сегмента) и почему точка на
// коре не стала точкой прицела. В логе 14.09.2026 источник был 3 (декаль) во
// всех 10887 строках и ни разу 2 (кора) — без сырых значений не отличить «поля
// пусты» от «значения есть, но проверка их отвергла».
// Сглаженный радиус ствола на высоте крестика (см. постановку прицела на кору
// в esp_farm_get_target). Один узел за раз, поэтому хватает одной ячейки.
static unsigned long long s_bark_id = 0;

static float              s_bark_r  = -1.0F;

static Vec3  g_farmSpotRawA{}, g_farmSpotRawB{};

static int   g_farmSpotRawWhy = 3;      // 0 A принят, 1 не конечен, 2 вне узла, 3 не читали

static float g_farmSpotRawLen = -1.0F;  // |A-B| в метрах, -1 = не считалось

void esp_farm_spot_raw(float& ax, float& ay, float& az, float& bx, float& by, float& bz,
                       int& why, float& len) {
    ax = g_farmSpotRawA.x; ay = g_farmSpotRawA.y; az = g_farmSpotRawA.z;
    bx = g_farmSpotRawB.x; by = g_farmSpotRawB.y; bz = g_farmSpotRawB.z;
    why = g_farmSpotRawWhy; len = g_farmSpotRawLen;
}

static bool farm_read_spot(const FarmEntity& entity, Vec3& out, int& source, int& streak, float& life_left) {
    out = {}; source = 0; streak = 0; life_left = -1.0F;
    g_farmSpotRawA = {}; g_farmSpotRawB = {}; g_farmSpotRawWhy = 3; g_farmSpotRawLen = -1.0F;
    if (!valid_obj(entity.ext)) return false;

    if (entity.ext_kind == FARM_EXT_ORE) {
        // MoW == null означает «крестика сейчас нет» — игра сама обнуляет поле,
        // когда X потух (giq: Destroy маркера + str xzr,[x19,#0x30]).
        const uint64_t marker = rd_ptr(entity.ext + OREHS_MARKER);
        if (!valid_obj(marker)) return false;
        // Обнуление поля и гашение GameObject происходят не одним тактом, а
        // отсчёт своих 15 секунд маркер ведёт сам (Update: lHG += dt; lHG > 15
        // -> SetActive(false) + вызов владельца). Пока поле ещё заполнено, но
        // время вышло, прицел стоит на невидимой точке и удары уходят в никуда.
        const float life = farm_marker_life_left(marker, OREMARK_AGE, OREMARK_LIFETIME);
        if (life == 0.0F) return false;
        const uint64_t transform = native_component_transform(managed_object_native(marker));
        if (!transform || !marker_world_position(transform, out)) return false;
        if (!farm_spot_on_node(out, entity.pos, entity.kind)) return false;
        const int32_t s = rd<int32_t>(entity.ext + OREHS_STREAK_INDEX);
        streak = (s >= 0 && s < 10000) ? s : 0;
        life_left = life;
        source = 1;
        return true;
    }

    if (entity.ext_kind == FARM_EXT_TREE) {
        // Все проверки попадания у дерева начинаются с MTn != null — без живого
        // клона серия не засчитывается, так что это и есть признак «X есть».
        const uint64_t marker = rd_ptr(entity.ext + TREEHS_MARKER);
        if (!valid_obj(marker)) return false;
        // HitMarkerItem живёт свой lifetime, а по истечении уходит в пул
        // (Update: lzr += dt; lzr > lifetime -> вызов владельца lzT), где его
        // переиспользуют для другого дерева. Возраст поэтому проверяем до того,
        // как поверить в координаты.
        float lifetime = 0.0F;
        if (!rd_exact(marker + HITMARK_LIFETIME, lifetime) || !std::isfinite(lifetime) || lifetime < 0.0F)
            lifetime = 0.0F;
        const float life = farm_marker_life_left(marker, HITMARK_AGE, lifetime);
        if (life == 0.0F) return false;
        const int32_t s = rd<int32_t>(entity.ext + TREEHS_STREAK);
        streak = (s >= 0 && s < 10000) ? s : 0;

        // MTQ/MTu (0x88/0xA4) — ЛОКАЛЬНЫЕ координаты дерева, а не мировые:
        // в логе 14.09 у всех 6 крестиков A = (0, h, 0) — точка на ОСИ ствола
        // на высоте X, — а B отстоит от неё на радиус ствола (0.40 м у дерева
        // с корой 0.354 м, 0.04 м у тонкого, у которого кора 0.037 м). Отношение
        // к мировым размерам одинаковое для обоих (0.885 и 0.93) — это масштаб
        // дерева. Мировой точкой A поэтому быть не может (узел в 1200 м от
        // начала координат), и прежняя ветка «прицел = A» была мертва: она
        // срабатывала только у деревьев рядом с (0,0,0) и давала там мусор.
        // Сами значения нужны — из них берётся радиус ствола на высоте X
        // (см. постановку прицела на кору в esp_farm_get_target).
        const Vec3 a = rd_v3(entity.ext + TREEHS_SPOT_A);
        const Vec3 b = rd_v3(entity.ext + TREEHS_SPOT_B);
        g_farmSpotRawA = a;
        g_farmSpotRawB = b;
        if (vec3_is_finite(a) && vec3_is_finite(b)) {
            const float lx = b.x - a.x, ly = b.y - a.y, lz = b.z - a.z;
            g_farmSpotRawLen = sqrtf(lx * lx + ly * ly + lz * lz);
        }
        g_farmSpotRawWhy = !vec3_is_finite(a) ? 1
                         : !farm_spot_on_node(a, entity.pos, entity.kind) ? 2 : 0;
        // Мировая точка X берётся из трансформа декали (единственный источник
        // мировых координат крестика), а локальный отрезок A..B идёт на радиус
        // ствола: декаль стоит не на коре, поэтому прицел отдельно ставится на
        // кору — см. «Точка крестика на коре» в esp_farm_get_target.
        // Проверка попадания у дерева меряет дистанцию до ОТРЕЗКА MTQ..MTu
        // радиусом 0.15 м: прицел на коре в направлении декали попадает ровно
        // в конец B этого отрезка, то есть в середину допуска.
        const uint64_t mark = rd_ptr(marker + HITMARK_MARK);
        uint64_t transform = valid_obj(mark) ? managed_object_native(mark) : 0;
        if (!transform) transform = native_component_transform(managed_object_native(marker));
        if (!transform || !marker_world_position(transform, out)) return false;
        if (!farm_spot_on_node(out, entity.pos, entity.kind)) return false;
        life_left = life;
        source = 3;
        return true;
    }
    return false;
}

void esp_farm_debug(int& nodes_cached, int& idle_reason) {
    nodes_cached = (int)g_farm_entities.size();
    idle_reason = g_farm_idle_reason;
}

void esp_farm_tool_info(int& purposes_have, int& purposes_need) {
    purposes_have = g_farm_tool_have;
    purposes_need = g_farm_tool_need;
}

// Упёрся ли луч прицела в САМ узел добычи, а не в перекрытие.
//
// Зачем: у камня точка прицела — пивот узла с зажимом по высоте, то есть точка
// ВНУТРИ породы. Луч игры останавливается на ближней поверхности камня гораздо
// раньше неё (лог 14.09, узел 86a3c0: ray 0.51..0.77 м при aim_3d 1.97..2.33 м
// на всех 1595 кадрах окна), поэтому прежняя формула «луч дошёл заметно раньше
// точки прицела => перекрыт» принимала за стену собственный камень. Дальше
// контроллер честно отрабатывал перекрытие: «перекрыт — обхожу», круги вокруг
// узла, ноль тапов и ноль крестиков. Дерево при этом работало: там поправка на
// кору ставит прицел НА поверхность, ray_distance ≈ aim_3d и «перекрыт» не
// возникало — на деревья эта проверка не влияет.
//
// Удар по своему же камню игра засчитывает: FPMelee.ZkX сравнивает с
// m_MaxReach + hitRadius distance ЭТОГО луча (0.5..0.8 м при дальности 1.5 м),
// а не нашу дистанцию до точки прицела. После первого удара OreHitstreaks.giu
// ставит X через Collider.ClosestPoint — уже на поверхности, и дальше прицел
// стоит на крестике, как у дерева.
//
// Своим считается попадание по любому из двух признаков:
//   1) m_Collider луча — коллайдер экстеншена руды (OREHS_COLLIDER: тот самый
//      Collider, которым игра ставит X). Сначала сравниваем managed-указатели
//      напрямую, потом их нативные объекты — обёртка может быть другой;
//   2) GameObject попадания — GameObject узла. У узла в кеше transform уже
//      нативный, у попадания managed, поэтому сравниваем нативные.
// Вызывается только когда старая формула сказала «перекрыт», так что лишние
// чтения (3..5) достаются лишь редким кадрам с реальным подозрением на стену.
// Трансформ, о который остановился луч: у попадания есть GameObject (managed,
// читаем его native), а у коллайдера — прямой путь до Transform. Коллайдер
// точнее (это ровно та геометрия, в которую попал луч), GameObject — запасной
// путь, когда m_Collider пуст.
static uint64_t ray_hit_transform(const MeleeReach& reach) {
    if (reach.ray_collider) {
        const uint64_t col = managed_object_native(reach.ray_collider);
        if (col) {
            const uint64_t t = native_component_transform(col);
            if (t) return t;
        }
    }
    const uint64_t go = managed_object_native(reach.ray_hit_go);
    if (!go) return 0;
    const uint64_t pairs = rd_ptr(go + GAMEOBJECT_COMPONENT_ARRAY);
    if (!pairs) return 0;
    const uint64_t t = rd_ptr(pairs + COMPONENT_PAIR_PTR);
    if (!t || rd_ptr(t + COMPONENT_GAMEOBJECT) != go) return 0;
    return t;
}

// Лежит ли попадание луча ВНУТРИ поддерева узла.
//
// Проверка «GameObject попадания == GameObject узла» слишком строга: у камня и
// дерева меш с коллайдером висит на дочернем GameObject (LOD-модели, обломки,
// «корка»), и корневой GO узла с ним не совпадает НИКОГДА. Лог 15.09.2026 это и
// показал: у руды луч упирался в собственную породу (0.5 м при точке прицела
// 2.3 м), покидал узел как «перекрытый» — четыре обхода, чёрный список, ноль
// ударов. Поэтому сравниваем с ПОДДЕРЕВОМ трансформа узла: попадание в любую его
// деталь — это попадание в сам узел.
//
// Обход стоит несколько чтений на узел поддерева, поэтому вердикт кешируется по
// паре (узел, трансформ попадания): луч стоит в одной и той же детали десятки
// кадров подряд, а обход делается один раз.
static bool ray_hit_in_node_subtree(const MeleeReach& reach, const FarmEntity& node,
                                    uint64_t& hit_transform_out, int& nodes_walked) {
    hit_transform_out = 0;
    nodes_walked = 0;
    if (!node.transform) return false;
    const uint64_t hit_transform = ray_hit_transform(reach);
    if (!hit_transform) return false;
    hit_transform_out = hit_transform;
    if (hit_transform == node.transform) return true;

    static unsigned long long s_node_id = 0;
    static uint64_t s_hit_transform = 0;
    static bool     s_verdict = false;
    if (s_node_id == node.identity && s_hit_transform == hit_transform) return s_verdict;

    static std::vector<uint64_t> s_subtree;
    collect_transform_subtree(node.transform, s_subtree, 128);
    nodes_walked = (int)s_subtree.size();
    bool verdict = false;
    for (uint64_t t : s_subtree) {
        if (t == hit_transform) { verdict = true; break; }
    }
    s_node_id = node.identity;
    s_hit_transform = hit_transform;
    s_verdict = verdict;
    return verdict;
}

static bool ray_hit_is_self_node(const MeleeReach& reach, const FarmEntity& node, int& why) {
    why = 0;
    if (!reach.ray_valid || !reach.ray_hit_go) return false;
    // 1. Самая дешёвая проверка: коллайдер попадания — тот самый, которым игра
    //    ставит крестик руды (OREHS_COLLIDER, lzR). Поле заполняется в gir, то
    //    есть уже ПОСЛЕ первого попадания (и живёт, пока жив крестик).
    if (reach.ray_collider && node.ext && node.ext_kind == FARM_EXT_ORE) {
        const uint64_t col = rd_ptr(node.ext + OREHS_COLLIDER);
        if (col && (col == reach.ray_collider ||
                    managed_object_native(col) == managed_object_native(reach.ray_collider))) {
            why = 1;
            return true;
        }
    }
    if (!node.transform) return false;
    // 2. Коллайдер (или сам узел) висит на корневом GameObject узла.
    const uint64_t node_go = rd_ptr(node.transform + COMPONENT_GAMEOBJECT);
    if (!node_go) return false;
    if (managed_object_native(reach.ray_hit_go) == node_go) {
        why = 2;
        return true;
    }
    // 3. Меш с коллайдером — деталь узла: попадание внутрь его поддерева.
    uint64_t hit_transform = 0;
    int nodes_walked = 0;
    if (ray_hit_in_node_subtree(reach, node, hit_transform, nodes_walked)) {
        why = 3;
        return true;
    }
    return false;
}

static bool ray_hit_is_self_node(const MeleeReach& reach, const FarmEntity& node) {
    int why = 0;
    return ray_hit_is_self_node(reach, node, why);
}

// Сырая прикидка «где на узле сидит точка попадания» из самого узла, без
// крестика: MineableObject.LXX (0xC8) — Transform, который игра использует как
// якорь точки удара (ZgL() отдаёт его мировую позицию, а сам он живёт в
// hitInfo). Если это действительно якорь поверхности, он даёт точку прицела для
// руды ещё ДО первого удара — тогда «перекрытие собственным камнем» исчезает
// само, без всяких послаблений. Пока это только замер для лога: пишем позицию
// якоря и дистанцию до точки попадания луча, чтобы на устройстве увидеть,
// совпадают ли они (0.0x м = якорь на поверхности, десятки метров = мусор).
static bool farm_ore_anchor_point(const FarmEntity& node, Vec3& out, float& dist_to_ray, int& valid) {
    valid = 0;
    dist_to_ray = -1.0F;
    if (node.kind == 0 || !valid_obj(node.component)) return false;
    const uint64_t anchor = rd_ptr(node.component + MINEABLE_HIT_ANCHOR);
    if (!valid_obj(anchor)) return false;
    const uint64_t transform = native_component_transform(managed_object_native(anchor));
    if (!transform) return false;
    if (!marker_world_position(transform, out)) return false;
    valid = 1;
    return true;
}

bool esp_farm_get_target(FarmTarget& out) {
    out = FarmTarget{};
    if (!g_farm_mask) { g_farm_idle_reason = 1; return false; }
    if (g_pid <= 0 || !g_il2cpp_base) { g_farm_idle_reason = 2; return false; }
    // Та же самопочинка, что у маркеров: если конвейер боксов не опубликовал
    // кадр (пустой список игроков и т.п.), собираем его прямо из камеры.
    if (!g_frame_local_valid || !g_frame_vp_valid) {
        if (!publish_camera_only_frame(g_last_overlay_sw, g_last_overlay_sh)) {
            g_farm_idle_reason = 2;
            return false;
        }
    }

    // Учёт чёрного списка (контроллер вызывает нас один раз в кадр).
    for (auto it = g_farm_blacklist.begin(); it != g_farm_blacklist.end();) {
        if (--(it->second) <= 0) it = g_farm_blacklist.erase(it);
        else ++it;
    }

    farm_scan_tick();

    // Липкая цель: пока текущий узел жив, работаем по нему — иначе контроллер
    // переключался бы между двумя равноудалёнными узлами каждый кадр.
    static uint64_t s_last_identity = 0;

    // Орудие в руках читаем один раз на кадр: из него и дальность удара, и ритм,
    // и умения (какой ресурс этим орудием вообще добывается).
    MeleeReach reach{};
    bool have_reach = false;
    have_reach = read_local_melee_reach(reach);
    g_farm_tool_have = (have_reach && reach.purposes_valid) ? reach.tool_purposes : 0;
    int tool_need = 0;

    const float kMaxFarmDistance = g_farm_max_distance;
    const FarmEntity* best = nullptr;
    float best_score = 1e18F;
    float best_dist = 0.0F;
    for (const FarmEntity& entity : g_farm_entities) {
        if (!(g_farm_mask & (1u << entity.kind))) continue;
        if (!entity.pos_valid) continue;
        if (g_farm_blacklist.count(entity.identity)) continue;

        // Узел, который нечем взять текущим орудием, целью не становится:
        // сравнение побитовое, тип у обоих полей один (FPTool.ToolPurpose).
        // Без этого бот с киркой в руках вечно кружил вокруг дерева (и
        // наоборот), теряя на каждый узел весь give-up таймер.
        if (have_reach && reach.purposes_valid && entity.required_purpose != 0 &&
            (reach.tool_purposes & entity.required_purpose) == 0) {
            tool_need |= entity.required_purpose;
            continue;
        }

        // Дистанция по горизонтали (в Unity ось Y вверх). Контроллер сравнивает
        // её с дальностью удара, а пивот высокого дерева сидит в метрах над
        // землёй — 3D-дистанция до него никогда не опускается до порога, из-за
        // чего бот вечно кружил вокруг тонких стволов.
        const float dx = entity.pos.x - g_frame_local_pos.x;
        const float dy = entity.pos.y - g_frame_local_pos.y;
        const float dz = entity.pos.z - g_frame_local_pos.z;
        const float dist = sqrtf(dx * dx + dz * dz);
        if (!std::isfinite(dist) || dist > kMaxFarmDistance) continue;
        // Узлы на другом вертикальном уровне (скала сверху/снизу) не берём.
        if (!std::isfinite(dy) || fabsf(dy) > 30.0F) continue;

        float score = dist;
        // Похоже добытые узлы отправляются в конец очереди, а не отсеиваются
        // сразу: точный смысл fractionRemaining на всех сборках не гарантирован,
        // и ошибочная догадка здесь оставила бы фарм вообще без целей. Если
        // оценка неверна, watchdog контроллера всё равно снимет узел за
        // секунды. Для ТЕКУЩЕЙ цели не делаем никогда: один мусорный замер
        // остатка в середине работы отправлял её в конец очереди, и метка
        // прыгала на другой узел, хотя этот был ещё наполовину полон.
        if (entity.identity != s_last_identity) {
            // Освежаем кешированный остаток не чаще раза в секунду на узел —
            // иначе один ближний узел стоил по syscall'у на узел каждый кадр.
            FarmEntity& mut = const_cast<FarmEntity&>(entity);
            if (--mut.frac_age <= 0) {
                // 120 кадров: «раз в секунду» задумывалось при 60 fps, а на
                // 118 fps выходило вдвое чаще — по syscall'у на каждый ближний
                // узел дважды в секунду.
                mut.frac_age = 120;
                mut.fraction = rd<float>(entity.component + MINEABLE_FRACTION);
            }
            if (std::isfinite(mut.fraction) && mut.fraction >= 0.0F &&
                mut.fraction <= 1.001F && mut.fraction < 0.03F)
                score += 1000.0F;
        } else {
            score *= 0.6F; // липкость
        }
        if (score < best_score) { best_score = score; best = &entity; best_dist = dist; }
    }
    if (!best) {
        s_last_identity = 0;
        g_farm_tool_need = tool_need;
        // 6 — рядом есть узлы, но текущим орудием они не добываются.
        g_farm_idle_reason = tool_need ? 6 : (g_farm_entities.empty() ? 3 : 4);
        return false;
    }
    g_farm_tool_need = 0;
    s_last_identity = best->identity;
    FarmEntity& node = const_cast<FarmEntity&>(*best);

    // ---- Крестик -------------------------------------------------------------
    Vec3 spot{};
    int spot_source = 0, streak = 0;
    float spot_life = -1.0F;
    bool has_spot = false;
    farm_resolve_extension(node);
    has_spot = farm_read_spot(node, spot, spot_source, streak, spot_life);

    // С какой стороны узла X. Если с обратной, то (а) идти к нему — значит
    // упираться в ствол/камень, и (б) удар сквозь меш не засчитается в серию.
    // Тогда бьём по корпусу: урон идёт, а игра пересоздаст крестик на месте
    // нашего попадания, как только текущий потухнет (15 с).
    bool spot_front = true;
    if (has_spot) {
        const float px = g_frame_local_pos.x - best->pos.x;
        const float pz = g_frame_local_pos.z - best->pos.z;
        const float sx = spot.x - best->pos.x;
        const float sz = spot.z - best->pos.z;
        const float pl = sqrtf(px * px + pz * pz);
        const float sl = sqrtf(sx * sx + sz * sz);
        // Только когда обе стороны различимы: у камня пивот бывает зарыт, и X
        // стоит почти над ним — там «сторона» не определена, считаем своей.
        if (pl > 0.35F && sl > 0.35F)
            spot_front = ((px * sx + pz * sz) / (pl * sl)) > -0.1F;
    }
    const bool at_spot = has_spot && spot_front;

    // ---- Точка крестика: поставить на кору, а не на декаль и не внутрь ствола --
    // Декаль X висит СНАРУЖИ коры (сдвиг по нормали против z-файта), поэтому
    // прицел по ней на боку ствола уходил мимо дерева: луч «глаз -> декаль» не
    // цеплял меш, GKo оставался пустым и FPMelee.ZkX играл один On_Woosh.
    // Первая поправка тянула прицел к оси ствола на 0.25 м — «сдвиг декали из
    // дампа». Замер по логу 14.09 (55 замахов с живым лучом игры, у которого
    // известны и точка попадания m_Point, и нормаль) показал, что 0.25 м —
    // много: декаль торчит из коры в среднем на 0.108 м, а прицел после
    // поправки уходил на 0.138 м ВНУТРЬ ствола (min 0.178, max +0.063 снаружи).
    // Урон при этом шёл (луч цеплял кору), но попадание оказывалось в 15 см от
    // крестика — за пределами 0.15 м, которыми игра меряет серию по X, поэтому
    // серия не росла: «крестик сбоку, а бот по нему мажет».
    //
    // Правильная точка — пересечение направления на декаль с корой. Радиус
    // ствола на высоте X берём из того, что игра меряет сама, по убыванию
    // точности:
    //   1. точка попадания её же луча прицела (m_Point) — это буквально кора;
    //   2. локальный отрезок X (MTQ на оси ствола, MTu на коре): его длина —
    //      радиус в локальных единицах, а масштаб дерева получается из высоты
    //      декали над пивотом (декаль стоит на той же высоте, что и X). Замер:
    //      локальный радиус 0.40 при коре 0.354 и 0.04 при коре 0.037 —
    //      масштаб 0.885 и 0.93, оба сходятся с высотой (1.93 -> 1.74);
    //   3. ни луча, ни полей — декаль торчит примерно на 0.3 радиуса, то есть
    //      кора это 0.77 от её вылета (по замеру 0.757..0.788 на двух деревьях).
    // Направление всегда от декали: нормаль ствола почти радиальна, а величина
    // сдвига по нормали нам как раз неизвестна — она и есть ошибка.
    if (has_spot && node.kind == 0) {
        const float cdx = spot.x - node.pos.x, cdz = spot.z - node.pos.z;
        const float choriz = sqrtf(cdx * cdx + cdz * cdz);
        if (choriz > 0.05F) {
            float bark = -1.0F;
            // 1) кора из луча самой игры. Попадание обязано быть на ЭТОМ стволе:
            //    правдоподобный радиус, не дальше декали с запасом и рядом по
            //    высоте — иначе это земля, соседнее дерево или крона.
            if (reach.ray_point_valid && reach.ray_hit_object) {
                const float hx = reach.ray_point.x - node.pos.x;
                const float hz = reach.ray_point.z - node.pos.z;
                const float rh = sqrtf(hx * hx + hz * hz);
                if (std::isfinite(rh) && rh > 0.03F && rh < 2.5F &&
                    rh < choriz * 1.6F && fabsf(reach.ray_point.y - spot.y) < 1.2F)
                    bark = rh;
            }
            // 2) локальный отрезок X + масштаб дерева из высоты декали
            if (bark < 0.0F && node.ext && node.ext_kind == FARM_EXT_TREE) {
                const Vec3 la = rd_v3(node.ext + TREEHS_SPOT_A);
                const Vec3 lb = rd_v3(node.ext + TREEHS_SPOT_B);
                const float h_world = spot.y - node.pos.y;   // высота X над пивотом
                if (vec3_is_finite(la) && vec3_is_finite(lb) && la.y > 0.25F && h_world > 0.25F) {
                    const float scale = h_world / la.y;
                    if (scale > 0.15F && scale < 6.0F) {
                        const float dx = lb.x - la.x, dz = lb.z - la.z;
                        const float r_local = sqrtf(dx * dx + dz * dz);
                        if (r_local > 0.005F) bark = r_local * scale;
                    }
                }
            }
            // 3) совсем ничего не прочиталось
            if (bark < 0.0F) bark = choriz * 0.77F;

            if (std::isfinite(bark) && bark > 0.02F && bark < 2.5F) {
                // Радиус ствола величина почти постоянная, а измерение по лучу
                // шумит на сантиметры из кадра в кадр (луч попадает в разные
                // места коры). Сглаживаем и держим на узел: иначе прицел
                // ползает вместе с шумом, а точка подхода дёргается за ним.
                if (s_bark_id != node.identity) { s_bark_id = node.identity; s_bark_r = bark; }
                else s_bark_r += (bark - s_bark_r) * 0.25F;
                if (s_bark_r > 0.02F && s_bark_r < 2.5F && s_bark_r < choriz * 1.6F) {
                    const float k = s_bark_r / choriz;
                    spot.x = node.pos.x + cdx * k;
                    spot.z = node.pos.z + cdz * k;
                }
            }
        }
    }

    // ---- Точка прицела -------------------------------------------------------
    Vec3 aim = at_spot ? spot : best->pos;
    if (!at_spot) aim.y += (best->kind == 0) ? 1.15F : 0.15F;

    // Полнокруговые углы от оси камеры (или выстрела): в отличие от
    // aim_angles_for() узел может быть и за спиной, поэтому проекция на
    // forward бывает отрицательной, а yaw охватывает +-180. Приоритет:
    // ось выстрела > поза трансформа > базис из матрицы вида этого кадра
    // (последний есть на устройствах, где поза не читается — именно из-за него
    // фарм раньше висел в «нет позиции камеры»).
    const bool ok_ref   = g_aim_ref_valid  && farm_cam_source_ok(g_aim_ref_origin);
    const bool ok_pose  = g_cam_pose_valid && farm_cam_source_ok(g_cam_pos);
    const bool ok_frame = g_frame_cam_basis_valid && farm_cam_source_ok(g_frame_cam_pos);
    if (!ok_ref && !ok_pose && !ok_frame) {
        g_farm_idle_reason = 5;
        return false;
    }
    // По живому крестику меряем от КАМЕРЫ: прицел должен стоять ровно на той
    // отметке, которую видит игрок (покачивание look-root давало пару градусов
    // промаха). По корпусу — от оси выстрела: вдоль неё и идёт удар, а сам узел
    // огромный.
    const bool use_ref  = ok_ref && !(at_spot && ok_pose);
    const bool use_pose = !use_ref && ok_pose;
    const Vec3& origin = use_ref ? g_aim_ref_origin  : use_pose ? g_cam_pos     : g_frame_cam_pos;
    const Vec3& fwd    = use_ref ? g_aim_ref_forward : use_pose ? g_cam_forward : g_frame_cam_fwd;
    const Vec3& right  = use_ref ? g_aim_ref_right   : use_pose ? g_cam_right   : g_frame_cam_right;
    const Vec3& up     = use_ref ? g_aim_ref_up      : use_pose ? g_cam_up      : g_frame_cam_up;

    // Прицел по корпусу зажимаем в полосу вокруг ГЛАЗА игрока — единственной
    // высоты, которая надёжна на всех префабах (пивот камня бывает на макушке,
    // у дерева — в центре ствола).
    if (!at_spot) {
        if (best->kind == 0) {
            // Дерево: грудь — чуть ниже глаза до уровня глаза.
            const float lo = origin.y - 0.9F, hi = origin.y + 0.1F;
            if (aim.y < lo) aim.y = lo;
            if (aim.y > hi) aim.y = hi;
        } else {
            // Руда: от колена до пояса, заметно ниже глаза.
            const float lo = origin.y - 1.3F, hi = origin.y - 0.55F;
            if (aim.y < lo) aim.y = lo;
            if (aim.y > hi) aim.y = hi;
        }
    }

    const Vec3 d = {aim.x - origin.x, aim.y - origin.y, aim.z - origin.z};
    const float fx = d.x * fwd.x + d.y * fwd.y + d.z * fwd.z;
    const float rx = d.x * right.x + d.y * right.y + d.z * right.z;
    const float ux = d.x * up.x + d.y * up.y + d.z * up.z;
    if (!std::isfinite(fx) || !std::isfinite(rx) || !std::isfinite(ux)) {
        g_farm_idle_reason = 5;
        return false;
    }
    constexpr float rad2deg = 57.29577951F;
    const float yaw   = atan2f(rx, fx) * rad2deg;
    const float pitch = atan2f(ux, sqrtf(fx * fx + rx * rx)) * rad2deg;
    if (!std::isfinite(yaw) || !std::isfinite(pitch)) { g_farm_idle_reason = 5; return false; }

    // ---- Точка подхода -------------------------------------------------------
    // С крестиком встаём ПЕРЕД ним (standoff наружу от оси узла), а не в сам
    // узел: дистанция удара меряется от X, а меш не даёт подойти к пивоту
    // вплотную. Без крестика идём к узлу — контроллер остановится сам.
    Vec3 goal = best->pos;
    if (at_spot) {
        float ox = spot.x - best->pos.x, oz = spot.z - best->pos.z;
        float ol = sqrtf(ox * ox + oz * oz);
        if (ol < 0.05F) {   // X почти над пивотом — заходим со стороны игрока
            ox = g_frame_local_pos.x - best->pos.x;
            oz = g_frame_local_pos.z - best->pos.z;
            ol = sqrtf(ox * ox + oz * oz);
        }
        if (ol > 0.05F) {
            const float standoff = (best->kind == 0) ? 0.45F : 0.95F;
            goal.x = spot.x + (ox / ol) * standoff;
            goal.z = spot.z + (oz / ol) * standoff;
            goal.y = spot.y;
        }
    }
    float walk_yaw = 0.0F, walk_dist = best_dist;
    {
        const float gx = goal.x - origin.x, gz = goal.z - origin.z;
        const float gd = sqrtf(gx * gx + gz * gz);
        if (std::isfinite(gd)) {
            walk_dist = gd;
            const float gyaw = atan2f(gx * right.x + gz * right.z,
                                      gx * fwd.x + gz * fwd.z) * rad2deg;
            if (std::isfinite(gyaw)) walk_yaw = gyaw;
        }
    }

    // ---- Экранная метка: ровно та точка, по которой бьёт бот ----------------
    if (g_frame_vp_valid) {
        Vec2 screen{};
        if (w2s(g_frame_vp, aim, g_frame_sw, g_frame_sh, screen, false) &&
            std::isfinite(screen.x) && std::isfinite(screen.y) &&
            screen.x >= -64.0F && screen.x <= g_frame_sw + 64.0F &&
            screen.y >= -64.0F && screen.y <= g_frame_sh + 64.0F) {
            out.on_screen = true;
            out.sx = screen.x;
            out.sy = screen.y;
        }
    }

    const float adx = aim.x - g_frame_local_pos.x;
    const float adz = aim.z - g_frame_local_pos.z;
    const float aim_dist = sqrtf(adx * adx + adz * adz);
    // 3D-дистанция от ТОЙ ЖЕ точки, от которой считаем углы (глаз / ось
    // выстрела): именно её игра сравнивает с m_MaxReach + hitRadius в
    // FPMelee.ZkX, решая засчитать удар или сыграть промах. От
    // g_frame_local_pos (корень игрока) её мерить нельзя: глаз выше примерно
    // на 1.5 м, а вся дальность удара — пара метров.
    const float edx = aim.x - origin.x, edy = aim.y - origin.y, edz = aim.z - origin.z;
    const float aim_3d = sqrtf(edx * edx + edy * edy + edz * edz);

    g_farm_idle_reason = 0;
    out.valid = true;
    out.id = best->identity;
    out.ext_found = (node.ext_kind != FARM_EXT_NONE);
    out.kind = best->kind;
    out.yaw = yaw;
    out.pitch = pitch;
    out.aim_dist = std::isfinite(aim_dist) ? aim_dist : best_dist;
    out.aim_3d = std::isfinite(aim_3d) ? aim_3d : out.aim_dist;
    out.aim_x = aim.x; out.aim_y = aim.y; out.aim_z = aim.z;
    out.node_x = best->pos.x; out.node_y = best->pos.y; out.node_z = best->pos.z;
    // Дальность удара текущего орудия — из игры (FPMelee.m_MaxReach +
    // hitRadius), а не «на глаз». Ноль значит «в руках не ближнее орудие или
    // чтение не удалось» — тогда контроллер остаётся на эмпирических порогах.
    if (reach.valid) {
        out.melee_reach = reach.total;
        out.melee_ray = reach.ray_length;
        out.tool_purposes = reach.purposes_valid ? reach.tool_purposes : 0;
        if (reach.time_between_attacks > 0.0F)
            out.attack_period = reach.time_between_attacks + reach.pause_after_attack;
        // Перекрыт ли узел: луч игры упёрся заметно раньше нашей точки
        // прицела. Полметра допуска — на разницу между камерой и осью
        // выстрела (качание/отдача) и на то, что X стоит на поверхности меша.
        out.ray_valid = reach.ray_valid;
        out.ray_distance = reach.ray_distance;
        out.ray_blocked = reach.ray_valid && reach.ray_hit_object &&
                          reach.ray_distance > 0.0F &&
                          reach.ray_distance < out.aim_3d - 0.6F;
        // Луч, упёршийся в собственный узел, перекрытием не считается: у камня
        // точка прицела внутри породы, и иначе бот вечно обходил бы свой же
        // камень (см. ray_hit_is_self_node). В out.ray_self_why остаётся, какая
        // именно проверка это доказала (1 коллайдер крестика, 2 GameObject узла,
        // 3 попадание в деталь поддерева) — по ней в логе видно, работает ли
        // послабление и почему нет.
        if (out.ray_blocked) {
            out.ray_self = ray_hit_is_self_node(reach, node, out.ray_self_why);
            if (out.ray_self) out.ray_blocked = false;
            // Страховка для руды, по которой ещё не было ни одного удара.
            // Крестика нет (spot 0) — значит, прицел стоит в ПИВОТЕ камня, то
            // есть внутри породы, а луч игры останавливается на её поверхности
            // заметно раньше. Проверки выше к такому лучу применимы только
            // после первого удара (коллайдер крестика заполняется в gir, а
            // поддерево узла может быть устроено иначе, чем представляется по
            // дампу), поэтому здесь работает признак, который даёт сама игра:
            // FPMelee.ZkX сравнивает с m_MaxReach + hitRadius дистанцию ЭТОГО
            // луча, и если он остановился в пределах дальности удара, удар
            // засчитается — по камню, в который мы и целимся вдоль оси прицела.
            // Такой узел надо бить, а не обходить: обход на 2 с водит камеру,
            // узел остаётся нетронутым и после четырёх обходов уходит в чёрный
            // список (ровно это и видно в логе 15.09.2026 по руде: 0 тапов).
            // Дерево сюда не попадает (kind == 0): у него прицел ставится на
            // кору, и «луч раньше точки прицела» там означает честное
            // перекрытие. От «бьём в чужой камень» страхует watchdog: ударов
            // нет прогресса — узел уйдёт в чёрный список.
            if (!out.ray_self && out.ray_blocked && node.kind != 0 && !at_spot &&
                out.melee_reach > 0.2F && reach.ray_distance > 0.0F &&
                reach.ray_distance <= out.melee_reach) {
                out.ray_self = true;
                out.ray_self_why = 4;
                out.ray_blocked = false;
            }
            // Якорь точки удара (MineableObject.LXX) — замер для лога: если его
            // мировая позиция совпадает с точкой попадания луча, это готовая
            // точка прицела на поверхности камня ещё до первого удара.
            if (node.kind != 0) {
                Vec3 anchor{};
                float to_ray = -1.0F;
                int   valid = 0;
                if (farm_ore_anchor_point(node, anchor, to_ray, valid)) {
                    out.ore_anchor_valid = true;
                    out.ore_anchor_x = anchor.x; out.ore_anchor_y = anchor.y;
                    out.ore_anchor_z = anchor.z;
                    if (reach.ray_point_valid) {
                        const float dx = anchor.x - reach.ray_point.x;
                        const float dy = anchor.y - reach.ray_point.y;
                        const float dz = anchor.z - reach.ray_point.z;
                        out.ore_anchor_to_ray = sqrtf(dx * dx + dy * dy + dz * dz);
                    }
                }
            }
        }
        out.ray_point_valid = reach.ray_point_valid;
        if (reach.ray_point_valid) {
            out.ray_px = reach.ray_point.x; out.ray_py = reach.ray_point.y;
            out.ray_pz = reach.ray_point.z;
            out.ray_nx = reach.ray_normal.x; out.ray_ny = reach.ray_normal.y;
            out.ray_nz = reach.ray_normal.z;
        }
    }
    // Состояние самого узла. Здоровье — самый тонкий признак того, что удары
    // доходят: fractionRemaining сдвигается на проценты, а m_CurrentHealth
    // падает уже от первого попадания.
    {
        float hp = 0.0F, hp_max = 0.0F;
        if (rd_exact(best->component + MINEABLE_CURRENT_HEALTH, hp) &&
            std::isfinite(hp) && hp >= 0.0F && hp < 1000000.0F)
            out.node_health = hp;
        if (rd_exact(best->component + MINEABLE_MAX_HEALTH, hp_max) &&
            std::isfinite(hp_max) && hp_max > 0.0F && hp_max < 1000000.0F)
            out.node_health_max = hp_max;
        int32_t xp = 0;
        if (rd_exact(best->component + MINEABLE_EXPERIENCE, xp) && xp >= 0 && xp < 1000000)
            out.node_experience = (int)xp;
    }
    out.at_spot = at_spot;
    out.has_spot = has_spot;
    out.spot_front = spot_front;
    out.spot_source = spot_source;
    out.streak = streak;
    out.spot_life = spot_life;
    out.walk_yaw = walk_yaw;
    out.walk_dist = walk_dist;
    out.node_dist = best_dist;
    const float fraction = rd<float>(best->component + MINEABLE_FRACTION);
    out.fraction = (std::isfinite(fraction) && fraction >= 0.0F && fraction <= 1.001F)
                 ? fraction : -1.0F;
    return true;
}
