// markers.cpp — Маркеры мира: скан реестра и выдача.
//
// Модуль разрезан из прежнего монолита jni/src/game.cpp;
// что здесь лежит и кто это зовёт — в шапке markers.h и в docs/CODE_MAP.md.

#include "esp/common.h"
#include "esp/farm_scan.h"
#include "esp/farm_target.h"
#include "esp/frame.h"
#include "esp/il2cpp.h"
#include "esp/managed.h"
#include "esp/marker_labels.h"
#include "esp/math.h"
#include "esp/mem.h"
#include "esp/names.h"
#include "esp/skeleton_cache.h"
#include "esp/skeleton_names.h"
#include "esp/transform.h"
#include "esp/weapons.h"
#include "markers.h"

static bool g_markers_ore_enabled = false;

static bool g_markers_animal_enabled = false;

static bool g_markers_loot_enabled = false;

static bool g_markers_pickup_enabled = false;

static float g_marker_max_distance = 150.0F;

// Когда можно начинать следующий проход скана маркеров, в mono_seconds().
// Раньше здесь был счётчик кадров (180, при пустом списке 30) из расчёта на
// 60 fps; устройство пользователя рисует 118 кадров/с, поэтому скан шёл вдвое
// чаще задуманного — 1.5 с вместо 3 с, а при пустом списке 0.25 с вместо 0.5 с.
static double g_marker_next_scan = 0.0;

static uint64_t g_network_client_class = 0;

static uint64_t g_network_identity_class = 0;

// Скан маркеров идёт порциями, а не одним кадром.
//
// Полный проход по словарю заспавненных объектов — это десятки тысяч обращений
// к памяти игры (на каждый компонент: класс, имя GameObject, трансформ,
// позиция), и раньше он делался целиком за один вызов. В спокойном режиме это
// стоило ~25 мс (в логе видно как dt_max 21-29 мс каждые ~3 с), а когда процесс
// уже придушен — до 280 мс одним кадром (те самые пики dt_max 212-283 мс в
// медленной части лога): игра в этот момент visibly дёргается.
//
// Поэтому порция ограничена ВРЕМЕНЕМ (kMarkerScanBudgetSec), а не числом
// объектов: цена записи словаря плавает от «не маркер, три чтения» до «кластер
// камней на 30 компонентов», а в придавленной троттлингом процессе те же чтения
// стоят в разы дороже — лимит по миллисекундам держит кадр ровным в обоих
// случаях. Курсор запоминается, собирается всё во временный список, и
// g_marker_entities подменяется разом, когда цикл завершён: старый список
// показывается до последнего кадра цикла, поэтому маркеры не мигают и не
// пропадают на время пересборки (это уже ломали однажды — см. «ESP flicker»).
// Узел разбирается целиком (проверка времени только между узлами): оборви мы
// его на середине, следующий кадр начал бы с того же узла и его уже собранные
// компоненты попали бы в список второй раз.
constexpr double kMarkerScanBudgetSec = 0.006;   // 6 мс на порцию при бюджете кадра 16.7 мс

static int32_t g_marker_scan_cursor = -1;    // -1: цикл не начат

static int32_t g_marker_scan_total  = 0;

static uint64_t g_marker_scan_identity_class = 0;

static std::vector<uint8_t> g_marker_scan_buffer;

// g_marker_scan_pending (список, который собирается сейчас) и marker_scan_abort()
// объявлены ниже, вместе с MarkerEntity; фильтр категорий меняется раньше по
// файлу, поэтому здесь только объявление.
static void marker_scan_abort();

void esp_set_markers_enabled(bool ore, bool animals, bool loot, bool pickups) {
    // Loot containers and ground pickups are filtered out during the registry
    // walk, so switching a category on has to invalidate the cached list.
    if (loot != g_markers_loot_enabled || pickups != g_markers_pickup_enabled) {
        g_marker_next_scan = 0.0;
        marker_scan_abort();   // половина списка собрана по старым фильтрам
    }
    g_markers_ore_enabled = ore;
    g_markers_animal_enabled = animals;
    g_markers_loot_enabled = loot;
    g_markers_pickup_enabled = pickups;
}

