#include "game.h"
#include "game_offsets.h"
#include "Vector.h"

#include <string.h>
#include <strings.h>   // strncasecmp (weapon prefab label cleanup)
#include <sys/uio.h>
#include <stdio.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <chrono>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <unistd.h>
#include <sys/syscall.h>

static ssize_t remote_vm_readv(pid_t pid, const struct iovec* local_iov, unsigned long liovcnt, const struct iovec* remote_iov, unsigned long riovcnt, unsigned long flags) {
    return syscall(__NR_process_vm_readv, pid, local_iov, liovcnt, remote_iov, riovcnt, flags);
}

using namespace game_offsets;

static pid_t     g_pid         = -1;
static uint64_t  g_il2cpp_base = 0;
static uint64_t  g_player_manager_class = 0;
static uint64_t  g_player_manager_static_fields = 0;
static uint64_t  g_game_controller_class = 0;
static uint64_t  g_local_player = 0;
static bool      g_matrix_configuration_validated = false;
static bool      g_camera_matrix_physical_match = false;
static uint64_t  g_player_position_offset = PLAYER_POSITION;

struct TransformHierarchyLayout {
    uint64_t data_offset = 0x38;
    uint64_t index_offset = 0x40;
    uint64_t matrices_offset = 0x18;
    uint64_t indices_offset = 0x20;
    bool matrices_indirect = false;
    bool indices_indirect = false;
};
static TransformHierarchyLayout g_transform_hierarchy_layout{};
static bool g_transform_hierarchy_layout_valid = false;

static bool      g_use_direct_player_position = true;
static bool      g_player_position_validated = false;

// Camera state captured by the last esp_get_boxes() call (used by the aimbot
// to convert bone positions into yaw/pitch offsets from the crosshair).
static float     g_cam_fov_deg = 0.0F;
static bool      g_cam_pose_valid = false;
// Reserved: was set when the pose had been recovered from the view matrix.
// That path is gone -- deriving the pose from the matrix made the aim throw
// itself across the screen, because the matrix the fallback reads is the
// stale cached one and every angle measured against it lags reality.
static bool      g_cam_pose_derived = false;
static Vec3      g_cam_pos{};
static Vec3      g_cam_right{}, g_cam_up{}, g_cam_forward{};

// The game fires along PlayerEventHandler.LookDirection (= MouseLook.m_LookRoot
// forward) from the KCC eye point, NOT along the camera transform (which carries
// visual sway/kick on top). Aim angles are therefore measured against this
// reference whenever it can be read, so the aimbot steers the actual firing
// direction onto the target instead of the camera.
static bool      g_aim_ref_valid = false;
static Vec3      g_aim_ref_origin{};
static Vec3      g_aim_ref_forward{}, g_aim_ref_right{}, g_aim_ref_up{};

// Маска ресурсов автофарма (bit0 дерево..bit3 сера).
static unsigned g_farm_mask = 0;


static bool vec3_is_finite(const Vec3& value);

template<typename T>
static T rd(uint64_t addr) {
    T v{};
    struct iovec lv = { &v, sizeof(T) };
    struct iovec rv = { (void*)addr, sizeof(T) };
    remote_vm_readv(g_pid, &lv, 1, &rv, 1, 0);
    return v;
}
template<typename T>
static bool rd_exact(uint64_t addr, T& value) {
    value = {};
    if (!addr) return false;
    struct iovec local = {&value, sizeof(T)};
    struct iovec remote = {(void*)addr, sizeof(T)};
    return remote_vm_readv(g_pid, &local, 1, &remote, 1, 0) == (ssize_t)sizeof(T);
}
static uint64_t rd_ptr(uint64_t a) { return rd<uint64_t>(a); }
static Vec3     rd_v3 (uint64_t a) { return rd<Vec3>(a);     }
static Mat4     rd_m4 (uint64_t a) { return rd<Mat4>(a);     }

static bool rd_buf(uint64_t addr, void* out, size_t size) {
    if (!addr || !size) return false;
    struct iovec local = {out, size};
    struct iovec remote = {(void*)addr, size};
    return remote_vm_readv(g_pid, &local, 1, &remote, 1, 0) == (ssize_t)size;
}

static std::string read_remote_string(uint64_t address) {
    if (!address) return {};
    char buffer[96]{};
    struct iovec local = {buffer, sizeof(buffer) - 1};
    struct iovec remote = {(void*)address, sizeof(buffer) - 1};
    ssize_t count = remote_vm_readv(g_pid, &local, 1, &remote, 1, 0);
    if (count <= 0) return {};
    buffer[sizeof(buffer) - 1] = '\0';
    return std::string(buffer);
}

static bool remote_string_equals(uint64_t address, const char* expected) {
    if (!address || !expected) return false;
    return read_remote_string(address) == expected;
}

// Managed Il2Cpp System.String (UTF-16) -> UTF-8, truncated to fit.
// Layout: klass @0x0, monitor @0x8, int32 length @0x10, chars @0x14.
// `max_chars` is a sanity bound on the managed length, not on the output: 31 is
// right for names and item ids, clan ids are GUID-shaped and need more.
static bool read_managed_string_ex(uint64_t str_obj, char* out, size_t cap, int32_t max_chars) {
    if (!str_obj || !out || cap < 2) return false;
    if ((str_obj & 0x1) != 0) return false;
    uint64_t klass = rd_ptr(str_obj);
    if (klass < 0x10000 || klass >= 0x0001000000000000ULL) return false;
    int32_t length = 0;
    if (!rd_exact(str_obj + IL2CPP_STRING_LENGTH, length)) return false;
    if (max_chars > 63) max_chars = 63;
    if (length <= 0 || length > max_chars) return false;
    uint16_t chars[64] = {};
    if (!rd_buf(str_obj + IL2CPP_STRING_CHARS, chars, (size_t)length * sizeof(uint16_t)))
        return false;
    size_t pos = 0;
    for (int32_t i = 0; i < length && pos + 1 < cap; ++i) {
        uint32_t cp = chars[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < length) {
            uint32_t lo = chars[i + 1];
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                ++i;
            } else {
                cp = '?';
            }
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            cp = '?';
        }
        char encoded[4];
        int encoded_len = 0;
        if (cp < 0x80) {
            if (cp < 0x20 || cp == 0x7F) cp = '?';
            encoded[0] = (char)cp; encoded_len = 1;
        } else if (cp < 0x800) {
            encoded[0] = (char)(0xC0 | (cp >> 6));
            encoded[1] = (char)(0x80 | (cp & 0x3F));
            encoded_len = 2;
        } else if (cp < 0x10000) {
            encoded[0] = (char)(0xE0 | (cp >> 12));
            encoded[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
            encoded[2] = (char)(0x80 | (cp & 0x3F));
            encoded_len = 3;
        } else if (cp <= 0x10FFFF) {
            encoded[0] = (char)(0xF0 | (cp >> 18));
            encoded[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
            encoded[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
            encoded[3] = (char)(0x80 | (cp & 0x3F));
            encoded_len = 4;
        } else {
            encoded[0] = '?'; encoded_len = 1;
        }
        if (pos + (size_t)encoded_len >= cap) break;
        for (int k = 0; k < encoded_len; ++k) out[pos++] = encoded[k];
    }
    out[pos] = '\0';
    return pos > 0;
}

static bool read_managed_string(uint64_t str_obj, char* out, size_t cap) {
    return read_managed_string_ex(str_obj, out, cap, 31);
}

// The legacy nicklabel widget keeps the "USERNAME" placeholder, so it is only
// a last resort. Primary source: Dissonance voice identity
// (PlayerManager.LLT -> VoicePlayerState.<Name>), secondary: the voice tracker
// string (PlayerManager.voicePlayer -> HvI).
static bool is_placeholder_name(const char* s) {
    if (!s) return true;
    const char* want = "USERNAME";
    size_t i = 0;
    for (; want[i]; ++i) {
        char c = s[i];
        if (!c) return true;
        if (c >= 'a' && c <= 'z') c -= (char)('a' - 'A');
        if (c != want[i]) return false;
    }
    return s[i] == '\0';
}

// Reject long pure-digit strings (SteamID-like), they are identifiers, not names.
static bool looks_like_long_id(const char* s) {
    if (!s || !s[0]) return true;
    size_t len = 0;
    for (; s[len]; ++len) {
        if (s[len] < '0' || s[len] > '9') return false;
    }
    return len >= 8;
}

// A machine-generated session/auth code (e.g. "F7GDJ6472D"): letters+digits
// only, no lowercase, no separators, 6..20 chars, and a mix of letters and
// digits. Such strings are identifiers, not display names, so a real nickname
// from any other source must win over them.
static bool looks_like_generated_id(const char* s) {
    if (!s || !s[0]) return true;
    size_t len = 0;
    bool has_lower = false, has_upper = false, has_digit = false, has_other = false;
    for (; s[len]; ++len) {
        unsigned char c = (unsigned char)s[len];
        if (c >= 'a' && c <= 'z') has_lower = true;
        else if (c >= 'A' && c <= 'Z') has_upper = true;
        else if (c >= '0' && c <= '9') has_digit = true;
        else has_other = true;
    }
    if (has_other || has_lower) return false;
    if (len < 6 || len > 20) return false;
    return has_upper && has_digit;
}

static bool accept_display_name(const char* s) {
    if (!s || !s[0]) return false;
    if (is_placeholder_name(s)) return false;
    if (looks_like_long_id(s)) return false;
    return true;
}

static bool valid_obj(uint64_t p) {
    return p >= 0x10000 && p < 0x0001000000000000ULL && (p & 0x7) == 0;
}

// Read one raw nickname candidate for a player. `src` selects the source:
//   0 PlayerManager.LLI (0x220) — the real human-readable display name
//   1 Dissonance VoicePlayerState.<Name>
//   2 Voice tracker (fuI) display string
//   3 Legacy nicklabel widget nickname text
static bool read_name_source(uint64_t player, int src, char* out, size_t cap) {
    if (!player || !out || cap < 2) return false;
    char tmp[32] = {};
    if (src == 0) {
        uint64_t str = rd_ptr(player + PLAYER_DISPLAY_NAME);
        if (!str || !read_managed_string(str, tmp, sizeof(tmp))) return false;
    } else if (src == 1) {
        uint64_t state = rd_ptr(player + PLAYER_VOICE_STATE);
        if (!valid_obj(state)) return false;
        uint64_t str = rd_ptr(state + VOICE_STATE_NAME);
        if (!str || !read_managed_string(str, tmp, sizeof(tmp))) return false;
    } else if (src == 2) {
        uint64_t tracker = rd_ptr(player + PLAYER_VOICE_PLAYER);
        if (!valid_obj(tracker)) return false;
        uint64_t str = rd_ptr(tracker + VOICE_PLAYER_TAG);
        if (!str || !read_managed_string(str, tmp, sizeof(tmp))) return false;
    } else {
        uint64_t label = rd_ptr(player + PLAYER_NICKLABEL);
        if (!valid_obj(label) || rd_ptr(label + NICKLABEL_PLAYER_BACKREF) != player) return false;
        uint64_t text = rd_ptr(label + NICKLABEL_NICKNAME_TEXT);
        if (!valid_obj(text)) return false;
        uint64_t str = rd_ptr(text + UI_TEXT_MTEXT);
        if (!str || !read_managed_string(str, tmp, sizeof(tmp))) return false;
    }
    if (!tmp[0]) return false;
    memcpy(out, tmp, cap);
    out[cap - 1] = '\0';
    return true;
}

// Visible nickname, best source first. Sources are ranked so a real,
// human-readable name always wins over a machine-generated id ("F7GDJ6472D");
// such an id is only used as a last resort when no real name is available.
// ---- Team / clan membership -------------------------------------------------
// PlayerManager syncs teamName / clanId / clanTag to every client, so two
// players can be compared directly. Team names are per-session and clan ids are
// stable, and either one matching makes the two players allies.
struct PlayerGroup {
    char team[40] = {};
    char clan[48] = {};
    char tag[16]  = {};
    bool any() const { return team[0] || clan[0] || tag[0]; }
};

static void read_player_group(uint64_t player, PlayerGroup& out) {
    out = PlayerGroup{};
    if (!valid_obj(player)) return;
    read_managed_string_ex(rd_ptr(player + PLAYER_TEAM_NAME), out.team, sizeof(out.team), 39);
    read_managed_string_ex(rd_ptr(player + PLAYER_CLAN_ID),   out.clan, sizeof(out.clan), 47);
    read_managed_string_ex(rd_ptr(player + PLAYER_CLAN_TAG),  out.tag,  sizeof(out.tag),  15);
}

static bool groups_are_allied(const PlayerGroup& local, const PlayerGroup& other) {
    if (local.team[0] && strcmp(local.team, other.team) == 0) return true;
    if (local.clan[0] && strcmp(local.clan, other.clan) == 0) return true;
    return false;
}

static bool player_display_name(uint64_t player, char* out, size_t cap) {
    if (!player || !out || cap < 2) return false;
    char human[32] = {};
    bool have_human = false;
    char fallback[32] = {};
    bool have_fallback = false;
    for (int src = 0; src < 4; ++src) {
        char tmp[32] = {};
        if (!read_name_source(player, src, tmp, sizeof(tmp))) continue;
        if (!accept_display_name(tmp)) continue;   // placeholder / pure-digit ids
        if (!have_fallback) { memcpy(fallback, tmp, sizeof(fallback)); have_fallback = true; }
        if (looks_like_generated_id(tmp)) continue; // machine code: fallback only
        if (!have_human) { memcpy(human, tmp, sizeof(human)); have_human = true; }
    }
    if (!have_human && !have_fallback) return false;
    const char* pick = have_human ? human : fallback;
    memcpy(out, pick, cap);
    out[cap - 1] = '\0';
    return true;
}

// Read a display name from an Oxide.ItemData: m_Name (0x18) else m_ShortName (0x20).
static bool read_item_data_display_name(uint64_t item_data, char* out, size_t cap) {
    if (!out || cap < 2) return false;
    out[0] = '\0';
    if (!valid_obj(item_data)) return false;
    char tmp[32] = {};
    uint64_t str = rd_ptr(item_data + ITEMDATA_NAME);
    if (!str || !read_managed_string(str, tmp, sizeof(tmp))) str = rd_ptr(item_data + ITEMDATA_SHORTNAME);
    if (str && read_managed_string(str, tmp, sizeof(tmp)) && tmp[0]) {
        memcpy(out, tmp, cap); out[cap - 1] = '\0'; return true;
    }
    return false;
}

// Held weapon display name. FP state is local-only (MonoBehaviour, never synced),
// so for remote players the FP objects may be missing: after the strict pass
// (FPObject -> player back-reference) a relaxed pass accepts the objects as-is.
static bool fp_object_display_name(uint64_t weapon, uint64_t player, bool strict,
                                   char* out, size_t cap) {
    if (!valid_obj(weapon)) return false;
    if (strict && rd_ptr(weapon + FPOBJECT_PLAYER_BACKREF) != player) return false;
    char tmp[32] = {};
    uint64_t str = rd_ptr(weapon + FPOBJECT_OBJECT_NAME);
    if (str && read_managed_string(str, tmp, sizeof(tmp)) && tmp[0]) {
        memcpy(out, tmp, cap);
        out[cap - 1] = '\0';
        return true;
    }
    // Fall back to the held item definition (Item -> ItemData m_Name/m_ShortName).
    uint64_t item = rd_ptr(weapon + FPOBJECT_ITEM);
    if (read_item_data_display_name(rd_ptr(item + ITEM_DATA), out, cap))
        return true;
    return false;
}

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
    {"assaultriffle",         "Assault Rifle",   "Автомат"},
    {"assaultrifle",          "Assault Rifle",   "Автомат"},
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

// Canonicalise the label: replace it with the game's own name for that weapon
// so the same gun always reads the same, whichever source it came from and
// whatever decoration its skin prefab carried. Weapons missing from the table
// keep their cleaned name, so nothing ever disappears from the box.
//
// Russian labels are on; set to false to print the game's own English names.
static constexpr bool kWeaponLabelRussian = true;

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
    const char* pick = (kWeaponLabelRussian && best->ru) ? best->ru : best->en;
    if (!pick || !pick[0]) return false;
    strncpy(label, pick, cap - 1);
    label[cap - 1] = '\0';
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
static bool player_weapon_name(uint64_t player, char* out, size_t cap, bool& definite) {
    if (!player_weapon_name_raw(player, out, cap, definite)) return false;
    fix_weapon_label_spelling(out);
    canonical_weapon_label(out, cap);
    return out[0] != '\0';
}

// Cached display strings per player (updated on success only, so a transient
// failed read never makes labels flicker). Refreshed periodically to pick up
// nickname/weapon changes.
struct PlayerTextCache {
    char name[32] = {};
    bool has_name = false;
    char weapon[48] = {}; // localized UTF-8, matches EspBox::weapon
    bool has_weapon = false;
    bool ally = false;    // shares the local player's team or clan
    char tag[16] = {};    // clan tag
    bool has_tag = false;
    int  revalidate = 30; // first sighting resolves immediately
};
static std::unordered_map<uint64_t, PlayerTextCache> g_player_text;

static void prune_player_text(const std::vector<uint64_t>& players) {
    for (auto it = g_player_text.begin(); it != g_player_text.end();) {
        bool present = false;
        for (uint64_t player : players) if (player == it->first) { present = true; break; }
        if (!present) it = g_player_text.erase(it); else ++it;
    }
}

static uint64_t get_base(const char* lib) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", g_pid);
    FILE* file = fopen(path, "r");
    if (!file) return 0;
    char line[512];
    uint64_t fallback = 0;
    while (fgets(line, sizeof(line), file)) {
        if (!strstr(line, lib)) continue;
        uint64_t start = 0, end = 0, file_offset = 0;
        char permissions[5]{};
        if (sscanf(line, "%lx-%lx %4s %lx", &start, &end, permissions, &file_offset) != 4) continue;
        uint64_t load_bias = start - file_offset;
        if (!fallback || load_bias < fallback) fallback = load_bias;
        if (file_offset == 0) { fclose(file); return start; }
    }
    fclose(file);
    return fallback;
}

static bool validate_player_list(uint64_t list, uint64_t player_class) {
    if (!list || !player_class) return false;
    uint64_t items = rd_ptr(list + IL2CPP_LIST_ITEMS);
    int32_t count = rd<int32_t>(list + IL2CPP_LIST_SIZE);
    if (!items || count < 0 || count > 512) return false;
    if (count == 0) {
        uint64_t list_class = rd_ptr(list);
        return remote_string_equals(rd_ptr(list_class + 0x10), "List`1") &&
            remote_string_equals(rd_ptr(list_class + 0x18), "System.Collections.Generic");
    }
    int32_t checked = 0;
    for (int32_t index = 0; index < count && checked < 4; ++index) {
        uint64_t player = rd_ptr(items + IL2CPP_ARRAY_FIRST_ELEMENT + (uint64_t)index * sizeof(uint64_t));
        if (!player) continue;
        if (rd_ptr(player) != player_class) return false;
        ++checked;
    }
    return checked > 0;
}

static bool player_list_contains(uint64_t list, uint64_t player) {
    if (!list || !player) return false;
    uint64_t items = rd_ptr(list + IL2CPP_LIST_ITEMS);
    int32_t count = rd<int32_t>(list + IL2CPP_LIST_SIZE);
    if (!items || count <= 0 || count > 512) return false;
    for (int32_t index = 0; index < count; ++index) {
        if (rd_ptr(items + IL2CPP_ARRAY_FIRST_ELEMENT + (uint64_t)index * sizeof(uint64_t)) == player)
            return true;
    }
    return false;
}

static constexpr uint64_t IL2CPP_CLASS_STATIC_FIELDS = 0xB8;

static uint64_t get_class_static_fields(uint64_t klass) {
    if (!klass) return 0;
    return rd_ptr(klass + IL2CPP_CLASS_STATIC_FIELDS);
}

static uint64_t resolve_runtime_player_list() {
    if (!g_player_manager_class && PLAYER_MANAGER_TYPEINFO_RVA != 0) {
        uint64_t candidate = rd_ptr(g_il2cpp_base + PLAYER_MANAGER_TYPEINFO_RVA);
        if (candidate) {
            std::string name = read_remote_string(rd_ptr(candidate + 0x10));
            std::string ns   = read_remote_string(rd_ptr(candidate + 0x18));
            if (name == "PlayerManager" && ns == "Oxide")
                g_player_manager_class = candidate;
        }
    }
    if (!g_player_manager_class) {
        return 0;
    }
    if (!g_player_manager_static_fields)
        g_player_manager_static_fields = get_class_static_fields(g_player_manager_class);
    if (!g_player_manager_static_fields) {
        return 0;
    }
    uint64_t list = rd_ptr(g_player_manager_static_fields + PLAYER_MANAGER_STATIC_FIELDS_LIST);
    if (!validate_player_list(list, g_player_manager_class)) {
        g_player_manager_static_fields = 0;
        return 0;
    }
    return list;
}

static uint64_t resolve_local_player() {
    if (g_local_player && rd_ptr(g_local_player) == g_player_manager_class)
        return g_local_player;
    g_local_player = 0;

    if (!g_game_controller_class && GAME_CONTROLLER_TYPEINFO_RVA != 0) {
        uint64_t candidate = rd_ptr(g_il2cpp_base + GAME_CONTROLLER_TYPEINFO_RVA);
        if (candidate) {
            std::string name = read_remote_string(rd_ptr(candidate + 0x10));
            std::string ns   = read_remote_string(rd_ptr(candidate + 0x18));
            if (name == "GameControllerBase" && ns == "Oxide")
                g_game_controller_class = candidate;
        }
    }

    if (!g_game_controller_class || !g_player_manager_class) return 0;

    uint64_t gcb_static_fields = get_class_static_fields(g_game_controller_class);
    if (!gcb_static_fields) return 0;
    uint64_t local_player = rd_ptr(gcb_static_fields + GAME_CONTROLLER_LOCAL_PLAYER_FIELD);
    if (local_player && rd_ptr(local_player) == g_player_manager_class) {
        g_local_player = local_player;
        return local_player;
    }
    return 0;
}

static uint64_t resolve_native_transform(uint64_t transform) {
    if (!transform) return 0;
    return rd_ptr(transform + MANAGED_CACHED_PTR);
}

static bool vec3_is_finite(const Vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) &&
        fabsf(value.x) < 1000000.0F && fabsf(value.y) < 1000000.0F && fabsf(value.z) < 1000000.0F;
}

static Vec3 cross_product(const Vec3& left, const Vec3& right) {
    return {left.y * right.z - left.z * right.y, left.z * right.x - left.x * right.z, left.x * right.y - left.y * right.x};
}

static Vec3 rotate_vector(const Vec4& quaternion, const Vec3& vector) {
    Vec3 q = {quaternion.x, quaternion.y, quaternion.z};
    Vec3 first_cross = cross_product(q, vector);
    Vec3 doubled = {first_cross.x * 2.0F, first_cross.y * 2.0F, first_cross.z * 2.0F};
    Vec3 second_cross = cross_product(q, doubled);
    return {vector.x + quaternion.w * doubled.x + second_cross.x, vector.y + quaternion.w * doubled.y + second_cross.y, vector.z + quaternion.w * doubled.z + second_cross.z};
}

static Vec4 multiply_quaternion(const Vec4& left, const Vec4& right) {
    return {
        left.w * right.x + left.x * right.w + left.y * right.z - left.z * right.y,
        left.w * right.y - left.x * right.z + left.y * right.w + left.z * right.x,
        left.w * right.z + left.x * right.y - left.y * right.x + left.z * right.w,
        left.w * right.w - left.x * right.x - left.y * right.y - left.z * right.z
    };
}

static bool normalize_quaternion(Vec4& quaternion) {
    float length_squared = quaternion.x * quaternion.x + quaternion.y * quaternion.y + quaternion.z * quaternion.z + quaternion.w * quaternion.w;
    if (!std::isfinite(length_squared) || length_squared < 0.000001F) return false;
    float inverse_length = 1.0F / sqrtf(length_squared);
    quaternion.x *= inverse_length; quaternion.y *= inverse_length; quaternion.z *= inverse_length; quaternion.w *= inverse_length;
    return true;
}

