// weapons.cpp — Оружие: метка в руках игрока.
//
// Модуль разрезан из прежнего монолита jni/src/game.cpp;
// что здесь лежит и кто это зовёт — в шапке weapons.h и в docs/CODE_MAP.md.

#include "esp/common.h"
#include "esp/boxes.h"
#include "esp/il2cpp.h"
#include "esp/markers.h"
#include "esp/mem.h"
#include "esp/names.h"
#include "esp/player_pose.h"
#include "esp/skeleton_cache.h"
#include "esp/skeleton_names.h"
#include "weapons.h"

// ===================== Weapon label localisation =====================
//
// Weapon labels arrive in three flavours: the item definition name for the
// local player ("Assault Rifle"), the item short name ("assault.rifle") and —
// for remote players — the prefab name of the model in their hands, which the
// game builds as "<NN>_Default<Name>" / "<NN>_Skin<Name>" (e.g.
// "07_DefaultAssault Riffle"). All three are reduced to a normalized key
// (lowercase letters and digits only) and mapped to one Russian name, so the
// skin/prefab decorations never reach the screen.
static void weapon_key_normalize(const char* in, char* out, size_t cap) {
    size_t n = 0;
    for (const char* p = in; *p && n + 1 < cap; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out[n++] = (char)c;
    }
    out[n] = '\0';
}

// Every weapon / tool / throwable the game ships (item list from class `rs`
// in dump.cs). `key` is the normalized form, `en` the name the game itself
// uses in its UI (ItemData.m_Name) and `ru` the Russian variant.
//
// The table is a canonicaliser, not a whitelist: a weapon that is missing here
// still shows up, just with the cleaned prefab name. Its job is to make the
// label identical no matter which source it came from (prefab name, item name
// or short name) and to survive any extra decoration a skin prefab may carry.
//
// Matching is exact first, then longest-substring, so "pickaxehammer" wins
// over "pickaxe" and "crossbow" over "bow" regardless of the order here.
struct WeaponName { const char* key; const char* en; const char* ru; };

