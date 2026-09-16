// names.cpp — Имена игроков: что считать настоящим ником.
//
// Модуль разрезан из прежнего монолита jni/src/game.cpp;
// что здесь лежит и кто это зовёт — в шапке names.h и в docs/CODE_MAP.md.

#include "esp/common.h"
#include "esp/boxes.h"
#include "esp/farm_scan.h"
#include "esp/farm_target.h"
#include "esp/frame.h"
#include "esp/il2cpp.h"
#include "esp/managed.h"
#include "esp/marker_labels.h"
#include "esp/markers.h"
#include "esp/melee.h"
#include "esp/mem.h"
#include "esp/weapons.h"
#include "names.h"

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

bool valid_obj(uint64_t p) {
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
    CopyTextUtf8(out, cap, tmp);
    return true;
}

void read_player_group(uint64_t player, PlayerGroup& out) {
    out = PlayerGroup{};
    if (!valid_obj(player)) return;
    read_managed_string_ex(rd_ptr(player + PLAYER_TEAM_NAME), out.team, sizeof(out.team), 39);
    read_managed_string_ex(rd_ptr(player + PLAYER_CLAN_ID),   out.clan, sizeof(out.clan), 47);
    read_managed_string_ex(rd_ptr(player + PLAYER_CLAN_TAG),  out.tag,  sizeof(out.tag),  15);
}

bool groups_are_allied(const PlayerGroup& local, const PlayerGroup& other) {
    if (local.team[0] && strcmp(local.team, other.team) == 0) return true;
    if (local.clan[0] && strcmp(local.clan, other.clan) == 0) return true;
    return false;
}

bool player_display_name(uint64_t player, char* out, size_t cap) {
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
bool read_item_data_display_name(uint64_t item_data, char* out, size_t cap) {
    if (!out || cap < 2) return false;
    out[0] = '\0';
    if (!valid_obj(item_data)) return false;
    char tmp[32] = {};
    uint64_t str = rd_ptr(item_data + ITEMDATA_NAME);
    if (!str || !read_managed_string(str, tmp, sizeof(tmp))) str = rd_ptr(item_data + ITEMDATA_SHORTNAME);
    if (str && read_managed_string(str, tmp, sizeof(tmp)) && tmp[0]) {
        CopyTextUtf8(out, cap, tmp); return true;
    }
    return false;
}

// Held weapon display name. FP state is local-only (MonoBehaviour, never synced),
// so for remote players the FP objects may be missing: after the strict pass
// (FPObject -> player back-reference) a relaxed pass accepts the objects as-is.
bool fp_object_display_name(uint64_t weapon, uint64_t player, bool strict,
                                   char* out, size_t cap) {
    if (!valid_obj(weapon)) return false;
    if (strict && rd_ptr(weapon + FPOBJECT_PLAYER_BACKREF) != player) return false;
    char tmp[32] = {};
    uint64_t str = rd_ptr(weapon + FPOBJECT_OBJECT_NAME);
    if (str && read_managed_string(str, tmp, sizeof(tmp)) && tmp[0]) {
        CopyTextUtf8(out, cap, tmp);
        return true;
    }
    // Fall back to the held item definition (Item -> ItemData m_Name/m_ShortName).
    uint64_t item = rd_ptr(weapon + FPOBJECT_ITEM);
    if (read_item_data_display_name(rd_ptr(item + ITEM_DATA), out, cap))
        return true;
    return false;
}

std::unordered_map<uint64_t, PlayerTextCache> g_player_text;

void prune_player_text(const std::vector<uint64_t>& players) {
    for (auto it = g_player_text.begin(); it != g_player_text.end();) {
        bool present = false;
        for (uint64_t player : players) if (player == it->first) { present = true; break; }
        if (!present) it = g_player_text.erase(it); else ++it;
    }
}
