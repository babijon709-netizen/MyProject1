#include "game.h"
#include "game_internal.h"
#include "game_offsets_active.h"   // оффсеты (go_active)
#include "lang.h"
#include <algorithm>
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
// The class of each behaviour is only inspected once (cached per Il2CppClass),
// and the whole registry is re-scanned every few seconds rather than per frame;
// each frame only re-reads the positions, and only for animals, since ore nodes
// never move.

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
static void farm_scan_abort();
static void farm_scan_reset();
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

// Barrels are smashed, not opened, so they are MineableObjects and never pass
// through the LootObject code at all -- the entityType below is the only place
// they can be recognised. They belong to the loot toggle all the same.
static const MarkerLook kBarrel    {ESP_MARKER_LOOT, "Бочка",  true,  false, {80, 200, 255}};
static const MarkerLook kSmashBox  {ESP_MARKER_LOOT, "Ящик",   false, false, {255, 255, 255}};

// Animals: the game's own EntityType only covers some of them, the rest (wolf,
// rat, ...) are recognised by the prefab name below.
static MarkerLook animal_look(const char* label) {
    MarkerLook look;
    look.kind = ESP_MARKER_ANIMAL;
    look.label = label;
    look.has_color = false;
    return look;
}

// EntityType -> what to draw. Trees, road signs, buildings and players are
// left out on purpose; barrels and smashable loot boxes are in, they are the
// scrap source and the game files them under the same enum.
static bool marker_for_entity_type(int32_t type, MarkerLook& look) {
    switch ((MineableEntityType)type) {
        case MineableEntityType::Stone:    look = kOreStone;  return true;
        case MineableEntityType::Iron:     look = kOreMetal;  return true;
        case MineableEntityType::Sulfur:   look = kOreSulfur; return true;
        case MineableEntityType::Barrel:   look = kBarrel;    return true;
        case MineableEntityType::Lootbox:  look = kSmashBox;  return true;
        // Air-drop balloon crates, new in this game build.
        case MineableEntityType::LootboxBaloon:
        case MineableEntityType::LootboxBaloonBig: look = kSmashBox; return true;
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
        {"barrel",     "Скрап",              2, false}, // barrels are the scrap source
        {"crate",      "Ящик",               2, false},
        {"lootbox",    "Ящик",               2, false},
        {"loot",       "Ящик",               2, false},
        {"container",  "Контейнер",          2, false},
        {"chest",      "Сундук",             2, false},
        {"case",       "Кейс",               2, false},
        {"cache",      "Тайник",             2, false},
        {"trash",      "Мусорка",            2, false},
        {"garbage",    "Мусорка",            2, false},
    };
    bool saw_box = false, saw_size = false;
    for_each_name_token(raw, [&](const char* word) {
        for (const char* bad : kDeployed)
            if (strcmp(word, bad) == 0) { info.player_box = true; return true; }
        if (strcmp(word, "box") == 0) saw_box = true;
        for (const char* size : kSizeWords)
            if (strcmp(word, size) == 0) { saw_size = true; break; }
        for (const auto& entry : kLabels) {
            if (strcmp(word, entry.word) == 0 && entry.rank > info.rank) {
                info.rank = entry.rank;
                info.label = entry.ru;
                info.rainbow = entry.rainbow;
            }
        }
        return false;
    });
    if (saw_box && saw_size) info.player_box = true;
}

static bool loot_marker(uint64_t component, const char* object_name, const char* root_name,
                        MarkerLook& look) {
    // A container that is part of a building is player-placed by definition.
    if (valid_obj(rd_ptr(component + LOOTOBJECT_BUILDING_PIECE))) return false;

    // Three name sources: the component's GameObject, the network object's
    // root GameObject, and the loot panel id the UI opens with. The panel is
    // often the only one that says "militarycrate" out loud.
    char panel[32] = {};
    read_managed_string(rd_ptr(component + LOOTOBJECT_PANEL_NAME), panel, sizeof(panel));

    LootNameInfo info;
    scan_loot_name(object_name, info);
    if (!info.player_box) scan_loot_name(root_name, info);
    if (!info.player_box) scan_loot_name(panel, info);
    if (info.player_box) return false;
    // No name we recognise -> not drawn. Everything anonymous down here is
    // something a player owns (a box in a house, a sleeping player, the bag a
    // corpse leaves), and labelling all of it "Ящик" is exactly the noise the
    // loot ESP must not produce. World containers always name themselves.
    if (info.rank <= 0 || !info.label) return false;

    look.kind = ESP_MARKER_LOOT;
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
    if (strstr(key, "sulfur")) { look = kOreSulfur; return 3; }
    if (strstr(key, "metal"))  { look = kOreMetal;  return 2; } // metal.ore, hq.metal.ore
    if (strstr(key, "stone"))  { look = kOreStone;  return 1; }
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

void reset_marker_caches() {
    g_marker_entities.clear();
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
        if (!g_marker_entities.empty()) g_marker_entities.clear();
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
                for (MarkerEntity& e : g_marker_entities)
                    e.position_valid = marker_world_position(e.transform, e.position);
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
        bool want_read = !entity.position_valid;
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
            entity.position_valid = marker_world_position(entity.transform, entity.position);
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
