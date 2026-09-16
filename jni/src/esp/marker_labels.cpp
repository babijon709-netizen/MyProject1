// marker_labels.cpp — Маркеры: подписи и цвета сущностей.
//
// Модуль разрезан из прежнего монолита jni/src/game.cpp;
// что здесь лежит и кто это зовёт — в шапке marker_labels.h и в docs/CODE_MAP.md.

#include "esp/common.h"
#include "esp/farm_scan.h"
#include "esp/managed.h"
#include "esp/markers.h"
#include "esp/mem.h"
#include "esp/names.h"
#include "marker_labels.h"

static const MarkerLook kOreStone  {ESP_MARKER_ORE, "Камень", true, false, {190, 190, 190}};

static const MarkerLook kOreMetal  {ESP_MARKER_ORE, "Железо", true, false, {255, 140,  40}};

static const MarkerLook kOreSulfur {ESP_MARKER_ORE, "Сера",   true, false, {255, 225,  50}};

// Barrels are smashed, not opened, so they are MineableObjects and never pass
// through the LootObject code at all -- the entityType below is the only place
// they can be recognised. They belong to the loot toggle all the same.
static const MarkerLook kBarrel    {ESP_MARKER_LOOT, "Бочка",  true,  false, {80, 200, 255}};

const MarkerLook kSmashBox  {ESP_MARKER_LOOT, "Ящик",   false, false, {255, 255, 255}};

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
bool marker_for_entity_type(int32_t type, MarkerLook& look) {
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
bool animal_look_from_object_name(const char* raw, MarkerLook& look) {
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
bool barrel_look_from_object_name(const char* raw, MarkerLook& look) {
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

bool loot_marker(uint64_t component, const char* object_name, const char* root_name,
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
bool marker_from_loot(uint64_t mineable, MarkerLook& look) {
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
bool gather_marker_from_loot(uint64_t mineable, MarkerLook& look) {
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
bool pickup_marker(uint64_t component, char* label, size_t label_cap) {
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
