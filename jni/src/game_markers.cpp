#include "game.h"
#include "bot_life.h"
#include "bot_lead.h"
#include "cfg.h"
#include "game_internal.h"
#include "game_offsets_active.h"   // оффсеты (go_active)
#include "manes_aim_source.h"
#include "esp_snapshot_policy.h"
#include "lang.h"
#include <algorithm>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

using namespace go_active;

// ===================== World markers: ore nodes and animals =====================
//
// Ore nodes, trees and animals are all Oxide.MineableObject subclasses
// (MineableStone / MineableTree / MineableAnimal / ...), and each one is a
// Mirror.NetworkBehaviour. The client therefore already keeps a complete list
// of the ones around the player:
//
//   Mirror.NetworkClient.spawned  (Dictionary<uint, NetworkIdentity>)
//     -> NetworkIdentity.NetworkBehaviours[]
//        -> the Mineable* component  -> entityType tells us what it is
//     -> the identity's GameObject transform -> world position
//
// Registry classification is incremental and cached. World positions are sampled
// on monotonic intervals under a small per-frame budget; projection still uses
// the current camera each frame.

static bool g_markers_ore_enabled = false;
static bool g_markers_animal_enabled = false;
static bool g_markers_loot_enabled = false;
static bool g_markers_pickup_enabled = false;
static bool g_markers_vehicle_enabled = false;
static bool g_markers_bot_enabled = false;
static bool g_markers_scrap_enabled = false;
static bool g_markers_turret_enabled = false;
static bool g_markers_backpack_enabled = false;
static bool g_markers_sleeper_enabled = false;
static bool g_markers_house_enabled = false;
static bool g_markers_cupboard_enabled = false;
// Аим по ботам: скан и позиции ботов живут, даже когда их маркеры выключены.
static bool g_bot_aim_enabled = false;
static float g_marker_max_distance = 150.0F;

static bool marker_visuals_enabled() {
    return g_markers_ore_enabled || g_markers_animal_enabled ||
           g_markers_loot_enabled || g_markers_pickup_enabled ||
           g_markers_vehicle_enabled || g_markers_bot_enabled ||
           g_markers_scrap_enabled || g_markers_turret_enabled ||
           g_markers_backpack_enabled || g_markers_sleeper_enabled ||
           g_markers_house_enabled || g_markers_cupboard_enabled;
}
// Scan cadence uses monotonic time so it does not depend on frame rate.
static double g_marker_next_scan = 0.0;
static double g_marker_next_layout_probe = 0.0;
static size_t g_marker_position_cursor = 0;
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
constexpr double kMarkerScanBudgetSec = 0.003;
constexpr double kMarkerPositionBudgetSec = 0.002;
constexpr double kMovingMarkerIntervalSec = 0.05;
constexpr double kStaticMarkerIntervalSec = 0.50;
constexpr double kMarkerPositionGraceSec = 0.25;
// A busy server can have well over 512 relevant components. Stopping at 512
// made the visible subset depend on Dictionary iteration order: after a resize
// existing markers could disappear and only return in a later pass. Keep a
// generous bounded cache and always finish walking the registry.
constexpr size_t kMarkerEntityLimit = 2048;
constexpr int32_t kMarkerRegistryLimit = 8192;
static int32_t g_marker_scan_cursor = -1;    // -1: цикл не начат
static int32_t g_marker_scan_total  = 0;
static uint64_t g_marker_scan_identity_class = 0;
static std::vector<uint8_t> g_marker_scan_buffer;
// g_marker_scan_pending (список, который собирается сейчас) и marker_scan_abort()
// объявлены ниже, вместе с MarkerEntity; фильтр категорий меняется раньше по
// файлу, поэтому здесь только объявление.
static void marker_scan_abort();
enum class MarkerScanResult { Complete, Pending, Unavailable };

void esp_set_markers_enabled(bool ore, bool animals, bool loot, bool pickups,
                             bool vehicles, bool bots, bool scrap, bool turrets,
                             bool backpacks, bool sleepers, bool house,
                             bool cupboards) {
    // Loot, pickups, scrap and house boxes are filtered during the registry walk.
    if (loot != g_markers_loot_enabled || pickups != g_markers_pickup_enabled ||
        scrap != g_markers_scrap_enabled || house != g_markers_house_enabled ||
        cupboards != g_markers_cupboard_enabled) {
        g_marker_next_scan = 0.0;
        marker_scan_abort();   // половина списка собрана по старым фильтрам
    }
    g_markers_ore_enabled = ore;
    g_markers_animal_enabled = animals;
    g_markers_loot_enabled = loot;
    g_markers_pickup_enabled = pickups;
    g_markers_vehicle_enabled = vehicles;
    g_markers_bot_enabled = bots;
    g_markers_scrap_enabled = scrap;
    g_markers_turret_enabled = turrets;
    g_markers_backpack_enabled = backpacks;
    g_markers_sleeper_enabled = sleepers;
    g_markers_house_enabled = house;
    g_markers_cupboard_enabled = cupboards;
    // Машины и боты всегда сканируются и фильтруются при отрисовке.
}


void esp_set_marker_max_distance(float metres) {
    if (!std::isfinite(metres)) return;
    if (metres < 10.0F) metres = 10.0F;
    if (metres > MAX_PLAYER_DISTANCE) metres = MAX_PLAYER_DISTANCE;
    g_marker_max_distance = metres;
}