void esp_set_marker_max_distance(float metres) {
    if (!std::isfinite(metres)) return;
    if (metres < 10.0F) metres = 10.0F;
    if (metres > MAX_PLAYER_DISTANCE) metres = MAX_PLAYER_DISTANCE;
    g_marker_max_distance = metres;
}

struct MarkerEntity {
    uint64_t identity = 0;
    uint64_t transform = 0;     // native Transform of the GameObject
    Vec3     position{};
    bool     position_valid = false;
    // Позицию надо перечитать при первой возможности (ставится, когда мир
    // сменился): адрес мог быть переиспользован новым миром, и прежняя позиция
    // держится только как «мост» на время, пока новый список не собран.
    bool     position_recheck = false;
    double   stale_since = 0.0; // когда чтения перестали проходить (0 = читается)
    int      pos_cooldown = 0;  // frames until the next position re-read (far animals)
    int      kind = ESP_MARKER_ORE;
    // Fixed labels point into static strings; ground pickups build their own
    // ("Ягоды x12"), in which case `text` holds it and `label` is null.
    const char* label = nullptr;
    char     text[40] = {};
    bool     has_color = false;
    bool     rainbow = false;
    unsigned char color_rgb[3] = {255, 255, 255};
};

static std::vector<MarkerEntity> g_marker_entities;

// Список, который собирается порциями прямо сейчас: g_marker_entities
// подменяется им только когда цикл завершён, чтобы маркеры не мигали.
static std::vector<MarkerEntity> g_marker_scan_pending;

// Прервать незавершённый цикл: следующая порция начнёт его заново. Нужно при
// смене фильтров (иначе в список доехала бы старая категория) и при перезагрузке
// мира. g_marker_entities при этом остаётся прежним — показываем его до конца
// следующего цикла.
static void marker_scan_abort() {
    g_marker_scan_cursor = -1;
    g_marker_scan_total  = 0;
    g_marker_scan_buffer.clear();
    g_marker_scan_buffer.shrink_to_fit();
    g_marker_scan_pending.clear();
}

static std::unordered_map<uint64_t, uint8_t> g_marker_class_kind;

uint8_t marker_class_of(uint64_t klass) {
    if (!valid_obj(klass)) return MARKER_CLASS_NONE;
    auto found = g_marker_class_kind.find(klass);
    if (found != g_marker_class_kind.end()) return found->second;
    bool readable = false;
    const std::string name = read_remote_string(rd_ptr(klass + IL2CPP_CLASS_NAME), &readable);
    uint8_t kind = readable ? MARKER_CLASS_NONE : MARKER_CLASS_UNKNOWN;
    if (readable) {
        if (name.rfind("Mineable", 0) == 0)                       kind = MARKER_CLASS_MINEABLE;
        else if (name == "LootObject" || name == "PumpkinTrick")  kind = MARKER_CLASS_LOOT;
        else if (name == "ItemPickup")                            kind = MARKER_CLASS_PICKUP;
        // Smashable scrap barrels: on this build their component class is
        // "LootDestroyable" (log: skip-class 'LootDestroyable' go='Barrel_v2') —
        // not a Mineable and not a LootObject. Any class that says Barrel out
        // loud is kept as a safety net for other builds.
        else if (name == "LootDestroyable" ||
                 name.find("Barrel") != std::string::npos)      kind = MARKER_CLASS_BARREL;
    }
    if (g_marker_class_kind.size() < 512) g_marker_class_kind[klass] = kind;
    return kind;
}

uint64_t resolve_network_client_spawned() {
    if (!g_network_client_class && NETWORK_CLIENT_TYPEINFO_RVA != 0) {
        const uint64_t candidate = rd_ptr(g_il2cpp_base + NETWORK_CLIENT_TYPEINFO_RVA);
        if (class_identity(candidate, "NetworkClient", "Mirror") != 0)
            g_network_client_class = candidate;
    }
    if (!g_network_client_class) return 0;
    uint64_t statics = get_class_static_fields(g_network_client_class);
    if (!statics) return 0;
    uint64_t spawned = rd_ptr(statics + NETWORK_CLIENT_SPAWNED);
    return valid_obj(spawned) ? spawned : 0;
}