static bool matrix34_is_valid(const Matrix34& matrix) {
    // NOTE: translation.w and scale.w are SIMD padding lanes. The game's
    // animation/IK code leaves garbage (NaN/huge values) there for bones it
    // actively writes every frame (arms while aiming, legs while walking), so
    // those lanes must NOT be validated - only the meaningful components.
    const float values[] = {
        matrix.translation.x, matrix.translation.y, matrix.translation.z,
        matrix.rotation.x, matrix.rotation.y, matrix.rotation.z, matrix.rotation.w,
        matrix.scale.x, matrix.scale.y, matrix.scale.z
    };
    for (float value : values) { if (!std::isfinite(value) || fabsf(value) > 1000000.0F) return false; }
    float quaternion_length = matrix.rotation.x * matrix.rotation.x + matrix.rotation.y * matrix.rotation.y + matrix.rotation.z * matrix.rotation.z + matrix.rotation.w * matrix.rotation.w;
    return quaternion_length >= 0.20F && quaternion_length <= 2.0F && fabsf(matrix.scale.x) <= 10000.0F && fabsf(matrix.scale.y) <= 10000.0F && fabsf(matrix.scale.z) <= 10000.0F;
}

static bool read_transform_hierarchy_arrays(uint64_t matrices, uint64_t indices, int32_t transform_index, Vec3& position, Vec4* world_rotation = nullptr) {
    if (!matrices || !indices || transform_index < 0 || transform_index > 100000) return false;
    Matrix34 current{};
    if (!rd_exact(matrices + (uint64_t)transform_index * sizeof(Matrix34), current) || !matrix34_is_valid(current)) return false;
    Vec3 result = {current.translation.x, current.translation.y, current.translation.z};
    Vec4 result_rotation = current.rotation;
    if (!vec3_is_finite(result)) return false;
    int32_t parent = -2;
    if (!rd_exact(indices + (uint64_t)transform_index * sizeof(int32_t), parent)) return false;
    int32_t previous_parent = transform_index;
    int depth = 0;
    while (parent >= 0 && depth++ < 128) {
        if (parent > 100000 || parent == previous_parent) return false;
        Matrix34 matrix{};
        if (!rd_exact(matrices + (uint64_t)parent * sizeof(Matrix34), matrix) || !matrix34_is_valid(matrix)) return false;
        Vec3 scaled = {result.x * matrix.scale.x, result.y * matrix.scale.y, result.z * matrix.scale.z};
        Vec3 rotated = rotate_vector(matrix.rotation, scaled);
        result = {matrix.translation.x + rotated.x, matrix.translation.y + rotated.y, matrix.translation.z + rotated.z};
        result_rotation = multiply_quaternion(matrix.rotation, result_rotation);
        if (!vec3_is_finite(result)) return false;
        previous_parent = parent;
        if (!rd_exact(indices + (uint64_t)parent * sizeof(int32_t), parent)) return false;
    }
    if (parent != -1 || depth >= 128 || !vec3_is_finite(result)) return false;
    if (world_rotation) { if (!normalize_quaternion(result_rotation)) return false; *world_rotation = result_rotation; }
    position = result;
    return true;
}

static bool read_transform_hierarchy_layout(uint64_t native_transform, const TransformHierarchyLayout& layout, Vec3& position, Vec4* world_rotation = nullptr) {
    if (!native_transform) return false;
    uint64_t transform_data = rd_ptr(native_transform + layout.data_offset);
    int32_t transform_index = rd<int32_t>(native_transform + layout.index_offset);
    if (!transform_data || transform_index < 0 || transform_index > 100000) return false;
    uint64_t matrices = rd_ptr(transform_data + layout.matrices_offset);
    uint64_t indices = rd_ptr(transform_data + layout.indices_offset);
    if (layout.matrices_indirect) matrices = rd_ptr(matrices);
    if (layout.indices_indirect) indices = rd_ptr(indices);
    return read_transform_hierarchy_arrays(matrices, indices, transform_index, position, world_rotation);
}

static bool read_transform_hierarchy_position(uint64_t native_transform, Vec3& position) {
    if (!native_transform) return false;
    if (g_transform_hierarchy_layout_valid)
        return read_transform_hierarchy_layout(native_transform, g_transform_hierarchy_layout, position);
    uint64_t transform_data = rd_ptr(native_transform + 0x38);
    int32_t transform_index = rd<int32_t>(native_transform + 0x40);
    if (!transform_data || transform_index < 0 || transform_index > 100000) return false;
    const uint64_t data_offsets[][2] = {{0x18, 0x20}, {0x08, 0x10}};
    for (const auto& offsets : data_offsets) {
        uint64_t matrix_pointer = rd_ptr(transform_data + offsets[0]);
        uint64_t index_pointer = rd_ptr(transform_data + offsets[1]);
        if (!matrix_pointer || !index_pointer) continue;
        const uint64_t matrix_candidates[] = {matrix_pointer, rd_ptr(matrix_pointer)};
        const uint64_t index_candidates[] = {index_pointer, rd_ptr(index_pointer)};
        for (uint64_t matrices : matrix_candidates) {
            for (uint64_t indices_ptr : index_candidates) {
                if (read_transform_hierarchy_arrays(matrices, indices_ptr, transform_index, position)) return true;
            }
        }
    }
    return false;
}

static uint64_t resolve_player_native_transform(uint64_t player) {
    if (!player) return 0;
    return resolve_native_transform(rd_ptr(player + PLAYER_TRANSFORM));
}

static bool likely_native_pointer(uint64_t value) {
    return value >= 0x10000 && value < 0x0001000000000000ULL && (value & 0x7) == 0;
}

static bool evaluate_transform_hierarchy_layout(const std::vector<uint64_t>& native_transforms, const TransformHierarchyLayout& layout, size_t& position_count, double& extent) {
    position_count = 0; extent = 0.0;
    Vec3 minimum{}, maximum{};
    bool initialized = false;
    for (uint64_t native_transform : native_transforms) {
        Vec3 position{};
        if (!read_transform_hierarchy_layout(native_transform, layout, position)) continue;
        ++position_count;
        if (!initialized) { minimum = position; maximum = position; initialized = true; }
        else {
            minimum.x = std::min(minimum.x, position.x); minimum.y = std::min(minimum.y, position.y); minimum.z = std::min(minimum.z, position.z);
            maximum.x = std::max(maximum.x, position.x); maximum.y = std::max(maximum.y, position.y); maximum.z = std::max(maximum.z, position.z);
        }
    }
    if (!initialized) return false;
    extent = fabs((double)maximum.x - minimum.x) + fabs((double)maximum.y - minimum.y) + fabs((double)maximum.z - minimum.z);
    return position_count >= 2 && std::isfinite(extent) && extent >= 0.1 && extent <= 1000000.0;
}

static bool discover_transform_hierarchy_layout(const std::vector<uint64_t>& players, size_t& best_position_count, size_t& candidate_count) {
    std::vector<uint64_t> native_transforms;
    std::unordered_set<uint64_t> unique_transforms;
    for (uint64_t player : players) {
        uint64_t native_transform = resolve_player_native_transform(player);
        if (native_transform && unique_transforms.insert(native_transform).second)
            native_transforms.push_back(native_transform);
    }
    if (native_transforms.size() < 2) return false;

    const int64_t index_deltas[] = {-8, 8, 16, 24};
    TransformHierarchyLayout best_layout{};
    double best_score = 0.0;
    best_position_count = 0; candidate_count = 0;
    size_t seed_count = std::min<size_t>(native_transforms.size(), 3);

    for (size_t seed_index = 0; seed_index < seed_count; ++seed_index) {
        uint64_t seed = native_transforms[seed_index];
        for (uint64_t data_offset = 0x10; data_offset <= 0x200; data_offset += 8) {
            uint64_t transform_data = rd_ptr(seed + data_offset);
            if (!likely_native_pointer(transform_data)) continue;
            for (int64_t index_delta : index_deltas) {
                int64_t signed_index_offset = (int64_t)data_offset + index_delta;
                if (signed_index_offset < 0x10 || signed_index_offset > 0x220) continue;
                uint64_t index_offset = (uint64_t)signed_index_offset;
                int32_t transform_index = rd<int32_t>(seed + index_offset);
                if (transform_index < 0 || transform_index > 100000) continue;
                for (uint64_t matrices_offset = 0; matrices_offset <= 0x100; matrices_offset += 8) {
                    uint64_t indices_offset = matrices_offset + 8;
                    uint64_t matrices = rd_ptr(transform_data + matrices_offset);
                    uint64_t indices_ptr = rd_ptr(transform_data + indices_offset);
                    if (!likely_native_pointer(matrices) || !likely_native_pointer(indices_ptr)) continue;
                    for (int matrices_indirect = 0; matrices_indirect < 2; ++matrices_indirect) {
                        for (int indices_indirect = 0; indices_indirect < 2; ++indices_indirect) {
                            TransformHierarchyLayout layout{};
                            layout.data_offset = data_offset; layout.index_offset = index_offset;
                            layout.matrices_offset = matrices_offset; layout.indices_offset = indices_offset;
                            layout.matrices_indirect = matrices_indirect != 0; layout.indices_indirect = indices_indirect != 0;
                            Vec3 seed_position{};
                            if (!read_transform_hierarchy_layout(seed, layout, seed_position)) continue;
                            ++candidate_count;
                            size_t position_count = 0; double extent = 0.0;
                            bool valid = evaluate_transform_hierarchy_layout(native_transforms, layout, position_count, extent);
                            best_position_count = std::max(best_position_count, position_count);
                            if (!valid) continue;
                            double score = (double)position_count * 1000000.0 + std::min(extent, 999999.0);
                            if (score > best_score) { best_score = score; best_layout = layout; }
                        }
                    }
                }
            }
        }
    }
    if (best_score <= 0.0) return false;
    g_transform_hierarchy_layout = best_layout;
    g_transform_hierarchy_layout_valid = true;
    return true;
}

static bool read_entity_position(uint64_t source, Vec3& position) {
    if (!source) return false;
    if (g_use_direct_player_position && g_player_position_offset != 0) { position = rd_v3(source + g_player_position_offset); return vec3_is_finite(position); }
    uint64_t native = resolve_player_native_transform(source);
    if (!native) return false;
    return read_transform_hierarchy_position(native, position);
}

static bool read_entity_pose(uint64_t source, Vec3& position, Vec4& rotation) {
    if (!source || g_use_direct_player_position || !g_transform_hierarchy_layout_valid) return false;
    uint64_t native = resolve_player_native_transform(source);
    if (!native) return false;
    return read_transform_hierarchy_layout(native, g_transform_hierarchy_layout, position, &rotation);
}

// A world position we would believe from a single sample: inside the map
// bounds and not a pile of denormals. Used when we are the only player on the
// server, where the "several players spread out" test below cannot run.
static bool position_looks_like_world_space(const Vec3& position) {
    if (!vec3_is_finite(position)) return false;
    if (fabsf(position.x) > 20000.0F || fabsf(position.z) > 20000.0F) return false;
    if (fabsf(position.y) > 10000.0F) return false;
    return fabsf(position.x) + fabsf(position.y) + fabsf(position.z) > 0.01F;
}

static bool evaluate_player_position_offset(const std::vector<uint64_t>& players, uint64_t offset, double& score) {
    score = 0.0;
    if (!offset) return false;
    size_t valid = 0, non_zero = 0;
    Vec3 minimum{}, maximum{};
    bool initialized = false;
    for (uint64_t player : players) {
        if (!player) continue;
        Vec3 position = rd_v3(player + offset);
        if (!vec3_is_finite(position)) continue;
        float magnitude = fabsf(position.x) + fabsf(position.y) + fabsf(position.z);
        if (magnitude < 0.01F) continue;
        ++valid; ++non_zero;
        if (!initialized) { minimum = position; maximum = position; initialized = true; }
        else {
            minimum.x = std::min(minimum.x, position.x); minimum.y = std::min(minimum.y, position.y); minimum.z = std::min(minimum.z, position.z);
            maximum.x = std::max(maximum.x, position.x); maximum.y = std::max(maximum.y, position.y); maximum.z = std::max(maximum.z, position.z);
        }
    }
    if (!initialized || valid < 1 || non_zero < 1) return false;
    double extent = fabs((double)maximum.x - minimum.x) + fabs((double)maximum.y - minimum.y) + fabs((double)maximum.z - minimum.z);
    if (!std::isfinite(extent) || extent > 1000000.0) return false;
    // Alone on the server (or everybody standing on the same spot) there is no
    // spread to measure, so a single plausible world position has to do. Not
    // accepting it used to kill the whole ESP after a solo respawn: the offset
    // never re-validated and every frame bailed out early until app restart.
    if (valid < 2 || extent < 0.1) {
        if (!position_looks_like_world_space(minimum)) return false;
        score = (double)valid * 1000000.0;
        return true;
    }
    score = (double)valid * 1000000.0 + std::min(extent, 999999.0);
    return true;
}

// Direct (PlayerManager field) position offsets, most trusted first. The
// canonical field is lastSavedPosition (0x1D0); lastTickPosition (0x1C8) is
// equivalent. The rest are legacy guesses kept as a last resort only.
static const uint64_t k_known_position_offsets[] = {0x1D0, 0x1C8, 0x1E0, 0x2D0, 0x2DC, 0x1D4, 0x1DC, 0x1E8};

static uint64_t find_direct_player_position_offset(const std::vector<uint64_t>& players) {
    bool saved_use_direct = g_use_direct_player_position;
    g_use_direct_player_position = true;
    uint64_t best_offset = 0;
    double best_score = 0.0;
    for (uint64_t offset : k_known_position_offsets) {
        double score = 0.0;
        if (!evaluate_player_position_offset(players, offset, score)) continue;
        // Prefer the offset that validates for the most players; on a tie the
        // earlier (more trusted) entry wins regardless of spatial extent.
        double count = floor(score / 1000000.0), best_count = floor(best_score / 1000000.0);
        if (count > best_count) { best_offset = offset; best_score = score; }
    }
    g_use_direct_player_position = saved_use_direct;
    return best_offset;
}

// Frames in a row the direct offsets failed to validate. Right after a world
// reload the position fields are still zero for a few frames; falling back to
// the transform-hierarchy path on the very first failure used to lock the ESP
// into that mode (boxes hanging 1.6 m below the player) until restart.
static int g_direct_position_fail_streak = 0;
static int g_direct_position_recheck = 0;
static bool g_body_caches_dirty = false; // clear per-player caches on the next frame

static bool discover_player_position_offset(const std::vector<uint64_t>& players) {
    uint64_t best_offset = find_direct_player_position_offset(players);
    if (best_offset) {
        g_direct_position_fail_streak = 0;
        g_use_direct_player_position = true; g_player_position_offset = best_offset;
        g_player_position_validated = true; g_matrix_configuration_validated = false;
        return true;
    }
    // Give the direct fields a full second to settle before trying anything else.
    if (++g_direct_position_fail_streak < 60) return false;
    size_t discovered_position_count = 0, hierarchy_candidate_count = 0;
    if (discover_transform_hierarchy_layout(players, discovered_position_count, hierarchy_candidate_count)) {
        g_use_direct_player_position = false;
        g_player_position_validated = true; g_matrix_configuration_validated = false;
        g_direct_position_recheck = 0;
        return true;
    }
    return false;
}

// While on the hierarchy fallback, keep probing the direct fields and switch
// back as soon as they validate again.
static void recheck_direct_player_position(const std::vector<uint64_t>& players) {
    if (g_use_direct_player_position || !g_player_position_validated) return;
    if (++g_direct_position_recheck < 15) return;
    g_direct_position_recheck = 0;
    uint64_t best_offset = find_direct_player_position_offset(players);
    if (!best_offset) return;
    g_direct_position_fail_streak = 0;
    g_use_direct_player_position = true; g_player_position_offset = best_offset;
    g_matrix_configuration_validated = false;
    g_body_caches_dirty = true;
}

// Unity Matrix4x4 is column-major in memory: m[col*4 + row].
static float mat_get(const Mat4& matrix, int row, int column) {
    return matrix.m[(size_t)column * 4 + row];
}
static void mat_set(Mat4& matrix, int row, int column, float value) {
    matrix.m[(size_t)column * 4 + row] = value;
}
static bool matrix_is_finite(const Mat4& matrix) {
    bool has_non_zero = false;
    for (float value : matrix.m) {
        if (!std::isfinite(value) || fabsf(value) > 1000000.0F) return false;
        if (fabsf(value) > 0.000001F) has_non_zero = true;
    }
    return has_non_zero;
}
static Mat4 mat_mul(const Mat4& a, const Mat4& b) {
    // result = a * b (column-major, same as Unity Matrix4x4 operator*)
    Mat4 result{};
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row) {
            float value = 0.0F;
            for (int k = 0; k < 4; ++k) value += mat_get(a, row, k) * mat_get(b, k, column);
            mat_set(result, row, column, value);
        }
    return result;
}

static Mat4 mat_perspective(float fov_degrees, float aspect, float z_near, float z_far) {
    Mat4 result{};
    if (!(fov_degrees > 0.1F && fov_degrees < 179.0F) || !(aspect > 0.05F) || !(z_near > 0.0F) || !(z_far > z_near))
        return result;
    float fov_rad = fov_degrees * 0.01745329251F;
    float cotangent = 1.0F / tanf(fov_rad * 0.5F);
    mat_set(result, 0, 0, cotangent / aspect);
    mat_set(result, 1, 1, cotangent);
    mat_set(result, 2, 2, -(z_far + z_near) / (z_far - z_near));
    mat_set(result, 2, 3, -(2.0F * z_far * z_near) / (z_far - z_near));
    mat_set(result, 3, 2, -1.0F);
    return result;
}

// worldToCamera from camera world pose (Unity camera looks down -Z).
static Mat4 mat_world_to_camera(const Vec3& position, const Vec4& rotation) {
    Vec3 right = rotate_vector(rotation, {1.0F, 0.0F, 0.0F});
    Vec3 up = rotate_vector(rotation, {0.0F, 1.0F, 0.0F});
    Vec3 forward = rotate_vector(rotation, {0.0F, 0.0F, 1.0F});
    // View basis: rows = right, up, -forward (camera space).
    Mat4 view{};
    mat_set(view, 0, 0, right.x);   mat_set(view, 0, 1, right.y);   mat_set(view, 0, 2, right.z);
    mat_set(view, 1, 0, up.x);      mat_set(view, 1, 1, up.y);      mat_set(view, 1, 2, up.z);
    mat_set(view, 2, 0, -forward.x); mat_set(view, 2, 1, -forward.y); mat_set(view, 2, 2, -forward.z);
    mat_set(view, 0, 3, -(right.x * position.x + right.y * position.y + right.z * position.z));
    mat_set(view, 1, 3, -(up.x * position.x + up.y * position.y + up.z * position.z));
    mat_set(view, 2, 3, -(-forward.x * position.x + -forward.y * position.y + -forward.z * position.z));
    mat_set(view, 3, 3, 1.0F);
    return view;
}

static bool camera_position_from_view(const Mat4& view, Vec3& position) {
    // For orthonormal worldToCamera: cam_pos = -R^T * t
    float r00 = mat_get(view, 0, 0), r01 = mat_get(view, 0, 1), r02 = mat_get(view, 0, 2);
    float r10 = mat_get(view, 1, 0), r11 = mat_get(view, 1, 1), r12 = mat_get(view, 1, 2);
    float r20 = mat_get(view, 2, 0), r21 = mat_get(view, 2, 1), r22 = mat_get(view, 2, 2);
    float tx = mat_get(view, 0, 3), ty = mat_get(view, 1, 3), tz = mat_get(view, 2, 3);
    position = {
        -(r00 * tx + r10 * ty + r20 * tz),
        -(r01 * tx + r11 * ty + r21 * tz),
        -(r02 * tx + r12 * ty + r22 * tz)
    };
    return vec3_is_finite(position);
}

static bool w2s(const Mat4& vp, const Vec3& world, float sw, float sh, Vec2& out, bool clip_to_screen = true) {
    // clip = VP * float4(world, 1) with column-major VP
    float clip_x = mat_get(vp, 0, 0) * world.x + mat_get(vp, 0, 1) * world.y + mat_get(vp, 0, 2) * world.z + mat_get(vp, 0, 3);
    float clip_y = mat_get(vp, 1, 0) * world.x + mat_get(vp, 1, 1) * world.y + mat_get(vp, 1, 2) * world.z + mat_get(vp, 1, 3);
    float clip_w = mat_get(vp, 3, 0) * world.x + mat_get(vp, 3, 1) * world.y + mat_get(vp, 3, 2) * world.z + mat_get(vp, 3, 3);
    if (!std::isfinite(clip_x) || !std::isfinite(clip_y) || !std::isfinite(clip_w) || clip_w <= 0.001F) return false;
    out.x = ((clip_x / clip_w) + 1.0F) * 0.5F * sw;
    out.y = ((-clip_y / clip_w) + 1.0F) * 0.5F * sh;
    if (!std::isfinite(out.x) || !std::isfinite(out.y)) return false;
    if (clip_to_screen && (out.x < 0.0F || out.x > sw || out.y < 0.0F || out.y > sh)) return false;
    return true;
}

static bool read_camera_transform_pose(uint64_t native_transform, Vec3& position, Vec4& rotation) {
    if (!native_transform) return false;
    if (g_transform_hierarchy_layout_valid) {
        if (read_transform_hierarchy_layout(native_transform, g_transform_hierarchy_layout, position, &rotation))
            return true;
    }
    // Probe the common TransformAccess layouts used elsewhere in this file.
    uint64_t transform_data = rd_ptr(native_transform + 0x38);
    int32_t transform_index = rd<int32_t>(native_transform + 0x40);
    if (!transform_data || transform_index < 0 || transform_index > 100000) {
        transform_data = rd_ptr(native_transform + 0x18);
        transform_index = rd<int32_t>(native_transform + 0x20);
    }
    if (!transform_data || transform_index < 0 || transform_index > 100000) return false;
    const uint64_t data_offsets[][2] = {{0x18, 0x20}, {0x08, 0x10}};
    for (const auto& offsets : data_offsets) {
        uint64_t matrix_pointer = rd_ptr(transform_data + offsets[0]);
        uint64_t index_pointer = rd_ptr(transform_data + offsets[1]);
        if (!matrix_pointer || !index_pointer) continue;
        const uint64_t matrix_candidates[] = {matrix_pointer, rd_ptr(matrix_pointer)};
        const uint64_t index_candidates[] = {index_pointer, rd_ptr(index_pointer)};
        for (uint64_t matrices : matrix_candidates) {
            for (uint64_t indices_ptr : index_candidates) {
                if (read_transform_hierarchy_arrays(matrices, indices_ptr, transform_index, position, &rotation))
                    return true;
            }
        }
    }
    return false;
}

static bool read_native_camera_matrices(uint64_t native_cam, float screen_aspect, Mat4& projection, Mat4& view) {
    if (!native_cam) return false;

    static Mat4 s_last_view{};
    static Mat4 s_last_proj{};
    static bool s_last_ok = false;

    // Fresh view from the Camera's live Transform (cam+0x20). The +0x70 cache is
    // only rebuilt inside Unity getters when dirty-flag +0x502 is set — we never
    // run those getters, so raw +0x70 drifts while the camera moves.
    bool have_live_view = false;
    uint64_t native_transform = rd_ptr(native_cam + CAMERA_NATIVE_TRANSFORM);
    if (native_transform) {
        Vec3 cam_pos{};
        Vec4 cam_rot{};
        bool pose_ok = read_camera_transform_pose(native_transform, cam_pos, cam_rot);
        bool fin_ok = pose_ok && vec3_is_finite(cam_pos);
        bool quat_ok = fin_ok && normalize_quaternion(cam_rot);
        // Teleport rejection: a read that lands mid-update inside the game
        // can return a garbage-but-finite pose. One such frame throws every
        // box/marker across the screen (the flicker artifacts). A camera
        // cannot move 30 m in one frame — reject the sample and reuse the
        // last view; a REAL teleport (respawn) sticks, so after a few
        // consecutive "jumps" the new position is accepted.
        static Vec3 s_last_cam_pos{};
        static bool s_last_cam_pos_ok = false;
        static int  s_pose_jump_streak = 0;
        if (quat_ok && s_last_cam_pos_ok && s_last_ok) {
            float jx = cam_pos.x - s_last_cam_pos.x;
            float jy = cam_pos.y - s_last_cam_pos.y;
            float jz = cam_pos.z - s_last_cam_pos.z;
            float j2 = jx * jx + jy * jy + jz * jz;
            if (j2 > 30.0F * 30.0F && s_pose_jump_streak < 4) {
                ++s_pose_jump_streak;
                quat_ok = false; // fall through to the cached view below
            } else {
                s_pose_jump_streak = 0;
            }
        }
        if (quat_ok) {
            view = mat_world_to_camera(cam_pos, cam_rot);
            have_live_view = matrix_is_finite(view);
            if (have_live_view) {
                g_cam_pos = cam_pos;
                g_cam_right = rotate_vector(cam_rot, {1.0F, 0.0F, 0.0F});
                g_cam_up = rotate_vector(cam_rot, {0.0F, 1.0F, 0.0F});
                g_cam_forward = rotate_vector(cam_rot, {0.0F, 0.0F, 1.0F});
                g_cam_pose_valid = true;
                s_last_cam_pos = cam_pos;
                s_last_cam_pos_ok = true;
            }
        }
    }
    if (!have_live_view) {
        if (s_last_ok && matrix_is_finite(s_last_view)) {
            view = s_last_view;
        } else {
            view = rd_m4(native_cam + CAMERA_VIEW_MATRIX);
            if (!matrix_is_finite(view)) return false;
        }
    } else {
        s_last_view = view;
    }

    // Projection params (FOV/aspect/clip) are stored as plain floats and stay hot.
    float fov = rd<float>(native_cam + CAMERA_FOV_DEGREES);
    float aspect = rd<float>(native_cam + CAMERA_ASPECT);
    float z_near = rd<float>(native_cam + CAMERA_NEAR_CLIP);
    float z_far = rd<float>(native_cam + CAMERA_FAR_CLIP);
    if (!(aspect > 0.1F && aspect < 10.0F))
        aspect = (screen_aspect > 0.1F && screen_aspect < 10.0F) ? screen_aspect : (9.0F / 16.0F);
    if (!(z_near > 0.001F && z_near < 100.0F)) z_near = 0.1F;
    if (!(z_far > z_near && z_far < 100000.0F)) z_far = 1000.0F;
    if (std::isfinite(fov) && fov > 1.0F && fov < 179.0F) g_cam_fov_deg = fov;
    projection = mat_perspective(fov, aspect, z_near, z_far);
    if (!matrix_is_finite(projection)) {
        if (s_last_ok && matrix_is_finite(s_last_proj)) {
            projection = s_last_proj;
        } else {
            projection = rd_m4(native_cam + CAMERA_PROJECTION_MATRIX);
            if (!matrix_is_finite(projection)) return false;
        }
    }
    if (matrix_is_finite(projection)) s_last_proj = projection;
    if (have_live_view && matrix_is_finite(view)) {
        s_last_view = view;
        s_last_ok = true;
    }
    return true;
}