struct MarkerEntity {
    uint64_t identity = 0;
    uint64_t id = 0;            // source component pointer, distinct within NetworkIdentity
    botlife::State life{};
    uint64_t transform = 0;     // native Transform of the GameObject
    Vec3 position{};
    bool position_valid = false;
    double position_sample_time = 0.0;
    double position_retry_time = 0.0;
    double position_failure_time = 0.0;
    uint64_t head_transform = 0;
    Vec3 head_position{};
    bool head_valid = false;
    botlead::Track lead{};
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

static bool marker_bot_dead(MarkerEntity& entity) {
    return entity.kind == ESP_MARKER_BOT && entity.life.refresh(
        [](uint64_t address, uint8_t& value) { return rd_exact(address, value); });
}

bool esp_marker_confirmed_dead(uint64_t id) {
    for (MarkerEntity& entity : g_marker_entities)
        if (entity.id == id) return marker_bot_dead(entity);
    return false;
}

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

// ---- Auto-farm state ---------------------------------------------------------
// The farm has its own entity cache (it wants trees, which the ore markers
// deliberately skip) and its own rescan cadence. Positions and loot are read
// the same way the markers read them; the walking/hitting itself is done with
// synthetic touches in main.cpp.


std::vector<FarmEntity> g_farm_entities;
std::unordered_map<uint64_t, int> g_farm_blacklist; // identity -> frames left
// Когда можно начинать следующий проход скана реестра, в mono_seconds().
// Раньше здесь был счётчик КАДРОВ (120, при пустом списке 30), рассчитанный на
// 60 fps: устройство пользователя рисует 118 кадров/с, поэтому скан запускался
// вдвое чаще задуманного — раз в ~1 с, а при пустом списке раз в 0.25 с.
double g_farm_next_scan = 0.0;
// Определение скана ниже (ему нужны FarmEntity и резолверы реестра), а
// сбрасывать его приходится и отсюда — при перезагрузке мира.
// Орудие в руках и что запросили отброшенные узлы (флаги ToolPurpose) — для
// строки статуса в меню: «нужен топор» вместо бессмысленного «все узлы вне
// радиуса».
int g_farm_tool_have = 0;
int g_farm_tool_need = 0;
// Why the picker returned nothing (surfaced in the menu status line):
// 0 ok, 1 off, 2 frame not published, 3 no nodes in registry, 4 none in
// range, 5 camera pose unreadable, 6 nodes are there but the tool in hand
// cannot harvest them (m_RequiredToolPurpose vs FPTool.m_ToolPurposes).
int g_farm_idle_reason = 1;

// What a single marker looks like: kind picks the toggle it belongs to, and ore
// markers carry a fixed colour per resource (stone grey, metal orange, sulfur
// yellow) instead of one configurable colour for all of them.
struct MarkerLook {
    int kind = ESP_MARKER_ORE;
    const char* label = nullptr;
    bool has_color = false;
    bool rainbow = false; // drawn in a cycling rainbow colour (elite crates)
    unsigned char rgb[3] = {255, 255, 255};
};

static const MarkerLook kOreStone  {ESP_MARKER_ORE, "Камень", true, false, {190, 190, 190}};
static const MarkerLook kOreMetal  {ESP_MARKER_ORE, "Железо", true, false, {255, 140,  40}};
static const MarkerLook kOreSulfur {ESP_MARKER_ORE, "Сера",   true, false, {255, 225,  50}};

static void apply_ore_cfg(MarkerLook& look, int which) {
    ImVec4 c = cfg::esp::ore_stone_col;
    if (which == 1) c = cfg::esp::ore_metal_col;
    else if (which == 2) c = cfg::esp::ore_sulfur_col;
    look.rgb[0] = (unsigned char)(c.x * 255.f);
    look.rgb[1] = (unsigned char)(c.y * 255.f);
    look.rgb[2] = (unsigned char)(c.z * 255.f);
    look.has_color = true;
}

// Barrels are smashed, not opened, so they are MineableObjects and never pass
// through the LootObject code at all -- the entityType below is the only place
// they can be recognised. They belong to the loot toggle all the same.
static const MarkerLook kBarrel    {ESP_MARKER_SCRAP, "Бочка",  true,  false, {80, 200, 255}};
static const MarkerLook kSmashBox  {ESP_MARKER_LOOT, "Ящик",   false, false, {255, 255, 255}};
// Дорожный знак — та же дробимая семья, что бочки и ящики: висит на тумблере
// «Ящики», но подпись у него своя (раньше тонул в общей «Ящик»).
static const MarkerLook kRoadSign  {ESP_MARKER_SCRAP, "Дорожный знак", false, false, {255, 255, 255}};

// Animals: the game's own EntityType only covers some of them, the rest (wolf,
// rat, ...) are recognised by the prefab name below.
static MarkerLook animal_look(const char* label) {
    MarkerLook look;
    look.kind = ESP_MARKER_ANIMAL;
    look.label = label;
    look.has_color = false;
    return look;
}

static MarkerLook bot_look(const char* label) {
    MarkerLook look;
    look.kind = ESP_MARKER_BOT;
    look.label = label;
    look.has_color = false;
    return look;
}

static MarkerLook vehicle_look(const char* label) {
    MarkerLook look;
    look.kind = ESP_MARKER_VEHICLE;
    look.label = label;
    look.has_color = false;
    return look;
}

static MarkerLook special_look(int kind, const char* label) {
    MarkerLook look;
    look.kind = kind;
    look.label = label;
    look.has_color = false;
    return look;
}

// EntityType -> what to draw. Trees, buildings and players are left out on
// purpose; barrels and smashable loot boxes are in, they are the scrap source
// and the game files them under the same enum. Road signs ride the loot
// toggle (same smashable family); bots and vehicles have their own toggles.
static bool marker_for_entity_type(int32_t type, MarkerLook& look) {
    switch ((MineableEntityType)type) {
        case MineableEntityType::Stone:    look = kOreStone;  apply_ore_cfg(look, 0); return true;
        case MineableEntityType::Iron:     look = kOreMetal;  apply_ore_cfg(look, 1); return true;
        case MineableEntityType::Sulfur:   look = kOreSulfur; apply_ore_cfg(look, 2); return true;
        case MineableEntityType::Barrel:   look = kBarrel;    return true;
        case MineableEntityType::Lootbox:  look = kSmashBox;  return true;
        // Air-drop balloon crates, new in this game build.
        case MineableEntityType::LootboxBaloon:
        case MineableEntityType::LootboxBaloonBig: look = kSmashBox; return true;
        case MineableEntityType::RoadSign: look = kRoadSign;  return true;
        case MineableEntityType::Human:    look = bot_look("Бот"); return true;
        case MineableEntityType::Vehicle:  look = vehicle_look("Машина"); return true;
        case MineableEntityType::Bear:     look = animal_look("Медведь");  return true;
        case MineableEntityType::Boar:     look = animal_look("Кабан");    return true;
        case MineableEntityType::Deer:     look = animal_look("Олень");    return true;
        case MineableEntityType::Rabbit:   look = animal_look("Кролик");   return true;
        case MineableEntityType::Hare:     look = animal_look("Заяц");     return true;
        case MineableEntityType::Chicken:  look = animal_look("Курица");   return true;
        case MineableEntityType::Fish:     look = animal_look("Рыба");     return true;
        case MineableEntityType::Cannibal: look = animal_look("Каннибал"); return true;
        default: return false; // Ice is deliberately not drawn
    }
}

static bool recovered_ore_look(const char* raw_name, MarkerLook& look) {
    if (manes_aim_source::classify_entity_name(raw_name) !=
        manes_aim_source::EntityName::CompositeStone)
        return false;
    look = kOreStone;
    apply_ore_cfg(look, 0);
    return true;
}

// Split a prefab name into lower-case words and hand each one to `visit`.
// Separators, digits and camelCase humps end a word, so "NPC_Wolf 02(Clone)"
// yields npc / wolf / clone. Matching whole words (never substrings) is what
// keeps "Crate" from looking like a rat and "Ratchet" from looking like loot.
// `visit` returning true stops the walk; that result is returned.
template <typename Visit>
static bool for_each_name_token(const char* raw, Visit&& visit) {
    if (!raw || !raw[0]) return false;
    char word[24];
    size_t n = 0;
    char prev = 0; // previous raw character, to spot camelCase humps
    for (const char* p = raw;; prev = *p, ++p) {
        char c = *p;
        bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        // A capital after a lower-case letter starts a new word ("BigWolf"),
        // and so does the LAST capital of an all-caps run when a lower-case
        // letter follows it ("NPCWolf" = npc + wolf). A pure all-caps run
        // ("WOLF") stays a single word.
        bool hump = letter && c >= 'A' && c <= 'Z' && n > 0 &&
                    ((prev >= 'a' && prev <= 'z') ||
                     (prev >= 'A' && prev <= 'Z' && p[1] >= 'a' && p[1] <= 'z'));
        if (letter && !hump && n + 1 < sizeof(word)) {
            word[n++] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
            continue;
        }
        if (n > 0) {
            word[n] = '\0';
            if (visit((const char*)word)) return true;
            n = 0;
        }
        if (!c) break;
        if (hump) { word[n++] = (char)(c - 'A' + 'a'); }
    }
    return false;
}

// Wolves and rats have no EntityType of their own (the enum stops at the
// analytics animals), so they are recognised by the prefab name of their
// GameObject.
static bool animal_look_from_object_name(const char* raw, MarkerLook& look) {
    static const struct { const char* word; const char* ru; } kAnimals[] = {
        {"wolf", "Волк"},      {"wolfs", "Волк"},     {"wolves", "Волк"},
        {"volk", "Волк"},      {"husky", "Волк"},
        {"rat", "Крыса"},      {"rats", "Крыса"},     {"mouse", "Крыса"},
        {"bear", "Медведь"},   {"boar", "Кабан"},     {"pig", "Кабан"},
        {"deer", "Олень"},     {"stag", "Олень"},     {"rabbit", "Кролик"},
        {"hare", "Заяц"},      {"chicken", "Курица"}, {"hen", "Курица"},
        {"fish", "Рыба"},      {"shark", "Акула"},    {"cannibal", "Каннибал"},
        {"horse", "Лошадь"},   {"goat", "Коза"},      {"sheep", "Овца"},
        {"cow", "Корова"},     {"fox", "Лиса"},       {"snake", "Змея"},
    };
    return for_each_name_token(raw, [&](const char* word) {
        size_t wlen = strlen(word);
        for (const auto& animal : kAnimals) {
            if (strcmp(word, animal.word) == 0) { look = animal_look(animal.ru); return true; }
            // Prefix match for glued prefab words ("wolfmale", "bearbig"):
            // only for 4+ letter animal tokens, so "rat" never claims
            // "ratchet" and other short-token accidents stay impossible.
            size_t alen = strlen(animal.word);
            if (alen >= 4 && wlen > alen && strncmp(word, animal.word, alen) == 0) {
                look = animal_look(animal.ru);
                return true;
            }
        }
        return false;
    });
}

// Same safety net as the animals: some barrels come with an empty entityType,
// and their prefab is the only thing that still says "barrel".
static bool barrel_look_from_object_name(const char* raw, MarkerLook& look) {
    return for_each_name_token(raw, [&](const char* word) {
        // Prefix match: covers "barrel", "barrels", "barrel02", glued names
        // like "barrelblue", plus the misspelled and localised variants some
        // prefabs ship with ("barel", "bochka").
        if (strncmp(word, "barrel", 6) == 0 || strncmp(word, "barel", 5) == 0 ||
            strncmp(word, "bochka", 6) == 0 ||
            strcmp(word, "keg") == 0 || strcmp(word, "drum") == 0) {
            look = kBarrel;
            return true;
        }
        return false;
    });
}

// Prefab names that disqualify a Mineable component completely, checked BEFORE
// the entityType (a borrowed type would otherwise win — the wolf says "Boar"):
//   * buggies — vehicle ESP is for regular cars only (user request);
//   * corpses / sleepers — a Human type on them would fake a "bot";
//   * copters — they share the Vehicle-ish component classes with cars;
//   * garages / hangars / depots — car-named buildings, not cars.
static bool skip_object_name(const char* raw) {
    return for_each_name_token(raw, [](const char* word) {
        if (strcmp(word, "buggy") == 0 || strcmp(word, "buggies") == 0) return true;
        if (strcmp(word, "corpse") == 0 || strcmp(word, "corpses") == 0 ||
            strcmp(word, "ragdoll") == 0 || strcmp(word, "sleeper") == 0 ||
            strcmp(word, "sleepers") == 0 || strcmp(word, "deathbag") == 0 ||
            strcmp(word, "playercorpse") == 0) return true;
        if (strcmp(word, "copter") == 0 || strcmp(word, "copters") == 0 ||
            strcmp(word, "heli") == 0 || strcmp(word, "helicopter") == 0 ||
            strcmp(word, "helicopters") == 0 || strcmp(word, "minicopter") == 0) return true;
        if (strcmp(word, "garage") == 0 || strcmp(word, "garages") == 0 ||
            strcmp(word, "hangar") == 0 || strcmp(word, "hangars") == 0 ||
            strcmp(word, "depot") == 0 || strcmp(word, "depots") == 0) return true;
        return false;
    });
}

// Одно слово говорит «машина»: целые слова, не префикс — "car" начинает
// cargo, carpet, cardboard и carepackage. Список один на имя префаба и на
// имя класса компонента (см. marker_class_of).
static bool backpack_look_from_object_name(const char* raw, MarkerLook& look) {
    return for_each_name_token(raw, [&](const char* word) {
        if (strcmp(word, "backpack") == 0 || strcmp(word, "deathbag") == 0 ||
            strcmp(word, "lootbag") == 0 || strcmp(word, "dropbag") == 0 ||
            strcmp(word, "droppedbackpack") == 0) {
            look = special_look(ESP_MARKER_BACKPACK, "Рюкзак");
            return true;
        }
        return false;
    });
}

// Спящие (отключившиеся игроки): класс компонента вращается обфускатором
// (последнее читаемое имя — DisconnectedSleeper), поэтому опознаём и по
// имени префаба/панели. Слова — только целые «sleeper»/«sleepers»:
// «sleeping» это спальник, спящим он не бывает.
static bool sleeper_look_from_object_name(const char* raw, MarkerLook& look) {
    return for_each_name_token(raw, [&](const char* word) {
        if (strcmp(word, "sleeper") == 0 || strcmp(word, "sleepers") == 0) {
            look = special_look(ESP_MARKER_SLEEPER, "Спящий");
            return true;
        }
        return false;
    });
}

// Контейнер трупа игрока (LootObject): то, что остаётся после смерти, — по сути
// тот же рюкзак. Слова «corpse/player...» проверяются ТОЛЬКО у лут-компонентов:
// у Mineable «corpse» — это туша животного, она рюкзаком не становится.
static bool corpse_container_look_from_object_name(const char* raw, MarkerLook& look) {
    return for_each_name_token(raw, [&](const char* word) {
        if (strcmp(word, "corpse") == 0 || strcmp(word, "corpses") == 0 ||
            strcmp(word, "playercorpse") == 0 || strcmp(word, "playerloot") == 0 ||
            strcmp(word, "deathloot") == 0 || strcmp(word, "death") == 0 ||
            strcmp(word, "died") == 0) {
            look = special_look(ESP_MARKER_BACKPACK, "Рюкзак");
            return true;
        }
        return false;
    });
}

static bool is_vehicle_word(const char* word) {
    return strcmp(word, "car") == 0 || strcmp(word, "cars") == 0 ||
           strcmp(word, "vehicle") == 0 || strcmp(word, "vehicles") == 0;
}

// Cars whose entityType is empty: prefab name only.
static bool vehicle_look_from_object_name(const char* raw, MarkerLook& look) {
    return for_each_name_token(raw, [&](const char* word) {
        if (is_vehicle_word(word)) {
            look = vehicle_look("Машина");
            return true;
        }
        return false;
    });
}

// Hostile NPCs with an empty entityType. Same matching rules as the animals:
// whole words, plus a prefix match for 4+ letter tokens ("scientistpatrol").
static bool bot_look_from_object_name(const char* raw, MarkerLook& look) {
    static const struct { const char* word; } kBots[] = {
        {"bot"}, {"bots"}, {"npc"}, {"npcs"}, {"scientist"}, {"bandit"},
    };
    return for_each_name_token(raw, [&](const char* word) {
        size_t wlen = strlen(word);
        for (const auto& bot : kBots) {
            if (strcmp(word, bot.word) == 0) { look = bot_look("Бот"); return true; }
            size_t blen = strlen(bot.word);
            if (blen >= 4 && wlen > blen && strncmp(word, bot.word, blen) == 0) {
                look = bot_look("Бот");
                return true;
            }
        }
        return false;
    });
}

// Road signs: the smashable family (LootDestroyable) labels everything it
// cannot name "Ящик", and signs used to drown in there. Whole words only.
static bool roadsign_look_from_object_name(const char* raw, MarkerLook& look) {
    return for_each_name_token(raw, [&](const char* word) {
        if (strcmp(word, "roadsign") == 0 || strcmp(word, "roadsigns") == 0 ||
            strcmp(word, "signroad") == 0 ||
            strcmp(word, "sign") == 0 || strcmp(word, "signs") == 0) {
            look = kRoadSign;
            return true;
        }
        return false;
    });
}

// ---- World loot containers --------------------------------------------------
// Oxide.LootObject is used both for the loot scattered around the map (crates,
// barrels, airdrops) and for the storage boxes players deploy. Deployed boxes
// are what we must not draw, and they give themselves away twice: they own a
// Building.BuildingPiece component, and their prefab is named after the size
// ("WoodenBoxLarge", "Small Box"). Both signals are used.
struct LootNameInfo {
    const char* label = nullptr;
    int  rank = 0;
    bool rainbow = false;
    bool player_box = false;
    // Бочка: слово «barrel» в любом имени побеждает любые «loot/crate» — раньше
    // ранги сравнивались поровну (оба 2), и «loot» из «LootBarrel» забирал
    // бочку в секцию «Ящик». Теперь бочка всегда живёт в секции «Скрап».
    bool scrap = false;
    // Какое слово сказало «это контейнер игрока» и был ли это ящик: по этому
    // ящики в домах показываются («Большой/Обычный ящик»), а печки, кровати,
    // шкафы и спальники по-прежнему скрыты.
    const char* deploy_word = nullptr;
    bool saw_box = false, saw_size = false, saw_big = false;
};

static void scan_loot_name(const char* raw, LootNameInfo& info) {
    // Containers a player deploys — never interesting, always hidden. The
    // concatenated spellings matter because `panelName` is one lower-case word
    // ("largewoodbox") and never splits into tokens.
    static const char* const kDeployed[] = {
        "storage", "stash", "cupboard", "furnace", "campfire", "locker", "bed",
        "sleeping", "sleepingbag", "bedroll", "shelf", "planter", "composter",
        "fridge", "mailbox", "workbench", "quarry", "turret", "smelter",
        "barbecue", "oven", "wardrobe", "rack",
        "woodbox", "woodenbox", "largewoodbox", "smallwoodbox", "largebox",
        "smallbox", "bigbox", "storagebox", "toolcupboard", "toolbox2",
        // Sleeping players and the bag a killed player leaves behind are
        // lootable too, and they used to show up as an anonymous "Ящик".
        "corpse", "corpses", "ragdoll", "sleeper", "sleepers", "player",
        "players", "human", "survivor", "backpack", "deathbag", "death",
        "died", "grave", "skeleton", "playercorpse", "playerloot",
        "lootbag", "dropbag", "deathloot",
        // NOTE: never put a word here that a world container can also use.
        // "generic" was in this list for one build and it hid every barrel:
        // barrels open the plain "generic" loot panel.
    };
    // A "box" that also says how big it is, is a player box ("Large Box").
    static const char* const kSizeWords[] = {"small", "large", "big", "wood", "wooden", "medium", "mini"};
    // label table; a higher rank wins so "MilitaryCrate" beats plain "crate".
    static const struct { const char* word; const char* ru; int rank; bool rainbow; } kLabels[] = {
        // Elite / military crates first: they are the ones worth crossing the
        // map for, so every spelling the prefab or the loot panel might use is
        // listed and outranks the plain "crate" match below. The elite ones
        // are flagged rainbow: the overlay cycles their colour.
        {"military",     "Военный ящик",     4, false},
        {"militarycrate","Военный ящик",     4, false},
        {"milcrate",     "Военный ящик",     4, false},
        {"mil",          "Военный ящик",     4, false},
        {"army",         "Военный ящик",     4, false},
        {"soldier",      "Военный ящик",     4, false},
        {"elite",        "Элитный ящик",     4, true},
        {"elitecrate",   "Элитный ящик",     4, true},
        {"eliteloot",    "Элитный ящик",     4, true},
        {"epic",         "Элитный ящик",     4, true},
        {"legendary",    "Элитный ящик",     4, true},
        {"rare",         "Редкий ящик",      4, false},
        {"airdrop",    "Аирдроп",            3, false},
        {"supply",     "Аирдроп",            3, false},
        {"medical",    "Мед. ящик",          3, false},
        {"medkit",     "Мед. ящик",          3, false},
        {"ammo",       "Ящик патронов",      3, false},
        {"toolbox",    "Ящик инструментов",  3, false},
        {"food",       "Ящик с едой",        3, false},
        {"heli",       "Ящик с вертолёта",   3, false},
        {"helicopter", "Ящик с вертолёта",   3, false},
        {"bradley",    "Ящик с танка",       3, false},
        {"oilrig",     "Ящик с вышки",       3, false},
        {"hackable",   "Взломной ящик",      3, false},
        {"safe",       "Сейф",               3, false},
        {"cash",       "Касса",              3, false},
        {"register",   "Касса",              3, false},
        {"vending",    "Автомат",            3, false},
        {"crate",      "Ящик",               2, false},
        {"lootbox",    "Ящик",               2, false},
        {"loot",       "Ящик",               2, false},
        {"container",  "Контейнер",          2, false},
        {"chest",      "Сундук",             2, false},
        {"case",       "Кейс",               2, false},
        {"cache",      "Тайник",              2, false},
        {"trash",      "Мусорка",            2, false},
        {"garbage",    "Мусорка",            2, false},
    };
    switch (manes_aim_source::classify_entity_name(raw)) {
        case manes_aim_source::EntityName::EliteCrate:
            info.rank = 4; info.label = "Элитный ящик"; info.rainbow = true;
            break;
        case manes_aim_source::EntityName::MilitaryCrate:
            info.rank = 4; info.label = "Военный ящик";
            break;
        default:
            break;
    }
    for_each_name_token(raw, [&](const char* word) {
        // Слово-исключение больше не останавливает разбор: после него имя может
        // ещё сказать «box»/«large» (например «storage box»), а ящик в доме мы
        // теперь показываем.
        for (const char* bad : kDeployed)
            if (strcmp(word, bad) == 0) {
                info.player_box = true;
                info.deploy_word = bad;
                break;
            }
        if (strcmp(word, "box") == 0) info.saw_box = true;
        for (const char* size : kSizeWords)
            if (strcmp(word, size) == 0) { info.saw_size = true; break; }
        if (strcmp(word, "large") == 0 || strcmp(word, "big") == 0) info.saw_big = true;
        // Бочка — те же слова, что в barrel_look_from_object_name.
        if (strncmp(word, "barrel", 6) == 0 || strncmp(word, "barel", 5) == 0 ||
            strcmp(word, "bochka") == 0 || strcmp(word, "keg") == 0 ||
            strcmp(word, "drum") == 0)
            info.scrap = true;
        for (const auto& entry : kLabels) {
            if (strcmp(word, entry.word) == 0 && entry.rank > info.rank) {
                info.rank = entry.rank;
                info.label = entry.ru;
                info.rainbow = entry.rainbow;
            }
        }
        return false;
    });
    if (info.saw_box && info.saw_size) info.player_box = true;
    // Бочка перебивает «loot/crate/lootbox» (ранг 2), но не военные/элитные
    // ящики (ранг 4) — таких бочек в игре нет, но порядок честный.
    if (info.scrap) {
        info.rank = 3;
        info.label = "Бочка";
        info.rainbow = false;
    }
}

static bool loot_marker(uint64_t component, const char* object_name, const char* root_name,
                        const char* panel, MarkerLook& look) {
    // Контейнер, встроенный в постройку, — поставлен игроком. Раньше такие
    // скрывались целиком; теперь ЯЩИКИ в домах показываются («Большой
    // ящик»/«Обычный ящик», тумблер «Ящики в домах»), а прочая мебель (печки,
    // кровати, шкафы, спальники) и безымянные постройчные контейнеры —
    // по-прежнему нет.
    const bool building_piece = valid_obj(rd_ptr(component + LOOTOBJECT_BUILDING_PIECE));

    // Three name sources: the component's GameObject, the network object's
    // root GameObject, and the loot panel id the UI opens with. The panel is
    // often the only one that says "militarycrate" out loud.
    LootNameInfo info;
    scan_loot_name(object_name, info);
    if (!info.player_box) scan_loot_name(root_name, info);
    if (!info.player_box) scan_loot_name(panel, info);

    if (info.player_box || building_piece) {
        // Ящик ли это: исключительное слово с «box» (woodbox, storagebox,
        // largewoodbox...) либо имя, где «box» стоит вместе с размером/на
        // постройке («WoodenBox», «Small Box»).
        const bool says_box =
            (info.deploy_word && strstr(info.deploy_word, "box") != nullptr) ||
            (info.saw_box && (info.saw_size || building_piece));
        if (!says_box) return false;
        const bool big = info.saw_big ||
                         (info.deploy_word &&
                          (strstr(info.deploy_word, "large") != nullptr ||
                           strstr(info.deploy_word, "big") != nullptr));
        // Свой вид маркера: у ящиков в домах отдельный тумблер («Ящики в
        // домах»), независимый от «Ящиков» (мировые контейнеры).
        look.kind = ESP_MARKER_HOUSE;
        look.label = big ? "Большой ящик" : "Обычный ящик";
        look.rainbow = false;
        look.has_color = false;
        return true;
    }
    // No name we recognise -> not drawn. Everything anonymous down here is
    // something a player owns (a sleeping player, the bag a corpse leaves),
    // and labelling all of it "Ящик" is exactly the noise the loot ESP must
    // not produce. World containers always name themselves.
    if (info.rank <= 0 || !info.label) return false;

    look.kind = info.scrap ? ESP_MARKER_SCRAP : ESP_MARKER_LOOT;
    look.label = info.label;
    look.rainbow = info.rainbow;
    look.has_color = false;
    return true;
}

// Elements of a managed List<T> or T[] (the dump types these fields as `?`, so
// the shape is decided at runtime from the class name).
int read_managed_collection(uint64_t object, uint64_t* out, int max_items) {
    if (!valid_obj(object) || !out || max_items <= 0) return 0;
    uint64_t klass = rd_ptr(object);
    if (!valid_obj(klass)) return 0;
    uint64_t array = object;
    int32_t count = 0;
    bool name_readable = false;
    const std::string klass_name = read_remote_string(rd_ptr(klass + IL2CPP_CLASS_NAME), &name_readable);
    const bool is_list = name_readable
        ? klass_name.rfind("List`1", 0) == 0
        // Имя класса недоступно: List и массив различаем по структуре — у List на
        // 0x10 лежит массив элементов, у массива там bounds (обычно ноль).
        : valid_obj(rd_ptr(object + IL2CPP_LIST_ITEMS));
    if (is_list) {
        array = rd_ptr(object + IL2CPP_LIST_ITEMS);
        count = rd<int32_t>(object + IL2CPP_LIST_SIZE);
    } else {
        count = rd<int32_t>(object + IL2CPP_ARRAY_LENGTH);
    }
    if (!valid_obj(array) || count <= 0) return 0;
    if (count > max_items) count = max_items;
    if (!rd_buf(array + IL2CPP_ARRAY_FIRST_ELEMENT, out, (size_t)count * sizeof(uint64_t))) return 0;
    return count;
}

// One loot item short name -> the resource it identifies. Rank breaks ties:
// sulfur and metal nodes drop stones as well, so the richer resource wins, and
// wood/cloth/meat (trees, animals, bushes) rank 0 and are ignored here.
static int ore_look_for_item_name(const char* item_name, MarkerLook& look) {
    if (!item_name || !item_name[0]) return 0;
    char key[40];
    size_t n = 0;
    for (const char* p = item_name; *p && n + 1 < sizeof(key); ++p) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        key[n++] = c;
    }
    key[n] = '\0';
    // Processed items (barrels are stuffed with them) are not ore: without
    // this "metal.fragments" turned every barrel into an iron node.
    if (strstr(key, "frag") || strstr(key, "pipe") || strstr(key, "sheet") ||
        strstr(key, "scrap") || strstr(key, "spring") || strstr(key, "gear"))
        return 0;
    if (strstr(key, "sulfur")) { look = kOreSulfur; apply_ore_cfg(look, 2); return 3; }
    if (strstr(key, "metal"))  { look = kOreMetal;  apply_ore_cfg(look, 1); return 2; }
    if (strstr(key, "stone"))  { look = kOreStone;  apply_ore_cfg(look, 0); return 1; }
    return 0;
}