// A known-good NetworkIdentity (the game controller's own) gives us the class
// pointer every dictionary entry must match — a cheap, exact sanity check that
// also protects against a wrong entry stride.
uint64_t resolve_network_identity_class() {
    if (g_network_identity_class) return g_network_identity_class;
    if (!g_game_controller_class) return 0;
    uint64_t statics = get_class_static_fields(g_game_controller_class);
    if (!statics) return 0;
    uint64_t identity = rd_ptr(statics + GAME_CONTROLLER_NET_IDENTITY_FIELD);
    if (!valid_obj(identity)) return 0;
    uint64_t klass = rd_ptr(identity);
    if (!valid_obj(klass)) return 0;
    bool name_readable = false;
    const std::string name = read_remote_string(rd_ptr(klass + IL2CPP_CLASS_NAME), &name_readable);
    if (name_readable && name != "NetworkIdentity") return 0;
    if (!class_looks_alive(klass)) return 0;
    g_network_identity_class = klass;
    return klass;
}

bool marker_world_position(uint64_t transform, Vec3& out) {
    if (!transform) return false;
    // The learned layouts (skeleton / hierarchy) come from PLAYER bones and
    // can go stale after a world reload or simply not match non-bone
    // transforms — they then return finite-but-garbage positions, which is
    // how markers died solo after a respawn (cache full, every distance
    // absurd, zero on screen). Any implausible read falls through to the
    // slow probing path instead of being trusted.
    if (g_skeleton_layout_valid && read_transform_hierarchy_layout(transform, g_skeleton_layout, out) &&
        vec3_is_finite(out) && position_looks_like_world_space(out))
        return true;
    return read_transform_hierarchy_position(transform, out) && vec3_is_finite(out) &&
           position_looks_like_world_space(out);
}