static bool w2s_transform_camera(const Vec3& camera_position, const Vec4& camera_rotation, const Vec3& world, float screen_width, float screen_height, Vec2& output, bool clip_to_screen = true) {
    if (screen_width < 100.0F || screen_height < 100.0F) return false;
    Vec3 relative = {world.x - camera_position.x, world.y - camera_position.y, world.z - camera_position.z};
    Vec4 inverse_rotation = {-camera_rotation.x, -camera_rotation.y, -camera_rotation.z, camera_rotation.w};
    Vec3 camera_space = rotate_vector(inverse_rotation, relative);
    if (!vec3_is_finite(camera_space) || camera_space.z <= 0.05F) return false;
    constexpr float vertical_fov_radians = 1.0471975512F;
    float tangent = tanf(vertical_fov_radians * 0.5F);
    float aspect = screen_width / screen_height;
    float normalized_x = camera_space.x / (camera_space.z * tangent * aspect);
    float normalized_y = camera_space.y / (camera_space.z * tangent);
    if (!std::isfinite(normalized_x) || !std::isfinite(normalized_y)) return false;
    if (clip_to_screen && (fabsf(normalized_x) > 1.0F || fabsf(normalized_y) > 1.0F)) return false;
    output.x = (normalized_x + 1.0F) * 0.5F * screen_width;
    output.y = (1.0F - normalized_y) * 0.5F * screen_height;
    return std::isfinite(output.x) && std::isfinite(output.y);
}

static bool optimize_matrix_configuration(uint64_t native_camera, const std::vector<uint64_t>& transforms) {
    std::vector<Vec3> samples;
    for (uint64_t source : transforms) {
        Vec3 position{};
        if (!read_entity_position(source, position)) continue;
        samples.push_back(position);
        if (samples.size() >= 24) break;
    }
    if (samples.empty()) {
        // Nothing readable this frame; keep the validated offset and retry.
        return false;
    }

    Vec3 minimum = samples[0], maximum = samples[0];
    for (const Vec3& position : samples) {
        minimum.x = std::min(minimum.x, position.x); minimum.y = std::min(minimum.y, position.y); minimum.z = std::min(minimum.z, position.z);
        maximum.x = std::max(maximum.x, position.x); maximum.y = std::max(maximum.y, position.y); maximum.z = std::max(maximum.z, position.z);
    }
    float extent = fabsf(maximum.x - minimum.x) + fabsf(maximum.y - minimum.y) + fabsf(maximum.z - minimum.z);
    // Several players that never move apart mean the offset is not a position
    // at all -- but only when there are several of them. Alone on the server a
    // zero extent is normal and must not invalidate anything.
    if (samples.size() >= 2 && extent < 0.1F) {
        g_player_position_validated = false;
        return false;
    }
    if (samples.size() < 2 && !position_looks_like_world_space(samples[0])) return false;

    Mat4 validated_projection{}, validated_view{};
    if (!read_native_camera_matrices(native_camera, 0.0F, validated_projection, validated_view))
        return false;
    Vec3 camera_position{};
    double nearest_camera_distance_squared = INFINITY;
    if (camera_position_from_view(validated_view, camera_position)) {
        for (const Vec3& sample : samples) {
            double dx = (double)sample.x - camera_position.x, dy = (double)sample.y - camera_position.y, dz = (double)sample.z - camera_position.z;
            double distance_squared = dx * dx + dy * dy + dz * dz;
            if (std::isfinite(distance_squared)) nearest_camera_distance_squared = std::min(nearest_camera_distance_squared, distance_squared);
        }
    }
    g_camera_matrix_physical_match = std::isfinite(nearest_camera_distance_squared) && nearest_camera_distance_squared <= 100.0;
    g_matrix_configuration_validated = true;
    return true;
}

static std::vector<uint64_t> read_configured_player_transforms() {
    std::vector<uint64_t> transforms;
    uint64_t list = resolve_runtime_player_list();
    if (!list) { return transforms; }

    uint64_t local_player = resolve_local_player();
    if (local_player && !player_list_contains(list, local_player)) {
        // Alone on the server the runtime list is empty — the local player
        // is only registered there while networked players are around. He is
        // still perfectly valid (his class just re-checked in the resolver),
        // and dropping him here was what killed markers/farm solo: the frame
        // below never got a local position. Only treat him as stale when the
        // list actually has entries that he is missing from.
        int32_t list_count = rd<int32_t>(list + IL2CPP_LIST_SIZE);
        if (list_count > 0) {
            if (local_player == g_local_player) g_local_player = 0;
            local_player = 0;
        }
    }
    uint64_t local_source = local_player;

    for (int attempt = 0; attempt < 3; ++attempt) {
        if (attempt > 0) {
            uint64_t refreshed_list = resolve_runtime_player_list();
            if (refreshed_list) list = refreshed_list;
        }
        uint64_t items = rd_ptr(list + IL2CPP_LIST_ITEMS);
        int32_t count = rd<int32_t>(list + IL2CPP_LIST_SIZE);
        if (!items || count <= 0 || count > 512) {
            // Empty list, but the local player himself is known: he alone is
            // enough for the whole pipeline (camera, matrix validation and
            // the local position all work from one sample).
            if (local_source && count == 0) return {local_source};
            continue;
        }

        std::vector<uint64_t> snapshot;
        snapshot.reserve((size_t)count + 1);
        if (local_source) snapshot.push_back(local_source);

        for (int32_t index = 0; index < count; ++index) {
            uint64_t player = rd_ptr(items + IL2CPP_ARRAY_FIRST_ELEMENT + (uint64_t)index * sizeof(uint64_t));
            if (!player) continue;
            if (g_player_manager_class && rd_ptr(player) != g_player_manager_class) continue;
            if (player == local_source) continue;
            snapshot.push_back(player);
        }

        uint64_t confirmed_items = rd_ptr(list + IL2CPP_LIST_ITEMS);
        int32_t confirmed_count = rd<int32_t>(list + IL2CPP_LIST_SIZE);
        if (items == confirmed_items && count == confirmed_count && !snapshot.empty())
            return snapshot;
    }
    return transforms;
}

// ===================== Skeleton ESP =====================
//
// External bone resolution: PlayerManager.characterModel (managed GameObject)
// -> m_CachedPtr -> native GameObject -> component[0] = native Transform (model
// root). The model subtree is walked through the native Transform children
// array (t+0x48 / count t+0x58) and bones are identified by their GameObject
// names ("Hips", "Spine", ..., "ToeBase.R"). World positions are then computed
// each frame from the shared TransformHierarchy arrays (local TRS matrices +
// parent indices) with two bulk reads per player.

enum SkeletonBone {
    BONE_HIPS = 0, BONE_SPINE, BONE_SPINE1, BONE_SPINE2, BONE_NECK, BONE_HEAD,
    BONE_SHOULDER_L, BONE_ARM_L, BONE_FOREARM_L, BONE_HAND_L,
    BONE_SHOULDER_R, BONE_ARM_R, BONE_FOREARM_R, BONE_HAND_R,
    BONE_UPLEG_L, BONE_LEG_L, BONE_FOOT_L, BONE_TOE_L,
    BONE_UPLEG_R, BONE_LEG_R, BONE_FOOT_R, BONE_TOE_R
};


struct SkeletonBoneName { const char* name; int bone; };
// Names are matched after normalization: lowercase, prefix up to the last ':'
// stripped, spaces removed, '_' turned into '.'. Aliases cover the Blender-style
// rig from the game plus common Unity/Mixamo/UE naming, just in case.
static const SkeletonBoneName kSkeletonBoneNames[] = {
    {"hips", BONE_HIPS}, {"pelvis", BONE_HIPS},
    {"spine", BONE_SPINE}, {"spine.01", BONE_SPINE},
    {"spine1", BONE_SPINE1}, {"chest", BONE_SPINE1}, {"spine.02", BONE_SPINE1},
    {"spine2", BONE_SPINE2}, {"upperchest", BONE_SPINE2}, {"spine.03", BONE_SPINE2},
    {"neck", BONE_NECK}, {"neck.01", BONE_NECK},
    {"head", BONE_HEAD},
    {"shoulder.l", BONE_SHOULDER_L}, {"leftshoulder", BONE_SHOULDER_L}, {"clavicle.l", BONE_SHOULDER_L},
    {"arm.l", BONE_ARM_L}, {"leftarm", BONE_ARM_L}, {"upperarm.l", BONE_ARM_L},
    {"forearm.l", BONE_FOREARM_L}, {"leftforearm", BONE_FOREARM_L}, {"lowerarm.l", BONE_FOREARM_L},
    {"hand.l", BONE_HAND_L}, {"lefthand", BONE_HAND_L},
    {"shoulder.r", BONE_SHOULDER_R}, {"rightshoulder", BONE_SHOULDER_R}, {"clavicle.r", BONE_SHOULDER_R},
    {"arm.r", BONE_ARM_R}, {"rightarm", BONE_ARM_R}, {"upperarm.r", BONE_ARM_R},
    {"forearm.r", BONE_FOREARM_R}, {"rightforearm", BONE_FOREARM_R}, {"lowerarm.r", BONE_FOREARM_R},
    {"hand.r", BONE_HAND_R}, {"righthand", BONE_HAND_R},
    {"upleg.l", BONE_UPLEG_L}, {"leftupleg", BONE_UPLEG_L}, {"thigh.l", BONE_UPLEG_L},
    {"leg.l", BONE_LEG_L}, {"leftleg", BONE_LEG_L}, {"calf.l", BONE_LEG_L},
    {"foot.l", BONE_FOOT_L}, {"leftfoot", BONE_FOOT_L},
    {"toebase.l", BONE_TOE_L}, {"lefttoebase", BONE_TOE_L}, {"toe.l", BONE_TOE_L}, {"ball.l", BONE_TOE_L},
    {"upleg.r", BONE_UPLEG_R}, {"rightupleg", BONE_UPLEG_R}, {"thigh.r", BONE_UPLEG_R},
    {"leg.r", BONE_LEG_R}, {"rightleg", BONE_LEG_R}, {"calf.r", BONE_LEG_R},
    {"foot.r", BONE_FOOT_R}, {"rightfoot", BONE_FOOT_R},
    {"toebase.r", BONE_TOE_R}, {"righttoebase", BONE_TOE_R}, {"toe.r", BONE_TOE_R}, {"ball.r", BONE_TOE_R}
};

struct CachedSkeleton {
    uint64_t bone_transform[ESP_BONE_COUNT] = {};
    Vec3     bone_world[ESP_BONE_COUNT] = {};
    uint8_t  bone_world_age[ESP_BONE_COUNT] = {}; // 0 = none, 1 = fresh, grows on reuse
    uint64_t model_root = 0;
    int      bone_count = 0;
    bool     valid = false;
    int      retry_cooldown = 0;
    int      fail_streak = 0;
    int      revalidate_timer = 0;
};

static std::unordered_map<uint64_t, CachedSkeleton> g_skeletons;
static bool g_skeleton_enabled = false;
// The aimbot needs bone positions regardless of the skeleton ESP toggle. Bone
// resolution is therefore driven by (skeleton ESP || aim requested).
static bool g_aim_bones_requested = false;

struct LocalAimState {
    bool     aiming = false;
    int      source = 0;          // 1 = event handler Aim activity, 2 = weapon isAiming
    uint64_t event_handler = 0;
    uint64_t aim_activity = 0;
    uint64_t fp_manager = 0;
    uint64_t weapon = 0;
    int      revalidate = 0;
};
static LocalAimState g_aim_state{};

// Per-player auxiliary data resolved through the KCC (character controller):
// the game-maintained head transform and the current pose. Used for
// crouch-aware boxes and as the aim target when rig bones are unavailable.
struct PlayerAux {
    uint64_t kcc = 0;
    uint64_t head_native = 0;   // native Transform of KCC.head
    uint64_t head_hitbox_transform = 0; // native Transform of the Head HitBox
    Vec3     head_hitbox_center{};      // HitBox.center (local to that transform)
    bool     head_hitbox_valid = false;
    float    normal_height = 1.8F;
    float    crouch_height = 1.1F;
    int      retry_cooldown = 0;
    int      revalidate = 0;
    // Held-weapon chain (resolved lazily, revalidated by their back-references).
    uint64_t weapon_component = 0; // PlayerWeapon (NetworkBehaviour)
    uint64_t model_info = 0;       // PlayerModelInfo (weapon holders)
    int      weapon_retry = 0;
};
static std::unordered_map<uint64_t, PlayerAux> g_player_aux;

static TransformHierarchyLayout g_skeleton_layout{};
static bool g_skeleton_layout_valid = false;

static uint64_t g_go_name_offset = 0;
static bool     g_go_name_plain_pointer = false; // fallback: name stored as raw char*
static bool     g_go_name_offset_valid = false;
static int      g_go_name_retry_cooldown = 0;
static int      g_skeleton_builds_this_frame = 0; // heavy rescans: max 1 per frame

void esp_set_skeleton_enabled(bool enabled) { g_skeleton_enabled = enabled; }
void esp_set_aim_bones_enabled(bool enabled) { g_aim_bones_requested = enabled; }

// characterModel (managed GameObject) -> native GameObject -> its Transform.
static uint64_t skeleton_model_root(uint64_t player) {
    if (!player) return 0;
    uint64_t managed_go = rd_ptr(player + PLAYER_CHARACTER_MODEL);
    if (!managed_go) return 0;
    uint64_t native_go = rd_ptr(managed_go + MANAGED_CACHED_PTR);
    if (!native_go) return 0;
    uint64_t pairs = rd_ptr(native_go + GAMEOBJECT_COMPONENT_ARRAY);
    if (!pairs) return 0;
    uint64_t transform = rd_ptr(pairs + COMPONENT_PAIR_PTR);
    if (!transform) return 0;
    // Sanity: the transform must point back at the same GameObject.
    if (rd_ptr(transform + COMPONENT_GAMEOBJECT) != native_go) return 0;
    return transform;
}

static int read_transform_children(uint64_t transform, uint64_t* out, int max_children) {
    if (!transform) return 0;
    int32_t count = rd<int32_t>(transform + TRANSFORM_CHILD_COUNT);
    if (count <= 0 || count > 128) return 0;
    if (count > max_children) count = max_children;
    uint64_t array = rd_ptr(transform + TRANSFORM_CHILDREN_ARRAY);
    if (!array) return 0;
    if (!rd_buf(array, out, (size_t)count * sizeof(uint64_t))) return 0;
    return count;
}

static void collect_transform_subtree(uint64_t root, std::vector<uint64_t>& nodes, size_t max_nodes) {
    nodes.clear();
    if (!root) return;
    nodes.push_back(root);
    size_t cursor = 0;
    uint64_t children[128];
    while (cursor < nodes.size() && nodes.size() < max_nodes) {
        uint64_t current = nodes[cursor++];
        int count = read_transform_children(current, children, 128);
        for (int i = 0; i < count && nodes.size() < max_nodes; ++i) {
            if (children[i]) nodes.push_back(children[i]);
        }
    }
}

static bool string_is_reasonable_name(const char* value) {
    size_t length = strnlen(value, 48);
    if (length == 0 || length >= 48) return false;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)value[i];
        if (c < 0x20 || c > 0x7E) return false;
    }
    return true;
}

// Lowercase, strip "prefix:" namespaces, drop spaces, unify '_' -> '.'.
static void normalize_bone_name(const char* in, char* out, size_t cap) {
    const char* start = in;
    for (const char* p = in; *p; ++p) {
        if (*p == ':') start = p + 1;
    }
    size_t n = 0;
    for (const char* p = start; *p && n + 1 < cap; ++p) {
        char c = *p;
        if (c == ' ') continue;
        if (c == '_') c = '.';
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        out[n++] = c;
    }
    out[n] = '\0';
}

static int match_bone_name(const char* raw_name) {
    char normalized[48];
    normalize_bone_name(raw_name, normalized, sizeof(normalized));
    if (!normalized[0]) return -1;
    for (const SkeletonBoneName& entry : kSkeletonBoneNames) {
        if (strcmp(normalized, entry.name) == 0) return entry.bone;
    }
    return -1;
}


// Read a GameObject name. Unity stores it as a 32-byte core::string with SSO:
// flags byte at +0x1F; when (flags >= 0x40) the first 8 bytes are a heap char*,
// otherwise the characters live inline at +0x0.
static bool read_gameobject_name_at(uint64_t native_go, uint64_t name_offset, bool plain_pointer, char* out, size_t cap) {
    if (!native_go || cap < 2) return false;
    char buffer[48] = {};
    if (plain_pointer) {
        uint64_t ptr = rd_ptr(native_go + name_offset);
        if (ptr < 0x10000 || ptr >= 0x0001000000000000ULL) return false;
        if (!rd_buf(ptr, buffer, 32)) return false;
        buffer[32] = '\0';
    } else {
        uint8_t raw[32];
        if (!rd_buf(native_go + name_offset, raw, sizeof(raw))) return false;
        uint8_t flags = raw[31];
        if (flags >= 0x40) {
            uint64_t heap = 0;
            memcpy(&heap, raw, sizeof(heap));
            if (heap < 0x10000 || heap >= 0x0001000000000000ULL) return false;
            if (!rd_buf(heap, buffer, 32)) return false;
            buffer[32] = '\0';
        } else {
            memcpy(buffer, raw, 23);
            buffer[23] = '\0';
        }
    }
    if (!string_is_reasonable_name(buffer)) return false;
    strncpy(out, buffer, cap - 1);
    out[cap - 1] = '\0';
    return true;
}

static bool read_transform_name(uint64_t transform, char* out, size_t cap) {
    if (!g_go_name_offset_valid || !transform) return false;
    uint64_t native_go = rd_ptr(transform + COMPONENT_GAMEOBJECT);
    if (!native_go) return false;
    return read_gameobject_name_at(native_go, g_go_name_offset, g_go_name_plain_pointer, out, cap);
}

// Find the GameObject name field offset by probing candidates against the
// character model subtree until known bone names show up.
static bool discover_gameobject_name_offset(const std::vector<uint64_t>& nodes) {
    static const uint64_t kCandidates[] = {GAMEOBJECT_NAME_GUESS, 0x40, 0x50, 0x38, 0x30, 0x28, 0x58, 0x20, 0x60};
    static const char* kProbeNames[] = {"hips", "spine", "spine1", "spine2", "neck", "head", "armature", "root", "pelvis"};

    std::vector<uint64_t> gameobjects;
    gameobjects.reserve(nodes.size());
    for (uint64_t node : nodes) {
        uint64_t go = rd_ptr(node + COMPONENT_GAMEOBJECT);
        if (go) gameobjects.push_back(go);
        if (gameobjects.size() >= 192) break;
    }
    if (gameobjects.size() < 8) return false;

    for (int plain_pointer = 0; plain_pointer < 2; ++plain_pointer) {
        for (uint64_t offset : kCandidates) {
            int matches = 0;
            for (uint64_t go : gameobjects) {
                char name[48];
                if (!read_gameobject_name_at(go, offset, plain_pointer != 0, name, sizeof(name))) continue;
                char normalized[48];
                normalize_bone_name(name, normalized, sizeof(normalized));
                for (const char* probe : kProbeNames) {
                    if (strcmp(normalized, probe) == 0) { ++matches; break; }
                }
                if (matches >= 3) break;
            }
            if (matches >= 3) {
                g_go_name_offset = offset;
                g_go_name_plain_pointer = plain_pointer != 0;
                g_go_name_offset_valid = true;
                return true;
            }
        }
    }
    return false;
}

