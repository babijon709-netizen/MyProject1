#pragma once
// Имена оружия в подписях ESP: таблица и сопоставление — чистые, без памяти
// игры и без ImGui, поэтому прогоняются хост-тестом
// (jni/tests/test_weapon_name.cpp). Всё, что читает игру, осталось в game.cpp.
//
// Weapon labels arrive in three flavours: the item definition name for the
// local player ("Assault Rifle"), the item short name ("assault.rifle") and —
// for remote players — the prefab name of the model in their hands, which the
// game builds as "<NN>_Default<Name>" / "<NN>_Skin<Name>" (e.g.
// "07_DefaultAssault Riffle"). All three are reduced to a normalized key
// (lowercase letters and digits only) and mapped to one label, so the
// skin/prefab decorations never reach the screen.
//
// Таблица — канонизатор, а не белый список: оружие, которого здесь нет, всё
// равно показывается, просто вычищенным именем префаба. Её задача — сделать
// подпись одинаковой независимо от источника и пережить decoration скина.
#include <cstddef>
#include <cstring>

#include "text_utf8.h"

namespace weaponname {

struct Name { const char* key; const char* en; const char* ru; };

// Every weapon / tool / throwable the game ships (item list from class `rs`
// in dump.cs). `key` is the normalized form, `en` the name the game itself uses
// in its UI (ItemData.m_Name) and `ru` the Russian variant. `ru == nullptr`
// means «русского названия не знаем» — тогда берется английское, и в русской
// подписи хотя бы не будет сырого имени префаба.
inline const Name kNames[] = {
    // ---- Firearms ----
    {"assaultriffle",         "AK-47",           "АК-47"},
    {"assaultrifle",          "AK-47",           "АК-47"},
    {"fnfal",                 "FN FAL",          "Автомат FAL"},
    {"thompson",              "Thompson",        "Томпсон"},
    {"krissvector",           "Kriss Vector",    "Вектор"},
    {"vector",                "Kriss Vector",    "Вектор"},
    {"submachinegun",         "Submachine Gun",  "Пистолет-пулемёт"},
    {"smg",                   "Submachine Gun",  "Пистолет-пулемёт"},
    {"steelballgun",          "Steel Ball Gun",  "Шаровое ружьё"},
    {"shotgun",               "Shotgun",         "Дробовик"},
    {"huntingriffle",         "Hunting Rifle",   "Охотничья винтовка"},
    {"huntingrifle",          "Hunting Rifle",   "Охотничья винтовка"},
    {"winchester",            "Winchester",      "Винчестер"},
    {"dmr",                   "DMR",             "Винтовка DMR"},
    {"dvl",                   "DVL",             "Снайперская винтовка"},
    {"sniperriffle",          "Sniper Rifle",    "Снайперская винтовка"},
    {"sniperrifle",           "Sniper Rifle",    "Снайперская винтовка"},
    {"hmlmg",                 "HMLMG",           "Пулемёт"},
    {"lmg",                   "LMG",             "Пулемёт"},
    {"machinegun",            "Machine Gun",     "Пулемёт"},
    {"revolver",              "Revolver",        "Револьвер"},
    {"deserteagle",           "Desert Eagle",    "Дезерт Игл"},
    {"handmadepistol",        "Handmade Pistol", "Самодельный пистолет"},
    {"pistol",                "Pistol",          "Пистолет"},
    {"semiautomaticpistol",   "Semi-Automatic Pistol", "Полуавтоматический пистолет"},
    {"semiautopistol",        "Semi-Automatic Pistol", "Полуавтоматический пистолет"},
    {"pistolsemiauto",        "Semi-Automatic Pistol", "Полуавтоматический пистолет"},
    {"pythonrevolver",        "Python Revolver", "Револьвер Питон"},
    {"pistolpython",          "Python Revolver", "Револьвер Питон"},
    {"m92pistol",             "M92 Pistol",      "Пистолет M92"},
    {"pistolm92",             "M92 Pistol",      "Пистолет M92"},
    {"mbr7",                  "MBR7",            nullptr},
    {"flamethrower",          "Flamethrower",    "Огнемёт"},
    {"watergun",              "Watergun",        "Водяной пистолет"},
    {"jerrycan",              "Jerry Can",       "Канистра"},
    {"bucket",                "Bucket",          "Ведро"},
    {"flaregun",              "Flare Gun",       "Ракетница"},
    {"rocketlauncher",        "Rocket Launcher", "РПГ"},
    // Дробовики: у «двустволки» имя префаба — DoubleBarrel, и без этой строки
    // подпись уходила в интерфейс как есть ( жалоба на «не переведённый
    // DoubleBarrel»). Ключи — и по имени префаба, и по shortname.
    {"doublebarrel",          "Double Barrel Shotgun", "Двустволка"},
    {"shotgundouble",         "Double Barrel Shotgun", "Двустволка"},
    {"pumpshotgun",           "Pump Shotgun",    "Помповое ружьё"},
    {"shotgunpump",           "Pump Shotgun",    "Помповое ружьё"},
    {"waterpipe",             "Waterpipe Shotgun", "Обрез"},
    {"spas12",                "SPAS-12",         "Дробовик SPAS-12"},
    {"seladora",              "Seladora",        "Селадора"},
    {"eoka",                  "Eoka Pistol",     "Эока"},
    // ---- Bows ----
    {"crossbow",              "Crossbow",        "Арбалет"},
    {"compositebow",          "Composite Bow",   "Составной лук"},
    {"bowcomposite",          "Composite Bow",   "Составной лук"},
    {"huntingbow",            "Hunting Bow",     "Охотничий лук"},
    {"bowhunting",            "Hunting Bow",     "Охотничий лук"},
    {"woodenbow",             "Wooden Bow",      "Лук"},
    {"bowarrow",              "Wooden Bow",      "Лук"},
    {"bow",                   "Wooden Bow",      "Лук"},
    // ---- Melee ----
    {"machete",               "Machete",             "Мачете"},
    {"mace",                  "Mace",                "Булава"},
    {"boneclub",              "Bone Club",           "Костяная дубина"},
    {"woodenspikedclub",      "Wooden Spiked Club",  "Шипованная дубина"},
    {"spikedclub",            "Wooden Spiked Club",  "Шипованная дубина"},
    {"woodenspear",           "Wooden Spear",        "Деревянное копьё"},
    {"ironspear",             "Iron Spear",          "Железное копьё"},
    {"icespear",              "Ice Spear",           "Ледяное копьё"},
    {"spear",                 "Spear",               "Копьё"},
    {"fists",                 "Fists",               "Кулаки"},
    {"unarmed",               "Fists",               "Кулаки"},
    // Пустые руки игра называет головой модели («MaleHead»).
    {"malehead",              "Hand",                "Рука"},
    {"femalehead",            "Hand",                "Рука"},
    // ---- Tools ----
    {"stonehatchet",          "Stone Hatchet",   "Каменный топор"},
    {"hatchet",               "Hatchet",         "Топор"},
    {"axe",                   "Axe",             "Топор"},
    {"pickaxehammer",         "Pickaxe Hammer",  "Кирка-молот"},
    {"pickaxe",               "Pickaxe",         "Кирка"},
    {"buildinghammer",        "Building Hammer", "Молоток"},
    {"hammer",                "Hammer",          "Молоток"},
    {"sawripper",             "Saw Ripper",      "Пила"},
    {"chainsaw",              "Chainsaw",        "Бензопила"},
    {"jackhammer",            "Jackhammer",      "Отбойный молоток"},
    {"torch",                 "Torch",           "Факел"},
    {"buildingplan",          "Building Plan",   "План постройки"},
    {"rock",                  "Rock",            "Камень"},
    // ---- Throwables / explosives ----
    {"explosivecharge",       "Explosive Charge",   "С4"},
    {"c4",                    "Explosive Charge",   "С4"},
    {"eventgrenadesmoke",     "Smoke Grenade",      "Дымовая граната"},
    {"smokegrenade",          "Smoke Grenade",      "Дымовая граната"},
    {"eventgrenademakeshift", "Makeshift Grenade",  "Самодельная граната"},
    {"makeshiftgrenade",      "Makeshift Grenade",  "Самодельная граната"},
    {"grenadecupcake",        "Cupcake Grenade",    "Граната-кекс"},
    {"grenademilitary",       "Grenade",            "Граната"},
    {"grenade",               "Grenade",            "Граната"},
    {"tacticalairmarker",     "Air Marker",         "Авиамаркер"},
    {"snowball",              "Snowball",           "Снежок"},
    // ---- Other things that can be in hands ----
    {"medkit",                "Medkit",         "Аптечка"},
    {"bandage",               "Bandage",        "Бинт"},
    {"waterbottle",           "Water Bottle",   "Бутылка воды"},
    {"fireworks",             "Fireworks",      "Фейерверк"},
    {"cctvcamera",            "CCTV Camera",    "Камера"},
    {"binoculars",            "Binoculars",     "Бинокль"},
    {"fishingrod",            "Fishing Rod",    "Удочка"},
    {"flaslight",             "Flashlight",     "Фонарик"},
    {"flashlight",            "Flashlight",     "Фонарик"},
};

inline void normalize(const char* in, char* out, size_t cap) {
    size_t n = 0;
    for (const char* p = in; *p && n + 1 < cap; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out[n++] = (char)c;
    }
    out[n] = '\0';
}

// Точное совпадение первым, затем самое длинное вхождение подстроки — поэтому
// «pickaxehammer» выигрывает у «pickaxe», а «crossbow» у «bow», независимо от
// порядка строк в таблице.
inline const Name* find(const char* key) {
    const Name* best = nullptr;
    size_t best_len = 0;
    for (const Name& entry : kNames) {
        if (!strcmp(key, entry.key)) return &entry;
        const size_t len = strlen(entry.key);
        if (len > best_len && strstr(key, entry.key)) { best = &entry; best_len = len; }
    }
    return best;
}

// Язык выбирает вызывающий: подпись строится раз на кадр на игрока, и язык
// интерфейса спрашивается один раз там же, где и все остальные подписи.
// Возвращает false, если в таблице ничего не нашлось — тогда оставляем
// вычищенное имя префаба, а не пустоту.
inline bool canonicalize(char* label, size_t cap, bool english) {
    if (!label || !label[0] || cap < 2) return false;
    char key[80];
    normalize(label, key, sizeof(key));
    if (!key[0]) return false;
    const Name* best = find(key);
    if (!best) return false;
    const char* pick = (english || !best->ru) ? best->en : best->ru;
    if (!pick || !pick[0]) return false;
    CopyTextUtf8(label, cap, pick);
    return true;
}
// Предметы, которые игрок держит в руках помимо оружия (факел и фонарик — это
// свет, аптечка/бинт — лечение, план постройки — строительство). Игра не
// всегда убирает их GameObject из холдера, и «первый подошедший ребёник»
// подписывал игрока чужим предметом: жалоба «арбалет отображается как факел» —
// это факел, оставшийся ребёнком правого холдера раньше самого арбалета.
// Отдельный список, а не поле таблицы: таких записей меньше десятка.
inline bool is_held_utility(const char* label) {
    static const char* const kKeys[] = {
        "torch", "flashlight", "flaslight", "medkit", "bandage", "buildingplan",
        "waterbottle", "cctvcamera", "binoculars", "fishingrod", "fireworks",
    };
    char key[48];
    normalize(label, key, sizeof(key));
    if (!key[0]) return false;
    for (const char* entry : kKeys)
        if (strstr(key, entry)) return true;
    return false;
}

}  // namespace weaponname