// Ore nodes do not always fill in entityType, but they always know what they
// drop: MineableObject.m_Loot is a list of Oxide.LootItem and LootItem.ItemName
// is the item short name ("stone", "metal.ore", "sulfur.ore", "wood", ...).
static bool marker_from_loot(uint64_t mineable, MarkerLook& look) {
    const uint64_t sources[2] = {MINEABLE_LOOT, MINEABLE_FINISH_BONUS};
    int best = 0;
    for (uint64_t source : sources) {
        uint64_t items[12];
        int count = read_managed_collection(rd_ptr(mineable + source), items, 12);
        for (int i = 0; i < count; ++i) {
            if (!valid_obj(items[i])) continue;
            char name[32] = {};
            if (!read_managed_string(rd_ptr(items[i] + LOOTITEM_ITEM_NAME), name, sizeof(name))) continue;
            MarkerLook candidate;
            int rank = ore_look_for_item_name(name, candidate);
            if (rank > best) { best = rank; look = candidate; }
            if (best == 3) return true;
        }
    }
    return best > 0;
}

// ---- Ground pickups ---------------------------------------------------------
// Oxide.ItemPickup carries the item short name ("cloth", "mushroom", ...) and
// the stack size, so the label needs nothing but a translation table. Matching
// is by substring because short names are dotted ("metal.fragments",
// "low.grade.fuel") and the specific entries are listed before the generic
// ones. Labels stay short on purpose: they share a 40 byte pill with the count.
static const char* pickup_label_for_item(const char* short_name) {
    static const struct { const char* key; const char* ru; } kItems[] = {
        // -- food and gatherables -------------------------------------------
        {"mushroom",       "Грибы"},        {"blueberr",       "Ягоды"},
        {"raspberr",       "Ягоды"},        {"berry",          "Ягоды"},
        // "berries" не содержит подстроку "berry" — нужен свой ключ; кириллица
        // в обоих регистрах, ASCII-лаускейс её не трогает.
        {"berri",          "Ягоды"},
        {"ягод",           "Ягоды"},        {"Ягод",           "Ягоды"},
        {"pumpkin",        "Тыква"},        {"corn",           "Кукуруза"},
        {"potato",         "Картофель"},    {"cactus",         "Кактус"},
        {"seed",           "Семена"},       {"hemp",           "Конопля"},
        {"granola",        "Батончик"},     {"chocolate",      "Шоколад"},
        {"candy",          "Конфета"},      {"apple",          "Яблоко"},
        {"bread",          "Хлеб"},         {"honey",          "Мёд"},
        {"egg",            "Яйцо"},         {"beans",          "Фасоль"},
        {"tuna",           "Тунец"},        {"soda",           "Газировка"},
        {"cola",           "Газировка"},    {"canned",         "Консервы"},
        {"can.",           "Консервы"},     {"chicken",        "Курятина"},
        {"meat",           "Мясо"},         {"fish",           "Рыба"},
        {"water",          "Вода"},         {"bottle",         "Бутылка"},
        // -- tools and weapons (before the resources: "Stone Hatchet" is a
        //    hatchet, not a stone) -------------------------------------------
        {"pickaxe",        "Кирка"},        {"pick axe",       "Кирка"},
        {"hatchet",        "Топор"},        {"axe",            "Топор"},
        {"hammer",         "Молоток"},      {"torch",          "Факел"},
        {"bucket",         "Ведро"},        {"jerry",          "Канистра"},
        {"explosive",      "Взрывчатка"},   {"grenade",        "Граната"},
        {"rocket",         "Ракета"},       {"c4",             "С4"},
        {"ammo",           "Патроны"},      {"arrow",          "Стрелы"},
        {"shell",          "Патроны"},      {"rifle",          "Винтовка"},
        {"pistol",         "Пистолет"},     {"revolver",       "Револьвер"},
        {"shotgun",        "Дробовик"},     {"crossbow",       "Арбалет"},
        {"bow",            "Лук"},          {"spear",          "Копьё"},
        {"machete",        "Мачете"},       {"knife",          "Нож"},
        {"helmet",         "Шлем"},         {"kevlar",         "Броня"},
        {"armor",          "Броня"},        {"armour",         "Броня"},
        {"hoodie",         "Одежда"},       {"jacket",         "Одежда"},
        {"shirt",          "Одежда"},       {"pants",          "Одежда"},
        {"boots",          "Одежда"},       {"gloves",         "Одежда"},
        {"mask",           "Одежда"},
        // -- resources -------------------------------------------------------
        {"cloth",          "Ткань"},        {"leather",        "Кожа"},
        {"fat",            "Жир"},          {"bone",           "Кости"},
        {"scrap",          "Скрап"},        {"sulfur",         "Сера"},
        {"high quality",   "Металл HQ"},    {"hq.metal",       "Металл HQ"},
        {"metal.refined",  "Металл HQ"},    {"metal frag",     "Фрагменты"},
        {"metal.frag",     "Фрагменты"},    {"fragment",       "Фрагменты"},
        {"sheet metal",    "Листы"},        {"sheetmetal",     "Листы"},
        {"sheet",          "Листы"},        {"metal",          "Металл"},
        {"stone",          "Камень"},       {"wood",           "Дерево"},
        {"charcoal",       "Уголь"},        {"coal",           "Уголь"},
        {"gunpowder",      "Порох"},        {"gun powder",     "Порох"},
        {"low.grade",      "Низкосорт"},    {"lowgrade",       "Низкосорт"},
        {"low grade",      "Низкосорт"},    {"crude",          "Нефть"},
        {"diesel",         "Солярка"},      {"fuel",           "Топливо"},
        {"tech.trash",     "Электроника"},  {"techtrash",      "Электроника"},
        {"tech trash",     "Электроника"},  {"battery",        "Батарея"},
        {"gear",           "Шестерни"},     {"spring",         "Пружина"},
        {"pipe",           "Труба"},        {"blade",          "Лезвие"},
        {"rope",           "Верёвка"},      {"tarp",           "Брезент"},
        {"propane",        "Пропан"},       {"sewing",         "Швейный"},
        {"glue",           "Клей"},         {"tape",           "Скотч"},
        // -- medical and the rest --------------------------------------------
        {"medkit",         "Аптечка"},      {"medical",        "Аптечка"},
        {"syringe",        "Шприц"},        {"bandage",        "Бинт"},
        {"antirad",        "Антирад"},      {"radiation",      "Антирад"},
        {"pills",          "Таблетки"},     {"blueprint",      "Чертёж"},
        {"wrench",         "Гаечный ключ"}, {"door",           "Дверь"},
        {"key",            "Ключ"},
    };
    if (!short_name || !short_name[0]) return nullptr;
    char key[48];
    size_t n = 0;
    for (const char* p = short_name; *p && n + 1 < sizeof(key); ++p) {
        char c = *p;
        key[n++] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    key[n] = '\0';
    // Exact short names from the release ItemDatabase. They run before the
    // fuzzy fallback so distinct items no longer collapse to generic labels
    // such as «Оружие», «Одежда» or an untranslated English display name.
    static const struct { const char* key; const char* ru; } kExact[] = {
        {"plank","Доски"}, {"stick","Палки"}, {"metal.ore","Металлическая руда"},
        {"metal.fragment","Фрагменты металла"}, {"sulfur.ore","Серная руда"},
        {"fat.animal","Животный жир"}, {"hq.metal.ore","Руда металла HQ"},
        {"building.plan","План постройки"}, {"building.hammer","Строительный молоток"},
        {"saw.ripper","Пила"}, {"chainsaw","Бензопила"}, {"jackhammer","Отбойный молоток"},
        {"campfire","Костёр"}, {"storage.box","Ящик"}, {"storage.box.large","Большой ящик"},
        {"furnace","Печь"}, {"sleeping.bag","Спальный мешок"}, {"bed","Кровать"},
        {"wooden.door","Деревянная дверь"}, {"door.metal","Металлическая дверь"},
        {"door.armored","Бронированная дверь"}, {"cupboard.tool","Шкаф"},
        {"workbench","Верстак"}, {"anvil","Наковальня"}, {"ceiling.light","Потолочная лампа"},
        {"turret","Турель"}, {"sam.turret","ПВО-турель"}, {"guntrap","Оружейная ловушка"},
        {"snowball.turret","Снежная турель"}, {"vending.machine","Торговый автомат"},
        {"mining.quarry","Карьер"}, {"sawmill.clan","Лесопилка"},
        {"scrap.recycler.clan","Переработчик"}, {"raw.meat","Сырое мясо"},
        {"cooked.meat","Жареное мясо"}, {"rotten.meat","Тухлое мясо"},
        {"raw.fish","Сырая рыба"}, {"cooked.fish","Жареная рыба"},
        {"raw.mushroms","Сырые грибы"}, {"cooked.mushroms","Жареные грибы"},
        {"blueberries","Черника"}, {"candy.cane","Леденец"},
        {"halloween.candy","Хэллоуинская конфета"}, {"ice.shard","Осколок льда"},
        {"event.cupids.potion","Зелье Купидона"}, {"event.love.elixir","Эликсир любви"},
        {"event.warming.potion","Согревающее зелье"}, {"event.chocolates","Шоколадные конфеты"},
        {"event.heart.fragment","Осколок сердца"}, {"easter.egg","Пасхальное яйцо"},
        {"aloe.food","Алоэ"}, {"cactus.food","Кактус"}, {"event.cheese","Сыр"},
        {"keylock","Ключевой замок"}, {"codelock","Кодовый замок"},
        {"high.wooden.wall","Высокая стена"},
        {"high.wooden.gates","Высокие ворота"}, {"window.wood","Окно"},
        {"wooden.spiked.club","Шипастая дубина"}, {"assault.rifle","Штурмовая винтовка"},
        {"desert.eagle","Дезерт Игл"}, {"hunting.rifle","Охотничья винтовка"},
        {"submachine.gun","Пистолет-пулемёт"}, {"flare.gun","Ракетница"},
        {"rocket.launcher","РПГ"}, {"explosive.charge","Взрывной заряд"},
        {"thompson","Томпсон"}, {"dmr","Винтовка DMR"}, {"winchester","Винчестер"},
        {"dvl","Снайперская винтовка"}, {"hmlmg","Пулемёт"},
        {"handmade.pistol","Самодельный пистолет"}, {"kriss.vector","Вектор"},
        {"steel.ball.gun","Шаромёт"}, {"fn.fal","Автомат FAL"},
        {"ammo.rifle","Патроны 5.56"}, {"ammo.pistol","Пистолетные патроны"},
        {"ammo.shotgun","Картечь 12 калибра"}, {"ammo.handmade.shell","Самодельные патроны"},
        {"sam.rocket","Ракета ПВО"}, {"event.grenade.smoke","Дымовая граната"},
        {"event.grenade.makeshift","Самодельная граната"}, {"tactical.air.marker","Авиамаркер"},
        {"grenade.military","Военная граната"}, {"weapon.mod.simple.scope","Простой прицел"},
        {"weapon.mod.holosight","Коллиматор"}, {"weapon.mod.optical.scope","Оптический прицел"},
        {"weapon.mod.compensator","Компенсатор"}, {"weapon.mod.muzzle.brake","Дульный тормоз"},
        {"weapon.mod.ext.magazine","Увеличенный магазин"}, {"cctv.camera","Камера наблюдения"},
        {"empty.beans.can","Пустая банка фасоли"}, {"empty.propane.tank","Пустой баллон"},
        {"empty.tuna.can","Пустая банка тунца"}, {"fuel.bottle","Бутылка топлива"},
        {"metal.blade","Металлическое лезвие"}, {"metal.pipe","Металлическая труба"},
        {"metal.spring","Метал. пружина"}, {"rifle.body","Корпус винтовки"},
        {"smg.body","Корпус ПП"}, {"dmr.body","Корпус DMR"}, {"road.signs","Дорожные знаки"},
        {"sewing.kit","Швейный набор"}, {"targeting.computer","Компьютер наведения"},
        {"hard.drive","Жёсткий диск"}, {"microprocessor","Микропроцессор"},
        {"portable.playstation","Портативная консоль"}, {"wires","Провода"},
        {"wooden.helmet","Деревянный шлем"}, {"bone.helmet","Костяной шлем"},
        {"leather.helmet","Кожаный шлем"}, {"metal.helmet","Металлическая маска"},
        {"wood.chestplate","Деревянный нагрудник"}, {"bone.chestplate","Костяной нагрудник"},
        {"leather.chestplate","Кожаный нагрудник"}, {"metal.chestplate","Метал. нагрудник"},
        {"wooden.leggings","Деревянные поножи"}, {"bone.leggings","Костяные поножи"},
        {"leather.leggings","Кожаные поножи"}, {"metal.leggings","Металлические поножи"},
        {"wooden.sandals","Деревянные сандалии"}, {"leather.boots","Кожаные ботинки"},
        {"metal.boots","Метал. ботинки"}, {"bone.boots","Костяные ботинки"},
        {"backpack","Рюкзак"}, {"backpack.big","Большой рюкзак"},
        {"hazmat","Химзащита"}, {"military.suit","Военный костюм"},
        {"christmas.lights","Гирлянда"}, {"fireworks","Фейерверк"},
        {"scarecrow","Пугало"}, {"christmas.tree","Ёлка"}, {"snow.machine","Снежная машина"},
        {"ice.statue","Ледяная статуя"}, {"statue.shop.stone","Каменная статуя"},
        {"winter.sleigh.frosty.shop","Сани"}, {"pinata.decor","Пиньята"},
        {"owl.statue","Статуя совы"}, {"event.textbook","Учебник"}, {"event.whistle","Свисток"},
    };
    if (!lang::english())
        for (const auto& item : kExact)
            if (strcmp(key, item.key) == 0) return item.ru;
    for (const auto& item : kItems)
        if (strstr(key, item.key)) return item.ru;
    return nullptr;
}

// Bushes, mushroom and berry clusters are harvested rather than picked up, so
// they arrive as MineableObjects whose loot says what they give. Trees (wood)
// stay out — they would bury the screen.
static int gather_look_for_item_name(const char* item_name, MarkerLook& look) {
    if (!item_name || !item_name[0]) return 0;
    char key[40];
    size_t n = 0;
    for (const char* p = item_name; *p && n + 1 < sizeof(key); ++p) {
        char c = *p;
        key[n++] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    key[n] = '\0';
    static const struct { const char* key; const char* ru; int rank; } kGathers[] = {
        {"mushroom", "Грибы",      3},
        {"berry",    "Ягоды",      3},
        {"berri",    "Ягоды",      3}, // "berries"/"blueberries"
        {"ягод",     "Ягоды",      3}, // русское имя: ASCII-лаускейс не трогает
        {"Ягод",     "Ягоды",      3}, // кириллицу, поэтому оба регистра
        {"pumpkin",  "Тыква",      3},
        {"corn",     "Кукуруза",   3},
        {"potato",   "Картофель",  3},
        {"hemp",     "Куст ткани", 2}, // seed.hemp comes from the cloth bush
        {"cloth",    "Куст ткани", 2},
    };
    for (const auto& gather : kGathers) {
        if (strstr(key, gather.key)) {
            look.kind = ESP_MARKER_PICKUP;
            look.label = gather.ru;
            look.has_color = false;
            return gather.rank;
        }
    }
    return 0;
}

// Same walk as marker_from_loot(), but for the gatherables table.
static bool gather_marker_from_loot(uint64_t mineable, MarkerLook& look) {
    const uint64_t sources[2] = {MINEABLE_LOOT, MINEABLE_FINISH_BONUS};
    int best = 0;
    for (uint64_t source : sources) {
        uint64_t items[12];
        int count = read_managed_collection(rd_ptr(mineable + source), items, 12);
        for (int i = 0; i < count; ++i) {
            if (!valid_obj(items[i])) continue;
            char name[32] = {};
            if (!read_managed_string(rd_ptr(items[i] + LOOTITEM_ITEM_NAME), name, sizeof(name))) continue;
            MarkerLook candidate;
            int rank = gather_look_for_item_name(name, candidate);
            if (rank > best) { best = rank; look = candidate; }
        }
    }
    return best > 0;
}

// One dropped / spawned item on the ground.
static bool pickup_marker(uint64_t component, char* label, size_t label_cap) {
    char short_name[48] = {};
    if (!read_managed_string_ex(rd_ptr(component + ITEMPICKUP_SHORTNAME), short_name,
                                sizeof(short_name), 47))
        return false;
    const char* translated = pickup_label_for_item(short_name);
    // Unknown short name: the game's own display name ("Blue Berry", "Mushroom")
    // goes through the same table -- most of the items that stayed English did
    // so because their short name is spelled differently from their name. Only
    // if that misses too is the English name shown as-is, so nothing on the
    // ground is ever silently dropped.
    char fallback[40] = {};
    if (!translated) {
        uint64_t item = rd_ptr(component + ITEMPICKUP_ITEM_OBJECT);
        uint64_t data = valid_obj(item) ? rd_ptr(item + ITEM_DATA) : 0;
        if (valid_obj(data)) read_managed_string(rd_ptr(data + ITEMDATA_NAME), fallback, sizeof(fallback));
        translated = pickup_label_for_item(fallback);
    }
    if (!translated) {
        if (!fallback[0]) snprintf(fallback, sizeof(fallback), "%.30s", short_name);
        translated = fallback;
    }
    // Перевод — до «x12»: количество дописывается к готовой подписи, и в
    // английском она получается такой же короткой, как в русском.
    const char* shown = lang::visual(translated);
    int32_t amount = rd<int32_t>(component + ITEMPICKUP_AMOUNT);
    if (amount > 1 && amount < 1000000)
        snprintf(label, label_cap, "%s x%d", shown, (int)amount);
    else
        snprintf(label, label_cap, "%s", shown);
    return label[0] != '\0';
}

// Il2CppClass -> which of the two component families this is (cached: the same
// handful of classes come back for every entity in the registry).


uint8_t marker_class_of(uint64_t klass) {
    if (!valid_obj(klass)) return MARKER_CLASS_NONE;
    auto found = g_marker_class_kind.find(klass);
    if (found != g_marker_class_kind.end()) return found->second;
    bool readable = false;
    const std::string name = read_remote_string(rd_ptr(klass + IL2CPP_CLASS_NAME), &readable);
    uint8_t kind = readable ? MARKER_CLASS_NONE : MARKER_CLASS_UNKNOWN;
    if (readable) {
        const auto recovered_name = manes_aim_source::classify_entity_name(name.c_str());
        if (name.rfind("Mineable", 0) == 0 ||
            recovered_name == manes_aim_source::EntityName::CompositeStone)
            kind = MARKER_CLASS_MINEABLE;
        else if (name == "LootObject" || name == "PumpkinTrick")
            kind = MARKER_CLASS_LOOT;
        else if (recovered_name == manes_aim_source::EntityName::ItemPickup)
            kind = MARKER_CLASS_PICKUP;
        else if (name == "AutoTurret" || name == "AutoSnowballTurret" ||
                 name == "AutoTurretShotgun" || name == "SamTurret")                             kind = MARKER_CLASS_TURRET;
        else if (name == "DisconnectedSleeper")                   kind = MARKER_CLASS_SLEEPER;
        // Шкаф: класс "Cupboard" не обфусцирован в обоих дампах (11946/11943);
        // TutorialCupboard — его наследник с учебного острова, подпись отличается.
        else if (name == "Cupboard" || name == "TutorialCupboard")
                                                                  kind = MARKER_CLASS_CUPBOARD;
        // Smashable scrap barrels: on this build their component class is
        // "LootDestroyable" (log: skip-class 'LootDestroyable' go='Barrel_v2') —
        // not a Mineable and not a LootObject. Any class that says Barrel out
        // loud is kept as a safety net for other builds.
        else if (name == "LootDestroyable" ||
                 name.find("Barrel") != std::string::npos)      kind = MARKER_CLASS_BARREL;
        // Машины: класс компонента (CarController, BaseVehicle, ...) — проход
        // режет camelCase на слова, поэтому Cargo/Carepackage сюда не попадают.
        // Раньше такие классы уходили в NONE и отсекались до чтения имён.
        else if (for_each_name_token(name.c_str(), is_vehicle_word)) kind = MARKER_CLASS_VEHICLE;
    }
    if (g_marker_class_kind.size() < 512) g_marker_class_kind[klass] = kind;
    return kind;
}

// Шкаф (Oxide.Cupboard): подпись «Шкаф ×N · Hч» — число авторизованных игроков
// и остаток защиты (protectedTime, SyncVar, секунды). Учебный шкаф помечается
// суффиксом, чтобы не путать с рейдовыми целями. Семантика protectedTime
// (окно защиты vs время до распада) по дампу не подтверждена — читаем как
// часы и подавляем неправдоподобное (> 30 суток: значение-похожее-на-штамп
// или мусор при переинициализации SyncVar).
static bool cupboard_marker(uint64_t component, char* out, size_t cap) {
    if (!out || cap == 0) return false;
    const bool tutorial = object_class_name_is(component, "TutorialCupboard");
    int auth_count = 0;
    const uint64_t auth_list = rd_ptr(component + CUPBOARD_AUTHORIZED_PLAYERS);
    if (valid_obj(auth_list)) {
        const int32_t n = rd<int32_t>(auth_list + IL2CPP_LIST_SIZE);
        if (n > 0 && n < 4096) auth_count = n;
    }
    int protected_hours = -1;
    const float protected_sec = rd<float>(component + CUPBOARD_PROTECTED_TIME);
    if (std::isfinite(protected_sec) && protected_sec > 0.0F) {
        const int hours = (int)(protected_sec / 3600.0F);
        if (hours <= 24 * 30) protected_hours = hours;
    }
    const char* base = lang::visual(tutorial ? "Шкаф (учеб.)" : "Шкаф");
    int len = snprintf(out, cap, "%s", base);
    if (auth_count > 0 && len > 0 && (size_t)len < cap)
        len += snprintf(out + len, cap - (size_t)len, " \xc3\x97%d", auth_count); // "×N"
    if (protected_hours >= 0 && len > 0 && (size_t)len < cap)
        len += snprintf(out + len, cap - (size_t)len, " %d%s", protected_hours,
                        lang::english() ? "h" : "\xd1\x87"); // "ч"/"h"
    return out[0] != '\0';
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

static MarkerScanResult rebuild_marker_entities() {
    auto unavailable = []() {
        marker_scan_abort();
        return MarkerScanResult::Unavailable;
    };
    if (g_marker_scan_cursor < 0) {
        // Начало цикла: действующий список НЕ трогаем — он показывается до
        // завершения сборки, поэтому маркеры не мигают.
        g_marker_scan_pending.clear();
        g_marker_scan_buffer.clear();
        uint64_t dictionary = resolve_network_client_spawned();
        if (!dictionary) return unavailable();
        // Needed to read prefab names (wolves / rats); harmless if it fails, the
        // loot and entityType paths still work.
        if (!g_go_name_offset_valid && g_local_player) ensure_gameobject_name_offset(g_local_player);
        g_marker_scan_identity_class = resolve_network_identity_class();

        int32_t count = 0;
        if (!rd_exact(dictionary + DICT_COUNT, count) || count < 0) return unavailable();
        if (count == 0) {
            g_marker_entities.clear();
            marker_scan_abort();
            return MarkerScanResult::Complete;
        }
        uint64_t entries = rd_ptr(dictionary + DICT_ENTRIES);
        if (!valid_obj(entries)) return unavailable();
        if (count > kMarkerRegistryLimit) count = kMarkerRegistryLimit;

        g_marker_scan_buffer.resize((size_t)count * DICT_ENTRY_STRIDE);
        if (!rd_buf(entries + IL2CPP_ARRAY_FIRST_ELEMENT, g_marker_scan_buffer.data(),
                    g_marker_scan_buffer.size()))
            return unavailable();
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

        uint64_t death_handler = 0;
        // NPCBrainHumanoid — единственный путь к модели бота, а через неё к
        // точной кости головы (modelInfo -> head). Ищется тем же проходом, что
        // и обработчик смерти: оба — соседи Mineable-компонента в behaviours.
        uint64_t bot_brain = 0;
        bool bot_components_searched = false;
        // Mirror collects behaviours with GetComponentsInChildren, so one
        // identity can carry a whole rock cluster: every Mineable* component
        // becomes its own marker, positioned by its own GameObject.
        for (int32_t b = 0; b < behaviour_count; ++b) {
            uint64_t component = behaviours[b];
            if (!valid_obj(component)) continue;
            // Заметка для crash_handler: скан — самое дорогое место кадра, и
            // падение в нём без узла нечем локализовать (лог 11:54:21 ушёл в
            // stage=menu, а стартовал скан из hud-пилюли).
            const uint8_t component_class = marker_class_of(rd_ptr(component));
            // Класс читаем, но семья не опознана: почти всегда это сервисный
            // компонент (аниматоры, менеджеры игрока), который маркером не
            // становится. Но после переименования классов обфускатором сюда
            // же попадают рюкзаки и спящие — поэтому ДО отбраковки проверяем
            // имя самого компонента (одно чтение): рюкзак/спящий опознаются
            // по префабу и без имени класса.
            MarkerLook look;
            char pickup_text[40] = {};
            char object_name[48] = {}, root_name[48] = {};
            bool known = false;
            if (component_class == MARKER_CLASS_NONE) {
                char none_name[48] = {};
                if (!managed_component_gameobject_name(component, none_name, sizeof(none_name)) ||
                    !(sleeper_look_from_object_name(none_name, look) ||
                      backpack_look_from_object_name(none_name, look)))
                    continue;
                known = true;   // рюкзак/спящий с переименованным классом
            }
            // Имя класса компонента может быть недоступно (см. marker_class_of):
            // тогда семейство неизвестно, и объект опознаётся по имени GameObject —
            // оно лежит в куче игры и читается всегда. Пробуем семейства по порядку.
            const bool family_unknown = (component_class == MARKER_CLASS_UNKNOWN);
            // «Дорогой» компонент: имя GameObject, трансформ и позиция — это
            // десятки обращений к памяти игры, поэтому время порции проверяется
            // снаружи, между узлами.
            managed_component_gameobject_name(component, object_name, sizeof(object_name));
            managed_component_gameobject_name(identity, root_name, sizeof(root_name));
            if (component_class == MARKER_CLASS_TURRET) {
                look = special_look(ESP_MARKER_TURRET, "Турель");
                known = true;
            } else if (component_class == MARKER_CLASS_SLEEPER) {
                look = special_look(ESP_MARKER_SLEEPER, "Спящий");
                known = true;
            } else if (component_class == MARKER_CLASS_CUPBOARD) {
                // Шкаф: класс знаем точно, имена GameObject не нужны. Подпись
                // динамическая (авторизованные + часы защиты) — как у пикапов:
                // look.label = nullptr, текст уезжает в entity.text.
                known = cupboard_marker(component, pickup_text, sizeof(pickup_text));
                if (known) {
                    look.kind = ESP_MARKER_CUPBOARD;
                    look.label = nullptr;
                    look.has_color = false;
                }
            }
            // Рюкзаки и спящие: их классы вращаются обфускатором, а списки
            // исключения (skip_object_name в ветке Mineable и kDeployed в
            // разборе имён лута) выкидывают их раньше всех прочих проверок —
            // поэтому опознаём по именам ДО этих списков. Панель лута часто
            // единственное место, где сказано «backpack»/«sleeper», поэтому
            // читаем её здесь же и дальше отдаём в loot_marker без перечитывания.
            char loot_panel[32] = {};
            if (component_class == MARKER_CLASS_LOOT || family_unknown)
                read_managed_string(rd_ptr(component + LOOTOBJECT_PANEL_NAME),
                                    loot_panel, sizeof(loot_panel));
            if (!known && (family_unknown || component_class == MARKER_CLASS_LOOT ||
                           component_class == MARKER_CLASS_MINEABLE ||
                           component_class == MARKER_CLASS_BARREL)) {
                if (sleeper_look_from_object_name(object_name, look) ||
                    sleeper_look_from_object_name(root_name, look) ||
                    backpack_look_from_object_name(object_name, look) ||
                    backpack_look_from_object_name(root_name, look) ||
                    sleeper_look_from_object_name(loot_panel, look) ||
                    backpack_look_from_object_name(loot_panel, look))
                    known = true;
                // Контейнер трупа — только у лут-компонентов: у Mineable «corpse»
                // это туша животного (её выкидывает skip_object_name ниже).
                if (!known && (component_class == MARKER_CLASS_LOOT || family_unknown) &&
                    (corpse_container_look_from_object_name(object_name, look) ||
                     corpse_container_look_from_object_name(root_name, look) ||
                     corpse_container_look_from_object_name(loot_panel, look)))
                    known = true;
            }
            if (!known && (component_class == MARKER_CLASS_PICKUP || family_unknown) &&
                g_markers_pickup_enabled) {
                known = pickup_marker(component, pickup_text, sizeof(pickup_text));
                if (known) {
                    look.kind = ESP_MARKER_PICKUP;
                    look.label = nullptr;
                    look.has_color = false;
                }
            }
            if (!known && (component_class == MARKER_CLASS_BARREL || family_unknown) &&
                (g_markers_loot_enabled || g_markers_scrap_enabled)) {
                if (g_markers_scrap_enabled)
                    known = barrel_look_from_object_name(object_name, look) ||
                            barrel_look_from_object_name(root_name, look);
                if (!known && g_markers_scrap_enabled)
                    known = roadsign_look_from_object_name(object_name, look) ||
                            roadsign_look_from_object_name(root_name, look);
                if (!known && component_class == MARKER_CLASS_BARREL && g_markers_scrap_enabled) {
                    // Безымянный LootDestroyable — всё равно семья бочек/знаков:
                    // бочки живут только в секции «Скрап», «Ящиком» они не
                    // становятся ни при каких тумблерах.
                    look = kBarrel; known = true;
                }
            }
            if (!known && (component_class == MARKER_CLASS_LOOT || family_unknown) &&
                (g_markers_loot_enabled || g_markers_scrap_enabled || g_markers_house_enabled)) {
                known = loot_marker(component, object_name, root_name, loot_panel, look);
                if (known && look.kind == ESP_MARKER_SCRAP && !g_markers_scrap_enabled) known = false;
                if (known && look.kind == ESP_MARKER_LOOT && !g_markers_loot_enabled) known = false;
                // Ящики в домах фильтруются при отрисовке, не здесь: их
                // список живёт вместе с loot-семьёй и не требует перескана.
            }
            if (!known && component_class == MARKER_CLASS_VEHICLE) {
                // Машина — только если И класс, И имя говорят «машина»:
                // общий Vehicle-класс висит и на коптерах, а гаражные
                // постройки — тоже «car-что-то». entityType здесь не читаем:
                // оффсет у немаинбл-компонента указывает в чужое поле.
                // Тумблера нет, как у руды и ботов: опознаётся всегда,
                // фильтруется при отрисовке (esp_get_markers).
                if ((object_name[0] && skip_object_name(object_name)) ||
                    (root_name[0] && skip_object_name(root_name)))
                    continue;
                if (object_name[0]) known = vehicle_look_from_object_name(object_name, look);
                if (!known && root_name[0]) known = vehicle_look_from_object_name(root_name, look);
            }
            if (!known && (component_class == MARKER_CLASS_MINEABLE || family_unknown)) {
                // The prefab name is asked first on purpose: animals that the
                // EntityType enum does not know (wolf, rat, ...) are shipped
                // with a borrowed entityType -- the wolf prefab says "Boar" --
                // so trusting the enum first labelled every wolf as a boar.
                if (object_name[0]) known = animal_look_from_object_name(object_name, look);
                if (!known && root_name[0]) known = animal_look_from_object_name(root_name, look);
                // Багги и трупы маркерами не становятся вообще (см.
                // skip_object_name); проверка раньше типа — borrowed-type.
                if (!known && ((object_name[0] && skip_object_name(object_name)) ||
                               (root_name[0] && skip_object_name(root_name))))
                    continue;
                if (!known && object_name[0]) known = recovered_ore_look(object_name, look);
                if (!known && root_name[0]) known = recovered_ore_look(root_name, look);
                if (!known) {
                    int32_t entity_type = rd<int32_t>(component + MINEABLE_ENTITY_TYPE);
                    known = marker_for_entity_type(entity_type, look);
                }
                if (!known && g_markers_scrap_enabled && object_name[0]) known = barrel_look_from_object_name(object_name, look);
                if (!known && g_markers_scrap_enabled && root_name[0])   known = barrel_look_from_object_name(root_name, look);
                // Бестиповые машины / боты / знаки: только по имени префаба.
                if (!known && object_name[0]) known = vehicle_look_from_object_name(object_name, look);
                if (!known && root_name[0])   known = vehicle_look_from_object_name(root_name, look);
                if (!known && object_name[0]) known = bot_look_from_object_name(object_name, look);
                if (!known && root_name[0])   known = bot_look_from_object_name(root_name, look);
                if (!known && g_markers_scrap_enabled && object_name[0]) known = roadsign_look_from_object_name(object_name, look);
                if (!known && g_markers_scrap_enabled && root_name[0])   known = roadsign_look_from_object_name(root_name, look);
                if (!known) known = marker_from_loot(component, look);
                // Cloth bushes, mushroom and berry clusters: harvestable, so
                // they are Mineables, but they belong to the pickup category.
                if (!known && g_markers_pickup_enabled) known = gather_marker_from_loot(component, look);
            }
            if (!known) continue;

            MarkerEntity entity;
            entity.identity = identity;
            entity.id = component;
            entity.kind = look.kind;
            if (entity.kind == ESP_MARKER_BOT) {
                // The death handler is a sibling behaviour, not the Mineable
                // component used to classify the marker. Never read 0xE8 on it.
                if (!bot_components_searched) {
                    bot_components_searched = true;
                    for (int32_t h = 0; h < behaviour_count; ++h) {
                        if (!valid_obj(behaviours[h])) continue;
                        if (!death_handler &&
                            object_class_name_is(behaviours[h], "NPCHumanoidDeathHandler"))
                            death_handler = behaviours[h];
                        if (!bot_brain &&
                            object_class_name_is(behaviours[h], "NPCBrainHumanoid"))
                            bot_brain = behaviours[h];
                        if (death_handler && bot_brain) break;
                    }
                }
                entity.life.handler = death_handler;
                if (bot_brain) {
                    const uint64_t model_info = rd_ptr(bot_brain + NPC_BRAIN_MODEL_INFO);
                    if (valid_obj(model_info))
                        entity.head_transform =
                            managed_object_native(rd_ptr(model_info + MODELINFO_HEAD));
                }
            }
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
            for (const MarkerEntity& old : g_marker_entities) {
                if (old.id != component || old.identity != identity) continue;
                if (entity.kind == ESP_MARKER_BOT && old.life.handler == entity.life.handler)
                    entity.life.dead = old.life.dead;
                if (old.transform == entity.transform) {
                    entity.position = old.position;
                    entity.position_valid = old.position_valid;
                    entity.position_sample_time = old.position_sample_time;
                    entity.position_retry_time = old.position_retry_time;
                    entity.position_failure_time = old.position_failure_time;
                    if (entity.kind == ESP_MARKER_BOT) {
                        entity.lead = old.lead;
                        if (old.head_transform == entity.head_transform) {
                            entity.head_position = old.head_position;
                            entity.head_valid = old.head_valid;
                        }
                    }
                }
                break;
            }
            if (entity.kind == ESP_MARKER_BOT) marker_bot_dead(entity);
            if (entity.transform) g_marker_scan_pending.push_back(entity);
            if (g_marker_scan_pending.size() >= kMarkerEntityLimit) break;
        }
        if (g_marker_scan_pending.size() >= kMarkerEntityLimit) break;
    }

    if (i < count && g_marker_scan_pending.size() < kMarkerEntityLimit) {
        // Порция израсходована (вышло время), цикл не закончен — продолжим на
        // следующем кадре с этого же узла.
        g_marker_scan_cursor = i;
        return MarkerScanResult::Pending;
    }
    // Конец цикла: словарь пройден либо уперлись в защитный лимит списка.
    // Порядок — по стабильному ключу, а не по порядку реестра: словарь игры
    // при ресайзе тасует узлы, и без этого прореживание пилюль после каждого
    // рескана перевыбирало бы победителя (метка прыгала бы между соседями).
    std::sort(g_marker_scan_pending.begin(), g_marker_scan_pending.end(),
              [](const MarkerEntity& a, const MarkerEntity& b) {
                  if (a.identity != b.identity) return a.identity < b.identity;
                  if (a.transform != b.transform) return a.transform < b.transform;
                  return a.id < b.id;
              });
    g_marker_entities.swap(g_marker_scan_pending);
    g_marker_position_cursor = 0;
    marker_scan_abort();
    return MarkerScanResult::Complete;
}

static void schedule_next_marker_scan(MarkerScanResult result, double now) {
    if (result == MarkerScanResult::Pending) g_marker_next_scan = now;
    else if (result == MarkerScanResult::Unavailable) g_marker_next_scan = now + 0.25;
    else g_marker_next_scan = now + (g_marker_entities.empty() ? 0.25 : 1.0);
}

void reset_marker_caches() {
    g_marker_entities.clear();
    g_marker_next_scan = 0.0;
    g_marker_next_layout_probe = 0.0;
    g_marker_position_cursor = 0;
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

// Use the model head transform when it produces a point close to the bot root.
static void marker_track_bot_head(MarkerEntity& entity) {
    entity.head_valid = false;
    if (!entity.head_transform || !entity.position_valid) return;

    Vec3 head{};
    if (!marker_world_position(entity.head_transform, head) ||
        head.y <= entity.position.y + 0.4F || head.y >= entity.position.y + 2.6F)
        return;
    const float dx = head.x - entity.position.x;
    const float dz = head.z - entity.position.z;
    if (dx * dx + dz * dz >= 2.5F * 2.5F) return;

    entity.head_position = head;
    entity.head_valid = true;
}

static bool marker_position_moves(const MarkerEntity& entity) {
    return entity.kind == ESP_MARKER_ANIMAL || entity.kind == ESP_MARKER_BOT ||
           entity.kind == ESP_MARKER_VEHICLE;
}

static double marker_sample_interval(const MarkerEntity& entity) {
    return marker_position_moves(entity) ? kMovingMarkerIntervalSec
                                         : kStaticMarkerIntervalSec;
}

static bool marker_position_usable(const MarkerEntity& entity, double now) {
    if (!entity.position_valid) return false;
    if (entity.position_failure_time > 0.0 &&
        !espsnapshot::within_age(now, entity.position_failure_time,
                                kMarkerPositionGraceSec))
        return false;
    if (marker_position_moves(entity)) {
        const double reference_time = entity.position_failure_time > 0.0
                                    ? entity.position_failure_time
                                    : entity.position_sample_time;
        if (!espsnapshot::within_age(now, reference_time, kMarkerPositionGraceSec))
            return false;
    }
    return true;
}

static bool marker_track_position(MarkerEntity& entity, bool allow_read = true) {
    const double now = mono_seconds();
    const double interval = marker_sample_interval(entity);
    if (now < entity.position_retry_time ||
        (entity.position_valid &&
         !espsnapshot::due(now, entity.position_sample_time, interval)))
        return marker_position_usable(entity, now);

    if (!allow_read) return marker_position_usable(entity, now);

    Vec3 sampled{};
    if (!marker_world_position(entity.transform, sampled)) {
        if (entity.position_failure_time <= 0.0) entity.position_failure_time = now;
        entity.position_retry_time = now + std::min(interval, 0.10);
        entity.head_valid = false;
        return marker_position_usable(entity, now);
    }

    entity.position = sampled;
    entity.position_valid = true;
    entity.position_sample_time = now;
    entity.position_retry_time = now + interval;
    entity.position_failure_time = 0.0;
    if (entity.kind == ESP_MARKER_BOT) {
        botlead::update(entity.lead, sampled, now);
        marker_track_bot_head(entity);
    }
    return true;
}

static bool marker_visual_enabled(const MarkerEntity& entity) {
    switch (entity.kind) {
        case ESP_MARKER_ORE: return g_markers_ore_enabled;
        case ESP_MARKER_ANIMAL: return g_markers_animal_enabled;
        case ESP_MARKER_LOOT: return g_markers_loot_enabled;
        case ESP_MARKER_SCRAP: return g_markers_scrap_enabled;
        case ESP_MARKER_PICKUP: return g_markers_pickup_enabled;
        case ESP_MARKER_VEHICLE: return g_markers_vehicle_enabled;
        case ESP_MARKER_BOT: return g_markers_bot_enabled;
        case ESP_MARKER_TURRET: return g_markers_turret_enabled;
        case ESP_MARKER_BACKPACK: return g_markers_backpack_enabled;
        case ESP_MARKER_SLEEPER: return g_markers_sleeper_enabled;
        case ESP_MARKER_HOUSE: return g_markers_house_enabled;
        case ESP_MARKER_CUPBOARD: return g_markers_cupboard_enabled;
        default: return false;
    }
}

std::vector<EspMarker> esp_get_markers() {
    std::vector<EspMarker> result;
    if (!marker_visuals_enabled() && !g_bot_aim_enabled) {
        g_marker_entities.clear();
        g_marker_next_scan = 0.0;
        g_marker_position_cursor = 0;
        marker_scan_abort();
        return result;
    }
    if (g_pid <= 0 || !g_il2cpp_base || !g_frame_vp_valid || !g_frame_local_valid)
        return result;

    const double scan_now = mono_seconds();
    if (scan_now >= g_marker_next_scan)
        schedule_next_marker_scan(rebuild_marker_entities(), scan_now);

    if (!g_skeleton_layout_valid && !g_marker_entities.empty() &&
        (scan_now >= g_marker_next_layout_probe || layout_scan_in_progress())) {
        bool any_position = false;
        std::vector<uint64_t> seeds;
        seeds.reserve(8);
        for (const MarkerEntity& entity : g_marker_entities) {
            if (entity.transform && seeds.size() < 8) seeds.push_back(entity.transform);
            Vec3 sample{};
            if (entity.transform && marker_world_position(entity.transform, sample)) {
                any_position = true;
                break;
            }
        }
        if (!any_position && !seeds.empty()) {
            size_t position_count = 0;
            size_t candidate_count = 0;
            const bool learned = discover_layout_from_native_transforms(
                seeds, position_count, candidate_count);
            g_marker_next_layout_probe = scan_now + (learned ? 30.0 : 1.0);
            if (!learned && layout_scan_in_progress()) g_marker_next_layout_probe = scan_now;
        }
    }

    const size_t entity_count = g_marker_entities.size();
    if (entity_count != 0) {
        const size_t start = g_marker_position_cursor % entity_count;
        g_marker_position_cursor = start;
        const double budget_start = mono_seconds();
        for (size_t step = 0; step < entity_count; ++step) {
            const size_t index = (start + step) % entity_count;
            if (mono_seconds() - budget_start >= kMarkerPositionBudgetSec) {
                g_marker_position_cursor = index;
                break;
            }
            MarkerEntity& entity = g_marker_entities[index];
            if (marker_visual_enabled(entity) ||
                (entity.kind == ESP_MARKER_BOT && g_bot_aim_enabled)) {
                if (!marker_bot_dead(entity)) marker_track_position(entity);
            }
            g_marker_position_cursor = (index + 1) % entity_count;
        }
    }

    const double position_now = mono_seconds();
    const float max_distance = g_marker_max_distance;
    const float screen_margin = std::max(g_frame_sw, g_frame_sh) * 0.25F;
    for (MarkerEntity& entity : g_marker_entities) {
        if (!marker_visual_enabled(entity) || marker_bot_dead(entity) ||
            !marker_position_usable(entity, position_now))
            continue;

        const float dx = entity.position.x - g_frame_local_pos.x;
        const float dy = entity.position.y - g_frame_local_pos.y;
        const float dz = entity.position.z - g_frame_local_pos.z;
        const float distance = sqrtf(dx * dx + dy * dy + dz * dz);
        if (!std::isfinite(distance) || distance > max_distance) continue;

        Vec3 anchor = entity.position;
        anchor.y += (entity.kind == ESP_MARKER_ANIMAL)  ? 1.2F
                  : (entity.kind == ESP_MARKER_LOOT)    ? 0.6F
                  : (entity.kind == ESP_MARKER_SCRAP)   ? 0.6F
                  : (entity.kind == ESP_MARKER_PICKUP)  ? 0.4F
                  : (entity.kind == ESP_MARKER_BOT)     ? 1.4F
                  : (entity.kind == ESP_MARKER_VEHICLE) ? 1.2F
                  : (entity.kind == ESP_MARKER_TURRET)  ? 1.3F
                  : (entity.kind == ESP_MARKER_SLEEPER) ? 0.8F
                  : (entity.kind == ESP_MARKER_BACKPACK)? 0.5F
                  : (entity.kind == ESP_MARKER_CUPBOARD)? 1.0F
                  : (entity.kind == ESP_MARKER_HOUSE)   ? 0.6F : 0.9F;
        Vec2 screen{};
        if (!w2s(g_frame_vp, anchor, g_frame_sw, g_frame_sh, screen, false) ||
            !std::isfinite(screen.x) || !std::isfinite(screen.y) ||
            screen.x < -screen_margin || screen.x > g_frame_sw + screen_margin ||
            screen.y < -screen_margin || screen.y > g_frame_sh + screen_margin)
            continue;

        EspMarker marker{};
        marker.id = entity.id;
        marker.x = screen.x;
        marker.y = screen.y;
        marker.distance = distance;
        marker.kind = entity.kind;
        marker.has_color = entity.has_color;
        marker.rainbow = entity.rainbow;
        marker.color_rgb[0] = entity.color_rgb[0];
        marker.color_rgb[1] = entity.color_rgb[1];
        marker.color_rgb[2] = entity.color_rgb[2];
        snprintf(marker.name, sizeof(marker.name), "%s",
                 lang::visual(entity.label ? entity.label : entity.text));
        result.push_back(marker);
    }

    std::stable_sort(result.begin(), result.end(), [](const EspMarker& a, const EspMarker& b) {
        return floorf(a.distance * 2.0F) < floorf(b.distance * 2.0F);
    });

    std::vector<EspMarker> thinned;
    thinned.reserve(result.size());
    for (const EspMarker& marker : result) {
        bool covered = false;
        for (const EspMarker& kept : thinned) {
            if (kept.kind != marker.kind) continue;
            if (marker.kind != ESP_MARKER_PICKUP && strcmp(kept.name, marker.name) != 0) continue;
            const float dx = kept.x - marker.x;
            const float dy = kept.y - marker.y;
            if (dx * dx + dy * dy < 22.0F * 22.0F) {
                covered = true;
                break;
            }
        }
        if (covered) continue;
        thinned.push_back(marker);
        if (thinned.size() >= 64) break;
    }
    return thinned;
}

void esp_set_bot_aim_enabled(bool enabled) { g_bot_aim_enabled = enabled; }

// Bot aim feed, see game.h: scan on schedule + refresh bot positions even
// when bot markers are not drawn. Called from esp_get_boxes(), i.e. always
// before esp_get_markers() in a frame, so drawn pills reuse fresh positions.
void esp_bot_aim_maintain() {
    if (!g_bot_aim_enabled) return;
    if (g_pid <= 0 || !g_il2cpp_base) return;
    const double now = mono_seconds();
    if (now >= g_marker_next_scan)
        schedule_next_marker_scan(rebuild_marker_entities(), now);
    for (MarkerEntity& entity : g_marker_entities) {
        if (entity.kind != ESP_MARKER_BOT || marker_bot_dead(entity)) continue;
        marker_track_position(entity);
    }
}

std::vector<BotAimEnt> esp_bot_aim_list() {
    std::vector<BotAimEnt> out;
    if (!g_bot_aim_enabled) return out;
    const double now = mono_seconds();
    for (MarkerEntity& entity : g_marker_entities) {
        if (entity.kind != ESP_MARKER_BOT || marker_bot_dead(entity) ||
            !marker_position_usable(entity, now)) continue;
        float dx = entity.position.x - g_frame_local_pos.x;
        float dy = entity.position.y - g_frame_local_pos.y;
        float dz = entity.position.z - g_frame_local_pos.z;
        float distance = sqrtf(dx * dx + dy * dy + dz * dz);
        if (!std::isfinite(distance) || distance > g_marker_max_distance) continue;
        BotAimEnt bot;
        bot.id = entity.identity;
        bot.x = entity.position.x; bot.y = entity.position.y; bot.z = entity.position.z;
        const double age = now - entity.position_sample_time;
        bot.pos_age = (age > 0.0 && std::isfinite(age)) ? static_cast<float>(age) : 0.0F;
        bot.head_valid = entity.head_valid;
        if (bot.head_valid) {
            bot.hx = entity.head_position.x;
            bot.hy = entity.head_position.y;
            bot.hz = entity.head_position.z;
        }
        if (botlead::fresh(entity.lead, now)) {
            bot.vel_valid = true;
            bot.vx = entity.lead.vel.x;
            bot.vy = entity.lead.vel.y;
            bot.vz = entity.lead.vel.z;
        }
        out.push_back(bot);
    }
    return out;
}