static bool resolve_skeleton_layout(uint64_t sample_transform) {
    Vec3 probe{};
    if (g_skeleton_layout_valid) {
        if (read_transform_hierarchy_layout(sample_transform, g_skeleton_layout, probe)) return true;
        g_skeleton_layout_valid = false;
    }
    if (g_transform_hierarchy_layout_valid &&
        read_transform_hierarchy_layout(sample_transform, g_transform_hierarchy_layout, probe)) {
        g_skeleton_layout = g_transform_hierarchy_layout;
        g_skeleton_layout_valid = true;
        return true;
    }
    const uint64_t base_offsets[][2] = {{0x38, 0x40}, {0x18, 0x20}};
    const uint64_t data_offsets[][2] = {{0x18, 0x20}, {0x08, 0x10}};
    for (const auto& base : base_offsets) {
        for (const auto& offsets : data_offsets) {
            for (int matrices_indirect = 0; matrices_indirect < 2; ++matrices_indirect) {
                for (int indices_indirect = 0; indices_indirect < 2; ++indices_indirect) {
                    TransformHierarchyLayout layout{};
                    layout.data_offset = base[0];
                    layout.index_offset = base[1];
                    layout.matrices_offset = offsets[0];
                    layout.indices_offset = offsets[1];
                    layout.matrices_indirect = matrices_indirect != 0;
                    layout.indices_indirect = indices_indirect != 0;
                    if (read_transform_hierarchy_layout(sample_transform, layout, probe)) {
                        g_skeleton_layout = layout;
                        g_skeleton_layout_valid = true;
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

// ===================== Ragdoll bone list (no name matching) =====================
//
// The game itself keeps the rig bone transforms in Ragdoll.m_Bones (BodyPart[])
// plus m_Pelvis. Resolving bones from there is immune to rig naming, duplicate
// meshes and BFS depth issues. Intermediate bones (spine chain, neck, shoulders,
// hands, feet) are recovered structurally from the transform hierarchy.

static uint64_t managed_object_native(uint64_t managed) {
    if (!managed) return 0;
    uint64_t native = rd_ptr(managed + MANAGED_CACHED_PTR);
    if (native < 0x10000 || native >= 0x0001000000000000ULL) return 0;
    return native;
}

static bool skeleton_transform_ptr_valid(uint64_t transform) {
    if (!transform) return false;
    uint64_t go = rd_ptr(transform + COMPONENT_GAMEOBJECT);
    if (!go) return false;
    uint64_t pairs = rd_ptr(go + GAMEOBJECT_COMPONENT_ARRAY);
    return pairs && rd_ptr(pairs + COMPONENT_PAIR_PTR) == transform;
}

// Any native Component -> the Transform of its GameObject.
static uint64_t native_component_transform(uint64_t native_component) {
    if (!native_component) return 0;
    uint64_t go = rd_ptr(native_component + COMPONENT_GAMEOBJECT);
    if (!go) return 0;
    uint64_t pairs = rd_ptr(go + GAMEOBJECT_COMPONENT_ARRAY);
    if (!pairs) return 0;
    uint64_t transform = rd_ptr(pairs + COMPONENT_PAIR_PTR);
    if (!transform || rd_ptr(transform + COMPONENT_GAMEOBJECT) != go) return 0;
    return transform;
}

// player -> KCC. kccReference (0xB0) may be an obfuscated wrapper, and its
// internal layout is unknown, so probe it and — as a last resort — scan the
// PlayerManager fields. A real KCC is recognized by its back-reference
// (KCC.player @0x78 == player); KCC.head (@0x88, managed Transform) and the
// CharacterAnimation slot (@0x108) strengthen the match.
static bool kcc_head_transform_valid(uint64_t kcc) {
    uint64_t head = managed_object_native(rd_ptr(kcc + KCC_HEAD_TRANSFORM));
    return skeleton_transform_ptr_valid(head);
}

static bool looks_like_kcc(uint64_t candidate, uint64_t player) {
    if (candidate < 0x10000 || candidate >= 0x0001000000000000ULL) return false;
    return rd_ptr(candidate + KCC_PLAYER_BACKREF) == player;
}

static uint64_t resolve_player_kcc(uint64_t player) {
    uint64_t reference = rd_ptr(player + PLAYER_KCC_REFERENCE);
    if (reference) {
        if (looks_like_kcc(reference, player)) return reference;
        for (uint64_t offset = 0x08; offset <= 0x60; offset += 8) {
            uint64_t candidate = rd_ptr(reference + offset);
            if (looks_like_kcc(candidate, player)) return candidate;
        }
    }
    // Field scan: prefer candidates whose head transform checks out, then
    // any with a plausible CharacterAnimation pointer.
    uint64_t weak = 0;
    for (uint64_t offset = 0x68; offset <= 0x2C8; offset += 8) {
        uint64_t candidate = rd_ptr(player + offset);
        if (!looks_like_kcc(candidate, player)) continue;
        if (kcc_head_transform_valid(candidate)) return candidate;
        if (!weak && rd_ptr(candidate + KCC_CHARACTER_ANIMATION)) weak = candidate;
    }
    return weak;
}

static uint64_t resolve_player_ragdoll(uint64_t player, uint64_t& kcc_out) {
    kcc_out = 0;
    uint64_t kcc = resolve_player_kcc(player);
    if (!kcc) return 0;
    kcc_out = kcc;
    uint64_t anim = rd_ptr(kcc + KCC_CHARACTER_ANIMATION);
    if (!anim) return 0;
    uint64_t back = rd_ptr(anim + CHAR_ANIM_PLAYER_BACKREF);
    if (back && back != player) return 0;
    return rd_ptr(anim + CHAR_ANIM_RAGDOLL);
}

// Ancestor index chain (excluding the start index) via remote reads.
static int read_parent_chain_remote(uint64_t indices, int32_t index, int32_t* chain, int cap) {
    int length = 0;
    int32_t current = index;
    while (length < cap) {
        int32_t parent = rd<int32_t>(indices + (uint64_t)current * 4);
        if (parent < 0 || parent > 100000 || parent == current) break;
        chain[length++] = parent;
        current = parent;
    }
    return length;
}

// Child of `parent_transform` whose hierarchy index lies on `chain`.
static uint64_t skeleton_child_on_chain(uint64_t parent_transform, uint64_t data,
                                        const int32_t* chain, int chain_length) {
    if (!parent_transform) return 0;
    uint64_t children[64];
    int count = read_transform_children(parent_transform, children, 64);
    for (int i = 0; i < count; ++i) {
        if (rd_ptr(children[i] + g_skeleton_layout.data_offset) != data) continue;
        int32_t child_index = rd<int32_t>(children[i] + g_skeleton_layout.index_offset);
        for (int j = 0; j < chain_length; ++j)
            if (chain[j] == child_index) return children[i];
    }
    return 0;
}

static bool build_skeleton_from_ragdoll(uint64_t player, CachedSkeleton& skeleton) {
    uint64_t kcc = 0;
    uint64_t ragdoll = resolve_player_ragdoll(player, kcc);
    if (!ragdoll) return false;

    uint64_t bones_array = rd_ptr(ragdoll + RAGDOLL_BONES_ARRAY);
    if (!bones_array) return false;
    int32_t element_count = rd<int32_t>(bones_array + IL2CPP_ARRAY_LENGTH);
    if (element_count < 4 || element_count > 64) return false;

    uint64_t pelvis = native_component_transform(
        managed_object_native(rd_ptr(ragdoll + RAGDOLL_PELVIS_RIGIDBODY)));
    if (pelvis && !skeleton_transform_ptr_valid(pelvis)) pelvis = 0;

    // Collect the ragdoll bone transforms. Elements are BodyPart objects
    // (transform at +0x10) but tolerate a plain Component[] as well.
    constexpr int kMaxSet = 24;
    uint64_t set_transform[kMaxSet];
    int set_count = 0;
    for (int32_t i = 0; i < element_count && set_count < kMaxSet; ++i) {
        uint64_t element = rd_ptr(bones_array + IL2CPP_ARRAY_FIRST_ELEMENT + (uint64_t)i * 8);
        if (!element) continue;
        uint64_t transform = managed_object_native(rd_ptr(element + RAGDOLL_BODYPART_TRANSFORM));
        if (!skeleton_transform_ptr_valid(transform))
            transform = native_component_transform(managed_object_native(element));
        if (!skeleton_transform_ptr_valid(transform)) continue;
        bool duplicate = false;
        for (int j = 0; j < set_count; ++j)
            if (set_transform[j] == transform) { duplicate = true; break; }
        if (!duplicate) set_transform[set_count++] = transform;
    }
    if (pelvis) {
        bool present = false;
        for (int j = 0; j < set_count; ++j)
            if (set_transform[j] == pelvis) { present = true; break; }
        if (!present && set_count < kMaxSet) set_transform[set_count++] = pelvis;
    }
    if (set_count < 5) return false;

    if (!resolve_skeleton_layout(pelvis ? pelvis : set_transform[0])) return false;

    // Bones may live in different TransformHierarchies (the game re-parents
    // parts of the rig at runtime), so resolve arrays per hierarchy.
    auto hierarchy_arrays = [&](uint64_t hierarchy, uint64_t& matrices, uint64_t& indices) -> bool {
        matrices = rd_ptr(hierarchy + g_skeleton_layout.matrices_offset);
        indices = rd_ptr(hierarchy + g_skeleton_layout.indices_offset);
        if (g_skeleton_layout.matrices_indirect) matrices = rd_ptr(matrices);
        if (g_skeleton_layout.indices_indirect) indices = rd_ptr(indices);
        return matrices && indices;
    };

    // Hierarchy, index, ancestor chain and world position per ragdoll bone.
    uint64_t set_data[kMaxSet];
    int32_t set_index[kMaxSet];
    int32_t chains[kMaxSet][48];
    int     chain_length[kMaxSet];
    Vec3    world[kMaxSet];
    {
        int write = 0;
        for (int i = 0; i < set_count; ++i) {
            uint64_t bone_data = rd_ptr(set_transform[i] + g_skeleton_layout.data_offset);
            if (!bone_data) continue;
            int32_t index = rd<int32_t>(set_transform[i] + g_skeleton_layout.index_offset);
            if (index < 0 || index > 100000) continue;
            uint64_t matrices = 0, indices = 0;
            if (!hierarchy_arrays(bone_data, matrices, indices)) continue;
            Vec3 position{};
            if (!read_transform_hierarchy_arrays(matrices, indices, index, position)) continue;
            set_transform[write] = set_transform[i];
            set_data[write] = bone_data;
            set_index[write] = index;
            world[write] = position;
            ++write;
        }
        set_count = write;
    }
    if (set_count < 5) return false;
    for (int i = 0; i < set_count; ++i) {
        uint64_t matrices = 0, indices = 0;
        chain_length[i] = hierarchy_arrays(set_data[i], matrices, indices)
            ? read_parent_chain_remote(indices, set_index[i], chains[i], 48) : 0;
    }

    // Ancestor relations only make sense inside one hierarchy, so slot lookup
    // matches both the index and the hierarchy the chain belongs to.
    auto set_slot_of_index = [&](int32_t index, uint64_t hierarchy) -> int {
        for (int i = 0; i < set_count; ++i)
            if (set_index[i] == index && set_data[i] == hierarchy) return i;
        return -1;
    };

    // Nearest set ancestor + number of set descendants for every bone.
    int nearest[kMaxSet];
    int descendants[kMaxSet];
    for (int i = 0; i < set_count; ++i) { nearest[i] = -1; descendants[i] = 0; }
    for (int i = 0; i < set_count; ++i) {
        for (int step = 0; step < chain_length[i]; ++step) {
            int ancestor = set_slot_of_index(chains[i][step], set_data[i]);
            if (ancestor < 0) continue;
            if (nearest[i] < 0) nearest[i] = ancestor;
            ++descendants[ancestor];
        }
    }

    // Pelvis: prefer the game's own m_Pelvis, else the widest ancestor.
    int pelvis_slot = -1;
    if (pelvis) {
        for (int i = 0; i < set_count; ++i)
            if (set_transform[i] == pelvis) { pelvis_slot = i; break; }
    }
    if (pelvis_slot < 0) {
        for (int i = 0; i < set_count; ++i)
            if (pelvis_slot < 0 || descendants[i] > descendants[pelvis_slot]) pelvis_slot = i;
    }
    if (pelvis_slot < 0 || descendants[pelvis_slot] < 2) return false;

    // Chest: pelvis branch with the most descendants; walk down while the
    // branch still splits (handles an intermediate spine rigidbody).
    int chest_slot = -1;
    for (int i = 0; i < set_count; ++i) {
        if (i == pelvis_slot || nearest[i] != pelvis_slot) continue;
        if (descendants[i] >= 2 && (chest_slot < 0 || descendants[i] > descendants[chest_slot]))
            chest_slot = i;
    }
    if (chest_slot < 0) {
        // Upper body re-parented into another hierarchy: its subtree root has
        // no set ancestor. Pick the rootless bone with the widest subtree.
        for (int i = 0; i < set_count; ++i) {
            if (i == pelvis_slot || nearest[i] >= 0) continue;
            if (descendants[i] >= 2 && (chest_slot < 0 || descendants[i] > descendants[chest_slot]))
                chest_slot = i;
        }
    }
    while (chest_slot >= 0) {
        int next = -1;
        for (int i = 0; i < set_count; ++i)
            if (nearest[i] == chest_slot && descendants[i] >= 2) { next = i; break; }
        if (next < 0) break;
        chest_slot = next;
    }
    if (chest_slot < 0) return false;

    bool on_spine[kMaxSet] = {};
    on_spine[chest_slot] = true;
    for (int step = 0; step < chain_length[chest_slot]; ++step) {
        int slot = set_slot_of_index(chains[chest_slot][step], set_data[chest_slot]);
        if (slot >= 0) on_spine[slot] = true;
    }

    // Legs: pelvis branches outside the spine, starting at/below the pelvis.
    int thigh_slot[2] = {-1, -1};
    for (int i = 0; i < set_count; ++i) {
        if (i == pelvis_slot || on_spine[i] || nearest[i] != pelvis_slot) continue;
        if (world[i].y > world[pelvis_slot].y + 0.2F) continue;
        if (thigh_slot[0] < 0) thigh_slot[0] = i;
        else if (thigh_slot[1] < 0) thigh_slot[1] = i;
    }

    // Head: leaf hanging off the chest (highest one); arms: chest branches
    // that continue (forearm below them).
    int head_slot = -1;
    int upperarm_slot[2] = {-1, -1};
    for (int i = 0; i < set_count; ++i) {
        if (i == pelvis_slot || on_spine[i] || nearest[i] != chest_slot) continue;
        if (descendants[i] == 0) {
            if (head_slot < 0 || world[i].y > world[head_slot].y) head_slot = i;
        } else if (upperarm_slot[0] < 0) {
            upperarm_slot[0] = i;
        } else if (upperarm_slot[1] < 0) {
            upperarm_slot[1] = i;
        }
    }
    int forearm_slot[2] = {-1, -1};
    for (int side = 0; side < 2; ++side) {
        if (upperarm_slot[side] < 0) continue;
        for (int i = 0; i < set_count; ++i)
            if (nearest[i] == upperarm_slot[side]) { forearm_slot[side] = i; break; }
    }

    // Consistent left/right split by local-space X (siblings share a parent).
    auto local_x = [&](int slot) -> float {
        uint64_t matrices = 0, indices = 0;
        if (!hierarchy_arrays(set_data[slot], matrices, indices)) return 0.0F;
        return rd<float>(matrices + (uint64_t)set_index[slot] * sizeof(Matrix34));
    };
    if (thigh_slot[0] >= 0 && thigh_slot[1] >= 0 && local_x(thigh_slot[0]) < local_x(thigh_slot[1])) {
        int swap = thigh_slot[0]; thigh_slot[0] = thigh_slot[1]; thigh_slot[1] = swap;
    }
    if (upperarm_slot[0] >= 0 && upperarm_slot[1] >= 0 && local_x(upperarm_slot[0]) < local_x(upperarm_slot[1])) {
        int swap = upperarm_slot[0]; upperarm_slot[0] = upperarm_slot[1]; upperarm_slot[1] = swap;
        swap = forearm_slot[0]; forearm_slot[0] = forearm_slot[1]; forearm_slot[1] = swap;
    }

    auto assign = [&](int bone, uint64_t transform) {
        if (bone >= 0 && bone < ESP_BONE_COUNT && transform && !skeleton.bone_transform[bone])
            skeleton.bone_transform[bone] = transform;
    };
    // Child pick for chain ends (hand/foot/toe): prefer a name match when the
    // name offset is known, otherwise the first child in the same hierarchy.
    auto pick_child = [&](uint64_t parent, int bone_hint) -> uint64_t {
        if (!parent) return 0;
        uint64_t parent_data = rd_ptr(parent + g_skeleton_layout.data_offset);
        uint64_t children[16];
        int count = read_transform_children(parent, children, 16);
        uint64_t fallback = 0;
        for (int i = 0; i < count; ++i) {
            if (rd_ptr(children[i] + g_skeleton_layout.data_offset) != parent_data) continue;
            if (!fallback) fallback = children[i];
            if (g_go_name_offset_valid) {
                char name[48];
                if (read_transform_name(children[i], name, sizeof(name)) &&
                    match_bone_name(name) == bone_hint) return children[i];
            }
        }
        return fallback;
    };

    assign(BONE_HIPS, set_transform[pelvis_slot]);
    assign(BONE_SPINE2, set_transform[chest_slot]);

    // Spine chain: pelvis -> ... -> chest along the chest's ancestor chain.
    {
        uint64_t cursor = set_transform[pelvis_slot];
        const int spine_bones[2] = {BONE_SPINE, BONE_SPINE1};
        for (int step = 0; step < 2 && cursor; ++step) {
            uint64_t next = skeleton_child_on_chain(cursor, set_data[chest_slot], chains[chest_slot], chain_length[chest_slot]);
            if (!next || next == set_transform[chest_slot]) break;
            assign(spine_bones[step], next);
            cursor = next;
        }
    }

    if (head_slot >= 0) {
        assign(BONE_HEAD, set_transform[head_slot]);
        uint64_t neck = skeleton_child_on_chain(set_transform[chest_slot], set_data[head_slot],
                                                chains[head_slot], chain_length[head_slot]);
        if (neck && neck != set_transform[head_slot]) assign(BONE_NECK, neck);
    }

    const int arm_bones[2][4] = {
        {BONE_SHOULDER_L, BONE_ARM_L, BONE_FOREARM_L, BONE_HAND_L},
        {BONE_SHOULDER_R, BONE_ARM_R, BONE_FOREARM_R, BONE_HAND_R}
    };
    for (int side = 0; side < 2; ++side) {
        int arm = upperarm_slot[side];
        if (arm < 0) continue;
        uint64_t shoulder = skeleton_child_on_chain(set_transform[chest_slot], set_data[arm],
                                                    chains[arm], chain_length[arm]);
        if (shoulder && shoulder != set_transform[arm]) assign(arm_bones[side][0], shoulder);
        assign(arm_bones[side][1], set_transform[arm]);
        if (forearm_slot[side] >= 0) {
            assign(arm_bones[side][2], set_transform[forearm_slot[side]]);
            assign(arm_bones[side][3], pick_child(set_transform[forearm_slot[side]], arm_bones[side][3]));
        }
    }

    const int leg_bones[2][4] = {
        {BONE_UPLEG_L, BONE_LEG_L, BONE_FOOT_L, BONE_TOE_L},
        {BONE_UPLEG_R, BONE_LEG_R, BONE_FOOT_R, BONE_TOE_R}
    };
    for (int side = 0; side < 2; ++side) {
        if (thigh_slot[side] < 0) continue;
        assign(leg_bones[side][0], set_transform[thigh_slot[side]]);
        int calf = -1;
        for (int i = 0; i < set_count; ++i)
            if (nearest[i] == thigh_slot[side]) { calf = i; break; }
        if (calf < 0) continue;
        assign(leg_bones[side][1], set_transform[calf]);
        uint64_t foot = pick_child(set_transform[calf], leg_bones[side][2]);
        assign(leg_bones[side][2], foot);
        assign(leg_bones[side][3], pick_child(foot, leg_bones[side][3]));
    }

    // Bonus: the game exposes the head transform directly on the KCC.
    if (kcc && !skeleton.bone_transform[BONE_HEAD]) {
        uint64_t head = managed_object_native(rd_ptr(kcc + KCC_HEAD_TRANSFORM));
        if (skeleton_transform_ptr_valid(head)) assign(BONE_HEAD, head);
    }

    // Final filter: keep bones with a resolvable hierarchy (any hierarchy).
    int valid_bones = 0;
    for (int bone = 0; bone < ESP_BONE_COUNT; ++bone) {
        uint64_t transform = skeleton.bone_transform[bone];
        if (!transform) continue;
        uint64_t bone_data = rd_ptr(transform + g_skeleton_layout.data_offset);
        int32_t index = rd<int32_t>(transform + g_skeleton_layout.index_offset);
        if (!bone_data || index < 0 || index > 100000) {
            skeleton.bone_transform[bone] = 0;
            continue;
        }
        ++valid_bones;
    }
    if (valid_bones < 6 || !skeleton.bone_transform[BONE_HIPS]) return false;

    skeleton.model_root = skeleton_model_root(player); // may be 0; revalidation compares equal
    skeleton.bone_count = valid_bones;
    skeleton.valid = true;
    return true;
}

static bool build_skeleton_from_names(uint64_t player, CachedSkeleton& skeleton) {
    skeleton = CachedSkeleton{};

    uint64_t root = skeleton_model_root(player);
    if (!root) return false;

    // Wide scan of the whole model to locate "Hips" candidates. Deep bones
    // (arms/head) may be far down, so the cap here is generous.
    static std::vector<uint64_t> nodes;
    collect_transform_subtree(root, nodes, 1024);
    if (nodes.size() < 8) return false;

    if (!g_go_name_offset_valid && !discover_gameobject_name_offset(nodes)) return false;

    // Collect up to a few Hips candidates (render rig, ragdoll copies, ...).
    uint64_t hips_candidates[4] = {};
    int hips_candidate_count = 0;
    for (uint64_t node : nodes) {
        char name[48];
        if (!read_transform_name(node, name, sizeof(name))) continue;
        if (match_bone_name(name) != BONE_HIPS) continue;
        hips_candidates[hips_candidate_count++] = node;
        if (hips_candidate_count >= 4) break;
    }
    if (!hips_candidate_count) return false;

    // Targeted parent->child descent along the known rig structure. Unlike a
    // breadth-first subtree scan this cannot starve on wide hierarchies (bone
    // attachments, hitboxes, gear), because it only ever looks at the children
    // of already-identified bones. A missing middle bone is tolerated: the
    // search for the next slot simply continues from the last found bone.
    auto find_child_bone = [&](uint64_t parent, int bone_id) -> uint64_t {
        if (!parent) return 0;
        uint64_t children[64];
        int count = read_transform_children(parent, children, 64);
        for (int i = 0; i < count; ++i) {
            char name[48];
            if (!read_transform_name(children[i], name, sizeof(name))) continue;
            if (match_bone_name(name) == bone_id) return children[i];
        }
        return 0;
    };

    uint64_t best_bones[ESP_BONE_COUNT] = {};
    int best_count = 0;
    for (int candidate = 0; candidate < hips_candidate_count; ++candidate) {
        uint64_t hips = hips_candidates[candidate];
        uint64_t bones[ESP_BONE_COUNT] = {};
        bones[BONE_HIPS] = hips;
        int found = 1;

        // Spine chain (cursor only advances on hits, so gaps are skipped).
        uint64_t cursor = hips;
        for (int slot = BONE_SPINE; slot <= BONE_SPINE2; ++slot) {
            uint64_t next = find_child_bone(cursor, slot);
            if (next) { bones[slot] = next; ++found; cursor = next; }
        }
        uint64_t chest = cursor; // deepest spine bone found (or hips)
        uint64_t neck = find_child_bone(chest, BONE_NECK);
        if (neck) { bones[BONE_NECK] = neck; ++found; }
        uint64_t head = find_child_bone(neck ? neck : chest, BONE_HEAD);
        if (head) { bones[BONE_HEAD] = head; ++found; }

        const int arm_chain[2][4] = {
            {BONE_SHOULDER_L, BONE_ARM_L, BONE_FOREARM_L, BONE_HAND_L},
            {BONE_SHOULDER_R, BONE_ARM_R, BONE_FOREARM_R, BONE_HAND_R}
        };
        for (int side = 0; side < 2; ++side) {
            // Shoulders may hang off any spine bone.
            uint64_t link = 0;
            const uint64_t roots[4] = {chest, bones[BONE_SPINE1], bones[BONE_SPINE], hips};
            for (uint64_t r : roots) {
                if (!r) continue;
                link = find_child_bone(r, arm_chain[side][0]);
                if (link) break;
            }
            if (link) { bones[arm_chain[side][0]] = link; ++found; }
            else link = chest;
            for (int i = 1; i < 4; ++i) {
                uint64_t next = find_child_bone(link, arm_chain[side][i]);
                if (next) { bones[arm_chain[side][i]] = next; ++found; link = next; }
            }
        }

        const int leg_chain[2][4] = {
            {BONE_UPLEG_L, BONE_LEG_L, BONE_FOOT_L, BONE_TOE_L},
            {BONE_UPLEG_R, BONE_LEG_R, BONE_FOOT_R, BONE_TOE_R}
        };
        for (int side = 0; side < 2; ++side) {
            uint64_t link = hips;
            for (int i = 0; i < 4; ++i) {
                uint64_t next = find_child_bone(link, leg_chain[side][i]);
                if (next) { bones[leg_chain[side][i]] = next; ++found; link = next; }
            }
        }

        if (found > best_count) {
            best_count = found;
            memcpy(best_bones, bones, sizeof(bones));
            if (found >= ESP_BONE_COUNT) break;
        }
    }
    if (best_count < 6 || !best_bones[BONE_HIPS]) return false;
    if (!resolve_skeleton_layout(best_bones[BONE_HIPS])) return false;

    uint64_t data = rd_ptr(best_bones[BONE_HIPS] + g_skeleton_layout.data_offset);
    if (!data) return false;

    // Keep bones with a resolvable hierarchy (re-parented bones included).
    int valid_bones = 0;
    for (int bone = 0; bone < ESP_BONE_COUNT; ++bone) {
        uint64_t transform = best_bones[bone];
        if (!transform) continue;
        uint64_t bone_data = rd_ptr(transform + g_skeleton_layout.data_offset);
        int32_t index = rd<int32_t>(transform + g_skeleton_layout.index_offset);
        if (!bone_data || index < 0 || index > 100000) continue;
        skeleton.bone_transform[bone] = transform;
        ++valid_bones;
    }
    if (valid_bones < 6 || !skeleton.bone_transform[BONE_HIPS]) return false;

    skeleton.model_root = root;
    skeleton.bone_count = valid_bones;
    skeleton.valid = true;
    return true;
}

// Build with both strategies and keep the richer skeleton. The name path
// identifies bones on the render rig directly, so it wins ties: the ragdoll
// list can reference physics/hitbox proxy transforms whose upper-body
// positions do not follow the animated model.
static bool build_skeleton(uint64_t player, CachedSkeleton& skeleton) {
    skeleton = CachedSkeleton{};

    CachedSkeleton from_ragdoll{};
    bool ragdoll_ok = build_skeleton_from_ragdoll(player, from_ragdoll);

    CachedSkeleton from_names{};
    bool names_ok = build_skeleton_from_names(player, from_names);

    if (names_ok && (!ragdoll_ok || from_names.bone_count >= from_ragdoll.bone_count)) {
        skeleton = from_names;
    } else if (ragdoll_ok) {
        skeleton = from_ragdoll;
    } else {
        return false;
    }
    return true;
}

// Same math as read_transform_hierarchy_arrays, but on locally buffered arrays.
// Returns 1 on success, 0 on failure, -1 when the parent chain leaves the
// buffered range (caller may retry with a remote walk).
static int skeleton_local_walk(const Matrix34* matrices, const int32_t* parents, int32_t count, int32_t index, Vec3& out) {
    if (index < 0 || index >= count) return -1;
    const Matrix34& current = matrices[index];
    if (!matrix34_is_valid(current)) return 0;
    Vec3 result = {current.translation.x, current.translation.y, current.translation.z};
    if (!vec3_is_finite(result)) return 0;
    int32_t parent = parents[index];
    int32_t previous = index;
    int depth = 0;
    while (parent >= 0 && depth++ < 128) {
        if (parent >= count) return -1;
        if (parent == previous) return 0;
        const Matrix34& matrix = matrices[parent];
        if (!matrix34_is_valid(matrix)) return 0;
        Vec3 scaled = {result.x * matrix.scale.x, result.y * matrix.scale.y, result.z * matrix.scale.z};
        Vec3 rotated = rotate_vector(matrix.rotation, scaled);
        result = {matrix.translation.x + rotated.x, matrix.translation.y + rotated.y, matrix.translation.z + rotated.z};
        if (!vec3_is_finite(result)) return 0;
        previous = parent;
        parent = parents[parent];
    }
    if (parent != -1 || depth >= 128) return 0;
    out = result;
    return 1;
}

static void prune_skeleton_cache(const std::vector<uint64_t>& players) {
    for (auto it = g_skeletons.begin(); it != g_skeletons.end();) {
        bool present = false;
        for (uint64_t player : players) {
            if (player == it->first) { present = true; break; }
        }
        if (!present) it = g_skeletons.erase(it);
        else ++it;
    }
}

static uint64_t resolve_player_kcc(uint64_t player);

static PlayerAux& player_aux(uint64_t player) {
    PlayerAux& aux = g_player_aux[player];
    if (aux.kcc) {
        // Cheap liveness check every frame; full re-resolve occasionally.
        if (rd_ptr(aux.kcc + KCC_PLAYER_BACKREF) != player || ++aux.revalidate >= 300) {
            aux = PlayerAux{};
        }
    }
    if (!aux.kcc) {
        if (aux.retry_cooldown > 0) { --aux.retry_cooldown; return aux; }
        uint64_t kcc = resolve_player_kcc(player);
        if (!kcc) { aux.retry_cooldown = 30; return aux; }
        aux.kcc = kcc;
        aux.revalidate = 0;
        float nh = rd<float>(kcc + KCC_NORMAL_HEIGHT);
        float ch = rd<float>(kcc + KCC_CROUCH_HEIGHT);
        if (std::isfinite(nh) && nh > 1.2F && nh < 2.6F) aux.normal_height = nh;
        if (std::isfinite(ch) && ch > 0.6F && ch < aux.normal_height) aux.crouch_height = ch;
        uint64_t head = rd_ptr(kcc + KCC_HEAD_TRANSFORM);
        aux.head_native = head ? rd_ptr(head + MANAGED_CACHED_PTR) : 0;
        if (aux.head_native && (aux.head_native < 0x10000 || aux.head_native >= 0x0001000000000000ULL))
            aux.head_native = 0;
        // Head hit volume (what the server actually tests shots against).
        aux.head_hitbox_valid = false;
        uint64_t hb_root = rd_ptr(kcc + KCC_HITBOX_ROOT);
        uint64_t hb_array = hb_root ? rd_ptr(hb_root + HITBOX_ROOT_ARRAY) : 0;
        int32_t hb_count = hb_array ? rd<int32_t>(hb_array + IL2CPP_ARRAY_LENGTH) : 0;
        if (hb_count > 0 && hb_count <= 64) {
            for (int32_t i = 0; i < hb_count; ++i) {
                uint64_t hb = rd_ptr(hb_array + IL2CPP_ARRAY_FIRST_ELEMENT + (uint64_t)i * 8);
                if (!hb || rd<int32_t>(hb + HITBOX_AREA) != 0) continue;
                Vec3 center = rd_v3(hb + HITBOX_CENTER);
                Vec3 size = rd_v3(hb + HITBOX_SIZE);
                if (!vec3_is_finite(center) || !vec3_is_finite(size)) continue;
                if (fabsf(center.x) > 1.0F || fabsf(center.y) > 1.0F || fabsf(center.z) > 1.0F) continue;
                if (!(size.x > 0.02F && size.x < 1.0F && size.y > 0.02F && size.y < 1.0F)) continue;
                uint64_t transform = native_component_transform(managed_object_native(hb));
                if (!skeleton_transform_ptr_valid(transform)) continue;
                aux.head_hitbox_transform = transform;
                aux.head_hitbox_center = center;
                aux.head_hitbox_valid = true;
                break;
            }
        }
    }
    return aux;
}

// Pose from KCC.Move: true when crouched (Pose == Crouch or State == CROUCHING).
static bool player_is_crouched(const PlayerAux& aux) {
    if (!aux.kcc) return false;
    int32_t state = rd<int32_t>(aux.kcc + KCC_MOVE + 0x00);
    int32_t pose  = rd<int32_t>(aux.kcc + KCC_MOVE + 0x04);
    return pose == 1 || state == 3;
}

static bool player_head_world(const PlayerAux& aux, Vec3& out) {
    if (!aux.head_native) return false;
    if (g_skeleton_layout_valid &&
        read_transform_hierarchy_layout(aux.head_native, g_skeleton_layout, out)) return vec3_is_finite(out);
    return read_transform_hierarchy_position(aux.head_native, out) && vec3_is_finite(out);
}

// World-space centre of the Head hit volume: transform TRS applied to the
// local centre (HitBox.transform.TransformPoint(center)).
static bool player_head_hitbox_world(const PlayerAux& aux, Vec3& out) {
    if (!aux.head_hitbox_valid || !aux.head_hitbox_transform) return false;
    Vec3 pos{}; Vec4 rot{};
    const TransformHierarchyLayout& layout = g_skeleton_layout_valid ? g_skeleton_layout : g_transform_hierarchy_layout;
    if (!(g_skeleton_layout_valid || g_transform_hierarchy_layout_valid)) return false;
    if (!read_transform_hierarchy_layout(aux.head_hitbox_transform, layout, pos, &rot)) return false;
    // Lossy scale is ~1 on character rigs; rotate the local centre and offset.
    Vec3 offset = rotate_vector(rot, aux.head_hitbox_center);
    out = {pos.x + offset.x, pos.y + offset.y, pos.z + offset.z};
    return vec3_is_finite(out);
}

static void prune_player_aux(const std::vector<uint64_t>& players) {
    for (auto it = g_player_aux.begin(); it != g_player_aux.end();) {
        bool present = false;
        for (uint64_t player : players) if (player == it->first) { present = true; break; }
        if (!present) it = g_player_aux.erase(it); else ++it;
    }
}

// ---- Vehicles and ghost copies ----------------------------------------------
// Two things go wrong once a player sits in a car:
//   * lastSavedPosition (the field every box is built from) stops being
//     updated and keeps pointing at the spot where he got in;
//   * the old PlayerManager object often stays in the player list next to the
//     one that drives away, so the same player is in there twice.
// Both are handled here: mounted players are positioned from their rendered
// transform (it is parented to the seat, so it follows the car), and objects
// that share a userID are reduced to the one that is actually moving.

struct PlayerTrack {
    char uid[40] = {};      // userID, empty when it could not be read
    int  uid_recheck = 0;   // objects are pooled and reused for other players
    Vec3 last{};
    bool has_last = false;
    int  still_frames = 0;  // consecutive frames without movement
    // How fast this player is actually moving through the world, in metres
    // per second. World space on purpose: it does not care where the camera
    // is pointing or how fast it is turning, so it stays correct even when
    // the camera angles cannot be read at all.
    Vec3   vel{};
    Vec3   vel_ref{};       // position the current estimate was measured from
    double vel_ref_t = 0.0; // and when
    bool   have_vel_ref = false;
};

static double mono_seconds() {
    using namespace std::chrono;
    return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
}
static std::unordered_map<uint64_t, PlayerTrack> g_player_track;
// uid -> the object we drew last time, so a tie does not flip between copies.
static std::unordered_map<std::string, uint64_t> g_player_track_pick;

static void prune_player_track(const std::vector<uint64_t>& players) {
    for (auto it = g_player_track.begin(); it != g_player_track.end();) {
        bool present = false;
        for (uint64_t player : players) if (player == it->first) { present = true; break; }
        if (!present) it = g_player_track.erase(it); else ++it;
    }
    if (g_player_track_pick.size() > 256) g_player_track_pick.clear();
}

static bool player_is_mounted(uint64_t player) {
    if (!player) return false;
    uint32_t vehicle = rd<uint32_t>(player + PLAYER_VEHICLE_ID);
    return vehicle != 0 && vehicle != 0xFFFFFFFFu;
}

// World position of the player's own transform (worldCameraRoot). While he is
// mounted this is the only source that moves with the vehicle.
static bool player_rendered_position(uint64_t player, Vec3& out) {
    uint64_t native = resolve_player_native_transform(player);
    if (!native) return false;
    if (g_skeleton_layout_valid && read_transform_hierarchy_layout(native, g_skeleton_layout, out)
        && vec3_is_finite(out))
        return true;
    return read_transform_hierarchy_position(native, out) && vec3_is_finite(out);
}

// worldCameraRoot sits at eye level, the box is built from the feet.
static constexpr float kCameraRootHeight = 1.60F;

static void apply_mounted_position(uint64_t player, Vec3& feet) {
    if (!g_use_direct_player_position) return; // already using the transform
    if (!player_is_mounted(player)) return;
    Vec3 rendered{};
    if (!player_rendered_position(player, rendered)) return;
    rendered.y -= kCameraRootHeight;
    if (!position_looks_like_world_space(rendered)) return;
    feet = rendered;
}

static PlayerTrack& track_player(uint64_t player, const Vec3& position) {
    PlayerTrack& track = g_player_track[player];
    if (--track.uid_recheck <= 0) {
        track.uid_recheck = 120; // ~2 s: pooled objects change owner
        char uid[40] = {};
        read_managed_string_ex(rd_ptr(player + PLAYER_USER_ID), uid, sizeof(uid), 39);
        memcpy(track.uid, uid, sizeof(track.uid));
    }
    // Velocity, measured between the moments the position actually changed.
    // A remote player's position only arrives on the network tick, so most
    // frames repeat the previous one; differencing those would read zero.
    {
        const double now = mono_seconds();
        if (!track.have_vel_ref) {
            track.vel_ref = position; track.vel_ref_t = now; track.have_vel_ref = true;
        } else {
            const float mx = position.x - track.vel_ref.x;
            const float my = position.y - track.vel_ref.y;
            const float mz = position.z - track.vel_ref.z;
            const float moved = mx * mx + my * my + mz * mz;
            const double span = now - track.vel_ref_t;
            if (moved > 0.0004F) {                     // moved more than 2 cm
                if (span > 0.02 && span < 0.5) {
                    const Vec3 v = { (float)(mx / span), (float)(my / span), (float)(mz / span) };
                    const float speed = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
                    if (std::isfinite(speed) && speed < 12.0F) {   // faster than a man can run: a teleport
                        track.vel.x = track.vel.x * 0.5F + v.x * 0.5F;
                        track.vel.y = track.vel.y * 0.5F + v.y * 0.5F;
                        track.vel.z = track.vel.z * 0.5F + v.z * 0.5F;
                    }
                }
                track.vel_ref = position; track.vel_ref_t = now;
            } else if (span > 0.3) {                   // stood still: stop leading
                track.vel = {};
                track.vel_ref = position; track.vel_ref_t = now;
            }
        }
    }
    if (track.has_last) {
        float dx = position.x - track.last.x;
        float dy = position.y - track.last.y;
        float dz = position.z - track.last.z;
        if (dx * dx + dy * dy + dz * dz > 0.0025F) track.still_frames = 0;      // > 5 cm
        else if (track.still_frames < 100000) ++track.still_frames;
    }
    track.last = position;
    track.has_last = true;
    return track;
}

// ===================== Remote (third-person) held weapon =====================
//
// FPManager/FPObject only exist for the local player, which is why enemies
// never got a weapon label. The networked source is PlayerWeapon (see
// game_offsets.h): PlayerManager.weaponReference -> PlayerWeapon, whose weapon
// view spawns the weapon prefab as a GameObject under the character rig. Two
// independent ways to name it, both ending at a GameObject name:
//   A) PlayerWeapon.playerWeaponViewReference -> Mo.WeaponBase / Mo.rootTransform
//   B) PlayerModelInfo.rightWeaponHolder (or left) -> first child transform
// Route B does not depend on the view's internal layout, so it runs as the
// fallback whenever A comes up empty.

// Il2CppClass name check (klass @0x0, name @0x10) — identifies PlayerWeapon
// without relying on the obfuscated wrapper layout.
static bool object_class_name_is(uint64_t obj, const char* expected) {
    if (!valid_obj(obj)) return false;
    uint64_t klass = rd_ptr(obj);
    if (!valid_obj(klass)) return false;
    return remote_string_equals(rd_ptr(klass + IL2CPP_CLASS_NAME), expected);
}

// The GameObject-name offset is normally discovered while building a skeleton.
// Weapon labels must work with skeleton ESP off, so discover it on demand from
// the character model subtree (cheap: one BFS, then cached process-wide).
static bool ensure_gameobject_name_offset(uint64_t player) {
    if (g_go_name_offset_valid) return true;
    if (g_go_name_retry_cooldown > 0) { --g_go_name_retry_cooldown; return false; }
    uint64_t root = skeleton_model_root(player);
    if (!root) { g_go_name_retry_cooldown = 60; return false; }
    static std::vector<uint64_t> nodes;
    collect_transform_subtree(root, nodes, 256);
    if (nodes.size() < 8 || !discover_gameobject_name_offset(nodes)) {
        g_go_name_retry_cooldown = 60;
        return false;
    }
    return true;
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
    strncpy(out, s, cap - 1);
    out[cap - 1] = '\0';
    return true;
}

// Managed MonoBehaviour/Component -> name of the GameObject it sits on.
static bool managed_component_gameobject_name(uint64_t managed_component, char* out, size_t cap) {
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
               object_class_name_is(candidate, "PlayerWeapon");
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

// Angular offset of a world point from the camera axis. Prefers the live
// camera pose; falls back to inverting the projection from the screen point,
// so aim angles never depend on the transform-pose path succeeding.
static bool aim_angles_for(const Vec3& world, const Vec2& screen, float sw, float sh, float& yaw_deg, float& pitch_deg) {
    constexpr float rad2deg = 57.29577951F;
    if (g_cam_pose_valid || g_aim_ref_valid) {
        // Prefer the real firing reference (look root direction from the eye
        // point); the camera pose is the fallback.
        const bool use_ref = g_aim_ref_valid;
        const Vec3& origin = use_ref ? g_aim_ref_origin : g_cam_pos;
        const Vec3& fwd = use_ref ? g_aim_ref_forward : g_cam_forward;
        const Vec3& right = use_ref ? g_aim_ref_right : g_cam_right;
        const Vec3& up = use_ref ? g_aim_ref_up : g_cam_up;
        Vec3 d = {world.x - origin.x, world.y - origin.y, world.z - origin.z};
        float fx = d.x * fwd.x + d.y * fwd.y + d.z * fwd.z;
        float rx = d.x * right.x + d.y * right.y + d.z * right.z;
        float ux = d.x * up.x + d.y * up.y + d.z * up.z;
        if (std::isfinite(fx) && std::isfinite(rx) && std::isfinite(ux) && fx > 0.05F) {
            yaw_deg = atan2f(rx, fx) * rad2deg;
            pitch_deg = atan2f(ux, sqrtf(fx * fx + rx * rx)) * rad2deg;
            if (std::isfinite(yaw_deg) && std::isfinite(pitch_deg)) return true;
        }
    }
    float fov = (g_cam_fov_deg > 1.0F && g_cam_fov_deg < 179.0F) ? g_cam_fov_deg : 60.0F;
    float tan_half_v = tanf(fov * 0.5F / rad2deg);
    float aspect = sw / sh;
    float ndc_x = (screen.x / sw) * 2.0F - 1.0F;
    float ndc_y = 1.0F - (screen.y / sh) * 2.0F;
    yaw_deg = atanf(ndc_x * tan_half_v * aspect) * rad2deg;
    pitch_deg = atanf(ndc_y * tan_half_v) * rad2deg;
    return std::isfinite(yaw_deg) && std::isfinite(pitch_deg);
}

// Extra vertical offset applied to every head aim point (metres, world up).
// Tuned in-game at fighting range: +12 cm lands centre-head. This is the
// far-range figure; head_lift_for_range() fades down to the near one below.
static constexpr float g_aim_head_lift = 0.12F;
// Up close the same 14 cm is a completely different shot: at five metres it
// is well over a degree, which puts the round over the top of the head, while
// at fifty it is a tenth of that and still inside the skull. The offset is
// therefore ramped in with range instead of being a constant.
static constexpr float g_aim_head_lift_near = 0.04F;

static float head_lift_for_range(const Vec3& world) {
    if (!g_cam_pose_valid) return g_aim_head_lift;
    float dx = world.x - g_cam_pos.x, dy = world.y - g_cam_pos.y, dz = world.z - g_cam_pos.z;
    float range = sqrtf(dx * dx + dy * dy + dz * dz);
    if (!std::isfinite(range)) return g_aim_head_lift;
    float t = (range - 8.0F) / 30.0F;          // full offset from ~38 m out
    if (t < 0.0F) t = 0.0F; else if (t > 1.0F) t = 1.0F;
    return g_aim_head_lift_near + (g_aim_head_lift - g_aim_head_lift_near) * t;
}

// Where the target will be by the time the finger move we are about to send
// has travelled through the phone and come back out as camera rotation. A man
// running past keeps running during that hundredth-of-a-second or two, and
// without this the crosshair sits permanently behind him, by a distance
// proportional to how fast he runs.
//
// It is his own speed through the world, so nothing here depends on knowing
// the look sensitivity, on reading the camera angles, or on separating our
// turning from his -- the three things that have no reliable answer on this
// build. Set per player just below, applied here, and it only shifts the
// point the aim steers to: the ESP box still draws where the man actually is.
static constexpr float kAimLeadSeconds = 0.060F;
static Vec3 g_aim_lead{};

static bool set_aim_point(EspBox& box, int slot, const Vec3& world_in, const Mat4& vp, float sw, float sh) {
    Vec3 world = world_in;
    world.x += g_aim_lead.x; world.y += g_aim_lead.y; world.z += g_aim_lead.z;
    if (slot == 0) world.y += head_lift_for_range(world_in);
    Vec2 screen{};
    if (!w2s(vp, world, sw, sh, screen, false)) return false;
    if (fabsf(screen.x) > sw * 4.0F || fabsf(screen.y) > sh * 4.0F) return false;
    float yaw = 0.0F, pitch = 0.0F;
    if (!aim_angles_for(world, screen, sw, sh, yaw, pitch)) return false;
    box.aim_pts[slot][0] = screen.x;
    box.aim_pts[slot][1] = screen.y;
    box.aim_yaw[slot] = yaw;
    box.aim_pitch[slot] = pitch;
    box.aim_valid[slot] = true;
    return true;
}

static bool fill_skeleton_box(uint64_t player, const Mat4& view_projection, float sw, float sh, EspBox& box) {
    box.has_skeleton = false;
    for (int bone = 0; bone < ESP_BONE_COUNT; ++bone) box.bone_valid[bone] = false;
    for (int i = 0; i < 3; ++i) box.aim_valid[i] = false;

    CachedSkeleton& skeleton = g_skeletons[player];
    if (!skeleton.valid) {
        if (skeleton.retry_cooldown > 0) { --skeleton.retry_cooldown; return false; }
        // Building walks the whole model; allow at most one rebuild per frame
        // so multiple new players never stall the overlay.
        if (g_skeleton_builds_this_frame >= 1) return false;
        ++g_skeleton_builds_this_frame;
        if (!build_skeleton(player, skeleton)) {
            skeleton.valid = false;
            skeleton.retry_cooldown = 20;
            return false;
        }
    }

    // Periodically make sure the player still uses the same character model.
    if (++skeleton.revalidate_timer >= 120) {
        skeleton.revalidate_timer = 0;
        if (skeleton_model_root(player) != skeleton.model_root) {
            skeleton = CachedSkeleton{};
            skeleton.retry_cooldown = 2;
            return false;
        }
    }

    // Bones may legitimately live in SEVERAL TransformHierarchies: the game
    // re-parents bones at runtime (Ragdoll.m_BonesToReparent, aim rigs, foot
    // IK), which moves them into a different hierarchy. Never drop a bone for
    // that — group bones by hierarchy data and bulk-read every group.
    constexpr int kMaxGroups = 4;
    uint64_t group_data[kMaxGroups] = {};
    int32_t  group_max[kMaxGroups] = {};
    int      group_count = 0;
    int32_t  bone_index[ESP_BONE_COUNT];
    int      bone_group[ESP_BONE_COUNT];
    for (int bone = 0; bone < ESP_BONE_COUNT; ++bone) {
        bone_index[bone] = -1;
        bone_group[bone] = -1;
        uint64_t transform = skeleton.bone_transform[bone];
        if (!transform) continue;
        uint64_t bone_data = rd_ptr(transform + g_skeleton_layout.data_offset);
        if (!bone_data) continue;
        int32_t index = rd<int32_t>(transform + g_skeleton_layout.index_offset);
        if (index < 0 || index > 100000) continue;
        int group = -1;
        for (int g = 0; g < group_count; ++g)
            if (group_data[g] == bone_data) { group = g; break; }
        if (group < 0) {
            if (group_count >= kMaxGroups) continue;
            group = group_count++;
            group_data[group] = bone_data;
            group_max[group] = -1;
        }
        bone_index[bone] = index;
        bone_group[bone] = group;
        if (index > group_max[group]) group_max[group] = index;
    }
    // Liveness: the hips transform no longer resolves => model despawned.
    if (group_count == 0 || bone_index[BONE_HIPS] < 0) {
        if (++skeleton.fail_streak > 30) { skeleton = CachedSkeleton{}; skeleton.retry_cooldown = 30; }
        return false;
    }

    // Bulk read the TRS + parent-index arrays of every hierarchy in use.
    static std::vector<Matrix34> local_matrices[kMaxGroups];
    static std::vector<int32_t>  local_parents[kMaxGroups];
    uint64_t group_matrices[kMaxGroups] = {};
    uint64_t group_indices[kMaxGroups] = {};
    bool     group_ok[kMaxGroups] = {};
    for (int g = 0; g < group_count; ++g) {
        int32_t needed = group_max[g] + 1;
        if (needed <= 0 || needed > 8192) continue;
        uint64_t matrices = rd_ptr(group_data[g] + g_skeleton_layout.matrices_offset);
        uint64_t indices = rd_ptr(group_data[g] + g_skeleton_layout.indices_offset);
        if (g_skeleton_layout.matrices_indirect) matrices = rd_ptr(matrices);
        if (g_skeleton_layout.indices_indirect) indices = rd_ptr(indices);
        if (!matrices || !indices) continue;
        local_matrices[g].resize((size_t)needed);
        local_parents[g].resize((size_t)needed);
        if (!rd_buf(matrices, local_matrices[g].data(), (size_t)needed * sizeof(Matrix34)) ||
            !rd_buf(indices, local_parents[g].data(), (size_t)needed * sizeof(int32_t))) continue;
        group_matrices[g] = matrices;
        group_indices[g] = indices;
        group_ok[g] = true;
    }

    int projected = 0;
    int remote_fallbacks = 0;
    for (int bone = 0; bone < ESP_BONE_COUNT; ++bone) {
        Vec3 world{};
        bool have_world = false;
        if (bone_index[bone] >= 0 && bone_group[bone] >= 0) {
            int g = bone_group[bone];
            int walked = 0;
            if (group_ok[g]) {
                walked = skeleton_local_walk(local_matrices[g].data(), local_parents[g].data(),
                                             (int32_t)local_matrices[g].size(), bone_index[bone], world);
                if (walked < 0 && remote_fallbacks < 8) {
                    // Parent chain leaves the buffered range (rare): remote walk.
                    ++remote_fallbacks;
                    walked = read_transform_hierarchy_arrays(group_matrices[g], group_indices[g],
                                                             bone_index[bone], world) ? 1 : 0;
                }
            } else if (remote_fallbacks < 8) {
                ++remote_fallbacks;
                walked = read_transform_hierarchy_layout(skeleton.bone_transform[bone],
                                                         g_skeleton_layout, world) ? 1 : 0;
            }
            if (walked == 1) {
                have_world = true;
                skeleton.bone_world[bone] = world;
                skeleton.bone_world_age[bone] = 1;
            }
        }
        // Transient read glitch (game mid-update): briefly reuse the last known
        // world position instead of letting the bone flicker off.
        if (!have_world && skeleton.bone_world_age[bone] >= 1 && skeleton.bone_world_age[bone] <= 8) {
            world = skeleton.bone_world[bone];
            ++skeleton.bone_world_age[bone];
            have_world = true;
        }
        if (!have_world) continue;
        Vec2 screen{};
        if (!w2s(view_projection, world, sw, sh, screen, false)) continue;
        if (fabsf(screen.x) > sw * 4.0F || fabsf(screen.y) > sh * 4.0F) continue;
        box.bones[bone][0] = screen.x;
        box.bones[bone][1] = screen.y;
        box.bone_valid[bone] = true;
        ++projected;
    }
    // Aim points: exact bone world positions -> screen + angular offsets.
    //   [0] head  : skull centre. The Head joint sits at the base of the skull,
    //               the head hitbox extends ~20 cm above it. Aim ~11 cm up the
    //               neck axis, plus a small distance-dependent lift so that at
    //               long range the shot lands inside the skull rather than at
    //               its lower edge (steering/animation error grows with range).
    //   [1] neck  : between the Neck and Head joints.
    //   [2] chest : upper spine.
    {
        auto bone_world_ok = [&](int bone, Vec3& out) -> bool {
            if (!box.bone_valid[bone]) return false;
            if (skeleton.bone_world_age[bone] < 1 || skeleton.bone_world_age[bone] > 9) return false;
            out = skeleton.bone_world[bone];
            return vec3_is_finite(out);
        };
        Vec3 target[3]{};
        bool have[3] = {false, false, false};

        Vec3 head{}, neck{};
        const bool have_head_bone = bone_world_ok(BONE_HEAD, head);
        const bool have_neck_bone = bone_world_ok(BONE_NECK, neck);
        if (have_head_bone) {
            Vec3 dir = {0.0F, 1.0F, 0.0F};
            if (have_neck_bone) {
                Vec3 d = {head.x - neck.x, head.y - neck.y, head.z - neck.z};
                float len = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
                if (len > 0.02F && len < 0.6F) dir = {d.x / len, d.y / len, d.z / len};
            }
            float range = 0.0F;
            if (g_cam_pose_valid) {
                float dx = head.x - g_cam_pos.x, dy = head.y - g_cam_pos.y, dz = head.z - g_cam_pos.z;
                range = sqrtf(dx * dx + dy * dy + dz * dz);
            }
            float lift = range * 0.0010F;          // +10 cm per 100 m
            if (lift > 0.08F) lift = 0.08F;
            target[0] = {head.x + dir.x * 0.11F, head.y + dir.y * 0.11F + lift, head.z + dir.z * 0.11F};
            have[0] = true;
        }
        if (have_neck_bone && have_head_bone) {
            target[1] = {(neck.x + head.x) * 0.5F, (neck.y + head.y) * 0.5F, (neck.z + head.z) * 0.5F};
            have[1] = true;
        } else if (have_neck_bone) {
            target[1] = {neck.x, neck.y + 0.05F, neck.z}; have[1] = true;
        } else if (have_head_bone) {
            target[1] = {head.x, head.y - 0.06F, head.z}; have[1] = true;
        }
        Vec3 chest{};
        if (bone_world_ok(BONE_SPINE2, chest) || bone_world_ok(BONE_SPINE1, chest) || bone_world_ok(BONE_SPINE, chest)) {
            target[2] = chest; have[2] = true;
        }

        for (int i = 0; i < 3; ++i) {
            if (!have[i]) continue;
            if (set_aim_point(box, i, target[i], view_projection, sw, sh)) box.aim_source = 1;
        }
    }

    if (projected < 4) {
        if (++skeleton.fail_streak > 30) { skeleton = CachedSkeleton{}; skeleton.retry_cooldown = 30; }
        return false;
    }
    skeleton.fail_streak = 0;
    box.has_skeleton = true;
    return true;
}

// ===================== Local player aim (ADS) state =====================

static bool read_local_aim_state() {
    uint64_t local = resolve_local_player();
    if (!local) { g_aim_state = {}; return false; }

    // Cheap fast path on cached pointers; re-validate the chain periodically.
    if (g_aim_state.aim_activity && --g_aim_state.revalidate > 0) {
        uint8_t active = 0;
        if (rd_exact(g_aim_state.aim_activity + ACTIVITY_ACTIVE_FLAG, active)) {
            g_aim_state.aiming = (active != 0);
            g_aim_state.source = 1;
            return true;
        }
    }
    if (!g_aim_state.aim_activity && g_aim_state.weapon && --g_aim_state.revalidate > 0) {
        uint8_t is_aiming = 0;
        if (rd_ptr(g_aim_state.weapon + FPOBJECT_PLAYER_BACKREF) == local &&
            rd_exact(g_aim_state.weapon + FPWEAPON_IS_AIMING, is_aiming)) {
            g_aim_state.aiming = (is_aiming != 0);
            g_aim_state.source = 2;
            return true;
        }
    }

    LocalAimState fresh{};
    fresh.revalidate = 60;

    // Primary: PlayerManager.playerEventHandler.Aim.Active
    uint64_t handler = rd_ptr(local + PLAYER_EVENT_HANDLER);
    if (handler && rd_ptr(handler + EVENT_HANDLER_MANAGER_BACKREF) == local) {
        uint64_t activity = rd_ptr(handler + EVENT_HANDLER_AIM_ACTIVITY);
        uint8_t active = 0;
        if (activity && rd_exact(activity + ACTIVITY_ACTIVE_FLAG, active) && active <= 1) {
            fresh.event_handler = handler;
            fresh.aim_activity = activity;
            fresh.aiming = (active != 0);
            fresh.source = 1;
            g_aim_state = fresh;
            return true;
        }
    }

    // Fallback: current first-person weapon isAiming flag.
    uint64_t fp_manager = rd_ptr(local + PLAYER_FP_MANAGER);
    if (fp_manager) {
        uint64_t weapon = rd_ptr(fp_manager + FPMANAGER_CURRENT_WEAPON);
        if (weapon && rd_ptr(weapon + FPOBJECT_PLAYER_BACKREF) == local) {
            uint8_t is_aiming = 0;
            if (rd_exact(weapon + FPWEAPON_IS_AIMING, is_aiming) && is_aiming <= 1) {
                fresh.fp_manager = fp_manager;
                fresh.weapon = weapon;
                fresh.aiming = (is_aiming != 0);
                fresh.source = 2;
                g_aim_state = fresh;
                return true;
            }
        }
        // Last resort: the FOV blend factor FPManager drives toward aimFOV.
        float blend = 0.0F;
        if (rd_exact(fp_manager + FPMANAGER_AIM_BLEND, blend) && std::isfinite(blend) && blend >= 0.0F && blend <= 1.0F) {
            fresh.fp_manager = fp_manager;
            fresh.aiming = blend > 0.5F;
            fresh.source = 3;
            g_aim_state = fresh;
            return true;
        }
    }

    g_aim_state = {};
    return false;
}

bool esp_local_player_is_aiming() {
    if (g_pid <= 0 || !g_il2cpp_base) return false;
    if (!read_local_aim_state()) return false;
    return g_aim_state.aiming;
}

float esp_camera_fov_deg() { return g_cam_fov_deg; }

// Bit 0: camera pose known. Bit 1: pose was derived from the view matrix
// rather than read from the Transform. Bit 2: the firing reference (look
// direction from the eye point) is in use instead of the camera axis.
int esp_camera_state() {
    return (g_cam_pose_valid ? 1 : 0) | (g_cam_pose_derived ? 2 : 0) | (g_aim_ref_valid ? 4 : 0);
}

bool esp_camera_angles(float& yaw_deg, float& pitch_deg) {
    if (!g_cam_pose_valid && !g_aim_ref_valid) return false;
    // Same reference the aim angles are measured against (firing direction
    // when available), so finger-gain learning and target lead stay consistent.
    const Vec3& f = g_aim_ref_valid ? g_aim_ref_forward : g_cam_forward;
    constexpr float rad2deg = 57.29577951F;
    float yaw = atan2f(f.x, f.z) * rad2deg;
    float horiz = sqrtf(f.x * f.x + f.z * f.z);
    float pitch = atan2f(f.y, horiz) * rad2deg;
    if (!std::isfinite(yaw) || !std::isfinite(pitch)) return false;
    yaw_deg = yaw; pitch_deg = pitch;
    return true;
}


// Pipeline status for the on-screen debug line:
//   R  = ragdoll build stage (0 ok; 2 no KCC, 3 no anim, 4 anim backref,
//        5 no ragdoll, 6 no array, 7 bad count, 8/11 few bones, 9 layout,
//        10 arrays, 12 pelvis, 13 chest, 14 final; -1 never ran)
//   H  = hips candidates (name path), N = best name-path bone count
//   P  = build path used (1 ragdoll, 2 names), B = cached bones
//   F  = fill failure (0 ok, 1 cooldown, 2 build, 3 root, 4 hips gone,
//        7 projected<4)
//   C  = players with a valid cached skeleton
//   D  = distinct transform hierarchies used by the bones (re-parenting),
//        then the per-bone mask torso|armL|armR|legL|legR.
bool esp_init(pid_t pid) {
    g_pid = pid;
    g_il2cpp_base = get_base("libil2cpp.so");
    if (!g_il2cpp_base) return false;
    return true;
}

// KCC.Move.Position: the simulated character position (capsule bottom) the
// game itself moves the character with. Independent of the discovered
// PlayerManager position field, and validated by the KCC back-reference.
static bool player_kcc_position(const PlayerAux& aux, Vec3& out) {
    if (!aux.kcc) return false;
    Vec3 p = rd_v3(aux.kcc + KCC_MOVE + 0x0C);
    if (!vec3_is_finite(p)) return false;
    float magnitude = fabsf(p.x) + fabsf(p.y) + fabsf(p.z);
    if (magnitude < 0.01F || magnitude > 100000.0F) return false;
    out = p;
    return true;
}

// Local firing reference: LookDirection from the event handler plus the eye
// point the hitscan ray starts from. Falls back to the camera when unavailable
// or implausible (must stay within ~20 deg of the camera forward).
static void read_local_aim_reference(uint64_t local_player, const PlayerAux* local_aux, bool local_crouched) {
    g_aim_ref_valid = false;
    if (!local_player || !g_cam_pose_valid) return;
    uint64_t handler = rd_ptr(local_player + PLAYER_EVENT_HANDLER);
    if (!handler || rd_ptr(handler + EVENT_HANDLER_MANAGER_BACKREF) != local_player) return;
    uint64_t look = rd_ptr(handler + EVENT_HANDLER_LOOK_DIRECTION);
    if (!look) return;
    Vec3 dir = rd_v3(look + SYNC_VALUE_OFFSET);
    if (!vec3_is_finite(dir)) return;
    float len = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (!(len > 0.5F && len < 2.0F)) return;
    dir = {dir.x / len, dir.y / len, dir.z / len};
    float dot = dir.x * g_cam_forward.x + dir.y * g_cam_forward.y + dir.z * g_cam_forward.z;
    if (!(dot > 0.94F)) return; // > ~20 deg away from the camera: not the look root
    Vec3 world_up = {0.0F, 1.0F, 0.0F};
    Vec3 right = cross_product(world_up, dir);
    float rl = sqrtf(right.x * right.x + right.y * right.y + right.z * right.z);
    if (!(rl > 0.01F)) return; // looking straight up/down: keep camera basis
    right = {right.x / rl, right.y / rl, right.z / rl};
    Vec3 up = cross_product(dir, right);

    // Eye point: KCC position + (capsule height + lookHeightOffset) * up. Only
    // trusted when it lands close to the camera; otherwise use the camera.
    Vec3 origin = g_cam_pos;
    if (local_aux && local_aux->kcc) {
        Vec3 kcc_pos{};
        if (player_kcc_position(*local_aux, kcc_pos)) {
            float h = local_crouched ? local_aux->crouch_height : local_aux->normal_height;
            float look_offset = rd<float>(local_aux->kcc + KCC_LOOK_HEIGHT_OFFSET);
            if (!std::isfinite(look_offset) || fabsf(look_offset) > 1.0F) look_offset = 0.0F;
            Vec3 eye = {kcc_pos.x, kcc_pos.y + h + look_offset, kcc_pos.z};
            float dx = eye.x - g_cam_pos.x, dy = eye.y - g_cam_pos.y, dz = eye.z - g_cam_pos.z;
            if (dx * dx + dy * dy + dz * dz < 0.5F * 0.5F) origin = eye;
        }
    }
    g_aim_ref_origin = origin;
    g_aim_ref_forward = dir;
    g_aim_ref_right = right;
    g_aim_ref_up = up;
    g_aim_ref_valid = true;
}

// Per-frame player list, kept across frames so a transient empty read does
// not blank the overlay, but dropped on a real world change.
static std::vector<uint64_t> g_frame_transforms;

// Frame projection state, published by esp_get_boxes() so that world markers
// (ore / animals) project through exactly the same camera as the player boxes.
static Mat4 g_frame_vp{};
static bool g_frame_vp_valid = false;
static float g_frame_sw = 0.0F, g_frame_sh = 0.0F;
static Vec3 g_frame_local_pos{};
static bool g_frame_local_valid = false;
// Camera basis recovered from this frame's VIEW MATRIX (not the transform
// pose). On devices where the transform pose read fails, this is the only
// camera orientation available — good enough for the farm's slow turns,
// though not for the aimbot (the fallback matrix lags a frame).
static bool g_frame_cam_basis_valid = false;
static Vec3 g_frame_cam_pos{}, g_frame_cam_fwd{}, g_frame_cam_right{}, g_frame_cam_up{};
// Players near us this frame (all 360 degrees, not only the ones projected
// on screen). Feeds the enemy-counter pill in the overlay.
static int  g_frame_player_count = 0;
static int      g_frame_transforms_empty_streak = 0;
// Frames in a row esp_get_boxes() gave up before publishing this frame's
// camera / local position (see the watchdog at the top of it).
static int      g_frame_publish_fail_streak = 0;

// Everything derived from a particular world/session. Called when the whole
// player population is replaced (scene reload / new session) or the player
// list disappears for a while, so no stale pointers survive into the next
// world. Deliberately NOT tied to the camera object: the game swaps cameras
// while aiming, which must not disturb boxes or skeletons.
// Defined with the marker code further down (needs its caches).
static void reset_marker_caches();

static void reset_world_caches() {
    g_matrix_configuration_validated = false; g_camera_matrix_physical_match = false;
    g_player_position_validated = false;
    g_use_direct_player_position = true; g_player_position_offset = PLAYER_POSITION;
    g_direct_position_fail_streak = 0; g_direct_position_recheck = 0;
    g_local_player = 0;
    g_aim_state = {};
    g_aim_ref_valid = false;
    g_player_aux.clear();
    g_player_text.clear();
    g_player_track.clear();
    g_player_track_pick.clear();
    g_skeletons.clear();
    reset_marker_caches();
}

void esp_reset() {
    g_pid = -1; g_il2cpp_base = 0;
    g_frame_transforms.clear(); g_frame_transforms_empty_streak = 0;
    g_frame_publish_fail_streak = 0;
    g_aim_ref_valid = false;
    g_player_manager_class = 0; g_player_manager_static_fields = 0;
    g_game_controller_class = 0; g_local_player = 0;
    g_matrix_configuration_validated = false; g_camera_matrix_physical_match = false;
    g_player_position_offset = PLAYER_POSITION;
    g_transform_hierarchy_layout = {}; g_transform_hierarchy_layout_valid = false;
    g_use_direct_player_position = true;
    g_player_position_validated = false;
    g_direct_position_fail_streak = 0; g_direct_position_recheck = 0;
    g_cam_fov_deg = 0.0F; g_cam_pose_valid = false; g_cam_pose_derived = false;
    g_aim_state = {};
    g_player_aux.clear();
    g_player_text.clear();
    g_player_track.clear();
    g_player_track_pick.clear();
    g_skeletons.clear();
    g_skeleton_layout = {}; g_skeleton_layout_valid = false;
    g_go_name_offset = 0; g_go_name_plain_pointer = false;
    g_go_name_offset_valid = false; g_go_name_retry_cooldown = 0;
    g_frame_vp_valid = false; g_frame_local_valid = false;
    reset_marker_caches();
}


// Last overlay size esp_get_boxes() was called with — the camera-only frame
// fallback below needs plausible screen dimensions even when the box pipeline
// bailed out before publishing anything.
static float g_last_overlay_sw = 1080.0F;
static float g_last_overlay_sh = 2400.0F;

// Publish a frame (VP matrix + "local position") straight from the game
// camera, with no players involved at all. This is what keeps markers and
// the autofarm alive when the player list is empty or the box pipeline
// failed: the camera IS where the local player is.
// ВРЕМЕННАЯ диагностика меток: где именно умирает цепочка. Показывается
// жёлтой строкой на экране, убрать после починки.
int g_marker_trace_step = 0;      // шаг, на котором цепочка оборвалась
int g_marker_trace_scan = -1;     // сколько сущностей нашёл последний скан
int g_marker_trace_dict = -1;     // счётчик entries словаря Mirror
int esp_marker_trace_step() { return g_marker_trace_step; }
int esp_marker_trace_scan() { return g_marker_trace_scan; }
int esp_marker_trace_dict() { return g_marker_trace_dict; }

static bool publish_camera_only_frame(float sw, float sh) {
    if (g_pid <= 0 || !g_il2cpp_base) { g_marker_trace_step = 10; return false; }
    // Resolves g_game_controller_class as a side effect — without it the
    // camera lookup below has no class to read statics from.
    resolve_local_player();
    if (!g_game_controller_class) { g_marker_trace_step = 11; return false; }
    uint64_t gcb_sf = get_class_static_fields(g_game_controller_class);
    if (!gcb_sf) { g_marker_trace_step = 12; return false; }
    uint64_t cam_mgr = rd_ptr(gcb_sf + GAME_CONTROLLER_CAMERA_MANAGER_FIELD);
    if (!cam_mgr) { g_marker_trace_step = 13; return false; }
    uint64_t managed_cam = rd_ptr(cam_mgr + CAMERA_MANAGER_CAMERA_FIELD);
    if (!managed_cam) { g_marker_trace_step = 14; return false; }
    uint64_t cam_native = rd_ptr(managed_cam + MANAGED_CACHED_PTR);
    if (!cam_native) { g_marker_trace_step = 15; return false; }
    if (!(sw >= 100.0F) || !(sh >= 100.0F)) { sw = 1080.0F; sh = 2400.0F; }
    Mat4 solo_proj{}, solo_view{};
    if (!read_native_camera_matrices(cam_native, sw / sh, solo_proj, solo_view)) { g_marker_trace_step = 16; return false; }
    Vec3 cam_pos{};
    if (!camera_position_from_view(solo_view, cam_pos)) { g_marker_trace_step = 17; return false; }
    g_frame_vp = mat_mul(solo_proj, solo_view);
    g_frame_vp_valid = true;
    g_frame_sw = sw; g_frame_sh = sh;
    g_frame_local_pos = cam_pos;
    g_frame_local_valid = true;
    g_frame_publish_fail_streak = 0;
    // Camera basis for the farm, same shape as the main path builds.
    Vec3 vr = {mat_get(solo_view, 0, 0), mat_get(solo_view, 0, 1), mat_get(solo_view, 0, 2)};
    Vec3 vu = {mat_get(solo_view, 1, 0), mat_get(solo_view, 1, 1), mat_get(solo_view, 1, 2)};
    Vec3 vf = {-mat_get(solo_view, 2, 0), -mat_get(solo_view, 2, 1), -mat_get(solo_view, 2, 2)};
    if (vec3_is_finite(vr) && vec3_is_finite(vu) && vec3_is_finite(vf)) {
        float fl = sqrtf(vf.x * vf.x + vf.y * vf.y + vf.z * vf.z);
        if (fl > 0.5F && fl < 2.0F) {
            g_frame_cam_pos = cam_pos;
            g_frame_cam_fwd = {vf.x / fl, vf.y / fl, vf.z / fl};
            g_frame_cam_right = vr;
            g_frame_cam_up = vu;
            g_frame_cam_basis_valid = true;
        }
    }
    return true;
}

std::vector<EspBox> esp_get_boxes(int overlay_width, int overlay_height) {
    std::vector<EspBox> result;


    // Watchdog: every frame that fails to publish a camera + local position
    // counts up, and a few seconds of that means some cached offset or class
    // pointer did not survive the last world reload. Rebuilding everything is
    // cheap and rare, and it is what keeps a death / respawn from killing the
    // whole ESP until the app is restarted.
    if (++g_frame_publish_fail_streak > 240) {
        g_frame_publish_fail_streak = 0;
        g_frame_transforms.clear();
        reset_world_caches();
    }

    // Markers reuse this frame's camera; invalidate it until it is rebuilt so
    // an early return here can never leave them projecting through a stale one.
    g_frame_vp_valid = false;
    g_frame_local_valid = false;
    g_frame_cam_basis_valid = false;
    g_frame_player_count = 0;

    if (g_pid <= 0 || !g_il2cpp_base) { return result; }

    uint64_t native_cam = 0;
    Mat4 projection{}, view{}, vp{};
    float sw = overlay_width >= 100 ? (float)overlay_width : 1080.0F;
    float sh = overlay_height >= 100 ? (float)overlay_height : 2400.0F;
    if (!std::isfinite(sw) || sw < 100.0F || sw > 10000.0F) sw = 1080.0F;
    if (!std::isfinite(sh) || sh < 100.0F || sh > 10000.0F) sh = 2400.0F;
    g_last_overlay_sw = sw;
    g_last_overlay_sh = sh;

    std::vector<uint64_t>& s_transforms = g_frame_transforms;
    std::vector<uint64_t> refreshed = read_configured_player_transforms();
    if (!refreshed.empty()) {
        // World reload: every PlayerManager object is new (no overlap with
        // the previous population). This also has to fire when we are alone on
        // the server -- respawning solo replaces our single PlayerManager and
        // used to leave every cache pointing at the dead one.
        if (!s_transforms.empty()) {
            bool overlap = false;
            for (uint64_t previous : s_transforms) {
                for (uint64_t current : refreshed) if (previous == current) { overlap = true; break; }
                if (overlap) break;
            }
            if (!overlap) reset_world_caches();
        }
        s_transforms = std::move(refreshed);
        g_frame_transforms_empty_streak = 0;
    } else if (++g_frame_transforms_empty_streak > 10) {
        // List gone for a while: the world is being torn down / reloaded —
        // or we are simply alone on the server. Clear the player caches, but
        // FALL THROUGH to the empty-list branch below: it publishes the
        // camera-only frame that keeps markers and the farm alive solo.
        if (!s_transforms.empty()) { s_transforms.clear(); reset_world_caches(); }
    }
    if (s_transforms.empty()) {
        // Alone on the server: no player boxes, but markers and the farm
        // still need this frame's camera + local position.
        publish_camera_only_frame(sw, sh);
        return result;
    }

    if (g_body_caches_dirty) { g_body_caches_dirty = false; g_player_aux.clear(); g_skeletons.clear(); }
    const bool want_bones = g_skeleton_enabled || g_aim_bones_requested;
    prune_player_aux(s_transforms);
    prune_player_text(s_transforms);
    prune_player_track(s_transforms);
    if (want_bones) prune_skeleton_cache(s_transforms);
    else if (!g_skeletons.empty()) g_skeletons.clear();
    g_skeleton_builds_this_frame = 0;

    if (!g_player_position_validated) {
        if (!discover_player_position_offset(s_transforms)) return result;
    } else {
        recheck_direct_player_position(s_transforms);
    }

    bool transform_camera_mode = false; // light fix: always use native cam matrices, avoid dead-body-as-camera on death
    if (!transform_camera_mode) {
        uint64_t managed_cam = 0;
        if (g_game_controller_class) {
            uint64_t gcb_sf = get_class_static_fields(g_game_controller_class);
            if (gcb_sf) {
                uint64_t cam_mgr = rd_ptr(gcb_sf + GAME_CONTROLLER_CAMERA_MANAGER_FIELD);
                if (cam_mgr) managed_cam = rd_ptr(cam_mgr + CAMERA_MANAGER_CAMERA_FIELD);
            }
        }
        if (!managed_cam) {
            return result;
        }
        native_cam = rd_ptr(managed_cam + MANAGED_CACHED_PTR);
        if (!native_cam) {
            return result;
        }
        if (!read_native_camera_matrices(native_cam, sw / sh, projection, view)) {
            return result;
        }
        if (!g_matrix_configuration_validated) {
            if (!optimize_matrix_configuration(native_cam, s_transforms)) {
                return result;
            }
            if (!read_native_camera_matrices(native_cam, sw / sh, projection, view)) return result;
        }
        // Unity worldToClip = projection * worldToCamera (same order as native 0xe2b90c).
        vp = mat_mul(projection, view);
    }

    bool has_local_position = false;
    Vec3 local{};
    size_t local_entity_index = s_transforms.size();
    // Positions are read once per frame here and reused below: the box loop
    // must see exactly the same values the local player was picked with.
    std::vector<Vec3> positions(s_transforms.size());
    std::vector<char> position_ok(s_transforms.size(), 0);

    {
        Vec3 camera_position{};
        bool has_camera_position = g_camera_matrix_physical_match && camera_position_from_view(view, camera_position);
        double nearest_distance_squared = INFINITY;
        size_t first_valid_index = s_transforms.size();
        Vec3 first_valid_position{};
        for (size_t index = 0; index < s_transforms.size(); ++index) {
            Vec3 candidate{};
            if (!read_entity_position(s_transforms[index], candidate)) continue;
            apply_mounted_position(s_transforms[index], candidate);
            positions[index] = candidate;
            position_ok[index] = 1;
            track_player(s_transforms[index], candidate);
            if (first_valid_index == s_transforms.size()) { first_valid_index = index; first_valid_position = candidate; }
            if (!has_camera_position) continue;
            double dx = (double)candidate.x - camera_position.x, dy = (double)candidate.y - camera_position.y, dz = (double)candidate.z - camera_position.z;
            double distance_squared = dx * dx + dy * dy + dz * dz;
            if (std::isfinite(distance_squared) && distance_squared < nearest_distance_squared) {
                nearest_distance_squared = distance_squared; local_entity_index = index; local = candidate;
            }
        }
        if (local_entity_index == s_transforms.size() && first_valid_index != s_transforms.size()) {
            local_entity_index = first_valid_index; local = first_valid_position;
        }
        has_local_position = local_entity_index != s_transforms.size();
        if (!has_local_position) {
            g_player_position_validated = false;
            return result;
        }
    }

    g_frame_vp = vp;
    g_frame_vp_valid = !transform_camera_mode;
    g_frame_sw = sw; g_frame_sh = sh;
    g_frame_local_pos = local;
    g_frame_local_valid = has_local_position;
    g_frame_publish_fail_streak = 0; // this frame is healthy

    // Fallback camera basis straight from the view matrix (rows: right, up,
    // -forward). Kept separate from g_cam_* — the pose path stays authoritative
    // for the aimbot; this one only feeds the farm when the pose read fails.
    g_frame_cam_basis_valid = false;
    if (!transform_camera_mode) {
        Vec3 vr = {mat_get(view, 0, 0), mat_get(view, 0, 1), mat_get(view, 0, 2)};
        Vec3 vu = {mat_get(view, 1, 0), mat_get(view, 1, 1), mat_get(view, 1, 2)};
        Vec3 vf = {-mat_get(view, 2, 0), -mat_get(view, 2, 1), -mat_get(view, 2, 2)};
        Vec3 vpos{};
        if (vec3_is_finite(vr) && vec3_is_finite(vu) && vec3_is_finite(vf) &&
            camera_position_from_view(view, vpos)) {
            float fl = sqrtf(vf.x * vf.x + vf.y * vf.y + vf.z * vf.z);
            if (fl > 0.5F && fl < 2.0F) {
                g_frame_cam_pos = vpos;
                g_frame_cam_fwd = {vf.x / fl, vf.y / fl, vf.z / fl};
                g_frame_cam_right = vr;
                g_frame_cam_up = vu;
                g_frame_cam_basis_valid = true;
            }
        }
    }


    Vec3 transform_camera_position{};
    Vec4 transform_camera_rotation{};
    if (transform_camera_mode) {
        if (local_entity_index >= s_transforms.size() || !read_entity_pose(s_transforms[local_entity_index], transform_camera_position, transform_camera_rotation)) {
            g_player_position_validated = false;
            return result;
        }
        local = transform_camera_position; has_local_position = true;
    }

    // Our own team / clan, read once per frame and compared against every
    // player below. Without it nobody can be an ally.
    PlayerGroup local_group;
    bool local_group_valid = false;
    if (local_entity_index < s_transforms.size()) {
        read_player_group(s_transforms[local_entity_index], local_group);
        local_group_valid = local_group.any();
    }

    // Firing reference for the aimbot (local look direction + eye point).
    {
        uint64_t local_player = (local_entity_index < s_transforms.size()) ? s_transforms[local_entity_index] : 0;
        const PlayerAux* local_aux = nullptr;
        bool local_crouched = false;
        if (local_player && g_aim_bones_requested) {
            PlayerAux& la = player_aux(local_player);
            local_aux = &la;
            local_crouched = player_is_crouched(la);
        }
        read_local_aim_reference(local_player, local_aux, local_crouched);
    }

    // One box per player: of several objects carrying the same userID (the
    // copy left behind when he mounted a vehicle, or a respawn leftover) only
    // the one that moved most recently is drawn. The previous winner keeps the
    // spot on a tie, otherwise a parked car would make the box flip about.
    std::vector<char> suppressed(s_transforms.size(), 0);
    {
        std::unordered_map<std::string, size_t> chosen;
        for (size_t index = 0; index < s_transforms.size(); ++index) {
            if (!position_ok[index]) continue;
            auto tracked = g_player_track.find(s_transforms[index]);
            if (tracked == g_player_track.end()) continue;
            std::string uid(tracked->second.uid);
            if (uid.empty()) {
                // No userID on this object: the display name identifies the
                // account just as well, and it is already cached.
                auto text = g_player_text.find(s_transforms[index]);
                if (text != g_player_text.end() && text->second.has_name && text->second.name[0])
                    uid.assign(text->second.name, strnlen(text->second.name, sizeof(text->second.name)));
            }
            if (uid.empty()) continue;
            auto found = chosen.find(uid);
            if (found == chosen.end()) { chosen.emplace(uid, index); continue; }
            size_t rival = found->second;
            int mine = tracked->second.still_frames;
            int theirs = g_player_track[s_transforms[rival]].still_frames;
            bool take_mine;
            if (index == local_entity_index)      take_mine = true;
            else if (rival == local_entity_index) take_mine = false;
            else if (mine != theirs)              take_mine = mine < theirs;
            else {
                auto sticky = g_player_track_pick.find(uid);
                take_mine = sticky != g_player_track_pick.end() && sticky->second == s_transforms[index];
            }
            suppressed[take_mine ? rival : index] = 1;
            if (take_mine) found->second = index;
        }
        for (const auto& entry : chosen) g_player_track_pick[entry.first] = s_transforms[entry.second];
    }

    for (size_t i = 0; i < s_transforms.size(); ++i) {
        if (i == local_entity_index) continue;
        if (!s_transforms[i]) continue;
        if (suppressed[i]) continue;
        if (!position_ok[i]) continue;
        Vec3 feet = positions[i];

        float distance = -1.0F;
        if (has_local_position) {
            float dx = feet.x - local.x, dy = feet.y - local.y, dz = feet.z - local.z;
            distance = sqrtf(dx * dx + dy * dy + dz * dz);
            if (!std::isfinite(distance) || distance < MIN_PLAYER_DISTANCE || distance > MAX_PLAYER_DISTANCE) continue;
        }

        // Counted before any screen-space checks: the pill counter must see
        // players behind us too (360 degrees), not only the ones on screen.
        ++g_frame_player_count;

        // Crouch-aware body height from the character controller.
        PlayerAux& aux = player_aux(s_transforms[i]);
        const bool crouched = player_is_crouched(aux);
        float body_height = crouched ? aux.crouch_height : aux.normal_height;
        if (!(body_height > 0.6F && body_height < 2.6F)) body_height = PLAYER_HEIGHT;

        Vec3 body_bottom = {feet.x, feet.y, feet.z};
        Vec3 body_top = {feet.x, feet.y + body_height, feet.z};
        if (transform_camera_mode || !g_use_direct_player_position) { body_bottom.y = feet.y - 1.60F; body_top.y = feet.y + 0.20F; }

        Vec2 sf{}, sh2{};
        bool bottom_visible = transform_camera_mode
            ? w2s_transform_camera(transform_camera_position, transform_camera_rotation, body_bottom, sw, sh, sf, false)
            : w2s(vp, body_bottom, sw, sh, sf, false);
        if (!bottom_visible) continue;
        bool top_visible = transform_camera_mode
            ? w2s_transform_camera(transform_camera_position, transform_camera_rotation, body_top, sw, sh, sh2, false)
            : w2s(vp, body_top, sw, sh, sh2, false);
        if (!top_visible) continue;

        float height = fabsf(sh2.y - sf.y);
        if (!std::isfinite(height) || height < 2.0F) continue;
        float cx = (sf.x + sh2.x) * 0.5F;
        float cy = (sf.y + sh2.y) * 0.5F;
        float half_w = height * PLAYER_BOX_WIDTH_RATIO * 0.5F;
        float half_h = height * 0.5F;

        constexpr float box_half_width = 0.35F, box_half_depth = 0.35F;
        const Vec3 world_corners[8] = {
            {feet.x - box_half_width, body_bottom.y, feet.z - box_half_depth},
            {feet.x + box_half_width, body_bottom.y, feet.z - box_half_depth},
            {feet.x + box_half_width, body_bottom.y, feet.z + box_half_depth},
            {feet.x - box_half_width, body_bottom.y, feet.z + box_half_depth},
            {feet.x - box_half_width, body_top.y, feet.z - box_half_depth},
            {feet.x + box_half_width, body_top.y, feet.z - box_half_depth},
            {feet.x + box_half_width, body_top.y, feet.z + box_half_depth},
            {feet.x - box_half_width, body_top.y, feet.z + box_half_depth}
        };
        EspBox box{};
        // Lead for this player, used by every set_aim_point call below.
        {
            auto vt = g_player_track.find(s_transforms[i]);
            if (vt != g_player_track.end()) {
                g_aim_lead.x = vt->second.vel.x * kAimLeadSeconds;
                g_aim_lead.y = vt->second.vel.y * kAimLeadSeconds;
                g_aim_lead.z = vt->second.vel.z * kAimLeadSeconds;
            } else {
                g_aim_lead = {};
            }
        }
        box.id = s_transforms[i];
        box.crouched = crouched;
        box.aim_source = 0;
        box.x1 = cx - half_w; box.y1 = cy - half_h;
        box.x2 = cx + half_w; box.y2 = cy + half_h;
        box.distance = distance;
        // Display strings (cached, update-on-success so labels never flicker).
        {
            PlayerTextCache& tc = g_player_text[s_transforms[i]];
            if (++tc.revalidate >= 30) {
                tc.revalidate = 0;
                char tmp[32] = {};
                if (player_display_name(s_transforms[i], tmp, sizeof(tmp))) {
                    memcpy(tc.name, tmp, sizeof(tc.name));
                    tc.has_name = true;
                }
                char weapon_tmp[48] = {};
                bool weapon_definite = false;
                if (player_weapon_name(s_transforms[i], weapon_tmp, sizeof(weapon_tmp), weapon_definite)) {
                    memcpy(tc.weapon, weapon_tmp, sizeof(tc.weapon));
                    tc.has_weapon = true;
                } else if (weapon_definite) {
                    // The synced weapon slot says the hands are empty — drop the
                    // stale label instead of showing the previous weapon forever.
                    tc.weapon[0] = '\0';
                    tc.has_weapon = false;
                }
                PlayerGroup group;
                read_player_group(s_transforms[i], group);
                tc.ally = local_group_valid && groups_are_allied(local_group, group);
                snprintf(tc.tag, sizeof(tc.tag), "%s", group.tag);
                tc.has_tag = tc.tag[0] != '\0';
            }
            box.has_name = tc.has_name;
            if (tc.has_name) memcpy(box.name, tc.name, sizeof(box.name));
            box.has_weapon = tc.has_weapon;
            if (tc.has_weapon) memcpy(box.weapon, tc.weapon, sizeof(box.weapon));
            box.ally = tc.ally;
            box.has_tag = tc.has_tag;
            if (tc.has_tag) memcpy(box.tag, tc.tag, sizeof(box.tag));
        }
        for (size_t corner = 0; corner < 8; ++corner) {
            Vec2 sc{};
            bool projected = transform_camera_mode
                ? w2s_transform_camera(transform_camera_position, transform_camera_rotation, world_corners[corner], sw, sh, sc, false)
                : w2s(vp, world_corners[corner], sw, sh, sc, false);
            box.corner_visible[corner] = projected && sc.x >= 0.0F && sc.x <= sw && sc.y >= 0.0F && sc.y <= sh;
            box.corners[corner][0] = projected ? sc.x : -1.0F;
            box.corners[corner][1] = projected ? sc.y : -1.0F;
        }
        if (want_bones && !transform_camera_mode)
            fill_skeleton_box(s_transforms[i], vp, sw, sh, box);

        // Head slot: prefer the centre of the server-side Head hit volume over
        // the rig-derived estimate. This is the exact volume the shot is tested
        // against, so it removes the constant model/hitbox offset that makes
        // long-range head shots land low. Only accepted when it lies within
        // the body (plausibility vs feet) so a stale transform cannot hijack it.
        if (g_aim_bones_requested && !transform_camera_mode) {
            Vec3 hb{};
            if (player_head_hitbox_world(aux, hb)) {
                float dy = hb.y - feet.y;
                float dx = hb.x - feet.x, dz = hb.z - feet.z;
                if (dy > 0.5F && dy < 2.4F && (dx * dx + dz * dz) < 1.0F) {
                    bool agree = true;
                    if (box.aim_valid[0]) {
                        // Compare against the rig head point in screen space:
                        // reject if wildly different (different body).
                        Vec2 hs{};
                        if (w2s(vp, hb, sw, sh, hs, false)) {
                            float ex = hs.x - box.aim_pts[0][0], ey = hs.y - box.aim_pts[0][1];
                            float bh = fabsf(box.y2 - box.y1);
                            agree = (ex * ex + ey * ey) < (bh * 0.25F) * (bh * 0.25F) + 4.0F;
                        }
                    }
                    if (agree && set_aim_point(box, 0, hb, vp, sw, sh) && box.aim_source == 0) box.aim_source = 1;
                }
            }
        }

        // Aim fallbacks when rig bones are not (yet) available, so the aimbot
        // always has a crouch-aware target instead of a fixed-height guess.
        if (g_aim_bones_requested && !transform_camera_mode &&
            !(box.aim_valid[0] && box.aim_valid[1] && box.aim_valid[2])) {
            Vec3 head{};
            bool have_head = false;
            // (2) game-maintained head transform (moves with crouch/animation)
            if (player_head_world(aux, head)) {
                float dy = head.y - feet.y;
                have_head = dy > 0.4F && dy < 2.4F;
            }
            const float n_down = 0.12F;                    // head -> neck
            const float c_down = crouched ? 0.26F : 0.34F; // head -> chest
            if (have_head) {
                if (!box.aim_valid[0]) { Vec3 t = head; t.y += 0.03F; if (set_aim_point(box, 0, t, vp, sw, sh) && box.aim_source == 0) box.aim_source = 2; }
                if (!box.aim_valid[1]) { Vec3 t = head; t.y -= n_down; if (set_aim_point(box, 1, t, vp, sw, sh) && box.aim_source == 0) box.aim_source = 2; }
                if (!box.aim_valid[2]) { Vec3 t = head; t.y -= c_down; if (set_aim_point(box, 2, t, vp, sw, sh) && box.aim_source == 0) box.aim_source = 2; }
            }
            // (3) feet + pose height estimate
            if (!box.aim_valid[0]) { Vec3 t = feet; t.y += body_height - 0.12F; if (set_aim_point(box, 0, t, vp, sw, sh) && box.aim_source == 0) box.aim_source = 3; }
            if (!box.aim_valid[1]) { Vec3 t = feet; t.y += body_height - 0.26F; if (set_aim_point(box, 1, t, vp, sw, sh) && box.aim_source == 0) box.aim_source = 3; }
            if (!box.aim_valid[2]) { Vec3 t = feet; t.y += body_height * 0.72F; if (set_aim_point(box, 2, t, vp, sw, sh) && box.aim_source == 0) box.aim_source = 3; }
        }
        result.push_back(box);
    }

    return result;
}

int esp_nearby_player_count() { return g_frame_player_count; }


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
static int g_marker_rescan_countdown = 0;
static uint64_t g_network_client_class = 0;
static uint64_t g_network_identity_class = 0;

void esp_set_markers_enabled(bool ore, bool animals, bool loot, bool pickups) {
    // Loot containers and ground pickups are filtered out during the registry
    // walk, so switching a category on has to invalidate the cached list.
    if (loot != g_markers_loot_enabled || pickups != g_markers_pickup_enabled)
        g_marker_rescan_countdown = 0;
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
static std::unordered_map<uint64_t, uint8_t> g_marker_class_kind;

// ---- Auto-farm state ---------------------------------------------------------
// The farm has its own entity cache (it wants trees, which the ore markers
// deliberately skip) and its own rescan cadence. Positions and loot are read
// the same way the markers read them; the walking/hitting itself is done with
// synthetic touches in main.cpp.
struct FarmEntity {
    uint64_t identity = 0;      // NetworkIdentity (stable id for the blacklist)
    uint64_t component = 0;     // the Mineable* component (fraction reads)
    uint64_t transform = 0;     // native Transform (position)
    Vec3     pos{};
    bool     pos_valid = false;
    int      kind = 0;          // 0 wood, 1 stone, 2 metal, 3 sulfur
};
static std::vector<FarmEntity> g_farm_entities;
static std::unordered_map<uint64_t, int> g_farm_blacklist; // identity -> frames left
static int g_farm_rescan = 0;
// Why the picker returned nothing (surfaced in the menu status line):
// 0 ok, 1 off, 2 frame not published, 3 no nodes in registry, 4 none in
// range, 5 camera pose unreadable.
static int g_farm_idle_reason = 1;

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
static const MarkerLook kBarrel    {ESP_MARKER_LOOT, "Скрап",  false, false, {255, 255, 255}};
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
        // while an all-caps run ("WOLF") stays a single word.
        bool hump = letter && c >= 'A' && c <= 'Z' && n > 0 && prev >= 'a' && prev <= 'z';
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
        {"rat", "Крыса"},      {"rats", "Крыса"},     {"mouse", "Крыса"},
        {"bear", "Медведь"},   {"boar", "Кабан"},     {"pig", "Кабан"},
        {"deer", "Олень"},     {"stag", "Олень"},     {"rabbit", "Кролик"},
        {"hare", "Заяц"},      {"chicken", "Курица"}, {"hen", "Курица"},
        {"fish", "Рыба"},      {"shark", "Акула"},    {"cannibal", "Каннибал"},
        {"horse", "Лошадь"},   {"goat", "Коза"},      {"sheep", "Овца"},
        {"cow", "Корова"},     {"fox", "Лиса"},       {"snake", "Змея"},
    };
    return for_each_name_token(raw, [&](const char* word) {
        for (const auto& animal : kAnimals) {
            if (strcmp(word, animal.word) == 0) { look = animal_look(animal.ru); return true; }
        }
        return false;
    });
}

// Same safety net as the animals: some barrels come with an empty entityType,
// and their prefab is the only thing that still says "barrel".
static bool barrel_look_from_object_name(const char* raw, MarkerLook& look) {
    return for_each_name_token(raw, [&](const char* word) {
        if (strcmp(word, "barrel") == 0 || strcmp(word, "barrels") == 0) {
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
static int read_managed_collection(uint64_t object, uint64_t* out, int max_items) {
    if (!valid_obj(object) || !out || max_items <= 0) return 0;
    uint64_t klass = rd_ptr(object);
    if (!valid_obj(klass)) return 0;
    uint64_t array = object;
    int32_t count = 0;
    if (read_remote_string(rd_ptr(klass + IL2CPP_CLASS_NAME)).rfind("List`1", 0) == 0) {
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
    int32_t amount = rd<int32_t>(component + ITEMPICKUP_AMOUNT);
    if (amount > 1 && amount < 1000000)
        snprintf(label, label_cap, "%s x%d", translated, (int)amount);
    else
        snprintf(label, label_cap, "%s", translated);
    return label[0] != '\0';
}

// Il2CppClass -> which of the two component families this is (cached: the same
// handful of classes come back for every entity in the registry).
enum MarkerClass : uint8_t {
    MARKER_CLASS_NONE = 0, MARKER_CLASS_MINEABLE = 1,
    MARKER_CLASS_LOOT = 2, MARKER_CLASS_PICKUP = 3,
};

static uint8_t marker_class_of(uint64_t klass) {
    if (!valid_obj(klass)) return MARKER_CLASS_NONE;
    auto found = g_marker_class_kind.find(klass);
    if (found != g_marker_class_kind.end()) return found->second;
    std::string name = read_remote_string(rd_ptr(klass + IL2CPP_CLASS_NAME));
    uint8_t kind = MARKER_CLASS_NONE;
    if (name.rfind("Mineable", 0) == 0)                       kind = MARKER_CLASS_MINEABLE;
    else if (name == "LootObject" || name == "PumpkinTrick")  kind = MARKER_CLASS_LOOT;
    else if (name == "ItemPickup")                            kind = MARKER_CLASS_PICKUP;
    if (g_marker_class_kind.size() < 512) g_marker_class_kind[klass] = kind;
    return kind;
}

static uint64_t resolve_network_client_spawned() {
    if (!g_network_client_class && NETWORK_CLIENT_TYPEINFO_RVA != 0) {
        uint64_t candidate = rd_ptr(g_il2cpp_base + NETWORK_CLIENT_TYPEINFO_RVA);
        if (valid_obj(candidate)) {
            std::string name = read_remote_string(rd_ptr(candidate + IL2CPP_CLASS_NAME));
            std::string ns   = read_remote_string(rd_ptr(candidate + IL2CPP_CLASS_NAMESPACE));
            if (name == "NetworkClient" && ns == "Mirror") g_network_client_class = candidate;
        }
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
static uint64_t resolve_network_identity_class() {
    if (g_network_identity_class) return g_network_identity_class;
    if (!g_game_controller_class) return 0;
    uint64_t statics = get_class_static_fields(g_game_controller_class);
    if (!statics) return 0;
    uint64_t identity = rd_ptr(statics + GAME_CONTROLLER_NET_IDENTITY_FIELD);
    if (!valid_obj(identity)) return 0;
    uint64_t klass = rd_ptr(identity);
    if (!valid_obj(klass)) return 0;
    std::string name = read_remote_string(rd_ptr(klass + IL2CPP_CLASS_NAME));
    if (name != "NetworkIdentity") return 0;
    g_network_identity_class = klass;
    return klass;
}

static bool marker_world_position(uint64_t transform, Vec3& out) {
    if (!transform) return false;
    if (g_skeleton_layout_valid && read_transform_hierarchy_layout(transform, g_skeleton_layout, out))
        return vec3_is_finite(out);
    return read_transform_hierarchy_position(transform, out) && vec3_is_finite(out);
}

// Walk Mirror's client registry and cache every ore node / animal in it.
static void rebuild_marker_entities() {
    g_marker_entities.clear();
    g_marker_trace_dict = -1;

    uint64_t dictionary = resolve_network_client_spawned();
    if (!dictionary) { g_marker_trace_dict = -2; return; }
    // Needed to read prefab names (wolves / rats); harmless if it fails, the
    // loot and entityType paths still work.
    if (!g_go_name_offset_valid && g_local_player) ensure_gameobject_name_offset(g_local_player);
    uint64_t identity_class = resolve_network_identity_class();

    uint64_t entries = rd_ptr(dictionary + DICT_ENTRIES);
    int32_t count = rd<int32_t>(dictionary + DICT_COUNT);
    if (!valid_obj(entries) || count <= 0) { g_marker_trace_dict = -3; return; }
    if (count > 4096) count = 4096;
    g_marker_trace_dict = count;

    // One bulk read for the whole entry array instead of one read per entry.
    std::vector<uint8_t> buffer((size_t)count * DICT_ENTRY_STRIDE);
    if (!rd_buf(entries + IL2CPP_ARRAY_FIRST_ELEMENT, buffer.data(), buffer.size())) { g_marker_trace_dict = -4; return; }

    uint64_t behaviours[32];
    for (int32_t i = 0; i < count; ++i) {
        uint64_t identity = 0;
        memcpy(&identity, buffer.data() + (size_t)i * DICT_ENTRY_STRIDE + DICT_ENTRY_VALUE, sizeof(identity));
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

            MarkerLook look;
            char pickup_text[40] = {};
            char object_name[48] = {}, root_name[48] = {};
            managed_component_gameobject_name(component, object_name, sizeof(object_name));
            managed_component_gameobject_name(identity, root_name, sizeof(root_name));
            bool known = false;
            if (component_class == MARKER_CLASS_PICKUP) {
                if (!g_markers_pickup_enabled) continue;
                known = pickup_marker(component, pickup_text, sizeof(pickup_text));
                if (known) {
                    look.kind = ESP_MARKER_PICKUP;
                    look.label = nullptr;
                    look.has_color = false;
                }
            } else if (component_class == MARKER_CLASS_LOOT) {
                if (!g_markers_loot_enabled) continue;
                known = loot_marker(component, object_name, root_name, look);
            } else {
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
            if (entity.transform) g_marker_entities.push_back(entity);
            if (g_marker_entities.size() >= 512) break;
        }
        if (g_marker_entities.size() >= 512) break;
    }
}

static void reset_marker_caches() {
    g_marker_entities.clear();
    g_marker_rescan_countdown = 0;
    g_marker_class_kind.clear();
    g_network_client_class = 0;
    g_network_identity_class = 0;
    // Farm entities come from the same registry: stale pointers must not
    // survive a world reload either.
    g_farm_entities.clear();
    g_farm_blacklist.clear();
    g_farm_rescan = 0;
}

std::vector<EspMarker> esp_get_markers() {
    std::vector<EspMarker> result;
    if (!g_markers_ore_enabled && !g_markers_animal_enabled &&
        !g_markers_loot_enabled && !g_markers_pickup_enabled) {
        if (!g_marker_entities.empty()) g_marker_entities.clear();
        g_marker_rescan_countdown = 0;
        return result;
    }
    if (g_pid <= 0 || !g_il2cpp_base) { g_marker_trace_step = 1; return result; }
    // The box pipeline publishes the frame while players are visible; when it
    // bailed out for ANY reason (empty player list, failed position read,
    // world reload), build a camera-only frame right here. Markers must never
    // depend on other players being around.
    if (!g_frame_vp_valid || !g_frame_local_valid) {
        if (!publish_camera_only_frame(g_last_overlay_sw, g_last_overlay_sh))
            return result; // trace step выставлен внутри
    }

    if (--g_marker_rescan_countdown <= 0) {
        rebuild_marker_entities();
        // ~3 s between scans: entities spawn and despawn slowly. An empty
        // result means the registry was not readable (world still loading in
        // after a respawn), so retry in half a second instead.
        g_marker_rescan_countdown = g_marker_entities.empty() ? 30 : 180;
        g_marker_trace_scan = (int)g_marker_entities.size();
    }
    g_marker_trace_step = g_marker_entities.empty() ? 2 : 0; // 2 = скан пуст

    const float max_distance = g_marker_max_distance;
    for (MarkerEntity& entity : g_marker_entities) {
        if (entity.kind == ESP_MARKER_ORE && !g_markers_ore_enabled) continue;
        if (entity.kind == ESP_MARKER_ANIMAL && !g_markers_animal_enabled) continue;
        if (entity.kind == ESP_MARKER_LOOT && !g_markers_loot_enabled) continue;
        if (entity.kind == ESP_MARKER_PICKUP && !g_markers_pickup_enabled) continue;
        // Ore nodes never move, so their position is only read on a rescan.
        if (entity.kind == ESP_MARKER_ANIMAL || !entity.position_valid)
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
        snprintf(marker.name, sizeof(marker.name), "%s",
                 entity.label ? entity.label : entity.text);
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


// ============================== Auto-farm ====================================
//
// Finds the nearest tree / stone / iron / sulfur node in Mirror's registry and
// reports where it is relative to the camera. Walking to it and swinging the
// tool is done with synthetic touches in main.cpp; nothing here writes to the
// game. The glowing "X" bonus spot is looked up as a child GameObject of the
// node, so the swings land on it when the game shows one.

// Farm kind from a loot item short name. Rank: richer resource wins (metal and
// sulfur nodes drop stones too). Processed items are filtered like the ore
// markers, so barrels never register.
static int farm_kind_for_item_name(const char* item_name, int& kind) {
    if (!item_name || !item_name[0]) return 0;
    char key[40];
    size_t n = 0;
    for (const char* p = item_name; *p && n + 1 < sizeof(key); ++p) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        key[n++] = c;
    }
    key[n] = '\0';
    if (strstr(key, "frag") || strstr(key, "pipe") || strstr(key, "sheet") ||
        strstr(key, "scrap") || strstr(key, "spring") || strstr(key, "gear"))
        return 0;
    if (strstr(key, "sulfur")) { kind = 3; return 4; }
    if (strstr(key, "metal"))  { kind = 2; return 3; }
    if (strstr(key, "stone"))  { kind = 1; return 2; }
    if (strstr(key, "wood"))   { kind = 0; return 1; }
    return 0;
}

// Same two loot lists the markers read, but wood ranks too (trees are the
// whole point here, while the markers skip them to keep the screen clean).
static bool farm_kind_from_loot(uint64_t mineable, int& kind) {
    const uint64_t sources[2] = {MINEABLE_LOOT, MINEABLE_FINISH_BONUS};
    int best = 0;
    for (uint64_t source : sources) {
        uint64_t items[12];
        int count = read_managed_collection(rd_ptr(mineable + source), items, 12);
        for (int i = 0; i < count; ++i) {
            if (!valid_obj(items[i])) continue;
            char name[32] = {};
            if (!read_managed_string(rd_ptr(items[i] + LOOTITEM_ITEM_NAME), name, sizeof(name))) continue;
            int candidate = 0;
            int rank = farm_kind_for_item_name(name, candidate);
            if (rank > best) { best = rank; kind = candidate; }
            if (best == 4) return true;
        }
    }
    return best > 0;
}

static void rebuild_farm_entities() {
    g_farm_entities.clear();

    uint64_t dictionary = resolve_network_client_spawned();
    if (!dictionary) return;
    if (!g_go_name_offset_valid && g_local_player) ensure_gameobject_name_offset(g_local_player);
    uint64_t identity_class = resolve_network_identity_class();

    uint64_t entries = rd_ptr(dictionary + DICT_ENTRIES);
    int32_t count = rd<int32_t>(dictionary + DICT_COUNT);
    if (!valid_obj(entries) || count <= 0) return;
    if (count > 4096) count = 4096;

    std::vector<uint8_t> buffer((size_t)count * DICT_ENTRY_STRIDE);
    if (!rd_buf(entries + IL2CPP_ARRAY_FIRST_ELEMENT, buffer.data(), buffer.size())) return;

    uint64_t behaviours[32];
    for (int32_t i = 0; i < count; ++i) {
        uint64_t identity = 0;
        memcpy(&identity, buffer.data() + (size_t)i * DICT_ENTRY_STRIDE + DICT_ENTRY_VALUE, sizeof(identity));
        if (!valid_obj(identity)) continue;
        if (identity_class && rd_ptr(identity) != identity_class) continue;
        uint64_t array = rd_ptr(identity + NETID_BEHAVIOURS);
        if (!valid_obj(array)) continue;
        int32_t behaviour_count = rd<int32_t>(array + IL2CPP_ARRAY_LENGTH);
        if (behaviour_count <= 0) continue;
        if (behaviour_count > 32) behaviour_count = 32;
        if (!rd_buf(array + IL2CPP_ARRAY_FIRST_ELEMENT, behaviours, (size_t)behaviour_count * sizeof(uint64_t)))
            continue;

        for (int32_t b = 0; b < behaviour_count; ++b) {
            uint64_t component = behaviours[b];
            if (!valid_obj(component)) continue;
            if (marker_class_of(rd_ptr(component)) != MARKER_CLASS_MINEABLE) continue;

            // Kind: the entityType enum first (cheap and exact), loot second.
            int kind = -1;
            switch ((MineableEntityType)rd<int32_t>(component + MINEABLE_ENTITY_TYPE)) {
                case MineableEntityType::Tree:   kind = 0; break;
                case MineableEntityType::Stone:  kind = 1; break;
                case MineableEntityType::Iron:   kind = 2; break;
                case MineableEntityType::Sulfur: kind = 3; break;
                default: break;
            }
            if (kind < 0 && !farm_kind_from_loot(component, kind)) continue;
            if (!(g_farm_mask & (1u << kind))) continue;

            FarmEntity entity;
            entity.identity = identity;
            entity.component = component;
            entity.kind = kind;
            entity.transform = native_component_transform(managed_object_native(component));
            if (!entity.transform)
                entity.transform = native_component_transform(managed_object_native(identity));
            if (!entity.transform) continue;

            // Fallen logs register as "Tree" but cannot be chopped the same
            // way — the bot just circles them. Filter them out by prefab
            // name (log / fallen / dead / driftwood variants).
            if (kind == 0 && g_go_name_offset_valid) {
                char go_name[48];
                if (read_transform_name(entity.transform, go_name, sizeof(go_name))) {
                    for (char* p = go_name; *p; ++p)
                        if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');
                    if (strstr(go_name, "log") || strstr(go_name, "fallen") ||
                        strstr(go_name, "dead") || strstr(go_name, "driftwood") ||
                        strstr(go_name, "stump"))
                        continue;
                }
            }

            entity.pos_valid = marker_world_position(entity.transform, entity.pos);
            g_farm_entities.push_back(entity);
            if (g_farm_entities.size() >= 512) break;
        }
        if (g_farm_entities.size() >= 512) break;
    }
}

void esp_farm_set_resources(unsigned mask) {
    if (g_farm_mask != mask) {
        g_farm_mask = mask;
        g_farm_entities.clear();
        g_farm_rescan = 0;
    }
    if (!mask) g_farm_blacklist.clear();
}

// Search radius for farm nodes, metres. Set from the menu slider.
static float g_farm_max_distance = 100.0F;

void esp_farm_set_range(float meters) {
    if (!std::isfinite(meters)) return;
    if (meters < 10.0F) meters = 10.0F;
    if (meters > 300.0F) meters = 300.0F;
    g_farm_max_distance = meters;
}

void esp_farm_blacklist(unsigned long long id, float seconds) {
    if (!id) return;
    int frames = (int)(seconds * 60.0F);
    if (frames < 60) frames = 60;
    g_farm_blacklist[(uint64_t)id] = frames;
    // Drop it from the cache right away and rescan soon: keeping a dead
    // node around until the next 2 s rescan is what made the bot jerk the
    // camera at garbage coordinates after finishing a tree.
    for (auto it = g_farm_entities.begin(); it != g_farm_entities.end(); ++it) {
        if (it->identity == (uint64_t)id) { g_farm_entities.erase(it); break; }
    }
    if (g_farm_rescan > 15) g_farm_rescan = 15;
}

// The glowing bonus "X" is a child GameObject of the node. Search the subtree
// for a name that looks like it AND that sits visibly away from the node
// pivot: every prefab also carries a dormant template child parked exactly at
// the pivot (tree base) until the real X activates, and returning that one
// made the bot chop the bottom of the trunk. The result is cached per
// component and re-checked because the spot jumps around between hits.
static uint64_t farm_find_spot(uint64_t node_transform, const Vec3& node_pos) {
    if (!node_transform) return 0;
    std::vector<uint64_t> nodes;
    // Trees carry a LOT of children (LODs, foliage, colliders) — a small cap
    // used to cut the walk off before it ever reached the X child.
    collect_transform_subtree(node_transform, nodes, 256);

    // Positions of the whole subtree, read once: the name pass scores with
    // them and the movement pass compares them against the previous scan.
    struct SpotCand { uint64_t node; Vec3 pos; };
    std::vector<SpotCand> live;
    live.reserve(nodes.size());
    for (uint64_t node : nodes) {
        if (node == node_transform) continue;
        Vec3 p{};
        if (!marker_world_position(node, p) || !vec3_is_finite(p)) continue;
        live.push_back({node, p});
    }

    auto plausible = [&](const Vec3& p) -> bool {
        float sx = p.x - node_pos.x, sy = p.y - node_pos.y, sz = p.z - node_pos.z;
        float d2 = sx * sx + sy * sy + sz * sz;
        if (d2 <= 0.35F * 0.35F || d2 >= 6.0F * 6.0F) return false; // at pivot / off the node
        return sy > 0.2F && sy < 2.8F;                              // swingable height
    };

    // Pass 1: by name (rock prefabs name their X clearly).
    uint64_t best_node = 0;
    if (g_go_name_offset_valid) {
        char name[48];
        int best_score = -1;
        float best_d2 = 0.0F;
        for (const SpotCand& cand : live) {
            if (!read_transform_name(cand.node, name, sizeof(name))) continue;
            size_t len = 0;
            for (char* p = name; *p; ++p, ++len) if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');
            bool looks = strstr(name, "spot") || strstr(name, "bonus") || strstr(name, "cross") ||
                         strstr(name, "weak") || strstr(name, "sweet") || strstr(name, "crit") ||
                         strstr(name, "marker") || strstr(name, "gather") || strstr(name, "target") ||
                         strstr(name, "hitpoint") || strstr(name, "plus") ||
                         (name[0] == 'x' && (len == 1 || name[1] == ' ' || name[1] == '_' || name[1] == '(' ||
                                             (name[1] >= '0' && name[1] <= '9')));
            if (!looks) continue;
            if (!plausible(cand.pos)) continue; // dormant template at the pivot etc.
            float sx = cand.pos.x - node_pos.x, sy = cand.pos.y - node_pos.y, sz = cand.pos.z - node_pos.z;
            float d2 = sx * sx + sy * sy + sz * sz;
            int score = 2;
            if (sx * sx + sz * sz > 0.04F) score += 1; // off the trunk axis
            if (score > best_score || (score == best_score && d2 > best_d2)) {
                best_score = score;
                best_d2 = d2;
                best_node = cand.node;
            }
        }
    }

    // Pass 2: by movement. Tree prefabs do not name their X anything
    // recognisable, but the X is the only child that JUMPS between hits —
    // LODs, colliders and foliage transforms never move. Compare against the
    // previous scan of the same node and take the biggest plausible jump.
    static uint64_t s_move_root = 0;
    static std::vector<SpotCand> s_move_prev;
    if (!best_node && s_move_root == node_transform) {
        float best_m2 = 0.15F * 0.15F; // ignore sub-15 cm jitter
        for (const SpotCand& cand : live) {
            for (const SpotCand& prev : s_move_prev) {
                if (prev.node != cand.node) continue;
                float mx = cand.pos.x - prev.pos.x;
                float my = cand.pos.y - prev.pos.y;
                float mz = cand.pos.z - prev.pos.z;
                float m2 = mx * mx + my * my + mz * mz;
                if (m2 > best_m2 && m2 < 5.0F * 5.0F && plausible(cand.pos)) {
                    best_m2 = m2;
                    best_node = cand.node;
                }
                break;
            }
        }
    }
    s_move_root = node_transform;
    s_move_prev = std::move(live);
    return best_node;
}

void esp_farm_debug(int& nodes_cached, int& idle_reason) {
    nodes_cached = (int)g_farm_entities.size();
    idle_reason = g_farm_idle_reason;
}

bool esp_farm_get_target(FarmTarget& out) {
    out = FarmTarget{};
    if (!g_farm_mask) { g_farm_idle_reason = 1; return false; }
    if (g_pid <= 0 || !g_il2cpp_base) { g_farm_idle_reason = 2; return false; }
    // Same self-repair as the markers: if the box pipeline did not publish a
    // frame (empty player list etc.), build one straight from the camera.
    if (!g_frame_local_valid || !g_frame_vp_valid) {
        if (!publish_camera_only_frame(g_last_overlay_sw, g_last_overlay_sh)) {
            g_farm_idle_reason = 2;
            return false;
        }
    }

    // Blacklist bookkeeping (called once per frame from the controller).
    for (auto it = g_farm_blacklist.begin(); it != g_farm_blacklist.end();) {
        if (--(it->second) <= 0) it = g_farm_blacklist.erase(it);
        else ++it;
    }

    if (--g_farm_rescan <= 0) {
        rebuild_farm_entities();
        g_farm_rescan = g_farm_entities.empty() ? 30 : 120; // ~0.5 s / ~2 s
    }

    // Sticky target: keep working the node we already picked while it is
    // alive, otherwise the controller would flip between equidistant nodes.
    static uint64_t s_last_identity = 0;

    const float kMaxFarmDistance = g_farm_max_distance;
    const FarmEntity* best = nullptr;
    float best_score = 1e18F;
    float best_dist = 0.0F;
    for (const FarmEntity& entity : g_farm_entities) {
        if (!(g_farm_mask & (1u << entity.kind))) continue;
        if (!entity.pos_valid) continue;
        if (g_farm_blacklist.count(entity.identity)) continue;

        // Horizontal distance (Unity: Y up). The controller compares this
        // against melee reach, and a tall tree's pivot sits metres up the
        // trunk — 3D distance never drops below the threshold there, which
        // had the bot circling thin trees forever and walking face-first
        // into nodes it was already standing at.
        float dx = entity.pos.x - g_frame_local_pos.x;
        float dy = entity.pos.y - g_frame_local_pos.y;
        float dz = entity.pos.z - g_frame_local_pos.z;
        float dist = sqrtf(dx * dx + dz * dz);
        // Still reject nodes on a different vertical level (cliff above/below).
        if (!std::isfinite(dist) || dist > kMaxFarmDistance) continue;
        if (!std::isfinite(dy) || fabsf(dy) > 30.0F) continue;

        float score = dist;
        // Nodes that look mined out go to the back of the queue instead of
        // being skipped outright: the exact meaning of fractionRemaining is
        // not certain on every build, and a wrong guess here would make the
        // farm ignore every node ("nothing happens"). If the pick is wrong
        // the mining watchdog blacklists it within seconds anyway.
        float fraction = rd<float>(entity.component + MINEABLE_FRACTION);
        if (std::isfinite(fraction) && fraction >= 0.0F && fraction <= 1.001F && fraction < 0.03F)
            score += 1000.0F;
        if (entity.identity == s_last_identity) score *= 0.6F; // stickiness
        if (score < best_score) { best_score = score; best = &entity; best_dist = dist; }
    }
    if (!best) {
        s_last_identity = 0;
        g_farm_idle_reason = g_farm_entities.empty() ? 3 : 4;
        return false;
    }
    s_last_identity = best->identity;

    // Where to look: the glowing spot when the node shows one, otherwise the
    // body of the node (trees are hit at chest height, rocks a bit lower).
    static uint64_t s_spot_component = 0;   // whose spot the cache belongs to
    static uint64_t s_spot_transform = 0;
    static int      s_spot_recheck = 0;
    if (s_spot_component != best->component) {
        s_spot_component = best->component;
        s_spot_transform = 0;
        s_spot_recheck = 0;
    }
    if (--s_spot_recheck <= 0) {
        // The spot only spawns after the first hit and jumps around between
        // hits, so keep the search hot: ~0.3 s while no live spot is known,
        // ~0.7 s to re-validate a known one. IMPORTANT: a scan that finds
        // nothing must NOT clear a known spot — the movement detector only
        // sees the X while it is jumping, and overwriting the cache with 0
        // between jumps was why the bot noticed the cross for half a second
        // and went back to chopping the trunk. The position sanity check
        // below is what retires a spot that actually went bad.
        s_spot_recheck = s_spot_transform ? 40 : 18;
        uint64_t found = farm_find_spot(best->transform, best->pos);
        if (found) s_spot_transform = found;
    }

    Vec3 aim{};
    bool spot_ok = false;
    if (s_spot_transform) {
        Vec3 spot{};
        if (marker_world_position(s_spot_transform, spot) && vec3_is_finite(spot)) {
            // Sanity: the spot must be near its node, else the cached
            // transform went stale (respawned node reuses memory).
            float sx = spot.x - best->pos.x, sy = spot.y - best->pos.y, sz = spot.z - best->pos.z;
            float d2 = sx * sx + sy * sy + sz * sz;
            // ...and it must sit visibly AWAY from the node pivot. A dormant
            // template child rests exactly at the pivot (tree base) until the
            // real X activates — aiming there is the "hits the bottom of the
            // tree" bug. Such a spot is ignored until it moves.
            if (d2 < 6.0F * 6.0F && d2 > 0.35F * 0.35F) { aim = spot; spot_ok = true; }
            else if (d2 >= 6.0F * 6.0F) s_spot_transform = 0;
        }
    }
    if (!spot_ok) {
        aim = best->pos;
        aim.y += (best->kind == 0) ? 1.15F : 0.15F;
        // Height is clamped against the camera eye below, once the camera
        // origin is known — node and player pivots are both unreliable.
    }

    // Full-circle angles from the camera (or firing) axis: unlike
    // aim_angles_for() this must work for nodes behind us, so the forward
    // component may be negative and yaw spans +-180. Order of preference:
    // firing reference > transform pose > basis from this frame's view matrix
    // (the last one exists on devices where the pose read fails — the reason
    // the farm used to sit in "нет позиции камеры").
    if (!g_cam_pose_valid && !g_aim_ref_valid && !g_frame_cam_basis_valid) {
        g_farm_idle_reason = 5;
        return false;
    }
    const bool use_ref = g_aim_ref_valid;
    const bool use_pose = !use_ref && g_cam_pose_valid;
    const Vec3& origin = use_ref ? g_aim_ref_origin  : use_pose ? g_cam_pos     : g_frame_cam_pos;
    const Vec3& fwd    = use_ref ? g_aim_ref_forward : use_pose ? g_cam_forward : g_frame_cam_fwd;
    const Vec3& right  = use_ref ? g_aim_ref_right   : use_pose ? g_cam_right   : g_frame_cam_right;
    const Vec3& up     = use_ref ? g_aim_ref_up      : use_pose ? g_cam_up      : g_frame_cam_up;

    // Body aim (no glowing spot): clamp the aim height against the CAMERA
    // EYE, the only height reference that is reliable on every prefab. Node
    // pivots lie (a rock's pivot rides near its top — that was "hits above
    // the ore"; a tall tree's sits metres up the trunk).
    if (!spot_ok) {
        if (best->kind == 0) {
            // Trees: chest band — slightly below the eye up to eye level.
            float lo = origin.y - 0.9F, hi = origin.y + 0.1F;
            if (aim.y < lo) aim.y = lo;
            if (aim.y > hi) aim.y = hi;
        } else {
            // Ore: knee-to-waist band, clearly below the eye.
            float lo = origin.y - 1.3F, hi = origin.y - 0.55F;
            if (aim.y < lo) aim.y = lo;
            if (aim.y > hi) aim.y = hi;
        }
    }

    Vec3 d = {aim.x - origin.x, aim.y - origin.y, aim.z - origin.z};
    float fx = d.x * fwd.x + d.y * fwd.y + d.z * fwd.z;
    float rx = d.x * right.x + d.y * right.y + d.z * right.z;
    float ux = d.x * up.x + d.y * up.y + d.z * up.z;
    if (!std::isfinite(fx) || !std::isfinite(rx) || !std::isfinite(ux)) { g_farm_idle_reason = 5; return false; }
    constexpr float rad2deg = 57.29577951F;
    float yaw = atan2f(rx, fx) * rad2deg;
    float pitch = atan2f(ux, sqrtf(fx * fx + rx * rx)) * rad2deg;
    if (!std::isfinite(yaw) || !std::isfinite(pitch)) { g_farm_idle_reason = 5; return false; }

    // Screen position of the aim point, for the on-screen target mark.
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

    g_farm_idle_reason = 0;
    out.valid = true;
    out.id = best->identity;
    out.kind = best->kind;
    out.yaw = yaw;
    out.pitch = pitch;
    out.dist = best_dist;
    out.has_spot = spot_ok;
    {
        // Horizontal distance to the aim point itself: the melee-reach check
        // must measure what the pick actually has to hit — a spot on the far
        // side of a boulder is metres further than the node centre.
        float ax = aim.x - g_frame_local_pos.x;
        float az = aim.z - g_frame_local_pos.z;
        float ad = sqrtf(ax * ax + az * az);
        out.aim_dist = std::isfinite(ad) ? ad : best_dist;
    }
    float fraction = rd<float>(best->component + MINEABLE_FRACTION);
    out.fraction = (std::isfinite(fraction) && fraction >= 0.0F && fraction <= 1.001F) ? fraction : -1.0F;
    return true;
}