// Walk Mirror's client registry and cache every ore node / animal in it.
// true  — цикл завершён: список собран целиком и подменён (или собирать нечего,
//         тогда он пустой);
// false — обработана только порция, продолжать нужно на следующем кадре.
// Все отказы (словарь не читается, мир грузится) возвращают true: иначе вызывающий
// поставит «продолжать немедленно» и будет долбить в стену каждый кадр.
static bool rebuild_marker_entities() {
    if (g_marker_scan_cursor < 0) {
        // Начало цикла: действующий список НЕ трогаем — он показывается до
        // завершения сборки, поэтому маркеры не мигают.
        g_marker_scan_pending.clear();
        g_marker_scan_buffer.clear();

        uint64_t dictionary = resolve_network_client_spawned();
        if (!dictionary) { marker_scan_abort(); return true; }
        // Needed to read prefab names (wolves / rats); harmless if it fails, the
        // loot and entityType paths still work.
        if (!g_go_name_offset_valid && g_local_player) ensure_gameobject_name_offset(g_local_player);
        g_marker_scan_identity_class = resolve_network_identity_class();

        uint64_t entries = rd_ptr(dictionary + DICT_ENTRIES);
        int32_t count = rd<int32_t>(dictionary + DICT_COUNT);
        if (!valid_obj(entries) || count <= 0) { marker_scan_abort(); return true; }
        if (count > 4096) count = 4096;

        // One bulk read for the whole entry array instead of one read per entry.
        // Буфер живёт до конца цикла: порции разбирают уже прочитанное.
        g_marker_scan_buffer.resize((size_t)count * DICT_ENTRY_STRIDE);
        if (!rd_buf(entries + IL2CPP_ARRAY_FIRST_ELEMENT, g_marker_scan_buffer.data(),
                    g_marker_scan_buffer.size())) {
            marker_scan_abort();
            return true;
        }
        g_marker_scan_total  = count;
        g_marker_scan_cursor = 0;
    }

    const uint64_t identity_class = g_marker_scan_identity_class;
    const int32_t count = g_marker_scan_total;
    const double t_budget = mono_seconds();
    uint64_t behaviours[32];
    int32_t i = g_marker_scan_cursor;
    for (; i < count; ++i) {
        // Первый узел порции разбираем всегда (иначе цикл мог бы встать),
        // дальше — пока не выбрано время.
        if (i > g_marker_scan_cursor && mono_seconds() - t_budget > kMarkerScanBudgetSec) break;
        uint64_t identity = 0;
        memcpy(&identity, g_marker_scan_buffer.data() + (size_t)i * DICT_ENTRY_STRIDE + DICT_ENTRY_VALUE, sizeof(identity));
        if (!valid_obj(identity)) continue;
        if (identity_class && rd_ptr(identity) != identity_class) continue;
        uint64_t array = rd_ptr(identity + NETID_BEHAVIOURS);
        if (!valid_obj(array)) continue;
        int32_t behaviour_count = rd<int32_t>(array + IL2CPP_ARRAY_LENGTH);
        if (behaviour_count <= 0) continue;
        if (behaviour_count > 32) behaviour_count = 32;
        if (!rd_buf(array + IL2CPP_ARRAY_FIRST_ELEMENT, behaviours, (size_t)behaviour_count * sizeof(uint64_t)))
            continue;

        // Mirror collects behaviours with GetComponentsInChildren, so one
        // identity can carry a whole rock cluster: every Mineable* component
        // becomes its own marker, positioned by its own GameObject.
        for (int32_t b = 0; b < behaviour_count; ++b) {
            uint64_t component = behaviours[b];
            if (!valid_obj(component)) continue;
            const uint8_t component_class = marker_class_of(rd_ptr(component));
            if (component_class == MARKER_CLASS_NONE) continue;
            // Имя класса компонента может быть недоступно (см. marker_class_of):
            // тогда семейство неизвестно, и объект опознаётся по имени GameObject —
            // оно лежит в куче игры и читается всегда. Пробуем семейства по порядку.
            const bool family_unknown = (component_class == MARKER_CLASS_UNKNOWN);
            // «Дорогой» компонент: имя GameObject, трансформ и позиция — это
            // десятки обращений к памяти игры, поэтому время порции проверяется
            // снаружи, между узлами.

            MarkerLook look;
            char pickup_text[40] = {};
            char object_name[48] = {}, root_name[48] = {};
            managed_component_gameobject_name(component, object_name, sizeof(object_name));
            managed_component_gameobject_name(identity, root_name, sizeof(root_name));
            bool known = false;
            if ((component_class == MARKER_CLASS_PICKUP || family_unknown) &&
                g_markers_pickup_enabled) {
                known = pickup_marker(component, pickup_text, sizeof(pickup_text));
                if (known) {
                    look.kind = ESP_MARKER_PICKUP;
                    look.label = nullptr;
                    look.has_color = false;
                }
            }
            if (!known && (component_class == MARKER_CLASS_BARREL || family_unknown) &&
                g_markers_loot_enabled) {
                // LootDestroyable covers every smashable loot prop; label by
                // prefab name — "Barrel_v2" is the scrap barrel, anything
                // else gets the generic smash-box label.
                known = barrel_look_from_object_name(object_name, look) ||
                        barrel_look_from_object_name(root_name, look);
                // Общая метка «ящик» — только для класса, о котором точно
                // известно, что он дробимый: при неизвестном семействе иначе
                // метку получил бы любой объект в реестре.
                if (!known && component_class == MARKER_CLASS_BARREL) {
                    look = kSmashBox; known = true;
                }
            }
            if (!known && (component_class == MARKER_CLASS_LOOT || family_unknown) &&
                g_markers_loot_enabled) {
                known = loot_marker(component, object_name, root_name, look);
            }
            if (!known && (component_class == MARKER_CLASS_MINEABLE || family_unknown)) {
                // The prefab name is asked first on purpose: animals that the
                // EntityType enum does not know (wolf, rat, ...) are shipped
                // with a borrowed entityType -- the wolf prefab says "Boar" --
                // so trusting the enum first labelled every wolf as a boar.
                if (object_name[0]) known = animal_look_from_object_name(object_name, look);
                if (!known && root_name[0]) known = animal_look_from_object_name(root_name, look);
                if (!known) {
                    int32_t entity_type = rd<int32_t>(component + MINEABLE_ENTITY_TYPE);
                    known = marker_for_entity_type(entity_type, look);
                }
                if (!known && object_name[0]) known = barrel_look_from_object_name(object_name, look);
                if (!known && root_name[0])   known = barrel_look_from_object_name(root_name, look);
                if (!known) known = marker_from_loot(component, look);
                // Cloth bushes, mushroom and berry clusters: harvestable, so
                // they are Mineables, but they belong to the pickup category.
                if (!known && g_markers_pickup_enabled) known = gather_marker_from_loot(component, look);
            }
            if (!known) continue;

            MarkerEntity entity;
            entity.identity = identity;
            entity.kind = look.kind;
            entity.label = look.label;
            if (!look.label) memcpy(entity.text, pickup_text, sizeof(entity.text));
            entity.has_color = look.has_color;
            entity.rainbow = look.rainbow;
            entity.color_rgb[0] = look.rgb[0];
            entity.color_rgb[1] = look.rgb[1];
            entity.color_rgb[2] = look.rgb[2];
            entity.transform = native_component_transform(managed_object_native(component));
            if (!entity.transform) // component without its own renderer: use the identity
                entity.transform = native_component_transform(managed_object_native(identity));
            entity.position_valid = marker_world_position(entity.transform, entity.position);
            if (entity.transform) g_marker_scan_pending.push_back(entity);
            if (g_marker_scan_pending.size() >= 512) break;
        }
        if (g_marker_scan_pending.size() >= 512) break;
    }

    if (i < count && g_marker_scan_pending.size() < 512) {
        // Порция израсходована (вышло время), цикл не закончен — продолжим на
        // следующем кадре с этого же узла.
        g_marker_scan_cursor = i;
        return false;
    }
    // Конец цикла: словарь пройден либо уперлись в лимит списка (512).
    // Подменяем список разом.
    g_marker_entities.swap(g_marker_scan_pending);
    marker_scan_abort();
    return true;
}