static const WeaponName kWeaponNames[] = {
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
    {"flaregun",              "Flare Gun",       "Ракетница"},
    {"rocketlauncher",        "Rocket Launcher", "РПГ"},
    // ---- Bows ----
    {"crossbow",              "Crossbow",        "Арбалет"},
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

static bool canonical_weapon_label(char* label, size_t cap) {
    if (!label || !label[0] || cap < 2) return false;
    char key[80];
    weapon_key_normalize(label, key, sizeof(key));
    if (!key[0]) return false;
    const WeaponName* best = nullptr;
    size_t best_len = 0;
    for (const WeaponName& entry : kWeaponNames) {
        if (strcmp(key, entry.key) == 0) { best = &entry; break; }
        size_t len = strlen(entry.key);
        if (len > best_len && strstr(key, entry.key)) { best = &entry; best_len = len; }
    }
    if (!best) return false;
    const char* pick = (lang::english() || !best->ru) ? best->en : best->ru;
    if (!pick || !pick[0]) return false;
    CopyTextUtf8(label, cap, pick);
    return true;
}

// Third-person (networked) held weapon. Defined further down, next to the
// GameObject-name helpers it needs; `definite` reports whether the weapon slot
// could be evaluated at all, so the caller can tell "nothing in hands" from
// "could not read".
static bool remote_weapon_display_name(uint64_t player, char* out, size_t cap, bool& definite);

static bool player_weapon_name_raw(uint64_t player, char* out, size_t cap, bool& definite) {
    definite = false;
    if (!player || !out || cap < 2) return false;
    out[0] = '\0';
    // (1) FP manager route — works for the local player (and rarely for a
    // remote player whose FP MonoBehaviour happens to be instantiated).
    uint64_t fp = rd_ptr(player + PLAYER_FP_MANAGER);
    if (valid_obj(fp)) {
        uint64_t candidates[2] = {
            rd_ptr(fp + FPMANAGER_CURRENT_WEAPON),
            rd_ptr(fp + FPMANAGER_CURRENT_OBJECT),
        };
        for (int strict = 1; strict >= 0; --strict) {
            for (int i = 0; i < 2; ++i) {
                if (fp_object_display_name(candidates[i], player, strict != 0, out, cap)) {
                    definite = true;
                    return true;
                }
            }
        }
    }
    // (2) PlayerWeapon (synced) -> weapon view / model holders. This is the
    // only route that works for other players; see remote_weapon_display_name.
    if (remote_weapon_display_name(player, out, cap, definite)) return true;
    if (definite) return false;   // synced slot says the hands are empty
    // (3) weaponReference @0xF0 read as a plain Oxide.Item / FPObject — kept as
    // a cheap last resort for layouts where it is not the wrapper we expect.
    uint64_t wr = rd_ptr(player + PLAYER_WEAPON_REFERENCE);
    if (valid_obj(wr)) {
        if (read_item_data_display_name(rd_ptr(wr + ITEM_DATA), out, cap))
            return true;
        if (fp_object_display_name(wr, player, false, out, cap))
            return true;
    }
    // (4) weapons[] @0x198 — inventory/belt weapon objects; try each as an Item.
    uint64_t arr = rd_ptr(player + PLAYER_WEAPONS_ARRAY);
    if (valid_obj(arr)) {
        int32_t len = rd<int32_t>(arr + IL2CPP_ARRAY_LENGTH);
        if (len > 0 && len <= 32) {
            for (int32_t i = 0; i < len; ++i) {
                uint64_t el = rd_ptr(arr + IL2CPP_ARRAY_FIRST_ELEMENT + (uint64_t)i * 8);
                if (!valid_obj(el)) continue;
                if (read_item_data_display_name(rd_ptr(el + ITEM_DATA), out, cap))
                    return true;
            }
        }
    }
    out[0] = '\0';
    return false;
}

// The prefab names carry the game's own misspelling ("Assault Riffle") while
// the item definitions spell it correctly ("Assault Rifle"), so normalise it —
// otherwise the same gun reads differently for the local and remote players.
static void fix_weapon_label_spelling(char* label) {
    if (!label) return;
    for (char* p = label; *p; ++p) {
        if (strncasecmp(p, "riffle", 6) != 0) continue;
        memmove(p + 3, p + 4, strlen(p + 4) + 1); // "riffle" -> "rifle"
        break;
    }
}

// Public entry point: resolve the held weapon and localise the label.
// Unknown weapons keep their cleaned original name rather than disappearing.
bool player_weapon_name(uint64_t player, char* out, size_t cap, bool& definite) {
    if (!player_weapon_name_raw(player, out, cap, definite)) return false;
    fix_weapon_label_spelling(out);
    canonical_weapon_label(out, cap);
    return out[0] != '\0';
}

// Lowercase, strip separators — used to recognise container/rig objects that
// are not the weapon itself.
static void normalize_weapon_token(const char* in, char* out, size_t cap) {
    size_t n = 0;
    for (const char* p = in; *p && n + 1 < cap; ++p) {
        char c = *p;
        if (c == ' ' || c == '_' || c == '-' || c == '.') continue;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        out[n++] = c;
    }
    out[n] = '\0';
}

// Reject holders/rig nodes so route B never labels a player with "WeaponHolder".
static bool weapon_name_is_junk(const char* normalized) {
    static const char* kJunk[] = {
        "weaponholder", "rightweaponholder", "leftweaponholder", "equipmentholder",
        "holder", "weapon", "weapons", "weaponroot", "weaponpivot", "attach",
        "attachpoint", "socket", "pivot", "muzzle", "muzzlepoint", "container",
        "root", "armature", "bone", "empty", "gameobject", "model", "mesh",
        "righthand", "lefthand", "hand", "handr", "handl", "item", "view",
        "default", "skin", "defaultskin", "none", "empty1",
    };
    if (!normalized[0]) return true;
    for (const char* junk : kJunk) if (strcmp(normalized, junk) == 0) return true;
    return false;
}

// "AssaultRifle_TP (Clone)" -> "AssaultRifle". Returns false for junk.
static bool weapon_label_from_object_name(const char* raw, char* out, size_t cap) {
    if (!raw || !out || cap < 2) return false;
    char buf[64];
    size_t n = 0;
    for (const char* p = raw; *p && n + 1 < sizeof(buf); ++p) {
        if (*p == '(') break;               // "(Clone)" and friends
        buf[n++] = *p;
    }
    buf[n] = '\0';
    // Trim separators on both ends.
    size_t start = 0;
    while (buf[start] == ' ' || buf[start] == '_' || buf[start] == '-') ++start;
    size_t end = strlen(buf + start);
    char* s = buf + start;
    while (end > 0 && (s[end - 1] == ' ' || s[end - 1] == '_' || s[end - 1] == '-')) s[--end] = '\0';
    if (end < 2) return false;
    // Prefab names come as "<NN>_Default<Name>" / "<NN>_Skin<Name>" (and the
    // skin variant may carry its own index), so drop the leading index and the
    // Default/Skin marker glued in front of the real name.
    while (*s >= '0' && *s <= '9') { ++s; --end; }
    while (*s == '_' || *s == '-' || *s == ' ') { ++s; --end; }
    for (int pass = 0; pass < 2; ++pass) {
        static const char* kMarkers[] = {"default", "skin"};
        for (const char* marker : kMarkers) {
            size_t len = strlen(marker);
            if (end > len && strncasecmp(s, marker, len) == 0) {
                s += len; end -= len;
                while (*s >= '0' && *s <= '9') { ++s; --end; }
                while (*s == '_' || *s == '-' || *s == ' ') { ++s; --end; }
                break;
            }
        }
    }
    if (end < 2) return false;
    // Strip decorative prefixes/suffixes the prefabs carry.
    static const char* kPrefixes[] = {"tp_", "fp_", "w_", "wep_", "weapon_", "view_", "prefab_", "pref_"};
    for (const char* pre : kPrefixes) {
        size_t len = strlen(pre);
        if (end > len + 1 && strncasecmp(s, pre, len) == 0) { s += len; end -= len; break; }
    }
    static const char* kSuffixes[] = {"_tp", "_fp", "_view", "_model", "_mesh", "_prefab", "_weapon", "_lod0", "_lod"};
    for (const char* suf : kSuffixes) {
        size_t len = strlen(suf);
        if (end > len + 1 && strncasecmp(s + end - len, suf, len) == 0) { end -= len; s[end] = '\0'; break; }
    }
    if (end < 2) return false;
    char normalized[64];
    normalize_weapon_token(s, normalized, sizeof(normalized));
    if (weapon_name_is_junk(normalized)) return false;
    bool has_alpha = false;
    for (const char* p = s; *p; ++p) if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')) { has_alpha = true; break; }
    if (!has_alpha) return false;
    CopyTextUtf8(out, cap, s);
    return true;
}

// Managed MonoBehaviour/Component -> name of the GameObject it sits on.
bool managed_component_gameobject_name(uint64_t managed_component, char* out, size_t cap) {
    if (!g_go_name_offset_valid) return false;
    uint64_t native = managed_object_native(managed_component);
    if (!native) return false;
    uint64_t go = rd_ptr(native + COMPONENT_GAMEOBJECT);
    if (!go) return false;
    return read_gameobject_name_at(go, g_go_name_offset, g_go_name_plain_pointer, out, cap);
}

// PlayerManager -> PlayerWeapon. weaponReference is an obfuscated lazy wrapper
// (same shape as kccReference), so probe it, then fall back to a field scan.
// A candidate is accepted on its back-reference plus its class name.
static uint64_t resolve_player_weapon_component(uint64_t player) {
    auto is_player_weapon = [&](uint64_t candidate) {
        return valid_obj(candidate) &&
               rd_ptr(candidate + PLAYERWEAPON_PLAYER_BACKREF) == player &&
               // Имя класса — второй признак; если память имён недоступна,
               // хватает обратной ссылки на игрока (см. object_class_name_is).
               object_class_name_is(candidate, "PlayerWeapon", true);
    };
    uint64_t reference = rd_ptr(player + PLAYER_WEAPON_REFERENCE);
    if (is_player_weapon(reference)) return reference;
    if (valid_obj(reference)) {
        for (uint64_t offset = 0x08; offset <= 0x60; offset += 8) {
            uint64_t candidate = rd_ptr(reference + offset);
            if (is_player_weapon(candidate)) return candidate;
        }
    }
    for (uint64_t offset = 0x68; offset <= 0x350; offset += 8) {
        uint64_t candidate = rd_ptr(player + offset);
        if (is_player_weapon(candidate)) return candidate;
    }
    return 0;
}

// PlayerManager -> PlayerModelInfo, via the inventory data or the KCC's
// CharacterAnimation (whichever resolves first).
static uint64_t resolve_player_model_info(uint64_t player, const PlayerAux& aux) {
    auto usable = [&](uint64_t candidate) {
        return valid_obj(candidate) && managed_object_native(candidate) != 0;
    };
    uint64_t inventory = rd_ptr(player + PLAYER_INVENTORY);
    if (valid_obj(inventory)) {
        uint64_t data = rd_ptr(inventory + INV_PLAYER_INVENTORY_DATA);
        if (valid_obj(data)) {
            uint64_t info = rd_ptr(data + INVDATA_PLAYER_MODEL_INFO);
            if (usable(info)) return info;
        }
    }
    if (aux.kcc) {
        uint64_t animation = rd_ptr(aux.kcc + KCC_CHARACTER_ANIMATION);
        if (valid_obj(animation)) {
            uint64_t info = rd_ptr(animation + CHARANIM_PLAYER_MODEL_INFO);
            if (usable(info)) return info;
        }
    }
    return 0;
}

// Route A: PlayerWeapon -> weapon view -> spawned weapon GameObject.
// Item id -> label, dumped straight from this build's Oxide.ItemDatabase
// (items.txt, /storage/emulated/0/benzhack). The id comes from the synced
// WeaponPiece.Number, so this is the FIRST choice for the weapon label:
// exact, cheap (no prefab-name reads), and survives prefab renames. The
// prefab-name path below stays as the fallback for ids not listed here.
static const char* weapon_label_for_item_id(int id) {
    switch (id) {
        // -- melee / tools --------------------------------------------------
        case 17:  return "Кам. топорик";     // stone.hatchet
        case 18:  return "Молоток";          // building.hammer
        case 19:  return "Топор";            // axe
        case 20:  return "Кирка";            // pickaxe
        case 21:  return "Факел";            // torch
        case 22:  return "Кирка";            // pickaxehammer
        case 23:  return "Пила";             // saw.ripper
        case 24:  return "Бензопила";        // chainsaw
        case 25:  return "Отбойник";         // jackhammer
        case 95:  return "Дер. копьё";       // wooden.spear
        case 96:  return "Жел. копьё";       // iron.spear
        case 97:  return "Дубина";           // bone.club
        case 98:  return "Мачете";           // machete
        case 99:  return "Булава";           // mace
        case 100: return "Шип. дубина";      // wooden.spiked.club
        case 112: return "Лед. копьё";       // ice.spear
        // -- firearms ---------------------------------------------------------
        case 102: return "АК-47";            // assault.rifle
        case 103: return "Револьвер";        // revolver
        case 104: return "Дигл";             // desert.eagle
        case 105: return "Дробовик";         // shotgun
        case 106: return "Охот. винтовка";   // hunting.rifle
        case 107: return "ПП";               // submachine.gun
        case 108: return "Ракетница";        // flare.gun
        case 113: return "Томпсон";          // thompson
        case 114: return "DMR";              // dmr
        case 115: return "Винчестер";        // winchester
        case 116: return "DVL";              // dvl
        case 117: return "Пулемёт";          // hmlmg
        case 118: return "Самопал";          // handmade.pistol
        case 119: return "Вектор";           // kriss.vector
        case 120: return "Шаромёт";          // steel.ball.gun
        case 225: return "FN FAL";           // fn.fal
        // -- bows / launchers ------------------------------------------------
        case 101: return "Лук";              // wooden.bow
        case 109: return "Арбалет";          // crossbow
        case 110: return "РПГ";              // rocket.launcher
        case 111: return "С4";               // explosive.charge
        // -- throwables -------------------------------------------------------
        case 128: case 130: return "Дымовуха"; // event.grenade.*
        case 132: return "Граната";          // grenade.military
        case 131: return "Возд. маркер";     // tactical.air.marker
        case 127: return "Снежок";           // snowball
        default: return nullptr;
    }
}

static bool weapon_name_from_view(uint64_t weapon_component, char* out, size_t cap) {
    uint64_t view = rd_ptr(weapon_component + PLAYERWEAPON_VIEW);
    char raw[48];
    for (int depth = 0; depth < 2 && valid_obj(view); ++depth) {
        uint64_t weapon_base = rd_ptr(view + WEAPONVIEW_WEAPON_BASE);
        if (managed_component_gameobject_name(weapon_base, raw, sizeof(raw)) &&
            weapon_label_from_object_name(raw, out, cap)) return true;
        uint64_t root = managed_object_native(rd_ptr(view + WEAPONVIEW_ROOT_TRANSFORM));
        if (root && read_transform_name(root, raw, sizeof(raw)) &&
            weapon_label_from_object_name(raw, out, cap)) return true;
        view = rd_ptr(view + WEAPONVIEW_INNER); // decorator wraps another view
    }
    return false;
}

// Route B: the weapon prefab is parented under the model's weapon holders.
static bool weapon_name_from_model_holders(uint64_t model_info, char* out, size_t cap) {
    const uint64_t holders[2] = {MODELINFO_RIGHT_WEAPON_HOLDER, MODELINFO_LEFT_WEAPON_HOLDER};
    char raw[48];
    for (uint64_t holder_offset : holders) {
        uint64_t holder = managed_object_native(rd_ptr(model_info + holder_offset));
        if (!holder) continue;
        uint64_t children[8];
        int count = read_transform_children(holder, children, 8);
        for (int i = 0; i < count; ++i) {
            if (!read_transform_name(children[i], raw, sizeof(raw))) continue;
            if (weapon_label_from_object_name(raw, out, cap)) return true;
            // The holder may add one wrapper node; look one level deeper.
            uint64_t grandchildren[4];
            int sub = read_transform_children(children[i], grandchildren, 4);
            for (int k = 0; k < sub; ++k) {
                if (read_transform_name(grandchildren[k], raw, sizeof(raw)) &&
                    weapon_label_from_object_name(raw, out, cap)) return true;
            }
        }
    }
    return false;
}

static bool remote_weapon_display_name(uint64_t player, char* out, size_t cap, bool& definite) {
    definite = false;
    if (!player || !out || cap < 2) return false;
    out[0] = '\0';
    if (!ensure_gameobject_name_offset(player)) return false;

    PlayerAux& aux = player_aux(player);
    // Revalidate the cached component by its back-reference; re-resolve rarely.
    if (aux.weapon_component &&
        rd_ptr(aux.weapon_component + PLAYERWEAPON_PLAYER_BACKREF) != player)
        aux.weapon_component = 0;
    if (!aux.weapon_component) {
        if (aux.weapon_retry > 0) --aux.weapon_retry;
        else {
            aux.weapon_component = resolve_player_weapon_component(player);
            if (!aux.weapon_component) aux.weapon_retry = 4;
        }
    }

    bool holds_weapon = false, knows_slot = false;
    int16_t piece_number = 0;
    if (aux.weapon_component) {
        uint64_t piece = aux.weapon_component + PLAYERWEAPON_PIECE;
        uint8_t enabled = rd<uint8_t>(piece + WEAPONPIECE_ENABLED);
        piece_number = rd<int16_t>(piece + WEAPONPIECE_NUMBER);
        knows_slot = true;
        holds_weapon = (enabled != 0) || piece_number != 0;
        // Exact match first: the synced item id resolves the label with zero
        // extra reads and never suffers from renamed/obfuscated prefabs.
        if (holds_weapon && piece_number != 0) {
            const char* byId = weapon_label_for_item_id((int)piece_number);
            if (byId) { snprintf(out, cap, "%s", byId); definite = true; return true; }
        }
        if (weapon_name_from_view(aux.weapon_component, out, cap)) { definite = true; return true; }
    }

    if (!aux.model_info || !managed_object_native(aux.model_info))
        aux.model_info = resolve_player_model_info(player, aux);
    if (aux.model_info && weapon_name_from_model_holders(aux.model_info, out, cap)) {
        definite = true;
        return true;
    }

    // Nothing found. If the synced slot says a weapon *is* equipped, fall back
    // to its item number so the label still identifies the weapon (and tells us
    // the component resolved but the prefab name did not).
    out[0] = '\0';
    if (knows_slot && holds_weapon && piece_number != 0) {
        snprintf(out, cap, "WPN %d", (int)piece_number);
        definite = true;
        return true;
    }
    definite = knows_slot && !holds_weapon;
    return false;
}