// Прочитать позицию маркера, не теряя прошлую из-за одного сбоя чтения.
//
// Почему так. Позиция маркера — это цепочка Transform (несколько syscall'ов), и
// после смерти, когда мир перезагружается, она не читается секундами. Прежний
// код в этом месте просто не рисовал маркер: пропадали руда, ящики, животные —
// «ESP мерцает». Держим прошлую позицию до предела (1.5 с, а пока идёт
// перезагрузка мира — 4 с): вечно держать нельзя (адрес мог достаться новому
// объекту), но и одного сбоя для пропажи мало.
static bool refresh_marker_position(MarkerEntity& entity) {
    Vec3 fresh{};
    if (marker_world_position(entity.transform, fresh)) {
        entity.position = fresh;
        entity.position_valid = true;
        entity.position_recheck = false;
        entity.stale_since = 0.0;
        return true;
    }
    if (!entity.position_valid) return false;
    const double now = mono_seconds();
    if (entity.stale_since <= 0.0) entity.stale_since = now;
    const double hold = world_reloading() ? 4.0 : 1.5;
    if (now - entity.stale_since > hold) {
        entity.position_valid = false;
        entity.stale_since = 0.0;
        return false;
    }
    return true;   // рисуем по позиции последнего удачного чтения
}

void reset_marker_caches() {
    // ВАЖНО: список сущностей здесь НЕ очищается. Раньше очищался — и после
    // смерти/респавна маркеры (руда, ящики, животные) исчезали с экрана на всё
    // время нового скана реестра (до трёх секунд и дольше, а пока мир грузился,
    // скан вообще возвращал пустой список). Теперь прежний список остаётся на
    // экране как «мост»: позиции перепроверяются при первой возможности, а
    // готовый новый список подменяет старый разом (см. rebuild_marker_entities).
    for (MarkerEntity& entity : g_marker_entities) {
        entity.position_recheck = true;
        if (entity.stale_since <= 0.0) entity.stale_since = mono_seconds();
    }
    g_marker_next_scan = 0.0;
    marker_scan_abort();   // незавершённая порция после перезагрузки мира не нужна
    g_marker_class_kind.clear();
    g_network_client_class = 0;
    g_network_identity_class = 0;
    // Farm entities come from the same registry: stale pointers must not
    // survive a world reload either.
    g_farm_entities.clear();
    g_farm_blacklist.clear();
    farm_scan_reset();
}

std::vector<EspMarker> esp_get_markers() {
    std::vector<EspMarker> result;
    if (!g_markers_ore_enabled && !g_markers_animal_enabled &&
        !g_markers_loot_enabled && !g_markers_pickup_enabled) {
        if (!g_marker_entities.empty()) g_marker_entities.clear();   // все категории выключены — держать нечего
        g_marker_next_scan = 0.0;
        marker_scan_abort();   // не тащить незавершённую порцию через выключенный ESP
        return result;
    }
    if (g_pid <= 0 || !g_il2cpp_base) return result;
    // The box pipeline publishes the frame while players are visible; when it
    // bailed out for ANY reason (empty player list, failed position read,
    // world reload), build a camera-only frame right here. Markers must never
    // depend on other players being around.
    if (!g_frame_vp_valid || !g_frame_local_valid) {
        if (!publish_camera_only_frame(g_last_overlay_sw, g_last_overlay_sh))
            return result;
    }

    {
        const double now = mono_seconds();
        if (now >= g_marker_next_scan) {
            const bool scan_done = rebuild_marker_entities();
            // ~3 s between scans: entities spawn and despawn slowly. An empty
            // result means the registry was not readable (world still loading in
            // after a respawn), so retry in half a second instead. Интервал в
            // секундах: при счёте в кадрах на 118 fps скан выходил вдвое чаще.
            // Незавершённую порцию продолжаем на следующем кадре (scan_done ==
            // false), иначе список собирался бы по кусочку раз в 3 секунды.
            g_marker_next_scan = scan_done
                ? now + (g_marker_entities.empty() ? 0.5 : 3.0)
                : now;
        }
    }

    // No usable layout (fresh join / respawn, nobody around): learn it from
    // the ENTITY transforms themselves. The layout discovery only needs a
    // couple of native transforms scattered across the map — ore nodes are
    // exactly that. This is what makes markers self-sufficient: previously
    // the layout could only be learned from other players' skeletons, and
    // solo after a world reload every position read failed (POS_INVALID).
    {
        static int s_solo_learn_cooldown = 0;
        bool positions_dead = false;
        if (!g_marker_entities.empty()) {
            size_t checked = 0, dead = 0;
            for (MarkerEntity& probe : g_marker_entities) {
                if (++checked > 6) break;
                Vec3 test{};
                if (!marker_world_position(probe.transform, test)) ++dead;
            }
            positions_dead = checked > 0 && dead >= checked - (checked > 4 ? 1 : 0);
        }
        if (positions_dead && --s_solo_learn_cooldown <= 0) {
            s_solo_learn_cooldown = 60; // ~1 s between attempts
            std::vector<uint64_t> seeds;
            for (const MarkerEntity& e : g_marker_entities) {
                if (e.transform) seeds.push_back(e.transform);
                if (seeds.size() >= 8) break;
            }
            size_t pos_count = 0, cand_count = 0;
            if (discover_layout_from_native_transforms(seeds, pos_count, cand_count)) {
                // Re-read every cached position with the fresh layout.
                for (MarkerEntity& e : g_marker_entities) {
                    e.position_recheck = true;     // раскладка сменилась — читаем заново
                    refresh_marker_position(e);
                }
            }
        }
    }

    const float max_distance = g_marker_max_distance;
    for (MarkerEntity& entity : g_marker_entities) {
        if (entity.kind == ESP_MARKER_ORE && !g_markers_ore_enabled) continue;
        if (entity.kind == ESP_MARKER_ANIMAL && !g_markers_animal_enabled) continue;
        if (entity.kind == ESP_MARKER_LOOT && !g_markers_loot_enabled) continue;
        if (entity.kind == ESP_MARKER_PICKUP && !g_markers_pickup_enabled) continue;
        // Ore nodes never move, so their position is only read on a rescan.
        // Animals DO move, but re-reading a transform chain (4+ syscalls)
        // for every animal every frame is the single hottest path here —
        // throttle far ones: within 60 m track every frame, beyond that a
        // few times a second is indistinguishable on screen.
        bool want_read = !entity.position_valid || entity.position_recheck;
        if (entity.kind == ESP_MARKER_ANIMAL) {
            if (--entity.pos_cooldown <= 0) {
                want_read = true;
                float ddx = entity.position.x - g_frame_local_pos.x;
                float ddz = entity.position.z - g_frame_local_pos.z;
                float d2 = ddx * ddx + ddz * ddz;
                entity.pos_cooldown = (!entity.position_valid || d2 < 60.0F * 60.0F) ? 1
                                    : (d2 < 150.0F * 150.0F) ? 6 : 15;
            }
        }
        if (want_read)
            refresh_marker_position(entity);
        if (!entity.position_valid) continue;

        float dx = entity.position.x - g_frame_local_pos.x;
        float dy = entity.position.y - g_frame_local_pos.y;
        float dz = entity.position.z - g_frame_local_pos.z;
        float distance = sqrtf(dx * dx + dy * dy + dz * dz);
        if (!std::isfinite(distance) || distance > max_distance) continue;

        Vec2 screen{};
        Vec3 anchor = entity.position;
        // above the model: animals are tall, crates sit low on the ground
        anchor.y += (entity.kind == ESP_MARKER_ANIMAL) ? 1.2F
                  : (entity.kind == ESP_MARKER_LOOT)   ? 0.6F
                  : (entity.kind == ESP_MARKER_PICKUP) ? 0.4F : 0.9F;
        if (!w2s(g_frame_vp, anchor, g_frame_sw, g_frame_sh, screen, false)) continue;
        if (!std::isfinite(screen.x) || !std::isfinite(screen.y)) continue;
        if (screen.x < -64.0F || screen.x > g_frame_sw + 64.0F) continue;
        if (screen.y < -64.0F || screen.y > g_frame_sh + 64.0F) continue;

        EspMarker marker;
        marker.x = screen.x;
        marker.y = screen.y;
        marker.distance = distance;
        marker.kind = entity.kind;
        marker.has_color = entity.has_color;
        marker.rainbow = entity.rainbow;
        marker.color_rgb[0] = entity.color_rgb[0];
        marker.color_rgb[1] = entity.color_rgb[1];
        marker.color_rgb[2] = entity.color_rgb[2];
        // Подпись маркера — на текущем языке (в русском это она и есть,
        // в английском — перевод по таблице имён; неизвестное остаётся как
        // есть, поэтому подпись никогда не пропадает).
        snprintf(marker.name, sizeof(marker.name), "%s",
                 lang::visual(entity.label ? entity.label : entity.text));
        result.push_back(marker);
    }

    std::sort(result.begin(), result.end(), [](const EspMarker& a, const EspMarker& b) {
        return a.distance < b.distance;
    });

    // Rock clusters put several nodes within a couple of metres, which would
    // stack their pills on top of each other: keep the nearest one of each
    // label per screen neighbourhood.
    std::vector<EspMarker> thinned;
    thinned.reserve(result.size());
    for (const EspMarker& marker : result) {
        bool covered = false;
        for (const EspMarker& kept : thinned) {
            if (kept.kind != marker.kind) continue;
            // Pickup labels carry a stack size, so two piles of berries never
            // compare equal — for that category position alone decides.
            if (marker.kind != ESP_MARKER_PICKUP && strcmp(kept.name, marker.name) != 0) continue;
            float dx = kept.x - marker.x, dy = kept.y - marker.y;
            if (dx * dx + dy * dy < 30.0F * 30.0F) { covered = true; break; }
        }
        if (covered) continue;
        thinned.push_back(marker);
        if (thinned.size() >= 64) break; // keep the screen readable
    }
    return thinned;
}
