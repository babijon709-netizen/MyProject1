#include "game.h"
#include "mem_io.h"                // чтение/запись памяти игры: /proc/<pid>/mem + кэш блоков
#include "game_internal.h"        // общий срез состояния/помощников для game_markers/farm.cpp
#include "maps_lookup.h"           // базовый адрес библиотеки по /proc/<pid>/maps
#include "game_offsets_active.h"   // активные оффсеты: релиз или бета (go::SelectBuild)
#include "Vector.h"
#include "logfile.h"     // диагностический лог (Загрузки)
#include "lang.h"      // РУ/EN: подписи визуалов (оружие, предметы, животные)
#include "text_utf8.h"  // копия подписи в буфер без разрезания символа UTF-8

#include <errno.h>     // errno при отказе записи (сайлент: ось выстрела)
#include <string.h>
#include <strings.h>   // strncasecmp (weapon prefab label cleanup)
#include <sys/uio.h>
#include <stdio.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <chrono>
#include <atomic>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <unistd.h>
#include <sys/syscall.h>

// ---- Доступ к памяти игры ---------------------------------------------------
// Подробности — в mem_io.h: только /proc/<pid>/mem (pread/pwrite), проверка
// доступа пробой по ELF-заголовку и кэш блоков на кадр. Здесь только обёртки,
// которыми пользуется весь остальной код чтения.
memio::Reader g_mem;

// адресами без метки тоже нельзя. Снимаем метку сразу, на входе.

// Оффсеты — из активного набора: значения переключаются между релизом и
// бетой в рантайме (см. game_offsets_active.h), имена те же, что были в
// game_offsets, поэтому весь код чтения памяти ниже не изменился.
using namespace go_active;

pid_t     g_pid         = -1;
uint64_t  g_il2cpp_base = 0;
static uint64_t  g_player_manager_class = 0;
static uint64_t  g_player_manager_static_fields = 0;
uint64_t  g_game_controller_class = 0;
uint64_t  g_local_player = 0;
static bool      g_matrix_configuration_validated = false;
static bool      g_camera_matrix_physical_match = false;
static uint64_t  g_player_position_offset = PLAYER_POSITION;


static TransformHierarchyLayout g_transform_hierarchy_layout{};
static bool g_transform_hierarchy_layout_valid = false;

static bool      g_use_direct_player_position = true;
static bool      g_player_position_validated = false;

// Camera state captured by the last esp_get_boxes() call (used by the aimbot
// to convert bone positions into yaw/pitch offsets from the crosshair).
static float     g_cam_fov_deg = 0.0F;
bool      g_cam_pose_valid = false;
// Reserved: was set when the pose had been recovered from the view matrix.
// That path is gone -- deriving the pose from the matrix made the aim throw
// itself across the screen, because the matrix the fallback reads is the
// stale cached one and every angle measured against it lags reality.
static bool      g_cam_pose_derived = false;
Vec3      g_cam_pos{};
Vec3      g_cam_right{}, g_cam_up{}, g_cam_forward{};

// The game fires along PlayerEventHandler.LookDirection (= MouseLook.m_LookRoot
// forward) from the KCC eye point, NOT along the camera transform (which carries
// visual sway/kick on top). Aim angles are therefore measured against this
// reference whenever it can be read, so the aimbot steers the actual firing
// direction onto the target instead of the camera.
bool      g_aim_ref_valid = false;
Vec3      g_aim_ref_origin{};
Vec3      g_aim_ref_forward{}, g_aim_ref_right{}, g_aim_ref_up{};

// Маска ресурсов автофарма (bit0 дерево..bit3 сера).
unsigned g_farm_mask = 0;



// ==== X-ray: камера отсекает всё ближе N метров (near clip plane) ==========
// Пишется прямо в native Camera каждый кадр, пока включено; при выключении
// восстанавливается исходное значение. Смена камеры игрой обрабатывается —
// старой камере возвращается её клип, новой сохраняется свой.
static float    g_xray_meters = 0.0F;      // 0 = выключено
static uint64_t g_xray_cam = 0;
static float    g_xray_saved_near = 0.1F;
static bool     g_xray_saved_valid = false;

void esp_set_xray(float meters) {
    if (!std::isfinite(meters) || meters < 0.0F) meters = 0.0F;
    if (meters > 50.0F) meters = 50.0F;
    g_xray_meters = meters;
}

// Нативный указатель камеры последнего кадра: его находит esp_get_boxes, а
// фрикаму нужен трансформ камеры, чтобы двигать её.
static uint64_t g_native_camera = 0;

static void xray_apply(uint64_t native_cam) {
    if (!native_cam) return;
    g_native_camera = native_cam;   // фрикам берёт отсюда трансформ камеры
    if (g_xray_meters > 0.05F) {
        if (g_xray_cam != native_cam || !g_xray_saved_valid) {
            // Другая камера: вернуть клип прежней, запомнить клип новой.
            if (g_xray_saved_valid && g_xray_cam)
                wr_buf(g_xray_cam + CAMERA_NEAR_CLIP, &g_xray_saved_near, sizeof(float));
            float current = rd<float>(native_cam + CAMERA_NEAR_CLIP);
            g_xray_saved_near = (std::isfinite(current) && current > 0.0001F && current < 5.0F)
                              ? current : 0.1F;
            g_xray_saved_valid = true;
            g_xray_cam = native_cam;
        }
        wr_buf(native_cam + CAMERA_NEAR_CLIP, &g_xray_meters, sizeof(float));
    } else if (g_xray_saved_valid) {
        if (g_xray_cam)
            wr_buf(g_xray_cam + CAMERA_NEAR_CLIP, &g_xray_saved_near, sizeof(float));
        g_xray_saved_valid = false;
        g_xray_cam = 0;
    }
}

// ==== Всегда день: TOD_Sky (ассет Time Of Day) ==============================
// Инстанс ищется сканом области TypeInfo-слотов и валидируется структурой
// (см. TOD_* в game_offsets.h): статик-список инстансов -> элемент того же
// класса -> Cycle -> Hour/Day/Month/Year в разумных пределах. Пока включено,
// фоновый поток пишет полдень прямо в Cycle.Hour — игровой Update сам
// разворачивает солнце.
static bool     g_day_enabled = false;
static uint64_t g_day_tod = 0;          // подтверждённый инстанс TimeOfDay
static std::atomic<uint64_t> g_day_cycle_addr{0}; // Cycle.Hour для писателя
static std::atomic<bool>     g_day_writer_running{false};
static int      g_day_retry = 0;

void esp_set_always_day(bool enabled) { g_day_enabled = enabled; }

static void always_day_tick() {
    if (!g_day_enabled) {
        // Выключили: писатель замолкает (адрес в 0), часы игры идут сами.
        g_day_cycle_addr.store(0);
        g_day_tod = 0;
        return;
    }
    if (!g_il2cpp_base || g_pid <= 0) return;
    if (g_day_tod) {
        // Живучесть: инстанс мог умереть при перезагрузке мира.
        uint64_t cyc = rd_ptr(g_day_tod + TOD_SKY_CYCLE);
        float hour = (cyc >= 0x10000) ? rd<float>(cyc + TOD_CYCLE_HOUR) : -1.0F;
        if (!(std::isfinite(hour) && hour >= 0.0F && hour <= 24.0F)) g_day_tod = 0;
    }
    // g_day_tod здесь — объект TOD_Sky (небесный менеджер ассета Time of
    // Day). Имя его класса обфусцировано и РОТИРУЕТ каждый билд: в дампе
    // 89e0b63 это был "IY", в 62a8534 — "UV" (в ещё более старом — "Gq").
    // Прежний Oxide.TimeOfDay в боевых сценах не существует: его ленивые
    // метадата-слоты так и не инициализированы (лог day_log: нечётные токены)
    // — Awake ни разу не вызывался. TOD_Sky ищем по СИГНАТУРЕ, без имён: у его
    // класса первое статик-поле — список инстансов List<Self>; элемент списка —
    // объект того же класса; у объекта по TOD_SKY_CYCLE (0x40) лежит
    // TOD_CycleParameters с полями Hour(float 0..24)/Day(1..31)/Month(1..12)/
    // Year(1900..2100). Имя TOD_CycleParameters не обфусцировано — по нему
    // класс и находится в новом дампе (grep 'TOD_CycleParameters_o\* Cycle').
    if (!g_day_tod) {
        static uint64_t s_scan_rva = TOD_SCAN_RVA_BEGIN;
        // const, а не constexpr: значения приходят из активного набора
        // оффсетов (релиз/бета), а он переключается в рантайме.
        const uint64_t kScanEnd = TOD_SCAN_RVA_END;
        const uint64_t kCycleOff = TOD_SKY_CYCLE;
        uint64_t slots[128];
        if (rd_buf(g_il2cpp_base + s_scan_rva, slots, sizeof(slots))) {
            for (int i = 0; i < 128 && !g_day_tod; ++i) {
                uint64_t klass = slots[i];
                if (klass < 0x10000 || (klass & 0x7) != 0) continue;
                uint64_t statics = rd_ptr(klass + 0xB8);
                if (statics < 0x10000) continue;
                uint64_t list = rd_ptr(statics);      // static List<Gq> instances
                if (list < 0x10000) continue;
                uint64_t items = rd_ptr(list + 0x10); // List._items
                int32_t size = rd<int32_t>(list + 0x18);
                if (items < 0x10000 || size <= 0 || size > 4) continue;
                uint64_t sky = rd_ptr(items + 0x20);  // [0]
                if (sky < 0x10000) continue;
                if (rd_ptr(sky) != klass) continue;   // элемент — того же класса
                uint64_t cyc = rd_ptr(sky + kCycleOff);
                if (cyc < 0x10000) continue;
                float hour = rd<float>(cyc + TOD_CYCLE_HOUR);
                int day = rd<int32_t>(cyc + TOD_CYCLE_DAY);
                int mon = rd<int32_t>(cyc + TOD_CYCLE_MONTH);
                int year = rd<int32_t>(cyc + TOD_CYCLE_YEAR);
                if (std::isfinite(hour) && hour >= 0.0F && hour <= 24.0F &&
                    day >= 1 && day <= 31 && mon >= 1 && mon <= 12 &&
                    year >= 1900 && year <= 2100)
                    g_day_tod = sky;
            }
        }
        s_scan_rva += 128 * 8;
        if (s_scan_rva >= kScanEnd) s_scan_rva = TOD_SCAN_RVA_BEGIN;
        if (!g_day_tod) return;
    }
    // Полдень: писатель-доминатор. Игровой писатель обновляет Cycle.Hour
    // каждый кадр, и запись раз в кадр оверлея с ним гонялась — отсюда
    // миллисекундные проблески старого времени. Теперь час пишет фоновый
    // поток с периодом ~2 мс: окно, в котором игра успевает и записать своё
    // время, и отрендерить его, практически исчезает.
    {
        uint64_t cyc = rd_ptr(g_day_tod + TOD_SKY_CYCLE);
        if (cyc >= 0x10000) {
            g_day_cycle_addr.store(cyc + TOD_CYCLE_HOUR);
            if (!g_day_writer_running.exchange(true)) {
                std::thread([]() {
                    while (g_day_writer_running.load()) {
                        uint64_t addr = g_day_cycle_addr.load();
                        if (addr && g_day_enabled && g_pid > 0) {
                            float noon = 12.0F;
                            wr_buf(addr, &noon, sizeof(float));
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    }
                }).detach();
            }
        } else {
            g_day_tod = 0; // объект умер (смена сцены) — переискать
            g_day_cycle_addr.store(0);
        }
    }
}

// Чтение C-строки (имени класса, пространства имён) из памяти игры.
//
// Кусками по 8 байт, а не «95 байт на всякий случай», как было раньше. Прежнее
// чтение ломалось на границе отображения: ядро отдаёт отображённые байты, а на
// следующем куске возвращает EIO — чтение считалось неудачным целиком, строка
// терялась, хотя начало прочиталось, и это ещё засчитывалось как потеря доступа.
// Теперь идём вперёд кусками и останавливаемся на нуле; если кусок упёрся в
// границу — добираем по байту и сохраняем прочитанное.
//
// readable (если передан) — прочитался ли первый кусок. По этому признаку видно,
// что строка недоступна вообще (в логе устройства все чтения имён классов падали
// с errno 5: память метаданных там не читается), — и тогда класс опознаётся по
// структуре, см. class_identity.
std::string read_remote_string(uint64_t address, bool* readable) {
    if (readable) *readable = false;
    if (!address) return {};
    const size_t kMax = 95;
    char buffer[kMax + 1] = {};
    size_t got = 0;
    while (got < kMax) {
        const size_t want = (kMax - got < sizeof(uint64_t)) ? (kMax - got) : sizeof(uint64_t);
        if (!rd_buf(address + got, buffer + got, want)) {
            if (got == 0) return {};   // первый кусок не читается: строки здесь нет
            size_t single = 0;
            while (single < want &&
                   g_mem.read_quiet(address + got + single, buffer + got + single, 1)) {
                if (buffer[got + single] == '\0') break;
                ++single;
            }
            got += single;
            break;
        }
        got += want;
        if (memchr(buffer + got - want, '\0', want)) break;
    }
    buffer[kMax] = '\0';
    if (readable) *readable = true;
    return std::string(buffer);
}

static bool remote_string_equals(uint64_t address, const char* expected) {
    if (!address || !expected) return false;
    return read_remote_string(address) == expected;
}

bool read_managed_string_ex(uint64_t str_obj, char* out, size_t cap, int32_t max_chars) {
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

bool read_managed_string(uint64_t str_obj, char* out, size_t cap) {
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
        CopyTextUtf8(out, cap, tmp); return true;
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
        CopyTextUtf8(out, cap, tmp);
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

// Canonicalise the label: replace it with the game's own name for that weapon
// so the same gun always reads the same, whichever source it came from and
// whatever decoration its skin prefab carried. Weapons missing from the table
// keep their cleaned name, so nothing ever disappears from the box.
//
// Язык подписи оружия берётся из переключателя в «Опциях»: русский (или
// короткое имя из таблицы, если русского нет) либо английское имя, как его
// пишет сама игра в ItemData.m_Name — оно и лежит в колонке `en`.

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

// Карта памяти процесса читается ЦЕЛИКОМ одним дескриптором (раньше был fgets по
// строкам): это и быстрее, и надёжнее — строку разбираем сами, а имя сверяем с
// последним сегментом пути, поэтому подстрока в чужом имени (или « (deleted)» в
// конце) больше не путает.
// Все базы-кандидаты той же библиотеки (см. maps::lookup_library_bases): после
// перезапуска игры образов в карте бывает несколько.
static int get_base_candidates(const char* lib, uint64_t* out, int max) {
    if (g_pid <= 0 || !out || max <= 0) return 0;
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", g_pid);
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    std::string text;
    char chunk[8192];
    for (;;) {
        const ssize_t n = read(fd, chunk, sizeof(chunk));
        if (n > 0) {
            text.append(chunk, (size_t)n);
            if (text.size() > (8u << 20)) break;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        break;
    }
    close(fd);
    return maps::lookup_library_bases(text, lib, out, max);
}

// Похож ли адрес на Il2CppClass — по одной структуре, без чтения имён.
bool class_looks_alive(uint64_t klass) {
    if (!valid_obj(klass)) return false;
    const uint64_t image = rd_ptr(klass);
    const uint64_t name  = rd_ptr(klass + IL2CPP_CLASS_NAME);
    const uint64_t space = rd_ptr(klass + IL2CPP_CLASS_NAMESPACE);
    if (!image || !name || !space) return false;
    // Имя и пространство имён — указатели в память метаданных: не ноль и не мусор.
    return name >= 0x10000 && space >= 0x10000;
}

// Сверка класса с ожидаемым именем и пространством имён.
//   1 — имя прочитано и совпало;
//   2 — имя прочитать не удалось, класс принят по структуре;
//   0 — не он.
//
// Почему не только имя. С устройства пришёл лог, где ВСЕ чтения имён классов
// падали с errno 5: адреса 0x2002a500 и 0x20026580 — это строки имён классов в
// памяти метаданных, и на том устройстве эта память не отображена вовсе (ядро
// отдаёт EIO, как по отданной странице), хотя всё остальное — классы, статические
// поля, куча — читается прекрасно. Из-за одних только имён чит не мог опознать
// ни одного класса, и «функционал не работает» целиком: отказов 35 тысяч, боксов
// ноль.
// Поэтому там, где имя доступно, сверяем его, как раньше (защита от чужой сборки
// игры); где нет — верим смещению из таблицы оффсетов и проверяем структуру.
int class_identity(uint64_t klass, const char* expected_name, const char* expected_ns) {
    if (!class_looks_alive(klass)) return 0;
    bool name_readable = false, ns_readable = false;
    const std::string name  = read_remote_string(rd_ptr(klass + IL2CPP_CLASS_NAME), &name_readable);
    const std::string space = read_remote_string(rd_ptr(klass + IL2CPP_CLASS_NAMESPACE), &ns_readable);
    if (!name_readable && !ns_readable) return 2;
    if (name_readable && name != expected_name) return 0;
    if (ns_readable && space != expected_ns) return 0;
    return 1;
}

// Живой ли это il2cpp выбранной сборки по адресу base: читаем Il2CppClass*
// PlayerManager из его typeinfo и сверяем имя. Проверка дешёвая (несколько сотен
// байт) и однозначная: в чужом/мёртвом образе (старый «(deleted)» после
// перезапуска игры) там мусор, и класс игры не резолвится — привязка при этом
// выглядит успешной, а весь чит молча не работает.
static bool base_resolves_game(uint64_t base) {
    if (!base) return false;
    if (PLAYER_MANAGER_TYPEINFO_RVA != 0) {
        const uint64_t candidate = rd_ptr(base + PLAYER_MANAGER_TYPEINFO_RVA);
        if (class_identity(candidate, "PlayerManager", "Oxide") != 0) return true;
    }
    if (GAME_CONTROLLER_TYPEINFO_RVA != 0) {
        const uint64_t candidate = rd_ptr(base + GAME_CONTROLLER_TYPEINFO_RVA);
        if (class_identity(candidate, "GameControllerBase", "Oxide") != 0) return true;
    }
    return false;
}

static bool validate_player_list(uint64_t list, uint64_t player_class) {
    if (!list || !player_class) return false;
    uint64_t items = rd_ptr(list + IL2CPP_LIST_ITEMS);
    int32_t count = rd<int32_t>(list + IL2CPP_LIST_SIZE);
    if (!items || count < 0 || count > 512) return false;
    if (count == 0) {
        // Пустой список — обычное состояние, пока игроки не заспавнились, и
        // сверить класс по элементам нечем. Раньше здесь сверялось имя класса
        // ("List`1"): там, где память имён недоступна (см. class_identity),
        // список отвергался — и чит не работал вообще. Теперь: массив элементов
        // на месте (List хранит его даже пустым), а класс — живой il2cpp-класс.
        if (!valid_obj(rd_ptr(list + IL2CPP_LIST_ITEMS))) return false;
        return class_looks_alive(rd_ptr(list));
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

uint64_t get_class_static_fields(uint64_t klass) {
    if (!klass) return 0;
    return rd_ptr(klass + IL2CPP_CLASS_STATIC_FIELDS);
}

static uint64_t resolve_runtime_player_list() {
    if (!g_player_manager_class && PLAYER_MANAGER_TYPEINFO_RVA != 0) {
        const uint64_t candidate = rd_ptr(g_il2cpp_base + PLAYER_MANAGER_TYPEINFO_RVA);
        if (class_identity(candidate, "PlayerManager", "Oxide") != 0)
            g_player_manager_class = candidate;
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

// Сколько кадров подряд прежний адрес игрока не подтверждается чтением. Три —
// пора искать заново (респавн, смена мира). Один кадр — нормальный сбой чтения:
// на устройстве до 4 % чтений падают с errno 5 (лог 16.09: отказов 51 тысяча
// на 1,16 млн чтений), а чтобы записать в память, адрес надо прочитать. Из-за
// одного такого кадра аим терял MouseLook, молчал полсекунды и показывал
// «не найден MouseLook» — со стороны «мемори-аим работает через раз».
static int  g_local_player_miss = 0;
static const int kLocalPlayerMissLimit = 3;

// Свежий адрес из статики GameController: источник, с которым сверяем кеш.
// Три чтения, поэтому чаще раза в секунду не зовём.
static uint64_t read_local_player_from_static() {
    if (!g_game_controller_class || !g_player_manager_class) return 0;
    uint64_t gcb_static_fields = get_class_static_fields(g_game_controller_class);
    if (!gcb_static_fields) return 0;
    uint64_t local_player = rd_ptr(gcb_static_fields + GAME_CONTROLLER_LOCAL_PLAYER_FIELD);
    if (local_player && rd_ptr(local_player) == g_player_manager_class) return local_player;
    return 0;
}

static uint64_t resolve_local_player() {
    const uint64_t cached = g_local_player;
    if (cached && g_player_manager_class) {
        uint64_t klass = rd_ptr(cached);
        if (klass != g_player_manager_class) klass = rd_ptr(cached); // чтение могло сорваться
        if (klass == g_player_manager_class) {
            g_local_player_miss = 0;
            // Класс совпал — но адрес мог остаться от умершего игрока: память
            // переиспользована, а класс в её первом поле тот же. Раньше кеш
            // держался до тех пор, пока чтение класса не срывалось, и все
            // производные (MouseLook, KCC, скелет) молча читали чужой объект.
            // Раз в секунду сверяемся с источником: он один и дешёвый.
            const double now = memio::now_seconds();
            static double s_checked_at = -1e9;
            if (now - s_checked_at >= 1.0) {
                s_checked_at = now;
                const uint64_t fresh = read_local_player_from_static();
                if (fresh && fresh != cached) {
                    LogLine("память: локальный игрок сменился 0x%llx -> 0x%llx (кеш был чужим)",
                            (unsigned long long)cached, (unsigned long long)fresh);
                    g_local_player = fresh;
                    return fresh;
                }
            }
            return cached;
        }
        if (++g_local_player_miss < kLocalPlayerMissLimit) return cached;
    }

    if (!g_game_controller_class && GAME_CONTROLLER_TYPEINFO_RVA != 0) {
        const uint64_t candidate = rd_ptr(g_il2cpp_base + GAME_CONTROLLER_TYPEINFO_RVA);
        if (class_identity(candidate, "GameControllerBase", "Oxide") != 0)
            g_game_controller_class = candidate;
    }

    if (!g_game_controller_class || !g_player_manager_class) return 0;

    const uint64_t local_player = read_local_player_from_static();
    if (local_player) {
        g_local_player = local_player;
        g_local_player_miss = 0;
        return local_player;
    }
    // Заново найти не вышло (статику не прочитали или игрока ещё нет). Прежний
    // адрес в кэше не обнуляем: следующий кадр проверит его снова, и если
    // чтение просто сорвалось — адрес пригодится. Но наружу его не отдаём:
    // он уже не подтверждён три кадра подряд.
    return 0;
}

static uint64_t resolve_native_transform(uint64_t transform) {
    if (!transform) return 0;
    return rd_ptr(transform + MANAGED_CACHED_PTR);
}

bool vec3_is_finite(const Vec3& value) {
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

bool read_transform_hierarchy_layout(uint64_t native_transform, const TransformHierarchyLayout& layout, Vec3& position, Vec4* world_rotation) {
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


// ==== Фрикам: камера летит отдельно от тела ================================
//
// Зачем. Перед рейдом надо знать, что внутри базы: где шкафы, где стены в два
// слоя, откуда заходить. Телом сквозь стены летать нельзя — сервер такой
// телепорт не простит, — поэтому отделяем только камеру: мир рисуется с нового
// места, персонаж остаётся стоять где стоял.
//
// Почему пишем ЛОКАЛЬНУЮ позицию трансформа, а не мировую. Unity хранит в
// иерархии локальные TRS (matrices[]) и каждый кадр сам разворачивает их в
// мировые матрицы. Значит:
//   * локальное смещение, записанное один раз, игра подхватывает и
//     пересчитывает сама — камера летит ровно, без нашей записи каждый кадр;
//   * мировую позицию игра затёрла бы в следующем же кадре, и камеру
//     колбасило бы между нашим местом и телом.
// Проверено по дампам (tools/offsets): ни MouseLook.ZJo, ни
// CustomCharacterController.Update сеттеры позиции трансформа не вызывают.
//
// Держим позицию в мировых координатах, а перед записью пересчитываем в
// локальные относительно родителя камеры.
static bool     g_freecam_on = false;
static bool     g_freecam_saved_ok = false;
static Vec3     g_freecam_saved{};        // локальная позиция камеры до включения
static Vec3     g_freecam_pos{};          // где камера сейчас (мировые)
static uint64_t g_freecam_transform = 0;
static uint64_t g_freecam_matrices = 0;   // базы массивов иерархии камеры
static uint64_t g_freecam_indices = 0;
static int32_t  g_freecam_index = -1;
static int      g_freecam_miss = 0;      // чтений подряд, где трансформ не прочитался
// Почему фрикам не включился. Показывается прямо в меню: таскать лог с
// устройства ради одной строки «камера не найдена» неудобно, а без причины
// чинить нечего. 0 = порядок, 1 = нет камеры, 2 = нет трансформа,
// 3 = массивы Transform не читаются, 4 = мировая позиция не совпала.
static int      g_freecam_fail = 0;
static float    g_freecam_fail_dist = -1.f;

static bool read_transform_world_trs(uint64_t matrices, uint64_t indices, int32_t index,
                                     Vec3& pos, Vec4& rot, Vec3& scale, bool fresh = false);
// Позиция камеры из матрицы вида (объявлена ниже, рядом с её разбором).
static bool camera_position_from_view(const Mat4& view, Vec3& position);

// Базы массивов иерархии для трансформа. Кандидатов несколько (раскладку
// учит скан по игрокам, а он живёт до перезагрузки мира), поэтому каждый
// проверяем: мировая позиция, посчитанная по этому кандидату, должна
// совпасть с той, что игра отдала для камеры. Иначе легко записать камеру
// не в ту ячейку — и получить «фрикам не работает» без всяких ошибок.
// Массивы Transform конкретного трансформа: matrices (локальные TRS),
// indices (родители) и сам индекс. Перебор — в точности как у рабочего чтения
// позы камеры (read_camera_transform_pose): и раскладка, и оба расположения
// TransformAccess, и прямой указатель с разыменованным. Первая сборка фрикама
// пробовала только прямые указатели и на устройстве не нашла ничего, хотя
// читать позу камеры по этим же массивам умела.
//
// Найденное проверяем: посчитанная мировая позиция обязана совпасть с той,
// что ESP уже знает (g_cam_pos), иначе это не камера. Расхождение наружу —
// чтобы в логе было видно, чем именно кончился перебор.
// Transform из GameObject. Раскладка взята не из догадок, а из libunity.so
// (сборка не менялась, Unity 6000.3.18f1):
//
//   * GetComponentFastPath(obj, range) — 716 вызовов в прошивке; объект в неё
//     подают как [компонент + 0x20], а внутри она читает
//         [obj + 0x20] — массив пар, [obj + 0x30] — число элементов,
//         пара 16 байт: {int typeIndex; Component* component} (значение +0x08).
//     Значит +0x20 у любого компонента — это его GameObject;
//   * Transform: 0x7ed62c берёт [x0 + 0x38] и [x0 + 0x40] (данные иерархии и
//     индекс), а при пустой иерархии подставляет [x0 + 0x28] и свой instanceID —
//     это и есть TransformAccess из нашей таблицы смещений.
//
// Отсюда главное: Camera + 0x20 — это GameObject камеры, а вовсе не Transform.
// Смещения совпадают (и там, и там 0x20), поэтому перепутать их легче лёгкого,
// и тогда все чтения иерархии идут по чужой структуре — ровно это и было
// «массивы Transform не читаются» в логе 19.09: кандидаты нашлись, годной
// позиции не дал ни один (GameObject на месте Transform даёт мусор в +0x38).
static constexpr uint64_t GAMEOBJECT_COMPONENT_COUNT  = 0x30;  // число компонентов
static constexpr uint64_t GAMEOBJECT_COMPONENT_STRIDE = 0x10;  // размер пары
static constexpr uint64_t TRANSFORM_ACCESS_HIERARCHY  = 0x38;  // данные иерархии

static uint64_t transform_from_gameobject(uint64_t gameobject) {
    if (gameobject < 0x10000) return 0;
    const uint64_t components = rd_ptr(gameobject + GAMEOBJECT_COMPONENT_ARRAY);
    if (components < 0x10000) return 0;
    const int32_t count = rd<int32_t>(gameobject + GAMEOBJECT_COMPONENT_COUNT);
    // Число не читается — берём первый слот: у Unity он и есть Transform.
    int n = (count > 0 && count <= 64) ? count : 1;
    uint64_t fallback = 0;
    for (int i = 0; i < n; ++i) {
        const uint64_t component =
            rd_ptr(components + (uint64_t)i * GAMEOBJECT_COMPONENT_STRIDE + COMPONENT_PAIR_PTR);
        if (component < 0x10000 || component == gameobject) continue;
        // Настоящий компонент ссылается назад на свой GameObject (+0x20).
        const bool backref = rd_ptr(component + COMPONENT_GAMEOBJECT) == gameobject;
        if (!backref) { if (!fallback) fallback = component; continue; }
        // Transform — тот, у кого живой указатель иерархии (+0x38).
        if (rd_ptr(component + TRANSFORM_ACCESS_HIERARCHY) >= 0x10000) return component;
        if (!fallback) fallback = component;
    }
    return fallback;
}

// Transform камеры. Camera наследует Component, поэтому её GameObject лежит
// по тому же +0x20, что мы раньше считали трансформом; настоящий Transform
// достаём из массива компонентов.
static uint64_t camera_transform_of(uint64_t native_cam) {
    if (!native_cam) return 0;
    const uint64_t gameobject = rd_ptr(native_cam + CAMERA_NATIVE_TRANSFORM);
    if (gameobject < 0x10000) return 0;
    const uint64_t transform = transform_from_gameobject(gameobject);
    return transform ? transform : gameobject;
}

static uint64_t camera_transform() { return camera_transform_of(g_native_camera); }

static bool resolve_transform_arrays(uint64_t native_transform, uint64_t& matrices,
                                     uint64_t& indices, int32_t& index,
                                     float* best_dist = nullptr) {
    if (!native_transform) return false;
    if (best_dist) *best_dist = -1.f;

    struct Cand { uint64_t m, n; int32_t i; };
    Cand cand[96];
    int  cand_src[96] = {};   // из какого толкования (tf[0..2]) кандидат
    int  cur_src = 0;         // текущее толкование для push()
    int count = 0;
    auto push = [&](uint64_t m, uint64_t n, int32_t i) {
        if (!m || !n || i < 0 || i > 100000 || count >= 96) return;
        const uint64_t ms[2] = {m, rd_ptr(m)};
        const uint64_t ns[2] = {n, rd_ptr(n)};
        for (int a = 0; a < 2 && count < 96; ++a)
            for (int b = 0; b < 2 && count < 96; ++b)
                if (ms[a] && ns[b]) { cand_src[count] = cur_src; cand[count++] = {ms[a], ns[b], i}; }
    };
    // Один проход по указателю трансформа: пара «где данные TransformAccess,
    // где индекс» и все известные пары «массивы матриц / массив родителей».
    auto probe = [&](uint64_t base, uint64_t data_offset, uint64_t index_offset) {
        const uint64_t data = rd_ptr(base + data_offset);
        const int32_t  idx  = rd<int32_t>(base + index_offset);
        if (!data || idx < 0 || idx > 100000) return;
        static const uint64_t kPairs[][2] = {{0x18, 0x20}, {0x08, 0x10}, {0x10, 0x18}, {0x20, 0x28}};
        for (const auto& pr : kPairs) push(rd_ptr(data + pr[0]), rd_ptr(data + pr[1]), idx);
    };

    // Указатели, которые пробуем как Transform. Что именно пришло на вход,
    // заранее неизвестно, поэтому пробуем все три толкования:
    //   tf[0] — сам адрес (вызывающий уже передал Transform);
    //   tf[1] — Transform из этого объекта как из GameObject (случай камеры:
    //           Camera + 0x20 — это GameObject, Transform лежит в его массиве
    //           компонентов, проверено по libunity.so);
    //   tf[2] — Transform из GameObject, если на вход дали компонент.
    uint64_t tf[3] = {native_transform, 0, 0};
    tf[1] = transform_from_gameobject(native_transform);
    const uint64_t as_component = rd_ptr(native_transform + COMPONENT_GAMEOBJECT);
    if (as_component >= 0x10000 && as_component != native_transform)
        tf[2] = transform_from_gameobject(as_component);

    // С чем сверяем найденную позицию. g_cam_pos НЕ годится: его пишет
    // read_camera_transform_pose, то есть ровно тот путь, который на
    // устройстве может не работать (ESP строит боксы из матрицы вида —
    // transform_camera_mode = false, — и мусор в g_cam_pos годами никому не
    // мешал). Сверяемся с позицией из матрицы вида: это та самая матрица,
    // по которой игра рисует кадр, она всегда свежая. Из-за сверки с
    // мусорным g_cam_pos фрикам и не включался.
    Vec3 ref{};
    bool know_cam = false;
    if (g_native_camera) {
        const Mat4 view = rd_m4(g_native_camera + CAMERA_VIEW_MATRIX);
        Vec3 vpos{};
        if (camera_position_from_view(view, vpos) && vec3_is_finite(vpos)) { ref = vpos; know_cam = true; }
    }
    if (!know_cam && g_frame_cam_basis_valid && vec3_is_finite(g_frame_cam_pos)) {
        ref = g_frame_cam_pos; know_cam = true;
    }
    if (!know_cam && g_cam_pose_valid && vec3_is_finite(g_cam_pos)) {
        ref = g_cam_pos; know_cam = true;
    }

    int   best_k = -1;
    float best_d = -1.f;
    int   positions = 0;                 // сколько кандидатов дали годную позицию
    auto evaluate = [&](int from, int to) {
        for (int k = from; k < to; ++k) {
            Vec3 pos{};
            if (!read_transform_hierarchy_arrays(cand[k].m, cand[k].n, cand[k].i, pos)) continue;
            if (!vec3_is_finite(pos)) continue;
            ++positions;
            if (!know_cam) { best_k = k; best_d = 0.f; return; }
            const float dx = pos.x - ref.x, dy = pos.y - ref.y, dz = pos.z - ref.z;
            const float d = sqrtf(dx * dx + dy * dy + dz * dz);
            if (best_k < 0 || d < best_d) { best_k = k; best_d = d; }
            // Камера не может стоять в двух метрах от себя самой: нашли — выходим.
            if (d < 2.0F) return;
        }
    };

    // 1) раскладка, выученная по игрокам; 2) оба известных расположения
    // TransformAccess на всех трёх указателях.
    if (g_transform_hierarchy_layout_valid) {
        const TransformHierarchyLayout& L = g_transform_hierarchy_layout;
        for (int t = 0; t < 3 && tf[t]; ++t) {
            cur_src = t;
            const uint64_t data = rd_ptr(tf[t] + L.data_offset);
            const int32_t  idx  = rd<int32_t>(tf[t] + L.index_offset);
            uint64_t m = data ? rd_ptr(data + L.matrices_offset) : 0;
            uint64_t n = data ? rd_ptr(data + L.indices_offset) : 0;
            if (L.matrices_indirect) m = rd_ptr(m);
            if (L.indices_indirect)  n = rd_ptr(n);
            if (m && n && idx >= 0 && idx <= 100000) {
                cand[count++] = {m, n, idx};
                break;                    // раскладка одна — хватит первого
            }
        }
    }
    for (int t = 0; t < 3 && tf[t]; ++t) {
        if (tf[t] == tf[0] && t > 0) continue;      // то же толкование — не надо
        cur_src = t;
        probe(tf[t], 0x38, 0x40);
        probe(tf[t], 0x18, 0x20);
    }
    evaluate(0, count);

    // 3) широкий перебор — только когда быстрое не дало ничего. Здесь другие
    // смещения TransformAccess и все три указателя: на устройстве первые два
    // прохода не нашли ни одной годной позиции, значит смещения сместились.
    // Проверка та же (мировая позиция должна совпасть с камерой), поэтому
    // ложного срабатывания не будет — только лишние чтения, и то при отказе.
    if (best_k < 0 || best_d > 2.0F) {
        static const uint64_t kExtended[][2] = {{0x20, 0x28}, {0x28, 0x30}, {0x30, 0x38},
                                                {0x40, 0x48}, {0x10, 0x18}, {0x48, 0x50}};
        const int saved = count;
        for (int t = 0; t < 3 && tf[t]; ++t) {
            if (tf[t] == tf[0] && t > 0) continue;
            cur_src = t;
            for (const auto& off : kExtended) probe(tf[t], off[0], off[1]);
        }
        // Лучший из быстрых остаётся лучшим: evaluate() сравнивается с ним,
        // а не ищет заново, — иначе потеряли бы расстояние первого прохода.
        evaluate(saved, count);
    }

    // Отказ объясняем полностью: какой трансформ, что дал компонентный путь,
    // сколько кандидатов вообще дали позицию и насколько далеко лучший.
    // Без этого по одной строке «массивы не читаются» не видно, где искать.
    if (best_k < 0 || best_d > 2.0F) {
        static double s_last = -1e9;
        const double now = memio::now_seconds();
        if (now - s_last >= 2.0) {
            s_last = now;
            LogLine("фрикам: перебор — трансформ 0x%llx, из компонента 0x%llx, кандидатов %d, позиций %d, ближайшее %.1f м",
                    (unsigned long long)tf[0], (unsigned long long)tf[1], count, positions,
                    (double)(best_d >= 0.f ? best_d : -1.f));
            // Которое из трёх толкований дало позицию и что вообще нашлось.
            // Без этого не видно, сломался путь GameObject или сама иерархия.
            LogLine("фрикам: источники — кандидат %d из толкования %d, трансформы 0x%llx/0x%llx/0x%llx, камера %d",
                    best_k, (best_k >= 0 ? cand_src[best_k] : -1),
                    (unsigned long long)tf[0], (unsigned long long)tf[1],
                    (unsigned long long)tf[2], (int)know_cam);
        }
        if (best_dist) *best_dist = (best_k >= 0 ? best_d : -1.f);
        return false;
    }
    if (best_dist) *best_dist = best_d;
    // Порог 2 м, а не полметра: g_cam_pos мог прийти из резервной матрицы вида
    // (она считается по другому источнику и чуть расходится), и из-за жёсткого
    // порога фрикам отказывался включаться там, где камера найдена верно.
    matrices = cand[best_k].m; indices = cand[best_k].n; index = cand[best_k].i;
    return true;
}

// Мировая TRS трансформа: идём вверх по родителям, как это делает Unity.
static bool read_transform_world_trs(uint64_t matrices, uint64_t indices, int32_t index,
                                     Vec3& pos, Vec4& rot, Vec3& scale, bool fresh) {
    if (!matrices || !indices || index < 0 || index > 100000) return false;
    // fresh — чтение в обход кэша блоков (см. rd_fresh в game_internal.h).
    auto load = [fresh](uint64_t addr, Matrix34& out) -> bool {
        const bool got = fresh ? rd_fresh(addr, out) : rd_exact(addr, out);
        return got && matrix34_is_valid(out);
    };
    auto link = [fresh](uint64_t addr, int32_t& out) -> bool {
        return fresh ? rd_fresh(addr, out) : rd_exact(addr, out);
    };
    Matrix34 m{};
    if (!load(matrices + (uint64_t)index * sizeof(Matrix34), m)) return false;
    pos = {m.translation.x, m.translation.y, m.translation.z};
    rot = m.rotation;
    scale = {m.scale.x, m.scale.y, m.scale.z};
    // Кватернион нормализуем обязательно. matrix34_is_valid пускает длину от
    // 0.45 до 1.41 (кость, которую игра пишет прямо сейчас, успевает
    // прочитаться наполовину), а rotate_vector() разворачивает вектор
    // ЕДИНИЧНЫМ кватернионом. С ненормированным прямой и обратный разворот
    // перестают быть взаимными: прямая даёт лишний множитель |q|², обратная
    // его не знает. Для фрикама это означает, что каждый шаг поправки
    // отличается от нужного в |q|² раз — контур не сходится.
    if (!normalize_quaternion(rot)) return false;
    int32_t parent = -2, previous = index;
    if (!link(indices + (uint64_t)index * sizeof(int32_t), parent)) return false;
    int depth = 0;
    while (parent >= 0 && depth++ < 128) {
        if (parent > 100000 || parent == previous) return false;
        Matrix34 p{};
        if (!load(matrices + (uint64_t)parent * sizeof(Matrix34), p)) return false;
        Vec4 pr = p.rotation;
        if (!normalize_quaternion(pr)) return false;
        // world = parentWorld * local (Unity: сначала масштаб, потом поворот,
        // потом перенос)
        Vec3 scaled = {pos.x * p.scale.x, pos.y * p.scale.y, pos.z * p.scale.z};
        Vec3 turned = rotate_vector(pr, scaled);
        pos = {p.translation.x + turned.x, p.translation.y + turned.y, p.translation.z + turned.z};
        rot = multiply_quaternion(pr, rot);
        if (!normalize_quaternion(rot)) return false;
        scale = {p.scale.x * scale.x, p.scale.y * scale.y, p.scale.z * scale.z};
        previous = parent;
        if (!link(indices + (uint64_t)parent * sizeof(int32_t), parent)) return false;
    }
    if (parent != -1 || depth >= 128) return false;
    return vec3_is_finite(pos) && std::isfinite(scale.x) && scale.x > 1e-6F;
}

// Мировую позицию -> локальную, с учётом родителя камеры.
static bool transform_world_to_local(uint64_t matrices, uint64_t indices, int32_t index,
                                     const Vec3& world, Vec3& local) {
    int32_t parent = -2;
    if (!rd_exact(indices + (uint64_t)index * sizeof(int32_t), parent)) return false;
    if (parent < 0) { local = world; return true; }
    Vec3 pPos{}, pScale{};
    Vec4 pRot{};
    if (!read_transform_world_trs(matrices, indices, parent, pPos, pRot, pScale)) return false;
    const Vec3 d = {world.x - pPos.x, world.y - pPos.y, world.z - pPos.z};
    // поворот назад: сопряжённый кватернион (направление нормализовано)
    Vec4 inv = pRot;
    const float len = sqrtf(inv.x * inv.x + inv.y * inv.y + inv.z * inv.z + inv.w * inv.w);
    if (!(len > 1e-6F)) return false;
    inv.x = -inv.x / len; inv.y = -inv.y / len; inv.z = -inv.z / len; inv.w = inv.w / len;
    const Vec3 turned = rotate_vector(inv, d);
    if (!(fabsf(pScale.x) > 1e-6F && fabsf(pScale.y) > 1e-6F && fabsf(pScale.z) > 1e-6F)) return false;
    local = {turned.x / pScale.x, turned.y / pScale.y, turned.z / pScale.z};
    return vec3_is_finite(local);
}

// Отказ включения — в лог, но не чаще раза в 2 с: меню пробует включить
// фрикам каждые полсекунды, и без этого лог забивался бы однотипными строками.
// Причина — номером, а не строкой (русские литералы вне вызова LogLine
// проверка переводов считает подписями визуалов).
static void log_freecam_fail(int reason, uint64_t transform, float dist) {
    static double s_last = -1e9;
    const double now = memio::now_seconds();
    if (now - s_last < 2.0) return;
    s_last = now;
    if (dist >= 0.f) {
        LogLine("фрикам: не включён — мировая позиция не совпала с камерой (трансформ 0x%llx, ближайшее расхождение %.1f м)",
                (unsigned long long)transform, (double)dist);
        return;
    }
    switch (reason) {
        case 0: LogLine("фрикам: не включён — камера ещё не найдена"); break;
        case 1: LogLine("фрикам: не включён — нет трансформа камеры (0x%llx)", (unsigned long long)transform); break;
        case 2: LogLine("фрикам: не включён — массивы Transform не читаются (0x%llx)", (unsigned long long)transform); break;
        default: LogLine("фрикам: не включён — локальная матрица камеры не читается (0x%llx)", (unsigned long long)transform); break;
    }
}

bool esp_freecam_active() { return g_freecam_on; }

void esp_freecam_diag(int& code, float& dist) {
    code = g_freecam_fail;
    dist = g_freecam_fail_dist;
}

// Массивы иерархии именно для трансформа камеры.
//
// Почему не общий resolve_transform_arrays() с перебором 96 кандидатов: у
// камеры есть ИЗВЕСТНАЯ точка — позиция из матрицы вида (той самой, по которой
// игра рисует кадр), и известный трансформ (camera_transform(): GameObject
// камеры -> её Transform). Значит годную пару массивов не надо угадывать по
// «чья позиция ближе»: годится только та, которая даёт ровно позицию камеры.
// Перебор же легко брал чужой трансформ, чья позиция случайно оказалась в
// двух метрах, — и тогда фрикам двигал не камеру, а что-то рядом, а камера
// оставалась привязанной к телу и каждые полсекунды прыгала между своей
// точкой и телом (жалоба 19.09).
// Все проверенные слоты камеры, а не один лучший.
//
// Лог 19:53:35 — «в памяти (-39.8, 52.2, 38.6), игра видит (-0.0, -0.0, -0.0)»,
// через секунду наоборот — «в памяти (0.0, 0.0, 0.0), игра видит (-39.8, 52.2,
// 38.6)». Значит массивов иерархии ДВОЕ, и игра их чередует: пока мы пишем в
// одни, камера читает другие (пустая ячейка, ноль — отсюда «под картой»), и
// наоборот. Раньше брался первый слот, подошедший по позиции, поэтому камера
// половину времени оказывалась в нуле: жалоба «фрикам мерцает». Писать надо
// во ВСЕ слоты, чья посчитанная позиция совпала с камерой.
struct FcSlot { uint64_t m = 0, n = 0; int32_t i = -1; float d = -1.f; };
static constexpr int kFcMaxSlots = 4;
// Насколько далеко посчитанная позиция слота может стоять от камеры, чтобы
// слот считался её собственным. 2 м, а не полметра: g_cam_pos мог прийти из
// резервной матрицы вида (она считается по другому источнику и чуть
// расходится), и из-за жёсткого порога фрикам отказывался включаться там,
// где камера найдена верно.
static constexpr float kFcSlotMaxDist = 2.0f;
static FcSlot g_fc_slots[kFcMaxSlots];
static int    g_fc_slot_n = 0;

// Добавить слот в список проверенных, если его ещё нет. Держим ближайшие.
static void fc_slot_add(uint64_t m, uint64_t n, int32_t i, float d) {
    if (!m || !n || i < 0) return;
    for (int k = 0; k < g_fc_slot_n; ++k)
        if (g_fc_slots[k].m == m && g_fc_slots[k].n == n && g_fc_slots[k].i == i) return;
    if (g_fc_slot_n < kFcMaxSlots) {
        g_fc_slots[g_fc_slot_n++] = FcSlot{m, n, i, d};
        return;
    }
    int worst = 0;
    for (int k = 1; k < g_fc_slot_n; ++k)
        if (g_fc_slots[k].d > g_fc_slots[worst].d) worst = k;
    if (g_fc_slots[worst].d > d) g_fc_slots[worst] = FcSlot{m, n, i, d};
}

static bool resolve_camera_arrays(uint64_t& matrices, uint64_t& indices, int32_t& index,
                                  Vec3& cam_pos, float& err_m) {
    matrices = indices = 0; index = -1; err_m = -1.f;
    if (!g_native_camera) return false;
    const Mat4 view = rd_m4(g_native_camera + CAMERA_VIEW_MATRIX);
    if (!camera_position_from_view(view, cam_pos) || !vec3_is_finite(cam_pos)) return false;
    const uint64_t gameobject = rd_ptr(g_native_camera + CAMERA_NATIVE_TRANSFORM);
    const uint64_t transform = transform_from_gameobject(gameobject);
    if (!transform) return false;
    // TransformAccess: данные +0x38, индекс +0x40 — подтверждено кодом самого
    // Unity (libunity.so 0x7ed62c: читает [x0+0x38], пишет [x0+0x40], а при
    // пустой иерархии подставляет [x0+0x28]).
    const uint64_t data = rd_ptr(transform + TRANSFORM_ACCESS_HIERARCHY);
    const int32_t  idx  = rd<int32_t>(transform + 0x40);
    if (!data || idx < 0 || idx > 100000) return false;
    static const uint64_t kPairs[][2] = {{0x18, 0x20}, {0x08, 0x10}, {0x10, 0x18}, {0x20, 0x28}};
    for (const auto& pr : kPairs) {
        const uint64_t m0 = rd_ptr(data + pr[0]);
        const uint64_t n0 = rd_ptr(data + pr[1]);
        if (!m0 || !n0) continue;
        // Каждый из указателей может быть ещё и таблицей указателей.
        const uint64_t ms[2] = {m0, rd_ptr(m0)};
        const uint64_t ns[2] = {n0, rd_ptr(n0)};
        for (uint64_t m : ms) {
            for (uint64_t n : ns) {
                if (!m || !n) continue;
                Vec3 pos{};
                Vec4 rot{};
                if (!read_transform_hierarchy_arrays(m, n, idx, pos, &rot)) continue;
                if (!vec3_is_finite(pos)) continue;
                const float dx = pos.x - cam_pos.x, dy = pos.y - cam_pos.y, dz = pos.z - cam_pos.z;
                const float d = sqrtf(dx * dx + dy * dy + dz * dz);
                if (err_m < 0.f || d < err_m) err_m = d;
                // Позиция камеры из её же трансформа обязана совпасть с той,
                // что в матрице вида: это один и тот же объект. Метр допуска
                // закрывает расхождение на кадр (камера движется между чтениями).
                if (d > 1.0F) continue;
                // Раньше здесь был немедленный выход: брался ПЕРВЫЙ подошедший
                // набор массивов. Лог 19:53 показал, что камера половину
                // времени оказывается в нуле — «в памяти (0,0,0), игра видит
                // (-39.8, 52.2, 38.6)» и через секунду наоборот. Значит,
                // подходящих наборов больше одного (игра чередует буферы
                // иерархии), и писать надо во все: иначе в тот набор, куда мы
                // не пишем, камера приходит за нулевой матрицей.
                fc_slot_add(m, n, idx, d);
                if (!matrices) { matrices = m; indices = n; index = idx; }
            }
        }
    }
    // Наборов могло найтись несколько: matrices/index — первый (эталон для
    // лога), остальные лежат в g_fc_slots.
    return matrices != 0;
}

// Массивы камеры ПРЯМО СЕЙЧАС — по раскладке, подтверждённой самим Unity.
//
// В libunity.so релиза функция мировой позиции трансформа (0x7F19D8) читает
// [transform+0x38] (блок данных) и [transform+0x40] (индекс), внутри блока
// берёт матрицы по [data+0x18], индексы родителей по [data+0x20] и ОБХОДИТ
// цепочку родителей, собирая мировую матрицу на лету (шаг 0x30 = Matrix34).
// Отсюда два вывода:
//   * отдельного массива мировых матриц нет и кэш тут ни при чём: что лежит в
//     локальной матрице, то игра и нарисует;
//   * указатели живут в блоке данных трансформа, а игрушка этот блок
//     ПЕРЕСОБИРАЕТ (переродитель костей, перестройка иерархии при спавне).
//     Запомненные один раз при включении фрикама указатели устаревают, и мы
//     писали в уже брошенный массив — отсюда «камера возвращается к телу»
//     (жалобы 19.09), сколько ни пиши.
// Поэтому указатели перечитываются на каждом такте писателя.
static bool camera_arrays_now(uint64_t& matrices, uint64_t& indices, int32_t& index) {
    if (!g_native_camera) return false;
    const uint64_t gameobject = rd_ptr(g_native_camera + CAMERA_NATIVE_TRANSFORM);
    const uint64_t transform = transform_from_gameobject(gameobject);
    if (!transform) return false;
    uint64_t data = rd_ptr(transform + TRANSFORM_ACCESS_HIERARCHY);   // 0x38
    int32_t  idx  = rd<int32_t>(transform + 0x40);
    if (!data) {
        // Данные ещё не инициализированы: Unity кладёт сюда собственный блок
        // трансформа (+0x28) и берёт индекс из +0x30 (то же видно в 0x7ED640
        // и 0x7F19E0).
        data = transform + 0x28;
        idx  = rd<int32_t>(transform + 0x30);
    }
    if (!data || idx < 0 || idx > 100000) return false;
    matrices = rd_ptr(data + 0x18);
    indices  = rd_ptr(data + 0x20);
    if (!matrices || !indices) return false;
    index = idx;
    return true;
}

// Писатель-доминатор фрикама.
//
// Почему одного кадра оверлея мало. Камера привязана к кости головы, поэтому
// игра пишет её TRS каждый раз, когда шевелится тело. Мы же писали раз в кадр
// оверлея — ПОСЛЕ отрисовки, то есть ровно в тот момент, когда следующий
// игровой кадр нашу запись гарантированно затрёт. Отсюда прыжки «наша точка
// <-> тело» (лог 13:38): пока тело стоит — видна наша позиция, тело побежало —
// видна позиция тела.
//
// Выход: фоновый поток с периодом 4 мс (тот же приём, что у «всегда день»).
// Окно, в котором игра успевает записать своё И отрендерить, исчезает — кадр
// игры длится 15—30 мс.
//
// И почему поправка ЗАМКНУТАЯ, а не абсолютная. Раньше мы считали локальную
// позицию от цели через матрицу родителя (transform_world_to_local). Если
// матрица родителя прочиталась неверно (а родитель — это кость, и её матрица
// обновляется игрой отдельно), локальная позиция выходила далёкой от нужной,
// и камера ТЕЛЕПОРТИРОВАЛАСЬ в произвольное место. Теперь поток каждый такт
// читает, где камера оказалась на самом деле, и доводит её до цели маленькой
// поправкой: ошибка в матрице родителя влияет только на величину шага, а не
// на саму точку, и контур всё равно сходится.
// Адреса и значения — ПО СЛОТУ (см. FcSlot): игра чередует наборы массивов,
// поэтому писатель обязан вести их все, иначе половина кадров камеры читает
// набор, в который мы не пишем.
static std::atomic<uint64_t> g_fc_addr[kFcMaxSlots];   // matrices + index*48 + 0x24
static std::atomic<float>    g_fc_lx[kFcMaxSlots], g_fc_ly[kFcMaxSlots], g_fc_lz[kFcMaxSlots];
// Сколько слотов сейчас ведёт писатель.
static std::atomic<int>      g_fc_addr_n{0};
static std::atomic<bool>     g_fc_writer_running{false};
static std::atomic<unsigned> g_fc_write_count{0};
static std::atomic<bool>     g_fc_write_failed{false};
// Счётчик «игра затёрла нашу запись»: сколько раз за секунду позиция камеры
// между нашей записью и чтением успела поменяться. По нему видно, кто кого
// переписывает — это главный вопрос фрикама.
static std::atomic<unsigned> g_fc_overwrites{0};
static std::atomic<float>    g_fc_over_m{0.f};
// Сколько раз за секунду игра подменила массивы иерархии камеры.
static std::atomic<unsigned> g_fc_array_changes{0};
static std::atomic<float>    g_fc_gap{-1.f};
// Насколько близко камера подошла к цели по последней оценке (метры).

// Посчитать локальную позицию, которая поставит камеру в цель. Писать НЕ здесь:
// пишет поток (см. ниже). Замеряем, где камера оказалась на самом деле, и
// доводим её поправкой — ошибка в матрице родителя тогда влияет только на
// величину шага, а не на саму точку.
// Чем именно контур получил свою поправку. Заполняется здесь, печатается раз в
// секунду в freecam_tick. Без этих чисел «фрикам летит в одну сторону сам»
// остаётся загадкой: по ним видно, какое звено врёт — локальная позиция,
// мировая, матрица родителя или сам шаг.
static Vec3    g_fc_diag_local{};
static Vec3    g_fc_diag_cur{};
static Vec3    g_fc_diag_step{};
static Vec3    g_fc_diag_parent_pos{};
static Vec4    g_fc_diag_parent_rot{};
static Vec3    g_fc_diag_parent_scale{};
static int32_t g_fc_diag_parent = -2;
static bool    g_fc_diag_ok = false;
static bool    g_fc_diag_absolute = false;   // записали цель целиком (а не шаг)
// Что лежало в памяти камеры, когда нашу запись затёрли (для лога).
static std::atomic<float> g_fc_ow_x{0}, g_fc_ow_y{0}, g_fc_ow_z{0};

static bool freecam_local_for_target(uint64_t matrices, uint64_t indices, int32_t index,
                                     const Vec3& target, Vec3& local_out, float& gap) {
    if (!matrices || !indices || index < 0) return false;
    // Все чтения здесь — в обход кэша блоков (последний аргумент true).
    // Кэш живёт до конца кадра, а freecam_write() за кадр вызывается несколько
    // раз: из такта кадра и из каждого события джойстика. Читая из кэша, второй
    // и следующие вызовы видели позицию ДО своей же предыдущей поправки и
    // добавляли её ещё раз — контур превращался в интегратор с запаздыванием и
    // разгонялся. Это и есть «фрикам летит в одну сторону сам» (жалоба 19.09).
    Vec3 cur{}, scale{};
    Vec4 rot{};
    if (!read_transform_world_trs(matrices, indices, index, cur, rot, scale, true)) return false;
    if (!vec3_is_finite(cur)) return false;
    Vec3 d = {target.x - cur.x, target.y - cur.y, target.z - cur.z};
    if (!vec3_is_finite(d)) return false;
    const float dl = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
    gap = dl;
    if (dl < 0.002F) return true;                 // уже там
    // Поправку режем: прочитанная позиция иногда мусорная, и огромный шаг унёс
    // бы камеру неизвестно куда. Два метра за такт при 60 Гц — это 120 м/с,
    // то есть расхождение в десятки метров гасится за считанные кадры, а один
    // мусорный отсчёт не уносит камеру дальше чем на два метра.
    if (dl > 2.0F) { const float kk = 2.0F / dl; d = {d.x * kk, d.y * kk, d.z * kk}; }

    int32_t parent = -2;
    if (!rd_fresh(indices + (uint64_t)index * sizeof(int32_t), parent)) return false;
    Matrix34 self{};
    if (!rd_fresh(matrices + (uint64_t)index * sizeof(Matrix34), self)) return false;
    if (!matrix34_is_valid(self)) return false;
    Vec3 local = {self.translation.x, self.translation.y, self.translation.z};
    // Поправку больше не интегрируем — пишем цель ЦЕЛИКОМ.
    //
    // Почему интегратор пришлось выбросить. Лог 15:59 показал, что камера
    // ходит ровно двухметровыми шагами вдоль луча «начало координат -> цель»
    // (все позиции — точные доли 2 м от цели), но шаги эти идут в ОБЕ стороны:
    // недолёт за секунду менялся на десятки метров то вниз, то вверх. То есть
    // контур не сходился, а болтался, и величина болтанки ровно равна нашему
    // ограничению шага. Интегратор в замкнутом контуре, где отсчёт запаздывает
    // или его кто-то ещё переписывает, разгоняется — это и есть «фрикам
    // бесконечно летит в одну сторону». Абсолютная запись такой болезни не
    // имеет в принципе: каждый такт в памяти лежит ровно цель, и сколько бы
    // тактов ни прошло, камера не уедет.
    Vec3 step{};
    bool absolute = false;
    if (parent < 0) {
        // Родителя нет — локальная позиция и есть мировая (лог: «родитель -1»).
        // Тогда цель пишется прямо, без всякой матрицы.
        step = {target.x - cur.x, target.y - cur.y, target.z - cur.z};
        local = target;
        absolute = true;
    } else {
        Vec3 pPos{}, pScale{};
        Vec4 pRot{};
        if (!read_transform_world_trs(matrices, indices, parent, pPos, pRot, pScale, true)) return false;
        g_fc_diag_parent_pos = pPos; g_fc_diag_parent_rot = pRot; g_fc_diag_parent_scale = pScale;
        // Абсолютная локальная позиция цели. Берём её, если она не уводит
        // камеру дальше 50 м за такт: так мусорная матрица родителя даст не
        // телепорт через карту, а длинный, но ограниченный шаг.
        Vec3 abs_local{};
        if (transform_world_to_local(matrices, indices, index, target, abs_local) &&
            vec3_is_finite(abs_local)) {
            const float jx = abs_local.x - local.x, jy = abs_local.y - local.y, jz = abs_local.z - local.z;
            if (sqrtf(jx * jx + jy * jy + jz * jz) <= 50.f) {
                step = {jx, jy, jz};
                local = abs_local;
                absolute = true;
            }
        }
        if (!absolute) {
            Vec4 inv = pRot;
            const float len = sqrtf(inv.x * inv.x + inv.y * inv.y + inv.z * inv.z + inv.w * inv.w);
            if (!(len > 1e-6F)) return false;
            inv = {-inv.x / len, -inv.y / len, -inv.z / len, inv.w / len};
            const Vec3 turned = rotate_vector(inv, d);
            if (!(fabsf(pScale.x) > 1e-6F && fabsf(pScale.y) > 1e-6F && fabsf(pScale.z) > 1e-6F)) return false;
            step = {turned.x / pScale.x, turned.y / pScale.y, turned.z / pScale.z};
            local = {local.x + step.x, local.y + step.y, local.z + step.z};
        }
    }
    g_fc_diag_parent = parent;
    g_fc_diag_local = local;
    g_fc_diag_cur = cur;
    g_fc_diag_step = step;
    g_fc_diag_absolute = absolute;
    g_fc_diag_ok = true;
    if (!vec3_is_finite(local)) return false;
    // Предохранитель: один шаг длиннее 200 м — это мусорный отсчёт, а не полёт.
    const float sl = sqrtf(step.x * step.x + step.y * step.y + step.z * step.z);
    if (sl > 200.f) {
        LogLine("фрикам: поправка отброшена — шаг %.1f м длиннее 200 м", (double)sl);
        return false;
    }
    local_out = local;
    return true;
}

static uint64_t g_fc_last_m = 0, g_fc_last_n = 0;
static int32_t  g_fc_last_i = -1;
// Слоты, которые фрикам ведёт сам. Снимок делается на включении: общий и
// список g_fc_slots перезаполняется при каждом вызове resolve_camera_arrays,
// а вызывают его и другие пути (поза камеры), и чужой вызов не должен менять
// набор слотов у уже летящей камеры.
static FcSlot   g_fc_own[kFcMaxSlots];
static int      g_fc_own_n = 0;
// Сколько тактов подряд проверенный слот камеры не читается. Перерешаем
// массивы только после серии отказов: одиночный сбой — это игра, которая
// прямо сейчас обновляет объект, а не переезд массивов.
static int      g_fc_slot_miss = 0;

// Сколько раз за период камера, по глазам игры, была там, где мы ей велим.
// Мерцание (жалоба 19.09) видно в логе как «игра видит (-0.0, -0.0, -0.0)»,
// но раз в секунду — это один отсчёт, и непонятно, мигает камера каждый кадр
// или раз в секунду. Считаем каждый такт писателя: сколько отсчётов пришлось
// на цель, сколько на ноль и сколько куда-то ещё. По соотношению видно, что
// именно ломается — наша запись не доходит (тогда ноль каждый второй кадр)
// или игра пересчитывает мировую матрицу сама (тогда ноль реже).
static std::atomic<unsigned> g_fc_samp{0}, g_fc_hit{0}, g_fc_zero{0}, g_fc_other{0};

static void freecam_writer_start() {
    if (g_fc_writer_running.exchange(true)) return;
    std::thread([]() {
        unsigned iter = 0;
        double probe_at = memio::now_seconds() + 3.0;   // первая проба — через 3 с
        while (g_fc_writer_running.load()) {
            const uint64_t addr = g_fc_addr[0].load();
            const int      nslots = g_fc_addr_n.load();
            if (addr && g_pid > 0) {
                const float v[3] = {g_fc_lx[0].load(), g_fc_ly[0].load(), g_fc_lz[0].load()};
                // Проба «игрушка переписала нашу запись».
                //
                // Прежний счётчик читал адрес назад сразу после своей же
                // записи. Это ничего не меряло: поток пишет две тысячи раз в
                // секунду, поэтому чужое значение живёт в памяти не дольше
                // полмиллисекунды из шестнадцати, и честный «0/с» получался бы
                // даже при живом конфликте с игрой (лог 15:59: «затёрто игрой
                // 0/с» при камере, которая болтается на десятки метров).
                //
                // Теперь раз в две секунды поток на один игровой кадр ЗАМОЛКАЕТ
                // и каждые 2 мс читает адрес. Если его кто-то переписывает, это
                // видно сразу — и по величине, и по самому значению.
                const double now = memio::now_seconds();
                if (now >= probe_at) {
                    probe_at = now + 2.0;
                    float worst = 0.f, seen[3] = {};
                    int changes = 0;
                    for (int s = 0; s < 12; ++s) {
                        float c[3] = {};
                        if (rd_buf(addr, c, sizeof(c))) {
                            const float dx = c[0] - v[0], dy = c[1] - v[1], dz = c[2] - v[2];
                            const float d = sqrtf(dx * dx + dy * dy + dz * dz);
                            if (d > 0.05F) ++changes;
                            if (d > worst) { worst = d; seen[0] = c[0]; seen[1] = c[1]; seen[2] = c[2]; }
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    }
                    if (changes) {
                        g_fc_overwrites.fetch_add((unsigned)changes);
                        g_fc_over_m.store(g_fc_over_m.load() + worst);
                        g_fc_ow_x.store(seen[0]); g_fc_ow_y.store(seen[1]); g_fc_ow_z.store(seen[2]);
                    }
                    continue;      // в этом проходе не пишем
                }
                if (wr_buf(addr, v, sizeof(v))) g_fc_write_count.fetch_add(1);
                else g_fc_write_failed.store(true);
                // Остальные проверенные слоты — тем же значением: игра читает
                // то один набор массивов, то другой, и оба должны лежать.
                for (int sl = 1; sl < nslots; ++sl) {
                    const uint64_t a2 = g_fc_addr[sl].load();
                    if (!a2) continue;
                    const float v2[3] = {g_fc_lx[sl].load(), g_fc_ly[sl].load(), g_fc_lz[sl].load()};
                    if (wr_buf(a2, v2, sizeof(v2))) g_fc_write_count.fetch_add(1);
                }
                ++iter;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    }).detach();
}

bool esp_freecam_set(bool on) {
    if (on == g_freecam_on) return g_freecam_on;
    if (!on) {
        // Возвращаем камеру на место: пишем сохранённую локальную позицию.
        // Писатель останавливается ПЕРВЫМ: иначе он добил бы нашу позицию
        // поверх только что восстановленной.
        for (int sl = 0; sl < kFcMaxSlots; ++sl) g_fc_addr[sl].store(0);
        g_fc_addr_n.store(0);
        g_fc_writer_running.store(false);
        g_fc_last_m = g_fc_last_n = 0; g_fc_last_i = -1;
        if (g_freecam_saved_ok && g_freecam_matrices && g_freecam_index >= 0)
            wr_buf(g_freecam_matrices + (uint64_t)g_freecam_index * sizeof(Matrix34) +
                   offsetof(Matrix34, translation), &g_freecam_saved, sizeof(Vec3));
        g_freecam_on = false;
        g_freecam_saved_ok = false;
        LogLine("фрикам: выключен, камера вернулась к телу");
        return false;
    }
    if (g_pid <= 0 || !g_il2cpp_base || !g_mem.bound()) return false;
    uint64_t transform = camera_transform();
    if (!transform) {
        g_freecam_fail = g_native_camera ? 2 : 1; g_freecam_fail_dist = -1.f;
        log_freecam_fail(g_native_camera ? 1 : 0, 0, -1.f);
        return false;
    }
    uint64_t matrices = 0, indices = 0;
    int32_t index = -1;
    float dist = -1.f;
    Vec3 cam_pos{};
    g_fc_slot_n = 0;
    if (!resolve_camera_arrays(matrices, indices, index, cam_pos, dist)) {
        // Проверенный путь не дал массивов — тогда старый перебор: он хоть и
        // грубее, но находил камеру до починки GameObject.
        if (!resolve_transform_arrays(transform, matrices, indices, index, &dist)) {
            g_freecam_fail = dist >= 0.f ? 4 : 3; g_freecam_fail_dist = dist;
            log_freecam_fail(2, transform, dist);
            return false;
        }
    }
    Matrix34 m{};
    if (!rd_exact(matrices + (uint64_t)index * sizeof(Matrix34), m)) {
        g_freecam_fail = 3; g_freecam_fail_dist = -1.f;
        log_freecam_fail(3, transform, -1.f);
        return false;
    }
    Vec3 pos{}, scale{};
    Vec4 rot{};
    if (!read_transform_world_trs(matrices, indices, index, pos, rot, scale)) return false;
    g_freecam_saved = {m.translation.x, m.translation.y, m.translation.z};
    g_freecam_saved_ok = true;
    g_freecam_transform = transform;
    g_freecam_matrices = matrices;
    g_freecam_indices = indices;
    g_freecam_index = index;
    // Запоминаем и в счётчике подмен: отсюда массивы считаются эталонными,
    // и freecam_write() больше не имеет права их подменять.
    g_fc_last_m = matrices; g_fc_last_n = indices; g_fc_last_i = index;
    // Снимок слотов. Проверенный путь их уже собрал; у старого перебора
    // (resolve_transform_arrays) списка нет — тогда ведём один найденный слот.
    g_fc_own_n = g_fc_slot_n;
    for (int k = 0; k < g_fc_own_n; ++k) g_fc_own[k] = g_fc_slots[k];
    if (g_fc_own_n <= 0) { g_fc_own[0] = FcSlot{matrices, indices, index, dist}; g_fc_own_n = 1; }
    g_freecam_pos = pos;
    g_freecam_on = true;
    g_freecam_fail = 0; g_freecam_fail_dist = dist;   // 0 = порядок
    // Сколько слотов ведём — по логу видно, один набор массивов у камеры или
    // два чередующихся (жалоба «мерицает»): при одном слоте второй записи нет.
    LogLine("фрикам: включён, старт (%.1f, %.1f, %.1f), трансформ=0x%llx, слотов %d",
            (double)pos.x, (double)pos.y, (double)pos.z,
            (unsigned long long)transform, (int)g_fc_own_n);
    return true;
}

// Сдвинуть камеру: метры вдоль оси взгляда (вперёд), вправо и вверх.
// Горизонтальные оси берём из самой камеры, но без наклона: иначе «вперёд»
// при взгляде вниз уводило бы камеру под землю.
// Записать текущую цель в память. Пишем КАЖДЫЙ кадр, а не только когда палец
// на джойстике: массивы иерархии игра может пересобирать каждый кадр со
// своего authoritative-состояния, и тогда разовая запись прожила бы ровно
// один кадр. Плюс камера перестаёт ехать вместе с телом: мировая цель стоит,
// а локальная позиция пересчитывается под текущего родителя.
// Позиция камеры из матрицы вида; определена ниже, у проверок MouseLook.
static bool current_camera_position(Vec3& out);

static bool freecam_write() {
    if (!g_freecam_on || !g_freecam_matrices || !g_freecam_indices || g_freecam_index < 0) return false;
    if (!vec3_is_finite(g_freecam_pos)) return false;
    // Массивы НЕ перерешаем: берём те, что нашёл esp_freecam_set().
    //
    // Раньше здесь каждый такт вызывался camera_arrays_now() — перебор по
    // раскладке TransformAccess БЕЗ проверки. Он всегда возвращал один и тот
    // же ответ («подмен массивов 0» в логе), и из-за этого выглядело будто
    // массивы в порядке. На деле он находил НЕ тот слот: лог 16:33 показывает
    // «разбор — индекс 0, родитель -1, посчитано (0.0, 0.0, 0.0)», хотя
    // камера в это время у цели. Проверенный слот (resolve_camera_arrays —
    // сверяет позицию с матрицей вида, то есть с тем, чем игра реально
    // рисует) подменялся непроверенным уже на первом такте после включения.
    // Отсюда и «камера мерцает и телепортируется под землю»: половину
    // времени мы писали в пустую ячейку с нулевой матрицей.
    //
    // Перерешаем только если проверенный слот перестал читаться подряд
    // несколько тактов — тогда действительно что-то переехало.
    uint64_t m = g_freecam_matrices, n = g_freecam_indices;
    int32_t  idx = g_freecam_index;
    {
        Matrix34 probe{};
        if (!rd_exact(m + (uint64_t)idx * sizeof(Matrix34), probe) || !matrix34_is_valid(probe)) {
            if (++g_fc_slot_miss < 8) return false;
            g_fc_slot_miss = 0;
            Vec3 view_pos{};
            float err = -1.f;
            uint64_t nm = 0, nn = 0;
            int32_t nidx = -1;
            if (resolve_camera_arrays(nm, nn, nidx, view_pos, err) && nm && nn && nidx >= 0) {
                if (nm != g_fc_last_m || nn != g_fc_last_n || nidx != g_fc_last_i) {
                    g_fc_array_changes.fetch_add(1);
                    g_fc_last_m = nm; g_fc_last_n = nn; g_fc_last_i = nidx;
                    LogLine("фрикам: массивы перерешены — была позиция (%.1f, %.1f, %.1f), в матрице вида (%.1f, %.1f, %.1f), расхождение %.1f м",
                            (double)g_freecam_pos.x, (double)g_freecam_pos.y, (double)g_freecam_pos.z,
                            (double)view_pos.x, (double)view_pos.y, (double)view_pos.z, (double)err);
                }
                g_freecam_matrices = nm; g_freecam_indices = nn; g_freecam_index = nidx;
                m = nm; n = nn; idx = nidx;
            } else {
                return false;
            }
        } else {
            g_fc_slot_miss = 0;
        }
    }
    if (!m || !n || idx < 0) return false;
    // Поправку считаем здесь (60 Гц), а пишет её поток каждые полмиллисекунды:
    // так наша позиция лежит в памяти почти всё время игрового кадра.
    //
    // Считаем и пишем для КАЖДОГО проверенного слота. Лог 19:53 показал, что
    // наборов массивов два и игра их чередует: пока мы вели один, камера в
    // половине кадров читала другой, где лежал ноль, — отсюда мерцание и
    // провал под карту. Локальная позиция у слотов может различаться: она
    // считается от родителя, найденного в этом же наборе массивов.
    bool ok = false;
    int  fed = 0;
    for (int sl = 0; sl < g_fc_own_n && fed < kFcMaxSlots; ++sl) {
        const FcSlot& S = g_fc_own[sl];
        if (!S.m || !S.n || S.i < 0) continue;
        Vec3 local{};
        float gap = -1.f;
        if (!freecam_local_for_target(S.m, S.n, S.i, g_freecam_pos, local, gap)) continue;
        const uint64_t addr = S.m + (uint64_t)S.i * sizeof(Matrix34) + offsetof(Matrix34, translation);
        g_fc_lx[fed].store(local.x);
        g_fc_ly[fed].store(local.y);
        g_fc_lz[fed].store(local.z);
        g_fc_addr[fed].store(addr);
        if (fed == 0) g_fc_gap.store(gap);
        ++fed;
        if (wr_buf(addr, &local, sizeof(Vec3))) ok = true;
    }
    g_fc_addr_n.store(fed);
    if (!fed) return false;
    // Где камера на самом деле — по матрице вида (ею игра и рисует кадр).
    {
        Vec3 cam{};
        if (current_camera_position(cam)) {
            const float dx = cam.x - g_freecam_pos.x, dy = cam.y - g_freecam_pos.y,
                        dz = cam.z - g_freecam_pos.z;
            const float d = sqrtf(dx * dx + dy * dy + dz * dz);
            const float r = sqrtf(cam.x * cam.x + cam.y * cam.y + cam.z * cam.z);
            g_fc_samp.fetch_add(1);
            if (d <= 2.0F) g_fc_hit.fetch_add(1);
            else if (r <= 1.0F) g_fc_zero.fetch_add(1);
            else g_fc_other.fetch_add(1);
        }
    }
    freecam_writer_start();
    return ok;
}

bool esp_freecam_move(float forward, float right, float up) {
    if (!g_freecam_on || !g_freecam_matrices || !g_freecam_indices || g_freecam_index < 0) return false;
    if (g_pid <= 0 || !g_il2cpp_base || !g_mem.bound()) return false;
    if (!std::isfinite(forward) || !std::isfinite(right) || !std::isfinite(up)) return false;
    Vec3 f = g_cam_forward, r = g_cam_right;
    if (!g_cam_pose_valid || !vec3_is_finite(f) || !vec3_is_finite(r)) { f = {0, 0, 1}; r = {1, 0, 0}; }
    float fh = sqrtf(f.x * f.x + f.z * f.z);
    if (fh > 1e-4F) { f.x /= fh; f.z /= fh; } else { f = {0, 0, 1}; }
    float rh = sqrtf(r.x * r.x + r.z * r.z);
    if (rh > 1e-4F) { r.x /= rh; r.z /= rh; } else { r = {1, 0, 0}; }
    g_freecam_pos.x += f.x * forward + r.x * right;
    g_freecam_pos.z += f.z * forward + r.z * right;
    g_freecam_pos.y += up;
    if (!vec3_is_finite(g_freecam_pos)) { g_freecam_pos = {}; return false; }
    return freecam_write();       // сразу, не дожидаясь следующего кадра
}

// Трансформ камеры мог переехать (смена мира, респавн): если камера уехала
// далеко от того места, где мы её оставили, — это уже не наш фрикам.
static void freecam_tick() {
    if (!g_freecam_on) return;
    // Трансформ может умереть (смена мира, респавн), но и на живом объекте
    // чтение иногда срывается. Одного сбойного кадра мало: по нему фрикам
    // выключался бы сам посреди полёта.
    if (g_freecam_transform && rd_ptr(g_freecam_transform)) g_freecam_miss = 0;
    else if (++g_freecam_miss >= 8) { esp_freecam_set(false); return; }

    // Что записали и что игра видит на самом деле: если она пересчитывает
    // матрицу камеры сама, числа разойдутся, и по одному логу сразу видно,
    // надо ли писать позицию каждый кадр.
    static double s_next = 0.0;
    const double now = memio::now_seconds();
    if (now >= s_next) {
        s_next = now + 1.0;
        Vec3 in_mem{}, scale{};
        Vec4 rot{};
        if (read_transform_world_trs(g_freecam_matrices, g_freecam_indices, g_freecam_index,
                                     in_mem, rot, scale)) {
            const unsigned wc = g_fc_write_count.exchange(0);
            const bool wf = g_fc_write_failed.exchange(false);
            const float gap = g_fc_gap.load();
            const unsigned ac = g_fc_array_changes.exchange(0);
            const unsigned ow = g_fc_overwrites.exchange(0);
            const float owm = g_fc_over_m.exchange(0.f);
            LogLine("фрикам: цель (%.1f, %.1f, %.1f), в памяти (%.1f, %.1f, %.1f), игра видит (%.1f, %.1f, %.1f), писатель %u/с, затёрто игрой %u/с (в среднем %.1f м), недолёт %.2f м, подмен массивов %u%s",
                    (double)g_freecam_pos.x, (double)g_freecam_pos.y, (double)g_freecam_pos.z,
                    (double)in_mem.x, (double)in_mem.y, (double)in_mem.z,
                    (double)g_cam_pos.x, (double)g_cam_pos.y, (double)g_cam_pos.z,
                    wc, ow, (double)(ow ? owm / (float)ow : 0.f), (double)gap, ac,
                    wf ? " (отказы записи)" : "");
            // Сколько наборов массивов ведём и что лежит в_parent_ у каждого.
            // Мерцание камеры (жалоба 19.09) живёт ровно здесь: в логе 19:53
            // «игра видит» через строку меняется с цели на (-0.0, -0.0, -0.0),
            // а в_parent_ у ведомого набора нули. По этой строке видно, ВСЕ ли
            // наборы лежат, и не в нулевом ли parent дело, а не в частоте.
            {
                char buf[256] = {};
                int  off = 0;
                for (int sl = 0; sl < g_fc_own_n && off < 200; ++sl) {
                    const FcSlot& S = g_fc_own[sl];
                    int32_t parent = -2;
                    rd_fresh(S.n + (uint64_t)S.i * sizeof(int32_t), parent);
                    Matrix34 pm{};
                    const bool have_p = (parent >= 0) &&
                        rd_fresh(S.m + (uint64_t)parent * sizeof(Matrix34), pm);
                    const bool zero_p = !have_p ||
                        (pm.translation.x == 0.f && pm.translation.y == 0.f && pm.translation.z == 0.f &&
                         pm.rotation.x == 0.f && pm.rotation.y == 0.f && pm.rotation.z == 0.f &&
                         pm.scale.x == 0.f && pm.scale.y == 0.f && pm.scale.z == 0.f);
                    off += snprintf(buf + off, sizeof(buf) - (size_t)off,
                                    " [набор %d: 0x%llx/0x%llx индекс %d родитель %d%s]",
                                    sl, (unsigned long long)S.m, (unsigned long long)S.n,
                                    (int)S.i, (int)parent,
                                    parent < 0 ? " (корень)" : (zero_p ? " НУЛЕВОЙ" : ""));
                }
                LogLine("фрикам: наборы — всего %d%s", (int)g_fc_own_n, buf);
                const unsigned sf = g_fc_samp.exchange(0), hf = g_fc_hit.exchange(0),
                               zf = g_fc_zero.exchange(0), of = g_fc_other.exchange(0);
                LogLine("фрикам: за период отсчётов %u — у цели %u, в нуле %u, другое %u",
                        sf, hf, zf, of);
            }
            // Чем именно оказалась затёрта наша запись. По этому значению
            // видно, кто второй пишет в ту же ячейку: если это текущая позиция
            // головы — запись перебивает игра, и бороться надо её кадром, а не
            // частотой нашего писателя.
            if (ow)
                LogLine("фрикам: затёрто игрой %u раз за период, расхождение до %.1f м, в памяти оказалось (%.1f, %.1f, %.1f)",
                        ow, (double)(owm / (float)ow),
                        (double)g_fc_ow_x.load(), (double)g_fc_ow_y.load(), (double)g_fc_ow_z.load());
            // Чем контур посчитал поправку. По этой строке проверяется вся
            // арифметика: мировая позиция камеры обязана быть равна
            // «позиция родителя + поворот родителя × (масштаб × локальную)»,
            // а шаг — ровно той величине, которая эту мировую позицию доводит
            // до цели. Разошлось — значит врёт конкретное звено, и видно какое.
            if (g_fc_diag_ok) {
                const Vec3& L = g_fc_diag_local;
                const Vec3& C = g_fc_diag_cur;
                const Vec3& S = g_fc_diag_step;
                const Vec3& P = g_fc_diag_parent_pos;
                const Vec4& Q = g_fc_diag_parent_rot;
                const Vec3& Z = g_fc_diag_parent_scale;
                // Литерал с ведущим пробелом: генератор таблиц перевода
                // считает подписью визуала любой русский литерал без пробела
                // в начале, а эта строка уходит только в лог.
                LogLine("фрикам: разбор — индекс %d, родитель %d,%s, лок (%.2f, %.2f, %.2f), посчитано (%.1f, %.1f, %.1f), шаг (%.2f, %.2f, %.2f)",
                        (int)g_freecam_index, (int)g_fc_diag_parent,
                        g_fc_diag_absolute ? " запись целиком" : " шаг",
                        (double)L.x, (double)L.y, (double)L.z,
                        (double)C.x, (double)C.y, (double)C.z,
                        (double)S.x, (double)S.y, (double)S.z);
                LogLine("фрикам: родитель — поза (%.1f, %.1f, %.1f), поворот (%.3f, %.3f, %.3f, %.3f), масштаб (%.3f, %.3f, %.3f), разница с камерой %.2f м",
                        (double)P.x, (double)P.y, (double)P.z,
                        (double)Q.x, (double)Q.y, (double)Q.z, (double)Q.w,
                        (double)Z.x, (double)Z.y, (double)Z.z,
                        (double)sqrtf((C.x - g_cam_pos.x) * (C.x - g_cam_pos.x) +
                                      (C.y - g_cam_pos.y) * (C.y - g_cam_pos.y) +
                                      (C.z - g_cam_pos.z) * (C.z - g_cam_pos.z)));
            }
        }
    }
    // Читать надо ДО записи: после неё в массиве лежит наше значение, и
    // расхождение было бы не видно.
    freecam_write();
}

bool read_transform_hierarchy_position(uint64_t native_transform, Vec3& position) {
    if (!native_transform) return false;
    // The learned layout is only a fast path. It is learned from PLAYER
    // transforms and dies with a world reload — after a respawn it fails (or
    // reads garbage) for every entity. Returning its result directly here
    // was the solo-markers-after-respawn bug: the self-probing fallback
    // below (which needs no players and no learning) was never reached.
    if (g_transform_hierarchy_layout_valid &&
        read_transform_hierarchy_layout(native_transform, g_transform_hierarchy_layout, position) &&
        vec3_is_finite(position) && position_looks_like_world_space(position))
        return true;
    // Same probing as read_camera_transform_pose: TransformAccess lives at
    // +0x38/+0x40 on older builds and at +0x18/+0x20 on this one. Markers ran
    // only the first probe, which is why they worked ONLY once a nearby
    // player's skeleton had taught us the layout — the camera (with both
    // probes) worked solo all along.
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

bool discover_layout_from_native_transforms(const std::vector<uint64_t>& native_transforms,
                                                   size_t& best_position_count, size_t& candidate_count) {
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

static bool discover_transform_hierarchy_layout(const std::vector<uint64_t>& players, size_t& best_position_count, size_t& candidate_count) {
    std::vector<uint64_t> native_transforms;
    std::unordered_set<uint64_t> unique_transforms;
    for (uint64_t player : players) {
        uint64_t native_transform = resolve_player_native_transform(player);
        if (native_transform && unique_transforms.insert(native_transform).second)
            native_transforms.push_back(native_transform);
    }
    return discover_layout_from_native_transforms(native_transforms, best_position_count, candidate_count);
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
bool position_looks_like_world_space(const Vec3& position) {
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
// Когда началась текущая серия неудач прямых полей. Ожидание «поля допишутся»
// считается и по кадрам, и по времени: 60 кадров на перезагрузке мира (там же
// просаживается FPS) растягивались в две-три секунды, и всё это время боксов не
// было вовсе — ровно то, на что жалоба «после смерти прогружаются не сразу».
static double g_direct_position_fail_since = 0.0;
static constexpr double kDirectPositionSettleSeconds = 0.6;
static bool g_body_caches_dirty = false; // clear per-player caches on the next frame

static bool discover_player_position_offset(const std::vector<uint64_t>& players) {
    uint64_t best_offset = find_direct_player_position_offset(players);
    if (best_offset) {
        g_direct_position_fail_streak = 0;
        g_direct_position_fail_since = 0.0;
        g_use_direct_player_position = true; g_player_position_offset = best_offset;
        g_player_position_validated = true; g_matrix_configuration_validated = false;
        return true;
    }
    // Даём прямым полям устояться, прежде чем уходить на обход иерархии, но не
    // дольше положенного: и по кадрам (60), и по времени (0.6 с) — что раньше.
    const double now = mono_seconds();
    if (g_direct_position_fail_since == 0.0) g_direct_position_fail_since = now;
    ++g_direct_position_fail_streak;
    if (g_direct_position_fail_streak < 60 &&
        (now - g_direct_position_fail_since) < kDirectPositionSettleSeconds) return false;
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
    g_direct_position_fail_since = 0.0;
    g_use_direct_player_position = true; g_player_position_offset = best_offset;
    g_matrix_configuration_validated = false;
    g_body_caches_dirty = true;
}

// ---- Чувствительность взгляда (настройка игрока) ---------------------------
//
// Oxide.MouseLook.m_Sensitivity — единственный множитель, которым игра
// превращает накопленный сдвиг касания в поворот камеры: в MouseLook.ZJo
// (RVA 0x64e312c) накопленное значение читается с +0x88, умножается на +0x34 и
// уходит в применение поворота; больше ничего на путь «касание -> угол» не
// влияет. Значит град/px строго пропорционален m_Sensitivity, и аиму нужен
// именно он, а не зашитое число.
//
// Поле публичное и в обеих версиях игры лежит на одном месте (сверено по
// dump.cs релиза 205619 и беты 207986; PlayerManager.mouseLook — +0x70 в обоих).
// Поэтому это НЕ часть таблицы переключения версий (tools/offsets): там только
// то, что между сборками разъезжается.
static constexpr uint64_t PLAYER_MOUSE_LOOK_OFFSET      = 0x70;
static constexpr uint64_t MOUSE_LOOK_SENSITIVITY_OFFSET = 0x34;

bool esp_read_look_sensitivity(float& out) {
    if (g_pid <= 0 || !g_il2cpp_base || !g_mem.bound()) return false;

    uint64_t player = resolve_local_player();
    if (!player) {
        // Своей PlayerManager ещё нет (загрузка, только что респавнулись):
        // настройка клиентская, у любой PlayerManager она одна и та же.
        uint64_t list = resolve_runtime_player_list();
        if (!list) return false;
        uint64_t items = rd_ptr(list + IL2CPP_LIST_ITEMS);
        int32_t  count = rd<int32_t>(list + IL2CPP_LIST_SIZE);
        if (!items || count <= 0 || count > 512) return false;
        player = rd_ptr(items + IL2CPP_ARRAY_FIRST_ELEMENT);
    }
    if (!player) return false;
    if (g_player_manager_class && rd_ptr(player) != g_player_manager_class) return false;

    uint64_t mouse_look = rd_ptr(player + PLAYER_MOUSE_LOOK_OFFSET);
    if (!mouse_look) return false;
    float value = rd<float>(mouse_look + MOUSE_LOOK_SENSITIVITY_OFFSET);
    // Мусор в поле (объект переиспользован, память переехала) отдаём как отказ:
    // заведомо чужое число здесь хуже, чем запасное.
    if (!std::isfinite(value) || value < 0.05F || value > 100.0F) return false;
    out = value;
    return true;
}

// ---- Мемори-аим: запись в память игры -------------------------------------
//
// Остальные поля Oxide.MouseLook, которыми аим вертит камерой. Сверены по
// dump.cs релиза (класс Oxide.MouseLook, строка 205611) и беты (строка 207978)
// — раскладка одна и та же, отличаются только обфусцированные имена (релиз
// LGa/Ljj против беты KXf/KXz). Как и m_Sensitivity, это НЕ часть таблицы
// переключения версий (tools/offsets).
//
// Семантика — из дизассемблера MouseLook.ZJo (libil2cpp.so, RVA 0x64e312c,
// вызывается из MouseLook.Update каждый кадр):
//
//   0x64e31d0  ldr  s2, [x19, #0x34]        ; m_Sensitivity
//   0x64e31e0  fmul s3, s1, s2              ; s1 — вертикаль ввода (+0x8C)
//   0x64e31e4  fnmul s1, s1, s2             ; знак вертикали: -(y * sens)
//   0x64e31ec  fmul s0, s0, s2              ; s0 — горизонталь ввода (+0x88)
//   0x64e31f0  fcsel s1, s1, s3, eq         ; при m_Invert знак не переворачиваем
//   0x64e32a0  fadd s0, s8, s2              ; прибавить к углам взгляда
//   0x64e32ac  bl   #0x72fdd14              ; (поворот KCC/хендлера)
//   0x64e32c0  stur d0, [x19, #0x4c]        ; углы: +0x4C тангаж, +0x50 рысканье
//   0x64e3328  ldr  s1, [x19, x8]           ; x8 = 0x38/0x40: пределы взгляда
//   0x64e3348  str  s0, [x19, #0x4c]        ; тангаж зажат пределами
//   0x64e3418  ldr  s9, [x19, #0x4c]        ; и ушёл в localRotation камеры
//   0x64e3470  fmul s1, s9, s0              ; s0 = 0,01745 — градусы в радианы
//
// Поле ввода (+0x88) игра ПЕРЕЗАПИСЫВАЕТ каждый кадр в MouseLook.ZJX
// (RVA 0x64e412c) — туда она кладёт сдвиг касания; с гироскопом (объект +0x78)
// ZJo вместо перезаписи прибавляет к полю показания гироскопа. Поэтому наша
// запись — это «ввод одного кадра»: сыграет она или нет, зависит от того, кто
// последним коснулся поля перед чтением. Контроллер в aim.cpp это учитывает:
// шаг считается по остатку ошибки, а не по «мы записали, значит повернули».
// Поля MouseLook (дамп билда 62a8534: oxiclean; в бете те же смещения, имена
// обфусцированы и ротируют — сверено по dump.7z и dump_beta.7z).
static constexpr uint64_t MOUSELOOK_INVERT = 0x30;   // m_Invert
static constexpr uint64_t MOUSELOOK_ANGLES = 0x4C;   // Vector2 накопителя:
                                                     // [0] rotationX (вверх — меньше),
                                                     // [1] rotationY (вправо — больше)
static constexpr uint64_t MOUSELOOK_GYRO   = 0x78;   // объект гироскопа (0 = выключен)
// Кому принадлежит объект: MouseLook.m_EventHandler (+0x20) -> его manager
// (+0xD0) — тот самый PlayerManager, из которого MouseLook и был взят. Держим
// рядом с остальными полями, потому что это тот же класс из дампа
// (Oxide_MouseLook_Fields, сверено по il2cpp.h).
static constexpr uint64_t MOUSELOOK_EVENT_HANDLER = 0x20;
// Поля ввода взгляда (+0x88) в расчёте больше нет: MouseLook.ZJo сначала зовёт
// ZJX, а тот перезаписывает +0x88 вводом касания (str d0, [x19, #0x88], файл
// libil2cpp.so 0x64e0364), и только потом ZJo читает это поле — наша запись
// между кадрами игры стирается гарантированно.

// MouseLook локального игрока. В отличие от esp_read_look_sensitivity() здесь
// НИКАКИХ запасных вариантов «взять у любого игрока»: запись идёт в память, и
// попади она в чужой объект — камеру крутило бы не тому игроку.
// Класс MouseLook, у которого уже проверяли m_Sensitivity (см.
// esp_mem_aim_look_params). Запись в память игры разрешена ТОЛЬКО в объект
// этого класса: указатель по смещению мог остаться от умершего игрока или
// указывать на чужой объект, а записать 8 байт не туда — это уже не «аим не
// работает», а падение игры. Имя класса на устройстве может не читаться
// (логи 14-16.09.2026: метаданные недоступны), поэтому опознаём по структуре.
static std::atomic<uint64_t> g_mouse_look_klass{0};

// Почему MouseLook не нашёлся. Нужен не для красоты: по одной строке «не найден
// MouseLook» не видно, что чинить — адрес игрока потерялся, объект чужой или
// просто чтение сорвалось. В лог пишется не чаще раза в 2 с, потому что при
// настоящей поломке это событие идёт каждый кадр.
enum class LookFail { none, noPlayer, wrongPlayer, noObject, badSens, wrongOwner };

// Сообщение — прямо в вызове лога: отдельные русские литералы в game.cpp
// проверка переводов считает подписями визуалов (строки лога она уже
// пропускает, см. tools/lang/gen_tables.py). Пишем не чаще раза в 2 с: при
// настоящей поломке событие идёт каждый кадр.
static void log_look_fail(LookFail f, uint64_t mouse_look, float sens) {
    static double s_last = -1e9;
    const double now = memio::now_seconds();
    if (now - s_last < 2.0) return;
    s_last = now;
    switch (f) {
        case LookFail::noPlayer:
            LogLine("память: MouseLook не найден — нет игрока");
            break;
        case LookFail::wrongPlayer:
            LogLine("память: MouseLook не найден — адрес игрока не той структуры");
            break;
        case LookFail::noObject:
            LogLine("память: MouseLook не найден — объекта нет (0x%llx)",
                    (unsigned long long)mouse_look);
            break;
        case LookFail::badSens:
            LogLine("память: MouseLook=0x%llx — чувствительность вне диапазона (%.3f)",
                    (unsigned long long)mouse_look, (double)sens);
            break;
        default: break;
    }
}

// Чужой MouseLook: адрес нашёлся и класс подошёл, но владелец (m_EventHandler
// -> manager) — не тот игрок. В лог не чаще раза в 2 с: при подмене объекта
// событие идёт каждый кадр.
static void log_look_wrong_owner(uint64_t mouse_look, uint64_t owner, uint64_t player) {
    static double s_last = -1e9;
    const double now = memio::now_seconds();
    if (now - s_last < 2.0) return;
    s_last = now;
    LogLine("память: MouseLook=0x%llx чужой — владелец 0x%llx, а не игрок 0x%llx",
            (unsigned long long)mouse_look, (unsigned long long)owner, (unsigned long long)player);
}

// Игрок найден, но помечен как НЕ локальный (Mirror.isLocalPlayer = false).
// Это ровно тот случай, который раньше проходил все проверки: MouseLook у
// чужого игрока той же структуры, с той же чувствительностью, — и углы мы
// писали ему, а своя камера не двигалась.
static void log_look_not_local(uint64_t player, uint64_t mouse_look) {
    static double s_last = -1e9;
    const double now = memio::now_seconds();
    if (now - s_last < 2.0) return;
    s_last = now;
    LogLine("память: игрок 0x%llx не локальный (isLocalPlayer=0) — MouseLook 0x%llx чужой",
            (unsigned long long)player, (unsigned long long)mouse_look);
}

// MouseLook той же структуры, но его m_LookRoot стоит не у камеры: объект
// чужого игрока или умершего. Проверка по геометрии, не по адресу.
static void log_look_wrong_camera(uint64_t mouse_look, float dist) {
    static double s_last = -1e9;
    const double now = memio::now_seconds();
    if (now - s_last < 2.0) return;
    s_last = now;
    LogLine("память: MouseLook=0x%llx не у камеры — m_LookRoot в %.1f м от неё",
            (unsigned long long)mouse_look, (double)dist);
}

// Кому принадлежит этот MouseLook. Почему нельзя верить одному адресу: объект
// лежит по указателю из игрока, но игрок мог уже умереть, а память — уехать
// под другой объект, у которого класс в первом поле тот же. Тогда мы читаем и
// пишем углы ЧУЖОГО игрока: камера не поворачивается, а в логе всё выглядит
// исправным («MouseLook найден») — ровно то, что жалоба называет «мемори
// теряет MouseLook». Связь проверяемая и дешёвая: m_EventHandler -> manager
// обязан вернуть того игрока, из которого объект взят (два чтения). Если поле
// не читается, владелец неизвестен — тогда решают остальные проверки.
static uint64_t mouse_look_owner(uint64_t mouse_look) {
    if (mouse_look < 0x10000) return 0;
    const uint64_t handler = rd_ptr(mouse_look + MOUSELOOK_EVENT_HANDLER);
    if (handler < 0x10000) return 0;
    const uint64_t owner = rd_ptr(handler + EVENT_HANDLER_MANAGER_BACKREF);
    return owner >= 0x10000 ? owner : 0;
}

// m_LookRoot — Transform, которым MouseLook вертит камеру (Oxide.MouseLook,
// dump.cs релиза). По нему и опознаём «свой» объект.
static constexpr uint64_t MOUSELOOK_LOOK_ROOT = 0x28;
// Mirror помечает игрока, которым управляет это устройство:
// NetworkBehaviour.netIdentity (+0x40) -> NetworkIdentity.isLocalPlayer
// (+0x4A, bool). У остальных игроков флаг false — это единственный признак,
// который читается у ЛЮБОГО игрока (в отличие от m_EventHandler: чужому
// MouseLook'у ввод не нужен, поле пустое, и проверка владельца молчит).
static constexpr uint64_t NETBEHAVIOUR_NET_IDENTITY   = 0x40;
static constexpr uint64_t NETIDENTITY_IS_LOCAL_PLAYER = 0x4A;

// -1 — прочитать не удалось, 0 — чужой, 1 — локальный.
static int player_is_local(uint64_t player) {
    if (player < 0x10000) return -1;
    const uint64_t identity = rd_ptr(player + NETBEHAVIOUR_NET_IDENTITY);
    if (identity < 0x10000) return -1;
    const uint8_t flag = rd<uint8_t>(identity + NETIDENTITY_IS_LOCAL_PLAYER);
    if (flag != 0 && flag != 1) return -1;   // читается не флаг, а мусор
    return flag ? 1 : 0;
}

// Позиция камеры для сверки. Берём из матрицы вида: это та самая матрица, по
// которой игра рисует кадр, она всегда свежая (g_cam_pos пишет тот путь,
// который на устройстве как раз и ломается).
static bool current_camera_position(Vec3& out) {
    if (g_native_camera) {
        const Mat4 view = rd_m4(g_native_camera + CAMERA_VIEW_MATRIX);
        Vec3 v{};
        if (camera_position_from_view(view, v) && vec3_is_finite(v)) { out = v; return true; }
    }
    if (g_frame_cam_basis_valid && vec3_is_finite(g_frame_cam_pos)) { out = g_frame_cam_pos; return true; }
    if (g_cam_pose_valid && vec3_is_finite(g_cam_pos)) { out = g_cam_pos; return true; }
    return false;
}

// Крутит ли этот MouseLook нашу камеру. Проверка не по адресу и не по игроку,
// а по геометрии: m_LookRoot — это трансформ-опора взгляда, камера висит на
// нём ребёнком, поэтому расстояние между ними — метры. У чужого игрока
// m_LookRoot стоит совсем в другом месте. Возврат: 1 — да, 0 — нет (проверено
// и не сошлось), -1 — сверить не удалось ( чтение сорвалось — тогда решают
// остальные проверки, как раньше).
// Мировая позиция трансформа по ЕГО СОБСТВЕННЫМ массивам (данные +0x38,
// индекс +0x40 — подтверждено кодом Unity). read_transform_hierarchy_position()
// для этого не годится: она первой пробует раскладку, выученную по игрокам, и
// для объекта камеры та уже устарела — оттого расстояние до камеры в логе
// 19.09 всегда было «-1» (проверка молчала и никого не отвергала).
static bool transform_position_direct(uint64_t transform, Vec3& out) {
    if (transform < 0x10000) return false;
    const uint64_t data = rd_ptr(transform + TRANSFORM_ACCESS_HIERARCHY);
    const int32_t  idx  = rd<int32_t>(transform + 0x40);
    if (!data || idx < 0 || idx > 100000) return false;
    static const uint64_t kPairs[][2] = {{0x18, 0x20}, {0x08, 0x10}, {0x10, 0x18}, {0x20, 0x28}};
    for (const auto& pr : kPairs) {
        const uint64_t m0 = rd_ptr(data + pr[0]);
        const uint64_t n0 = rd_ptr(data + pr[1]);
        if (!m0 || !n0) continue;
        const uint64_t ms[2] = {m0, rd_ptr(m0)};
        const uint64_t ns[2] = {n0, rd_ptr(n0)};
        for (uint64_t m : ms)
            for (uint64_t n : ns) {
                Vec3 p{};
                if (!m || !n) continue;
                if (!read_transform_hierarchy_arrays(m, n, idx, p)) continue;
                if (!vec3_is_finite(p)) continue;
                out = p;
                return true;
            }
    }
    return false;
}

static int mouse_look_camera_distance(uint64_t mouse_look, float& dist) {
    dist = -1.f;
    if (mouse_look < 0x10000) return -1;
    const uint64_t root = rd_ptr(mouse_look + MOUSELOOK_LOOK_ROOT);
    if (root < 0x10000) return -1;
    Vec3 p{}, cam{};
    if (!transform_position_direct(root, p)) return -1;
    if (!vec3_is_finite(p)) return -1;
    if (!current_camera_position(cam)) return -1;
    const float dx = p.x - cam.x, dy = p.y - cam.y, dz = p.z - cam.z;
    dist = sqrtf(dx * dx + dy * dy + dz * dz);
    return dist <= 8.0f ? 1 : 0;
}

// MouseLook, найденный от камеры, а не от игрока: идём по компонентам
// GameObject камеры и ищем объект того же класса. Зачем: путь «GameController
// -> локальный игрок -> PlayerManager.mouseLook» молча отдаёт объект ЧУЖОГО
// игрока, если адрес игрока успел смениться (лог 19.09: за полчаса 17 разных
// MouseLook, ни одного отказа), а камера — она одна, и привязка к ней не
// зависит от того, кого игра сейчас считает локальным игроком.
static uint64_t mouse_look_from_camera() {
    if (!g_native_camera) return 0;
    const uint64_t gameobject = rd_ptr(g_native_camera + CAMERA_NATIVE_TRANSFORM);
    if (gameobject < 0x10000) return 0;
    const uint64_t data  = rd_ptr(gameobject + GAMEOBJECT_COMPONENT_ARRAY);
    const int32_t  count = rd<int32_t>(gameobject + GAMEOBJECT_COMPONENT_COUNT);
    if (data < 0x10000) return 0;
    const int n = (count > 0 && count <= 64) ? count : 8;
    const uint64_t known = g_mouse_look_klass.load();
    // Класс должен быть уже выучен (по пути игрока). Без этого условие
    // вырождается в «указатель похож на указатель», и на камере находится
    // ЛЮБОЙ компонент: в логе 19.09 так нашёлся объект с чувствительностью
    // 30.577 и инверсией — не MouseLook, а что-то рядом, и углы мы писали
    // туда (отсюда «мемори не находит MouseLook» при живом объекте).
    if (!known) return 0;
    for (int i = 0; i < n; ++i) {
        const uint64_t component =
            rd_ptr(data + (uint64_t)i * GAMEOBJECT_COMPONENT_STRIDE + COMPONENT_PAIR_PTR);
        if (component < 0x10000 || component == gameobject) continue;
        if (rd_ptr(component + COMPONENT_GAMEOBJECT) != gameobject) continue;  // чужой слот
        const uint64_t klass = rd_ptr(component);
        if (known ? (klass != known) : (klass < 0x10000)) continue;
        const float sens = rd<float>(component + MOUSE_LOOK_SENSITIVITY_OFFSET);
        if (!std::isfinite(sens) || sens < 0.05F || sens > 100.0F) continue;
        float angles[2] = {};
        if (!rd_buf(component + MOUSELOOK_ANGLES, angles, sizeof(angles))) continue;
        if (!std::isfinite(angles[0]) || !std::isfinite(angles[1])) continue;
        if (fabsf(angles[0]) > 360.0F || fabsf(angles[1]) > 360.0F) continue;
        return component;
    }
    return 0;
}

// Откуда взят текущий MouseLook и что показали проверки. Пишется в ту же
// строку лога, что и углы: по одной строке видно и объект, и то, почему он
// считается своим.
static uint64_t g_look_player_addr = 0; // игрок, из которого взят объект
static int      g_look_source = -1;     // 0 — игрок, 1 — камера
static int      g_look_local  = -1;     // isLocalPlayer: 1/0/-1
static float    g_look_cam_dist = -1.f; // расстояние m_LookRoot..камера, м

// Последний подтверждённый MouseLook. Зачем. Игрок на устройстве терялся
// кадр через кадр (лог 17.09: «объект=0/1» по двадцать раз подряд), и каждый
// такой кадр аим молчал — на улице это выглядит как «мемори-аим отвалился на
// несколько секунд». Причины две: чтение сорвалось посередине кадра игры
// (память читается процессом, который в этот момент обновляет объект) и
// короткие переходы состояния. Обе пережидаемы, поэтому объект держим.
static uint64_t s_ml_cached = 0;         // сам MouseLook
static uint64_t s_ml_cached_player = 0;  // игрок, которому он принадлежал
static double   s_ml_cached_at = -1e9;
// Сколько верим кешу. Дольше — опаснее: после респавна объект мог быть
// переиспользован под что-то другое, и писать туда нельзя. Шесть секунд
// перекрывают паузы из лога устройства (объект пропадал на 0.5–1.5 с), а
// проверка класса и чувствительности каждый кадр страхует от подмены.
static constexpr double kMlCacheSec = 6.0;

// Самолечение выученного класса. g_mouse_look_klass учится один раз и потом
// служит пропуском: писать можно только в объект ровно этого класса. Но если
// выучился мусор (чтение сорвалось в момент обучения) или объект подменился
// (респавн, техника, Spectator), все дальнейшие проверки идут против чужого
// класса, и память-аим молчит до перезапуска игры — ровно то, на что жалуются:
// «теряет MouseLook и перестаёт работать на какое-то время». Поэтому если
// чужой класс идёт дольше двух секунд, забываем его и учим заново.
static double s_look_mismatch_since = 0.0;
static void look_mismatch(bool mismatch) {
    const double now = memio::now_seconds();
    if (!mismatch) { s_look_mismatch_since = 0.0; return; }
    if (!s_look_mismatch_since) { s_look_mismatch_since = now; return; }
    if (now - s_look_mismatch_since > 2.0 && g_mouse_look_klass.load()) {
        g_mouse_look_klass.store(0);
        s_look_mismatch_since = 0.0;
        LogLine("память: класс MouseLook забыт — учу заново");
    }
}

// Кеш годится, только если игрок тот же самый и объект живой: класс на месте
// и m_Sensitivity в правдоподобных пределах.
//
// Важно, что чтение, сорвавшееся В НОЛЬ, кеш не сбрасывает. Раньше любой
// нулевой указатель выбрасывал объект навсегда (до следующего удачного
// поиска), а сбой чтения происходит ровно в те же моменты, что и потеря
// объекта, — то есть кеш погибал первым же кадром, который должен был
// переждать. Сбрасываем только когда прочитался ЧУЖОЙ класс или заведомо
// неправдоподобная чувствительность: это уже не сбой чтения, а подмена.
static bool mouse_look_from_cache(uint64_t player, uint64_t& out) {
    if (!s_ml_cached) return false;
    if (player && player != s_ml_cached_player) return false;
    if (memio::now_seconds() - s_ml_cached_at > kMlCacheSec) return false;
    const uint64_t klass = rd_ptr(s_ml_cached);
    if (!klass) return false;                       // чтение сорвалось — кеш жив
    const uint64_t known = g_mouse_look_klass.load();
    if (known ? (klass != known) : (klass < 0x10000)) {
        look_mismatch(true);
        s_ml_cached = 0;
        return false;
    }
    const float sens = rd<float>(s_ml_cached + MOUSE_LOOK_SENSITIVITY_OFFSET);
    if (!std::isfinite(sens)) return false;         // тоже сбой чтения
    if (sens < 0.05F || sens > 100.0F) {
        look_mismatch(true);
        s_ml_cached = 0;
        return false;
    }
    // Тот ли это игрок. Класс и чувствительность совпали — но объект мог
    // остаться от умершего игрока, чья память уже переиспользована. Сверяем
    // владельца (m_EventHandler -> manager) с тем игроком, от которого объект.
    const uint64_t owner = mouse_look_owner(s_ml_cached);
    const uint64_t known_player = player ? player : s_ml_cached_player;
    if (owner && known_player && owner != known_player) {
        look_mismatch(true);
        s_ml_cached = 0;
        return false;
    }
    look_mismatch(false);
    out = s_ml_cached;
    return true;
}

// Объект взят из кеша, а не найден заново: в лог пишем не чаще раза в 5 с,
// потому что при долгой паузе это событие идёт каждый кадр.
// Причина передаётся номером, а не строкой: русские литералы, висящие не
// в самом вызове LogLine, проверка переводов считает подписями визуалов и
// требует для них перевод (см. tools/lang/gen_tables.py).
static void log_look_cached(int reason, uint64_t mouse_look) {
    static double s_last = -1e9;
    static int s_last_reason = -1;
    const double now = memio::now_seconds();
    if (now - s_last < 5.0 && reason == s_last_reason) return;
    s_last = now; s_last_reason = reason;
    switch (reason) {
        case 0: LogLine("память: MouseLook=0x%llx взят из кеша — игрок не подтверждён", (unsigned long long)mouse_look); break;
        case 1: LogLine("память: MouseLook=0x%llx взят из кеша — адрес игрока не той структуры", (unsigned long long)mouse_look); break;
        case 2: LogLine("память: MouseLook=0x%llx взят из кеша — объект не читается", (unsigned long long)mouse_look); break;
        case 4: LogLine("память: MouseLook=0x%llx взят из кеша — найденный объект чужой", (unsigned long long)mouse_look); break;
        case 5: LogLine("память: MouseLook=0x%llx взят из кеша — игрок не локальный", (unsigned long long)mouse_look); break;
        case 6: LogLine("память: MouseLook=0x%llx взят из кеша — объект не у камеры", (unsigned long long)mouse_look); break;
        default: LogLine("память: MouseLook=0x%llx взят из кеша — объект чужого класса", (unsigned long long)mouse_look); break;
    }
}

static bool resolve_local_mouse_look(uint64_t& out, LookFail* why = nullptr) {
    if (g_pid <= 0 || !g_il2cpp_base || !g_mem.bound()) return false;

    // 1) MouseLook от камеры. Первым стоит именно этот путь: он не зависит от
    // того, кого игра сейчас считает локальным игроком, а адрес локального
    // игрока на устройстве менялся 17 раз за полчаса (лог 19.09) — и каждый
    // такой переезд отдавал чужой MouseLook, из-за чего память-аим писал
    // углы в объект, камеру не крутящий (записи шли, отклика не было).
    float cam_dist = -1.f;
    uint64_t from_camera = mouse_look_from_camera();
    if (from_camera) {
        const int drives = mouse_look_camera_distance(from_camera, cam_dist);
        if (drives < 0) {                 // сверить не удалось — объект берём,
            g_look_source = 1;            // но в логе это будет видно
            g_look_local = -1;
            g_look_player_addr = 0;
            g_look_cam_dist = cam_dist;
            look_mismatch(false);
            s_ml_cached = from_camera;
            s_ml_cached_player = 0;
            s_ml_cached_at = memio::now_seconds();
            out = from_camera;
            return true;
        }
        if (drives == 0) {                // найден, но его m_LookRoot не у камеры
            log_look_wrong_camera(from_camera, cam_dist);
            from_camera = 0;
        } else {
            g_look_source = 1;
            g_look_local = -1;
            g_look_player_addr = 0;
            g_look_cam_dist = cam_dist;
            look_mismatch(false);
            s_ml_cached = from_camera;
            s_ml_cached_player = 0;
            s_ml_cached_at = memio::now_seconds();
            out = from_camera;
            return true;
        }
    }

    uint64_t player = resolve_local_player();
    if (!player) {
        // Игрок на этом кадре не подтвердился (статика GameController не
        // прочиталась или объект пересоздаётся). Раньше тут был немедленный отказ,
        // и ровно на этих кадрах память-аим молчал — это и есть «отваливается
        // на несколько секунд» из жалобы. MouseLook мы и так проверяем отдельно
        // (класс + чувствительность), поэтому адрес игрока для записи не нужен:
        // он был нужен только чтобы этот MouseLook найти.
        if (why) *why = LookFail::noPlayer;
        if (mouse_look_from_cache(s_ml_cached_player, out)) {
            log_look_cached(0, out);
            return true;
        }
        return false;
    }
    if (g_player_manager_class && rd_ptr(player) != g_player_manager_class) {
        if (why) *why = LookFail::wrongPlayer;
        if (mouse_look_from_cache(player, out)) { log_look_cached(1, out); return true; }
        return false;
    }
    uint64_t mouse_look = rd_ptr(player + PLAYER_MOUSE_LOOK_OFFSET);
    if (mouse_look < 0x10000) {  // ноль или мусор вместо указателя
        if (why) *why = LookFail::noObject;
        if (mouse_look_from_cache(player, out)) { log_look_cached(2, out); return true; }
        return false;
    }
    const uint64_t klass = rd_ptr(mouse_look);
    const uint64_t known = g_mouse_look_klass.load();
    if (known ? (klass != known) : (klass < 0x10000)) {
        if (why) *why = LookFail::noObject;
        if (mouse_look_from_cache(player, out)) { log_look_cached(3, out); return true; }
        return false;
    }
    // Свежий объект проверяем строже кеша: он только что найден по указателю,
    // который мог остаться от умершего игрока. Владелец известен и не совпал —
    // это чужой MouseLook, писать в него углы нельзя (повернём камеру не тому).
    const uint64_t owner = mouse_look_owner(mouse_look);
    if (owner && player && owner != player) {
        if (why) *why = LookFail::wrongOwner;
        look_mismatch(true);
        log_look_wrong_owner(mouse_look, owner, player);
        if (mouse_look_from_cache(player, out)) { log_look_cached(4, out); return true; }
        return false;
    }
    // Признак Mirror: isLocalPlayer. В отличие от владельца он читается у
    // любого игрока, поэтому ловит как раз тот случай, от которого проверка
    // владельца молчала: чужой MouseLook с пустым m_EventHandler.
    const int local = player_is_local(player);
    if (local == 0) {
        if (why) *why = LookFail::wrongOwner;
        look_mismatch(true);
        log_look_not_local(player, mouse_look);
        if (mouse_look_from_cache(player, out)) { log_look_cached(5, out); return true; }
        return false;
    }
    // И последняя сверка — геометрией: m_LookRoot обязан стоять у камеры.
    float d = -1.f;
    const int drives = mouse_look_camera_distance(mouse_look, d);
    if (drives == 0) {
        if (why) *why = LookFail::wrongOwner;
        look_mismatch(true);
        log_look_wrong_camera(mouse_look, d);
        if (mouse_look_from_cache(player, out)) { log_look_cached(6, out); return true; }
        return false;
    }
    g_look_source = 0;
    g_look_player_addr = player;
    g_look_local = local;
    g_look_cam_dist = d;
    look_mismatch(false);
    s_ml_cached = mouse_look;
    s_ml_cached_player = player;
    s_ml_cached_at = memio::now_seconds();
    out = mouse_look;
    return true;
}


bool esp_mem_aim_look_params(float& deg_per_unit, bool& invert_y, bool& gyro) {
    uint64_t mouse_look = 0;
    LookFail why = LookFail::none;
    if (!resolve_local_mouse_look(mouse_look, &why)) {
        log_look_fail(why, mouse_look, 0.f);
        return false;
    }
    const float value = rd<float>(mouse_look + MOUSE_LOOK_SENSITIVITY_OFFSET);
    if (!std::isfinite(value) || value < 0.05F || value > 100.0F) {
        look_mismatch(true);
        log_look_fail(LookFail::badSens, mouse_look, value);
        return false;
    }
    deg_per_unit = value;
    invert_y     = rd<uint8_t>(mouse_look + MOUSELOOK_INVERT) != 0;
    gyro         = rd_ptr(mouse_look + MOUSELOOK_GYRO) >= 0x10000;
    // Структура сошлась — запоминаем класс: с этого момента писать можно
    // только в объект ровно этого класса. Мусор в этом чтении не страшен:
    // ноль не пройдёт проверку выше, а чужой ненулевой адрес забудется
    // самолечением через две секунды (см. look_mismatch).
    g_mouse_look_klass.store(rd_ptr(mouse_look));
    return true;
}


// Накопитель углов: +0x4C rotationX (вверх — МЕНЬШЕ), +0x50 rotationY (вправо
// — больше). Значения всегда в пределах обзора игры: их же клампит сам ZJo,
// поэтому выход за +-360 означает, что читаем не тот объект.
// Класс MouseLook, о котором уже писали в лог (адрес меняется на респавне).
static std::atomic<uint64_t> g_look_addr_logged{0};

// Куда именно писать угол — вопрос, на который статический разбор ответа не
// даёт. По дизассемблеру Oxide.MouseLook (libil2cpp.so релиза):
//   * Update() само правит только рысканье: [+0x50] = [+0x50] + CEm();
//   * CEf() читает накопитель [+0x4C], прибавляет дельту (ввод x чувствительность),
//     клампит и раскладывает в поворот m_LookRoot — то есть запись в +0x4C
//     правильная и она survives;
//   * но VR/CEO/ZBV (обработчики ввода вида, вызываются системой ввода через
//     интерфейс, статических вызовов нет) КЛАДУТ в +0x4C значение из своего
//     аргумента, а не прибавляют к прочитанному. Пока палец на экране, они
//     идут каждый кадр — и тогда наша запись стирается прежде, чем сыграет.
// Поэтому раз в секунду печатаем все четыре величины: какая из них совпадает
// с углами камеры, та и есть настоящий рычаг.
static double   s_look_fields_at = -1e9;
// Держится ли наша запись до следующего кадра игры.
static bool     s_look_wrote = false;
static uint64_t s_look_wrote_addr = 0;
static float    s_look_wrote_x = 0.f, s_look_wrote_y = 0.f;
static double   s_look_wipe_at = -1e9;

// Накопитель MouseLook — это НЕ завёрнутый угол, а сумма всего ввода: игра
// заворачивает его сама, уже применяя к повороту камеры, а хранит как есть.
// Лог 20:45 показывает это прямо: накопитель=(0.81, 291.10) при камере
// (-0.81, -69.69), то есть 291.10 − 360 = −68.9 — и есть поворот камеры;
// то же на (−185.69 → 174.58) и на (−237.85 → 123.92).
//
// Проверка «больше 360 — значит читаем не тот объект» из-за этого была
// ошибкой: стоило игроку докрутить взгляд за пол-оборота, как аим слепнул.
// Лог 20:45:48–20:45:51: три секунды подряд «чтение=15 в кадр», ни одной
// записи при живом MouseLook (объект=1) и включённом прицеле — ровно то, что
// в жалобе названо «иногда не стреляет по цели».
//
// Поэтому обе грани — и чтение, и запись — работают с завёрнутым значением:
// поворот камеры от этого не меняется (разница ровно на 360°), зато мы
// остаёмся внутри любого клампа игры и не теряем объект из-за собственной
// проверки.
// Значения накопителя, которые мы всё-таки отвергли: миллионы градусов — это
// уже не накопитель, а мусор вместо объекта. Печатаем, чтобы отличие от
// законных ±360 было видно, а не угадывалось.
static void log_angles_out_of_range(uint64_t mouse_look, float x, float y) {
    static double s_last = -1e9;
    const double now = memio::now_seconds();
    if (now - s_last < 2.0) return;
    s_last = now;
    LogLine("память: MouseLook=0x%llx — накопитель не angles (%.1f, %.1f)",
            (unsigned long long)mouse_look, (double)x, (double)y);
}

static float wrap_deg180(float v) {
    if (!std::isfinite(v)) return v;
    float r = fmodf(v + 180.0F, 360.0F);
    if (r < 0.0F) r += 360.0F;
    return r - 180.0F;
}

bool esp_mem_aim_read_angles(float& x_deg, float& y_deg) {
    uint64_t mouse_look = 0;
    if (!resolve_local_mouse_look(mouse_look)) return false;
    float angles[2] = {};
    if (!rd_buf(mouse_look + MOUSELOOK_ANGLES, angles, sizeof(angles))) return false;
    if (!std::isfinite(angles[0]) || !std::isfinite(angles[1])) return false;
    // Широкая граница вместо прежних ±360: она ловит мусор вместо указателя
    // (миллионы градусов), а не законный накопитель в триста-четыреста.
    if (fabsf(angles[0]) > 1e6F || fabsf(angles[1]) > 1e6F) {
        log_angles_out_of_range(mouse_look, angles[0], angles[1]);
        return false;
    }
    x_deg = wrap_deg180(angles[0]);
    y_deg = wrap_deg180(angles[1]);
    // Наша предыдущая запись: игра должна была развернуть её в поворот камеры
    // в своём же кадре. Если к следующему чтению в накопителе уже другое число,
    // запись стёрта — и писать туда бессмысленно.
    if (s_look_wrote && s_look_wrote_addr == mouse_look) {
        s_look_wrote = false;
        const bool wiped = fabsf(angles[0] - s_look_wrote_x) > 0.01f ||
                           fabsf(angles[1] - s_look_wrote_y) > 0.01f;
        const double now = memio::now_seconds();
        if (wiped && now - s_look_wipe_at >= 2.0) {
            s_look_wipe_at = now;
            LogLine("память: запись стёрта игрой — писали (%.2f, %.2f), читается (%.2f, %.2f)",
                    (double)s_look_wrote_x, (double)s_look_wrote_y,
                    (double)angles[0], (double)angles[1]);
        }
    }
    // Все поля раз в секунду: ищем то, что совпадает с углами камеры.
    {
        const double now = memio::now_seconds();
        if (now - s_look_fields_at >= 1.0) {
            s_look_fields_at = now;
            float kqq[2] = {}, input[2] = {};
            rd_buf(mouse_look + 0x70, kqq, sizeof(kqq));
            rd_buf(mouse_look + 0x88, input, sizeof(input));
            float cam_yaw = 0.f, cam_pitch = 0.f;
            const bool have_cam = esp_aim_camera_angles(cam_yaw, cam_pitch);
            LogLine("память: поля MouseLook=0x%llx накопитель=(%.2f, %.2f) KQQ=(%.2f, %.2f) ввод=(%.3f, %.3f) камера=%d (%.2f, %.2f)",
                    (unsigned long long)mouse_look,
                    (double)angles[0], (double)angles[1],
                    (double)kqq[0], (double)kqq[1],
                    (double)input[0], (double)input[1],
                    (int)have_cam, (double)cam_pitch, (double)cam_yaw);
        }
    }
    if (mouse_look != g_look_addr_logged.exchange(mouse_look))
        LogLine("память: MouseLook=0x%llx углы=(%.2f, %.2f) чувствительность=%.3f инверсия=%d гироскоп=%d источник=%d игрок=0x%llx локальный=%d до камеры=%.1f м",
                (unsigned long long)mouse_look, angles[0], angles[1],
                (double)rd<float>(mouse_look + MOUSE_LOOK_SENSITIVITY_OFFSET),
                (int)(rd<uint8_t>(mouse_look + MOUSELOOK_INVERT) != 0),
                (int)(rd_ptr(mouse_look + MOUSELOOK_GYRO) >= 0x10000),
                g_look_source, (unsigned long long)g_look_player_addr,
                g_look_local, (double)g_look_cam_dist);
    return true;
}

bool esp_mem_aim_write_angles(float x_deg, float y_deg) {
    uint64_t mouse_look = 0;
    if (!resolve_local_mouse_look(mouse_look)) return false;
    if (!std::isfinite(x_deg) || !std::isfinite(y_deg)) return false;
    if (fabsf(x_deg) > 1e6F || fabsf(y_deg) > 1e6F) return false;
    // Заворачиваем и здесь: иначе следующий же кадр упрётся в накопитель за
    // ±360 и снова ослепнет (см. wrap_deg180 выше).
    const float xw = wrap_deg180(x_deg), yw = wrap_deg180(y_deg);
    float angles[2] = {xw, yw};
    const bool ok = wr_buf(mouse_look + MOUSELOOK_ANGLES, angles, sizeof(angles));
    if (ok) {
        s_look_wrote = true;
        s_look_wrote_addr = mouse_look;
        s_look_wrote_x = x_deg;
        s_look_wrote_y = y_deg;
    }
    return ok;
}

// Ось выстрела: PlayerManager.playerEventHandler (+0x78, класс Gum) ->
// LookDirection (+0x140) -> значение Vector3 (+0x20, обёртка синхронизации).
// MouseLook.Update каждый кадр пишет сюда forward камеры (ZJo, RVA 0x64e35a4:
// приёмник — [this+0x20][0x140], значение — s0/s1/s2 от Transform.get_forward),
// а FPHitscan пускает луч попадания ровно вдоль этого вектора.
static bool resolve_event_handler(uint64_t& out_handler) {
    if (g_pid <= 0 || !g_il2cpp_base || !g_mem.bound()) return false;
    uint64_t player = resolve_local_player();
    if (!player) return false;
    uint64_t handler = rd_ptr(player + PLAYER_EVENT_HANDLER);
    if (!handler || rd_ptr(handler + EVENT_HANDLER_MANAGER_BACKREF) != player) return false;
    out_handler = handler;
    return true;
}

static bool resolve_look_direction(uint64_t& out) {
    uint64_t handler = 0;
    if (!resolve_event_handler(handler)) return false;
    uint64_t look = rd_ptr(handler + EVENT_HANDLER_LOOK_DIRECTION);
    if (look < 0x10000) return false;
    out = look + SYNC_VALUE_OFFSET;
    return true;
}

bool esp_mem_aim_last_shot(float& time_sec) {
    uint64_t handler = 0;
    if (!resolve_event_handler(handler)) return false;
    const uint64_t sv = rd_ptr(handler + EVENT_HANDLER_LAST_HIT_TIME);
    if (sv < 0x10000) return false;
    const float v = rd<float>(sv + SYNC_VALUE_OFFSET);
    if (!std::isfinite(v)) return false;
    time_sec = v;
    return true;
}

// Цепь оси выстрела, о которой уже писали в лог: адрес меняется на респавне и
// при смене мира, и это первое, что стоит проверить, когда «сайлент молчит».
static std::atomic<uint64_t> g_axis_addr_logged{0};

static void log_axis_chain(uint64_t addr, const float* dir) {
    if (addr == g_axis_addr_logged.load()) return;
    g_axis_addr_logged.store(addr);
    if (!addr) { LogLine("сайлент: цепь оси потеряна (нет игрока, хендлера или оси)"); return; }
    if (!dir) { LogLine("сайлент: цепь оси адрес=0x%llx", (unsigned long long)addr); return; }
    const float len = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    LogLine("сайлент: цепь адрес=0x%llx значение=(%.4f, %.4f, %.4f) длина=%.4f",
            (unsigned long long)addr, dir[0], dir[1], dir[2], len);
}

bool esp_mem_aim_read_fire_dir(float& x, float& y, float& z) {
    uint64_t addr = 0;
    if (!resolve_look_direction(addr)) { log_axis_chain(0, nullptr); return false; }
    LogStage(kStageAimRead);
    const Vec3 dir = rd_v3(addr);
    if (!vec3_is_finite(dir)) return false;
    const float len = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    // Игра держит тут единичный вектор; всё остальное — не ось, а мусор.
    if (!(len > 0.5F && len < 2.0F)) return false;
    x = dir.x / len; y = dir.y / len; z = dir.z / len;
    {
        const float d[3] = {x, y, z};
        log_axis_chain(addr, d);
    }
    return true;
}

uint64_t Il2CppBase() { return g_il2cpp_base; }

bool esp_mem_aim_write_fire_dir(float x, float y, float z) {
    uint64_t addr = 0;
    if (!resolve_look_direction(addr)) return false;
    const float len = sqrtf(x * x + y * y + z * z);
    if (!std::isfinite(len) || len < 0.001F) return false;
    const float dir[3] = {x / len, y / len, z / len};
    LogStage(kStageAimWrite);
    const bool ok = wr_buf(addr, dir, sizeof(dir));
    if (!ok) {
        // Отказ записи — редкость (доступ отобрали или адрес умер); пишем, но
        // не чаще раза в секунду, чтобы не забить лог.
        static double last = 0.0;
        const double now = memio::now_seconds();
        if (now - last > 1.0) {
            last = now;
            LogLine("сайлент: запись оси не прошла, адрес=0x%llx errno=%d",
                    (unsigned long long)addr, errno);
        }
    }
    return ok;
}

// Писателя-доминатора оси выстрела (фонового потока, который добивал ось
// между кадрами) больше нет: ось пишется один раз за кадр оверлея из потока
// отрисовки, и этой записью кадр игры и закрывается.
//
// Почему убрали. Доминатор давал выигрыш в несколько кадров из сотни, а плата
// за него — зависший оверлей. Второй поток лез в память игры через общий
// /proc/<pid>/mem: тот же дескриптор, тот же кэш блоков, те же счётчики
// отказов, а на серии отказов — закрытие и переоткрытие файла прямо под
// чтениями потока отрисовки. Итог: игра идёт, а наш кадр встаёт (оверлей
// замирает, на тапы не отвечает). Проверено на устройстве 16-17.09.2026:
// период 2 мс — вставал сразу, 12 мс — через ~5 с работы сайлента.

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

// Базис камеры из матрицы вида. Строки поворота — это оси камеры в мировых
// координатах (Right, Up, и «назад»): камера смотрит вдоль -Z своей системы.
// Нужно, когда живая поза недоступна, а матрица вида есть: по ней тогда и
// позиция, и направления, — то есть ровно то, чем игра рисует кадр.
static bool camera_basis_from_view(const Mat4& view, Vec3& right, Vec3& up, Vec3& forward) {
    right   = {mat_get(view, 0, 0), mat_get(view, 0, 1), mat_get(view, 0, 2)};
    up      = {mat_get(view, 1, 0), mat_get(view, 1, 1), mat_get(view, 1, 2)};
    forward = {-mat_get(view, 2, 0), -mat_get(view, 2, 1), -mat_get(view, 2, 2)};
    if (!vec3_is_finite(right) || !vec3_is_finite(up) || !vec3_is_finite(forward)) return false;
    const float rl = sqrtf(right.x * right.x + right.y * right.y + right.z * right.z);
    const float ul = sqrtf(up.x * up.x + up.y * up.y + up.z * up.z);
    const float fl = sqrtf(forward.x * forward.x + forward.y * forward.y + forward.z * forward.z);
    if (!(rl > 1e-4F && ul > 1e-4F && fl > 1e-4F)) return false;
    right = {right.x / rl, right.y / rl, right.z / rl};
    up = {up.x / ul, up.y / ul, up.z / ul};
    forward = {forward.x / fl, forward.y / fl, forward.z / fl};
    return true;
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

bool w2s(const Mat4& vp, const Vec3& world, float sw, float sh, Vec2& out, bool clip_to_screen) {
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

// Массивы камеры, найденные проверенным путём (resolve_camera_arrays). Держим
// их: пара «данные/индекс» у трансформа камеры постоянна, пока жив мир, а
// искать её заново каждый кадр — это четыре чтения и разбор иерархии на каждом
// кадре оверлея.
static uint64_t s_cam_arr_m = 0, s_cam_arr_n = 0;
static int32_t  s_cam_arr_idx = -1;
static bool     s_cam_arr_valid = false;
static double   s_cam_arr_at = -1e9;

// Поза камеры, сверенная с матрицей вида.
//
// Почему нельзя читать позу как попало (read_camera_transform_pose): она
// первой пробует раскладку, ВЫУЧЕННУЮ ПО ИГРОКАМ, — и для камеры та годилась
// ровно до первого переезда: массивы подменяются, а раскладка остаётся старой.
// Тогда поза замирает в точке прошлого мира, и по ней строится вся проекция
// (боксы, метки, углы цели), пока картинка игры рисуется по другой камере.
// Отсюда и «маятник» аима: точка цели считалась по одной камере, а поворот —
// по другой (жалоба 19.09).
//
// Здесь поза берётся из массивов, найденных по самой камере, и каждый кадр
// сверяется с позицией из матрицы вида: разошлись — массивы устарели, ищем
// заново. Во фрикаме сверку отключаем: там камера НАРОЧНО не там, где её
// оставила игра, и матрица вида за ней просто не поспевает.
static bool camera_pose_checked(Vec3& position, Vec4& rotation, const Vec3& ref, bool verify) {
    if (!s_cam_arr_valid) {
        Vec3 cp{};
        float err = -1.f;
        if (!resolve_camera_arrays(s_cam_arr_m, s_cam_arr_n, s_cam_arr_idx, cp, err)) return false;
        s_cam_arr_valid = true;
        s_cam_arr_at = memio::now_seconds();
    }
    Vec3 p{};
    Vec4 r{};
    if (!read_transform_hierarchy_arrays(s_cam_arr_m, s_cam_arr_n, s_cam_arr_idx, p, &r)) {
        s_cam_arr_valid = false;
        return false;
    }
    if (!vec3_is_finite(p)) { s_cam_arr_valid = false; return false; }
    if (verify) {
        const float dx = p.x - ref.x, dy = p.y - ref.y, dz = p.z - ref.z;
        const float d2 = dx * dx + dy * dy + dz * dz;
        // Два метра: камера успевает сдвинуться между двумя чтениями, а
        // подмена массивов даёт десятки метров.
        if (d2 > 2.0F * 2.0F) {
            s_cam_arr_valid = false;
            return false;
        }
    }
    position = p;
    rotation = r;
    return true;
}

static bool read_native_camera_matrices(uint64_t native_cam, float screen_aspect, Mat4& projection, Mat4& view) {
    if (!native_cam) return false;

    static Mat4 s_last_view{};
    static Mat4 s_last_proj{};
    static bool s_last_ok = false;

    // Матрица вида из кеша камеры (+0x70) — эталон: по ней игра и рисует кадр.
    // Живая поза берётся только если она с этим эталоном согласна.
    const Mat4 cached_view = rd_m4(native_cam + CAMERA_VIEW_MATRIX);
    Vec3 ref_pos{};
    const bool have_ref = matrix_is_finite(cached_view) && camera_position_from_view(cached_view, ref_pos) &&
                          vec3_is_finite(ref_pos);

    bool have_live_view = false;
    uint64_t native_transform = camera_transform_of(native_cam);
    if (native_transform && have_ref) {
        Vec3 cam_pos{};
        Vec4 cam_rot{};
        bool pose_ok = camera_pose_checked(cam_pos, cam_rot, ref_pos, !g_freecam_on);
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
        if (have_ref) {
            view = cached_view;          // эталон: ровно то, чем рисует игра
        } else if (s_last_ok && matrix_is_finite(s_last_view)) {
            view = s_last_view;
        } else {
            return false;
        }
        // Поза берётся из той же матрицы: иначе в g_cam_pos остаётся поза
        // прошлого мира (её писал живой путь), и по ней строятся и боксы, и
        // углы цели, пока картинка рисуется по другой камере.
        Vec3 r{}, u{}, f{};
        if (have_ref && camera_basis_from_view(cached_view, r, u, f)) {
            g_cam_pos = ref_pos; g_cam_right = r; g_cam_up = u; g_cam_forward = f;
            g_cam_pose_valid = true;
            g_cam_pose_derived = true;
        }
    } else {
        g_cam_pose_derived = false;
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

// Кадры подряд, в которых выборка позиций игроков сошлась в одну точку (см.
// optimize_matrix_configuration): одному кадру столько же доверия, сколько
// серии — нельзя, за приговором следует перепоиск смещения с ожиданием 0.6 с.
static int g_offset_extent_fail_streak = 0;

static bool optimize_matrix_configuration(uint64_t native_camera, const std::vector<uint64_t>& transforms) {
    std::vector<Vec3> samples;
    for (uint64_t source : transforms) {
        Vec3 position{};
        if (!read_entity_position(source, position)) continue;
        // Нули после респауна и мусор из пула — не мировые позиции. Раньше они
        // попадали в выборку, сходились в одну точку и объявляли смещение
        // неверным: дальше шёл поиск смещения заново с ожиданием 0.6 с, и всё
        // это время боксов не было вовсе.
        if (!position_looks_like_world_space(position)) continue;
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
    //
    // Столпившиеся игроки и мигнувшее чтение выглядят одинаково, поэтому
    // приговор смещению выносим только по серии кадров: одиночный кадр
    // с одинаковыми отсчётами раньше запускал поиск смещения заново вместе с
    // ожиданием 0.6 с, и всё это время боксов не было («мерцание на полсекунды»).
    if (samples.size() >= 2 && extent < 0.1F) {
        if (++g_offset_extent_fail_streak >= 3) {
            g_offset_extent_fail_streak = 0;
            g_player_position_validated = false;
        }
        return false;
    }
    g_offset_extent_fail_streak = 0;
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
    if (!list) return transforms;

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

TransformHierarchyLayout g_skeleton_layout{};
bool g_skeleton_layout_valid = false;

static uint64_t g_go_name_offset = 0;
static bool     g_go_name_plain_pointer = false; // fallback: name stored as raw char*
bool     g_go_name_offset_valid = false;
static double   g_go_name_retry_at = 0.0;   // mono_seconds: не раньше этого момента
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

void collect_transform_subtree(uint64_t root, std::vector<uint64_t>& nodes, size_t max_nodes) {
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
    CopyTextUtf8(out, cap, buffer);
    return true;
}

bool read_transform_name(uint64_t transform, char* out, size_t cap) {
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

uint64_t managed_object_native(uint64_t managed) {
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
uint64_t native_component_transform(uint64_t native_component) {
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
    // Последняя ПРИНЯТАЯ позиция и счётчики временного фильтра (см.
    // filter_player_position): бокс рисуется по ним, а не по сырому чтению.
    Vec3   drawn{};
    bool   has_drawn = false;
    double drawn_t = 0.0;
    int    hold_frames = 0;  // сколько кадров живём без успешного чтения
    Vec3   jump{};           // подозрительный отсчёт, ждущий подтверждения
    bool   has_jump = false;
    int    jump_frames = 0;
};

double mono_seconds() {
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

static float vec3_horiz2(const Vec3& a, const Vec3& b) {
    float dx = a.x - b.x, dz = a.z - b.z;
    return dx * dx + dz * dz;
}

static bool player_is_mounted(uint64_t player) {
    if (!player) return false;
    uint32_t vehicle = rd<uint32_t>(player + PLAYER_VEHICLE_ID);
    if (vehicle != 0 && vehicle != 0xFFFFFFFFu) return true;
    // Copters / some seats leave vehicleID at 0; seatID is still non-zero.
    uint32_t seat = rd<uint32_t>(player + PLAYER_SEAT_ID);
    return seat != 0 && seat != 0xFFFFFFFFu && seat < 32u;
}

static bool player_saved_position(uint64_t player, Vec3& out) {
    uint64_t off = g_player_position_offset ? g_player_position_offset : (uint64_t)PLAYER_POSITION;
    out = rd_v3(player + off);
    return vec3_is_finite(out) && position_looks_like_world_space(out);
}

// World position of the player's own transform (worldCameraRoot). While he is
// mounted this is the only source that moves with the vehicle.
static bool player_rendered_position(uint64_t player, Vec3& out) {
    uint64_t native = resolve_player_native_transform(player);
    if (!native) return false;
    if (g_skeleton_layout_valid && read_transform_hierarchy_layout(native, g_skeleton_layout, out)
        && vec3_is_finite(out) && position_looks_like_world_space(out))
        return true;
    return read_transform_hierarchy_position(native, out) && vec3_is_finite(out)
        && position_looks_like_world_space(out);
}

// Seated body: characterModel is parented to the seat even when worldCameraRoot
// is left behind at the boarding point.
static bool player_model_position(uint64_t player, Vec3& out) {
    uint64_t root = skeleton_model_root(player);
    if (!root) return false;
    if (g_skeleton_layout_valid && read_transform_hierarchy_layout(root, g_skeleton_layout, out)
        && vec3_is_finite(out) && position_looks_like_world_space(out))
        return true;
    return read_transform_hierarchy_position(root, out) && vec3_is_finite(out)
        && position_looks_like_world_space(out);
}

// worldCameraRoot sits at eye level, the box is built from the feet.
static constexpr float kCameraRootHeight = 1.60F;

// Mounted-state latch. lastSavedPosition freezes at the boarding point while
// the rendered transform (and/or the character model) rides with the vehicle.
// vehicleID / the transform also flicker mid-tick. Engage on seat/vehicle OR
// on lastSaved freeze + a live visual that has left the mount; never snap the
// box back to that mount mid-ride.
struct MountLatch {
    int  unmounted_streak = 0;
    int  saved_still = 0;
    bool engaged = false;
    Vec3 last{};
    bool last_ok = false;
    Vec3 prev_saved{};
    bool have_saved = false;
};
static std::unordered_map<uint64_t, MountLatch> g_mount_latch;

static bool player_mount_engaged(uint64_t player) {
    auto found = g_mount_latch.find(player);
    return found != g_mount_latch.end() && found->second.engaged;
}

static void apply_mounted_position(uint64_t player, Vec3& feet) {
    Vec3 saved{};
    if (!player_saved_position(player, saved)) saved = feet;

    bool id_mounted = player_is_mounted(player);
    if (g_mount_latch.size() > 256 && g_mount_latch.find(player) == g_mount_latch.end()) return;
    MountLatch& latch = g_mount_latch[player];

    if (latch.have_saved) {
        if (vec3_horiz2(saved, latch.prev_saved) < 0.15F * 0.15F) {
            if (latch.saved_still < 100000) ++latch.saved_still;
        } else {
            latch.saved_still = 0;
        }
    }
    latch.prev_saved = saved;
    latch.have_saved = true;

    // On foot the box already sits on lastSaved. Skip the transform walks
    // unless the seat says mounted, we are already riding, or lastSaved has
    // been frozen long enough that this may be a boarding (copter vehicleID
    // is often 0).
    if (!id_mounted && !latch.engaged && (latch.saved_still < 8 || (latch.saved_still & 7) != 0))
        return;

    Vec3 cam{};
    bool have_cam = player_rendered_position(player, cam);
    Vec3 model{};
    bool have_model = player_model_position(player, model);

    // Direct path builds the box from feet; hierarchy path treats the value as
    // eye height and subtracts 1.60 later. Keep whichever space `feet` is in.
    const bool ground = g_use_direct_player_position;
    Vec3 live{};
    bool have_live = false;
    float live_h2 = -1.0F;
    if (have_cam) {
        Vec3 p = cam;
        if (ground) p.y -= kCameraRootHeight;
        if (position_looks_like_world_space(p) || position_looks_like_world_space(cam)) {
            live = p;
            have_live = true;
            live_h2 = vec3_horiz2(p, saved);
        }
    }
    if (have_model) {
        Vec3 p = model;
        if (!ground) p.y += 0.90F; // hips -> roughly eye, matches hierarchy boxes
        float h2 = vec3_horiz2(p, saved);
        if (!have_live || h2 > live_h2 + 0.25F) {
            live = p;
            have_live = true;
            live_h2 = h2;
        }
    }
    if (!ground && !have_live && position_looks_like_world_space(feet)) {
        live = feet;
        have_live = true;
        live_h2 = vec3_horiz2(feet, saved);
    }

    bool pos_mounted = latch.saved_still >= 8 && have_live && live_h2 > 1.6F * 1.6F;
    bool keep_mounted = latch.engaged && have_live && live_h2 > 1.6F * 1.6F;
    if (id_mounted || pos_mounted || keep_mounted) {
        latch.engaged = true;
        latch.unmounted_streak = 0;
    } else if (latch.engaged) {
        if (++latch.unmounted_streak >= 45) {
            latch.engaged = false;
            latch.last_ok = false;
            latch.unmounted_streak = 0;
        }
    }
    if (!latch.engaged) return;

    if (have_live) {
        float last_h2 = latch.last_ok ? vec3_horiz2(latch.last, saved) : 0.0F;
        // Rendered transform snapped back to the frozen boarding point.
        if (latch.last_ok && live_h2 < 1.25F * 1.25F && last_h2 > 2.5F * 2.5F) {
            feet = latch.last;
            return;
        }
        // Same flicker, but the bad sample did not land exactly on lastSaved.
        if (latch.last_ok && last_h2 > 4.0F &&
            vec3_horiz2(live, latch.last) > 4.0F * 4.0F && live_h2 < last_h2 * 0.25F) {
            feet = latch.last;
            return;
        }
        latch.last = live;
        latch.last_ok = true;
        feet = live;
        return;
    }
    if (latch.last_ok) feet = latch.last;
}

static void prune_mount_latch(const std::vector<uint64_t>& players) {
    for (auto it = g_mount_latch.begin(); it != g_mount_latch.end();) {
        bool present = false;
        for (uint64_t player : players) if (player == it->first) { present = true; break; }
        if (!present) it = g_mount_latch.erase(it); else ++it;
    }
}

static PlayerTrack& track_player(uint64_t player, const Vec3& position) {
    PlayerTrack& track = g_player_track[player];
    if (--track.uid_recheck <= 0) {
        char uid[40] = {};
        // Перезаписываем кеш ТОЛЬКО успешным чтением. Безусловный memcpy
        // стирал userID после любого сбоя чтения, и на пару секунд дубли
        // объекта (копия после посадки в транспорт, остаток респауна)
        // переставали подавляться: второй бокс вспыхивал в стороне и пропадал.
        if (read_managed_string_ex(rd_ptr(player + PLAYER_USER_ID), uid, sizeof(uid), 39)) {
            memcpy(track.uid, uid, sizeof(track.uid));
            track.uid_recheck = 120; // ~2 s: pooled objects change owner
        } else {
            // Старый кеш не трогаем. Если кеша нет вовсе, пробуем быстрее.
            track.uid_recheck = track.uid[0] ? 120 : 20;
        }
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

// ---- Временной фильтр позиции ------------------------------------------------
// Два артефакта, которые видели как «визуалы мерцают и телепаются в другую
// сторону»:
//   1. один кадр без успешного чтения позиции — бокс пропадал и возвращался;
//   2. один мусорный отсчёт (позиция не дописана игрой, объект из пула чужой,
//      чтение попало между кадрами симуляции) — бокс улетал в сторону и на
//      следующем кадре возвращался обратно.
// Ни то ни другое не похоже на настоящее движение: позиция игрока меняется
// плавно, а телепорт/респаун держится в памяти и на следующем кадре тоже.
// Поэтому одиночный сбой чтения дорисовываем по последней принятой позиции
// (продолжая её по измеренной скорости), а подозрительный скачок рисуем
// только когда он подтверждается подряд несколькими кадрами.
//
// Допуск скачка взят с запасом: бег ~8 м/с, техника до ~50 м/с, а координаты
// чужих игроков приходят пачкой раз в ~0.1 с, так что законный «прыжок» между
// кадрами может быть в несколько метров. Всё, что больше, почти всегда мусор —
// и даже если это настоящий телепорт, мы отстанем от него на пару кадров.
static constexpr int   kPosHoldFrames        = 3;     // ~50 мс без чтения — ещё не пропажа
static constexpr int   kPosJumpConfirmFrames = 2;     // скачок должен повториться
static constexpr float kPosJumpMeters        = 3.0F;  // базовый допуск, метры
static constexpr float kPosJumpSpeed         = 55.0F; // плюс м/с на каждый кадр
static constexpr float kPosExtrapolateLimit  = 1.5F;  // насколько дорисовываем по скорости
static constexpr float kPosJumpSameSpot      = 1.5F;  // «тот же» подозрительный отсчёт

// Возвращает false, когда бокс в этом кадре рисовать не надо. pos — вход
// (сырое чтение, при read_ok) и выход (то, что рисуем).
static bool filter_player_position(PlayerTrack& track, bool read_ok, Vec3& pos) {
    const double now = mono_seconds();

    if (!track.has_drawn) {
        if (!read_ok) return false;
        track.drawn = pos; track.drawn_t = now; track.has_drawn = true;
        track.hold_frames = 0; track.has_jump = false; track.jump_frames = 0;
        return true;
    }

    double dt = now - track.drawn_t;
    if (!(dt > 0.0)) dt = 0.0;
    if (dt > 0.25) dt = 0.25;
    const float fdt = (float)dt;

    // Последняя принятая позиция, продвинутая по измеренной скорости; дальше
    // метра-полутора не продлеваем, чтобы не унести бокс самим фильтром.
    Vec3 predicted = track.drawn;
    {
        const float mx = track.vel.x * fdt, my = track.vel.y * fdt, mz = track.vel.z * fdt;
        const float step2 = mx * mx + my * my + mz * mz;
        const float limit2 = kPosExtrapolateLimit * kPosExtrapolateLimit;
        const float scale = (step2 > limit2 && step2 > 0.0F) ? sqrtf(limit2 / step2) : 1.0F;
        predicted.x += mx * scale; predicted.y += my * scale; predicted.z += mz * scale;
    }

    if (!read_ok) {
        if (++track.hold_frames > kPosHoldFrames) return false;  // объект реально пропал
        pos = predicted;
        return true;
    }

    const float dx = pos.x - predicted.x, dy = pos.y - predicted.y, dz = pos.z - predicted.z;
    const float dev = sqrtf(dx * dx + dy * dy + dz * dz);
    const float limit = kPosJumpMeters + kPosJumpSpeed * fdt;
    if (std::isfinite(dev) && dev <= limit) {
        track.drawn = pos; track.drawn_t = now;
        track.hold_frames = 0; track.has_jump = false; track.jump_frames = 0;
        return true;
    }

    // Скачок за пределы правдоподобия. Один и тот же отсчёт подряд — похоже на
    // настоящий телепорт, принимаем; каждый кадр разный — это мусор, остаёмся
    // на последней хорошей позиции.
    bool same = false;
    if (track.has_jump) {
        const float jx = pos.x - track.jump.x, jy = pos.y - track.jump.y, jz = pos.z - track.jump.z;
        same = std::isfinite(jx) && (jx * jx + jy * jy + jz * jz) < kPosJumpSameSpot * kPosJumpSameSpot;
    }
    if (same) ++track.jump_frames;
    else { track.jump = pos; track.jump_frames = 1; }
    track.has_jump = true;

    if (track.jump_frames >= kPosJumpConfirmFrames) {
        track.drawn = pos; track.drawn_t = now;
        track.has_jump = false; track.jump_frames = 0; track.hold_frames = 0;
        // Скорость через телепорт не измеряется — сбрасываем, иначе упреждение
        // будет на пару кадров смотреть в старую сторону.
        track.vel = {}; track.have_vel_ref = false;
        return true;
    }
    pos = predicted;
    return true;
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
// Совпадает ли имя класса объекта с ожидаемым.
// accept_when_unreadable — что делать, если имя прочитать нельзя (память имён на
// части устройств недоступна, см. class_identity). Где рядом есть второй,
// независимый признак (обратная ссылка на игрока) — объект принимаем; где признак
// только имя — нет, иначе под проверку попадёт что угодно.
bool object_class_name_is(uint64_t obj, const char* expected,
                                 bool accept_when_unreadable) {
    if (!valid_obj(obj)) return false;
    uint64_t klass = rd_ptr(obj);
    if (!valid_obj(klass)) return false;
    bool readable = false;
    const std::string name = read_remote_string(rd_ptr(klass + IL2CPP_CLASS_NAME), &readable);
    if (!readable) return accept_when_unreadable;
    return name == expected;
}

// The GameObject-name offset is normally discovered while building a skeleton.
// Weapon labels must work with skeleton ESP off, so discover it on demand from
// the character model subtree (cheap: one BFS, then cached process-wide).
bool ensure_gameobject_name_offset(uint64_t player) {
    if (g_go_name_offset_valid) return true;
    // Кулдаун в СЕКУНДАХ, а не в вызовах. Функцию зовут из мест с совершенно
    // разной частотой: конвейер имён ESP — каждый кадр на каждого игрока, сканы
    // реестра — раз в пару секунд. Прежние «60 вызовов» на первом сценарии
    // означали новую попытку каждые ~0.1 с, а попытка — это обход поддерева
    // трансформов модели до 256 узлов, то есть сотни чтений памяти одним
    // кадром. Пока смещение не найдено (или не читается поза игрока), это был
    // один из самых дорогих периодических рывков: в логе 14.09 медленные кадры
    // шли с интервалами 8/52/68/112 — наложение нескольких таких периодик.
    const double now = mono_seconds();
    if (now < g_go_name_retry_at) return false;
    uint64_t root = skeleton_model_root(player);
    if (!root) { g_go_name_retry_at = now + 2.0; return false; }
    static std::vector<uint64_t> nodes;
    collect_transform_subtree(root, nodes, 256);
    if (nodes.size() < 8 || !discover_gameobject_name_offset(nodes)) {
        g_go_name_retry_at = now + 2.0;
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

// Угол до точки по её экранному положению — ровно по тому проецированию,
// которым нарисована точка (и бокс). Этим же путём углы считались до того,
// как заработал «живой» трансформ камеры; он остаётся основным, потому что
// не зависит ни от позы камеры, ни от оси выстрела.
static float wrap_deg_180(float d) {
    while (d > 180.0F) d -= 360.0F;
    while (d < -180.0F) d += 360.0F;
    return d;
}

static bool angles_from_screen(const Vec2& screen, float sw, float sh, float& yaw_deg, float& pitch_deg) {
    constexpr float rad2deg = 57.29577951F;
    if (!(sw > 1.f) || !(sh > 1.f)) return false;
    if (!std::isfinite(screen.x) || !std::isfinite(screen.y)) return false;
    float fov = (g_cam_fov_deg > 1.0F && g_cam_fov_deg < 179.0F) ? g_cam_fov_deg : 60.0F;
    const float tan_half_v = tanf(fov * 0.5F / rad2deg);
    const float aspect = sw / sh;
    const float ndc_x = (screen.x / sw) * 2.0F - 1.0F;
    const float ndc_y = 1.0F - (screen.y / sh) * 2.0F;
    yaw_deg = atanf(ndc_x * tan_half_v * aspect) * rad2deg;
    pitch_deg = atanf(ndc_y * tan_half_v) * rad2deg;
    return std::isfinite(yaw_deg) && std::isfinite(pitch_deg);
}

// Angular offset of a world point from the camera axis. Prefers the live
// camera pose; falls back to inverting the projection from the screen point,
// so aim angles never depend on the transform-pose path succeeding.
bool aim_angles_for(const Vec3& world, const Vec2& screen, float sw, float sh, float& yaw_deg, float& pitch_deg) {
    constexpr float rad2deg = 57.29577951F;
    // Углы обязаны быть посчитаны от той же камеры, по которой точка
    // нарисована на экране. Иначе аим гонится за точкой, смещённой на угол
    // между источниками, и ходит маятником: боксы строятся матрицей камеры, а
    // углы считались от оси выстрела (или от «живой» позы трансформа) — это
    // разные направления, расхождение плавает от кадра к кадру. Поэтому
    // сначала считаем угол по пикселю (то же проецирование, что у бокса), и
    // только если 3-D-путь с ним согласен — берём 3-D.
    float pix_yaw = 0.f, pix_pitch = 0.f;
    const bool have_pix = angles_from_screen(screen, sw, sh, pix_yaw, pix_pitch);
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
            if (std::isfinite(yaw_deg) && std::isfinite(pitch_deg)) {
                // Согласен с нарисованным — годится. Не согласен (источники
                // разошлись) — берём пиксельный угол: он по построению совпадает
                // с тем, что видит игрок.
                if (!have_pix) return true;
                const float dYaw = fabsf(wrap_deg_180(yaw_deg - pix_yaw));
                const float dPitch = fabsf(pitch_deg - pix_pitch);
                if (dYaw + dPitch <= 2.5F) return true;
                static double s_last = -1e9;
                const double now = memio::now_seconds();
                if (now - s_last >= 5.0) {
                    s_last = now;
                    LogLine("аим: углы разошлись с экраном на %.1f/%.1f° — считаю по экрану",
                            (double)dYaw, (double)dPitch);
                }
                yaw_deg = pix_yaw; pitch_deg = pix_pitch;
                return true;
            }
        }
    }
    if (have_pix) { yaw_deg = pix_yaw; pitch_deg = pix_pitch; return true; }
    return false;
}

// Vertical offset of the head aim point (metres, world up). Раньше здесь было
// +10/+4 см: точка прицела уходила ВЫШЕ головы, и на近距离 это давало промах
// поверх макушки, а на средней — «прицел стоит на лбу, а не на голове». По
// просьбе (19.09) смещение выключено совсем: цель — ровно кость головы. Обе
// величины оставлены на месте и названы своими именами, чтобы вернуть подъём
// было одной правкой константы.
static constexpr float g_aim_head_lift = 0.0F;       // вдали
static constexpr float g_aim_head_lift_near = 0.0F;  // вблизи

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
// Упреждение ВЫКЛЮЧЕНО (было 0.05 с). Оно уводило прицел туда, где цель
// ОКАЖЕТСЯ через несколько кадров, и на肉眼 это выглядело как «аим пытается
// запредиктить движение»: точка прыгала вперёд по ходу цели, аим за ней,
// цель меняла направление — начинались качели (жалоба 19.09). Теперь аим
// ведёт ровно по текущему положению кости. Константа оставлена: вернуть
// упреждение — одна правка числа.
static constexpr float kAimLeadSeconds = 0.0F;
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
            // Точка головы — ровно кость головы. Раньше здесь было +11 см вверх
            // по оси шеи плюс до +8 см по дальности (range * 0.001): суммарно
            // те самые «+12», из-за которых прицел стоял выше макушки. По
            // просьбе 19.09 смещение убрано совсем: аим ведёт ровно по скелету,
            // dir и range больше не нужны.
            (void)dir; (void)range;
            target[0] = head;
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
    // Габариты скелета в метрах. Нужны для признака смерти: труп ЛЕЖИТ, его
    // кости вытянуты по горизонтали и почти не имеют высоты. Читать здоровье
    // цели бессмысленно (жалоба 19.09): у убитого игрока оно на клиенте
    // остаётся прежним, а нули, которые мы ловили в vitals, принадлежат спящим —
    // на них аим и отвлекался, пропуская настоящие трупы.
    {
        float minY = 1e9f, maxY = -1e9f, minX = 1e9f, maxX = -1e9f, minZ = 1e9f, maxZ = -1e9f;
        int n = 0;
        for (int bone = 0; bone < ESP_BONE_COUNT; ++bone) {
            if (!box.bone_valid[bone]) continue;
            if (skeleton.bone_world_age[bone] < 1 || skeleton.bone_world_age[bone] > 9) continue;
            const Vec3 w = skeleton.bone_world[bone];
            if (!vec3_is_finite(w)) continue;
            if (w.y < minY) minY = w.y;
            if (w.y > maxY) maxY = w.y;
            if (w.x < minX) minX = w.x;
            if (w.x > maxX) maxX = w.x;
            if (w.z < minZ) minZ = w.z;
            if (w.z > maxZ) maxZ = w.z;
            ++n;
        }
        box.skel_bones = n;
        if (n >= 6) {
            box.skel_up   = maxY - minY;
            const float flatX = maxX - minX, flatZ = maxZ - minZ;
            box.skel_flat = flatX > flatZ ? flatX : flatZ;
        }
    }
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

int esp_local_aim_source() {
    if (g_pid <= 0 || !g_il2cpp_base) return 0;
    if (!read_local_aim_state()) return 0;
    return g_aim_state.source;
}

// ---- Дальность удара ближним орудием: FPMelee.m_MaxReach + hitRadius --------
// Сами числа сериализованы в префабе каждого инструмента, в дампе их нет: в
// конструкторе FPMelee стоят заглушки (m_MaxReach 0.5, hitRadius 0.1,
// m_TimeBetweenAttacks 0.85, m_DamagePerHit 15, m_ImpactForce 15). Поэтому
// читаем живой объект в руках.
//
// Как игру это использует (FPMelee.ZkX, дизасм билда 62a8534):
//   data = handler.RaycastData(0x160); если невалиден — handler.AimRaycast(0x168)
//   if (data.RaycastHit.distance < m_MaxReach + hitRadius) On_Hit(data)
//   else On_Woosh()
// distance — UnityEngine.RaycastHit.get_distance(), то есть 3D-метры от
// камеры/оси выстрела, а НЕ горизонтальное расстояние до узла.

// Имена классов ближнего орудия читаемые и между билдами не ротируют
// (dump.cs: Oxide.FPMelee -> Oxide.FPTool -> Oxide.FPChainsaw, плюс FPSpear,
// FPBuildingHammer, FPTorch). Проверка обязательна: у прочих FPObject на 0x128
// свои поля (у FPCrossbow/FPSnowball там m_MaxDistance — сотни метров).
static const char* const kMeleeFamily[] = {
    "FPTool", "FPChainsaw", "FPMelee", "FPSpear", "FPBuildingHammer", "FPTorch"};
static uint64_t s_melee_klass = 0;
static bool     s_melee_klass_ok = false;
static char     s_melee_klass_name[24] = {};

bool read_local_melee_reach(MeleeReach& out) {
    out = MeleeReach{};
    if (g_pid <= 0 || !g_il2cpp_base) return false;
    const uint64_t local = resolve_local_player();
    if (!local) return false;
    const uint64_t fp_manager = rd_ptr(local + PLAYER_FP_MANAGER);
    if (!valid_obj(fp_manager)) return false;
    uint64_t weapon = rd_ptr(fp_manager + FPMANAGER_CURRENT_WEAPON);
    if (!valid_obj(weapon) || rd_ptr(weapon + FPOBJECT_PLAYER_BACKREF) != local) {
        weapon = rd_ptr(fp_manager + FPMANAGER_CURRENT_OBJECT);
        if (!valid_obj(weapon) || rd_ptr(weapon + FPOBJECT_PLAYER_BACKREF) != local)
            return false;
    }
    const uint64_t klass = rd_ptr(weapon);
    if (!valid_obj(klass)) return false;
    if (klass != s_melee_klass) {  // имя класса читаем один раз на предмет в руках
        s_melee_klass = klass;
        s_melee_klass_ok = false;
        s_melee_klass_name[0] = '\0';
        const std::string name = read_remote_string(rd_ptr(klass + IL2CPP_CLASS_NAME));
        if (!name.empty() && name.size() < sizeof(s_melee_klass_name)) {
            for (const char* family : kMeleeFamily) {
                if (name == family) { s_melee_klass_ok = true; break; }
            }
            memcpy(s_melee_klass_name, name.c_str(), name.size() + 1);
        }
    }
    if (!s_melee_klass_ok) return false;

    float reach = 0.0F, radius = 0.0F;
    if (!rd_exact(weapon + FPMELEE_MAX_REACH, reach) ||
        !rd_exact(weapon + FPMELEE_HIT_RADIUS, radius)) return false;
    if (!std::isfinite(reach) || !std::isfinite(radius)) return false;
    // Правдоподобие: настоящие орудия — единицы метров. Вне диапазона значит
    // либо класс совпал случайно, либо чтение разъехалось после апдейта игры.
    if (reach < 0.2F || reach > 20.0F || radius < 0.0F || radius > 5.0F) return false;

    out.valid = true;
    out.max_reach = reach;
    out.hit_radius = radius;
    out.total = reach + radius;
    strncpy(out.tool, s_melee_klass_name, sizeof(out.tool) - 1);

    // Длина луча, которым игра ищет точку взаимодействия (префаб
    // RaycastManager, default 1.5). m_AimRayLength (0x3C) и радиус сферы (0x40)
    // при доставании орудия перезаписываются его же m_MaxReach/hitRadius
    // (FPMelee.On_Draw -> RaycastManager.ZIj), поэтому интересен именно 0x38.
    const uint64_t rm = rd_ptr(weapon + FPOBJECT_RAYCAST_MANAGER);
    if (valid_obj(rm) && rd_ptr(rm + RAYCASTMAN_PLAYER) == local) {
        float ray = 0.0F;
        if (rd_exact(rm + RAYCASTMAN_RAY_LENGTH, ray) && std::isfinite(ray) &&
            ray > 0.0F && ray < 500.0F) out.ray_length = ray;
    }

    // Ритм ударов. В конструкторе FPMelee стоят заглушки (0.85/0.15), настоящие
    // значения сериализованы в префабе каждого инструмента, поэтому читаем их
    // так же, как дальность, — из живого орудия, с проверкой правдоподобия.
    float tba = 0.0F, pause = 0.0F;
    if (rd_exact(weapon + FPMELEE_TIME_BETWEEN_ATTACKS, tba) &&
        std::isfinite(tba) && tba > 0.05F && tba < 10.0F)
        out.time_between_attacks = tba;
    if (rd_exact(weapon + FPMELEE_PAUSE_AFTER_ATTACK, pause) &&
        std::isfinite(pause) && pause >= 0.0F && pause < 10.0F)
        out.pause_after_attack = pause;

    // Умения орудия (какой ресурс оно вообще может добывать).
    if (!strcmp(s_melee_klass_name, "FPTool") || !strcmp(s_melee_klass_name, "FPChainsaw")) {
        int32_t purposes = 0;
        if (rd_exact(weapon + FPTOOL_TOOL_PURPOSES, purposes)) {
            const int32_t known = (int32_t)ToolPurpose::CutWood |
                                  (int32_t)ToolPurpose::BreakRocks |
                                  (int32_t)ToolPurpose::CutAnimals;
            if (purposes != 0 && (purposes & ~known) == 0) {
                out.tool_purposes = (int)purposes;
                out.purposes_valid = true;
            }
        }
    }

    // Луч прицела. Порядок ровно как в FPMelee.ZkX (0x6533a20): сначала
    // RaycastData (0x160), при null — AimRaycast (0x168); значение лежит в
    // обёртке GuI`1<GKo> на +0x20, а «валидность» там — просто data != null.
    {
        const uint64_t handler = rd_ptr(weapon + FPOBJECT_EVENT_HANDLER);
        if (valid_obj(handler)) {
            uint64_t data = 0;
            const uint64_t primary = rd_ptr(handler + GUM_RAYCAST_DATA);
            if (valid_obj(primary)) data = rd_ptr(primary + GUI_VALUE);
            if (!valid_obj(data)) {
                const uint64_t fallback = rd_ptr(handler + GUM_AIM_RAYCAST);
                if (valid_obj(fallback)) data = rd_ptr(fallback + GUI_VALUE);
            }
            if (valid_obj(data)) {
                out.ray_valid = true;
                // GameObject и Collider попадания держим указателями: по ним
                // проверяется, не в сам ли узел упёрся луч (у камня точка
                // прицела внутри породы, и без этого свой камень выглядит
                // стеной — бот вечно обходил его, не сделав ни удара).
                const uint64_t hit_go = rd_ptr(data + GKO_HIT_OBJECT);
                out.ray_hit_go = valid_obj(hit_go) ? hit_go : 0;
                out.ray_hit_object = (out.ray_hit_go != 0);
                const uint64_t hit_col = rd_ptr(data + GKO_RAYCAST_HIT + RAYCASTHIT_COLLIDER);
                out.ray_collider = valid_obj(hit_col) ? hit_col : 0;
                float dist = 0.0F;
                if (rd_exact(data + GKO_RAYCAST_HIT + RAYCASTHIT_DISTANCE, dist) &&
                    std::isfinite(dist) && dist > 0.0F && dist < 1000.0F)
                    out.ray_distance = dist;
                Vec3 rp{}, rn{};
                if (rd_exact(data + GKO_RAYCAST_HIT + RAYCASTHIT_POINT, rp) &&
                    vec3_is_finite(rp) && position_looks_like_world_space(rp)) {
                    out.ray_point = rp;
                    out.ray_point_valid = true;
                    if (rd_exact(data + GKO_RAYCAST_HIT + RAYCASTHIT_NORMAL, rn) &&
                        vec3_is_finite(rn))
                        out.ray_normal = rn;
                }
            }
        }
    }
    return true;
}

float esp_camera_fov_deg() { return g_cam_fov_deg; }

// Bit 0: camera pose known. Bit 2: the firing reference (look direction from
// the eye point) is in use instead of the camera axis. Bit 1 не выставляется
// (там когда-то отмечалась поза, выведенная из матрицы вида) и оставлен, чтобы
// не разошлись номера битов в разборе строки AIM: cam_st 0 означает «настоящей
// оси нет» — ровно то, что видит аим (esp_aim_camera_angles).
int esp_camera_state() {
    return (g_cam_pose_valid ? 1 : 0) | (g_cam_pose_derived ? 2 : 0) | (g_aim_ref_valid ? 4 : 0);
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
// Почему привязка не удалась — main.cpp показывает это тостом, иначе на
// «неудобных» устройствах чит молча ничего не делал.
static EspAttachState g_attach_state = ESP_ATTACH_OK;

EspAttachState esp_attach_state() { return g_attach_state; }

bool esp_init(pid_t pid) {
    esp_reset();
    g_pid = pid;
    g_mem.clear_last_error();

    uint64_t candidates[8] = {};
    const int candidate_count = get_base_candidates("libil2cpp.so", candidates, 8);
    if (candidate_count == 0) {
        g_attach_state = ESP_ATTACH_NO_LIB;
        g_pid = -1;
        // Без базового адреса читать нечего.
        return false;
    }

    // База выбирается не «первая по имени в карте», а та, на которой РЕАЛЬНО
    // резолвится класс игры. После перезапуска игры в карте остаётся ещё и
    // старый образ libil2cpp.so (обычно «(deleted)»): он читается, ELF-заголовок
    // на месте, поэтому прежняя проверка доступа его принимала — а метаданные
    // внутри мертвы, и весь чит молча ничего не находил до перезапуска чита.
    uint64_t chosen = 0;
    for (int i = 0; i < candidate_count; ++i) {
        if (!g_mem.bind(pid, candidates[i])) continue;
        if (base_resolves_game(candidates[i])) { chosen = candidates[i]; break; }
        g_mem.unbind();
    }
    if (!chosen) {
        // Ни одна база не подтвердилась (игра ещё грузится?): работаем на первой
        // пригодной, как раньше, — привязка не хуже прежней.
        for (int i = 0; i < candidate_count; ++i) {
            if (g_mem.bind(pid, candidates[i])) { chosen = candidates[i]; break; }
        }
    }
    if (!chosen) {
        // /proc/<pid>/mem не открылся или не читается: доступа к памяти нет.
        g_attach_state = ESP_ATTACH_NO_ACCESS;
        g_il2cpp_base = 0;
        g_pid = -1;
        return false;
    }

    g_il2cpp_base = chosen;
    g_attach_state = ESP_ATTACH_OK;
    return true;
}

// Начало кадра: кэш блоков памяти сбрасывается, чтобы кадр читал свежее
// состояние игры, но внутри кадра повторные обращения к тем же полям не стоили
// syscall'а (см. mem_io.h).
void esp_mem_frame_begin() { g_mem.frame_begin(); }

// Привязка ещё жива? Дешёвая проверка (одно чтение): процесс мог перезапуститься
// с тем же pid, а доступ — отобрали. Без неё чит оставался «привязанным» и молча
// ничего не делал до перезапуска приложения.
bool esp_alive_check() { return g_mem.verify(); }

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
// Игроки, пропавшие из реестра в последние пару кадров. Список
// PlayerManager читается по одному указателю на элемент, и одиночный
// сбой чтения (или непрочитавшаяся проверка класса) выбрасывал игрока
// из кадра — его бокс гас и загорался обратно. Держим пропавшего ещё
// кадр-два: настоящий уход/смерть задерживается на ~30 мс, а мерцание
// исчезает. Значение — сколько кадров подряд игрока нет в списке.
static std::unordered_map<uint64_t, int> g_frame_transforms_lost;

// Frame projection state, published by esp_get_boxes() so that world markers
// (ore / animals) project through exactly the same camera as the player boxes.
Mat4 g_frame_vp{};
bool g_frame_vp_valid = false;
float g_frame_sw = 0.0F, g_frame_sh = 0.0F;
Vec3 g_frame_local_pos{};
bool g_frame_local_valid = false;
// Camera basis recovered from this frame's VIEW MATRIX (not the transform
// pose). On devices where the transform pose read fails, this is the only
// camera orientation available — good enough for the farm's slow turns,
// though not for the aimbot (the fallback matrix lags a frame).
bool g_frame_cam_basis_valid = false;
Vec3 g_frame_cam_pos{}, g_frame_cam_fwd{}, g_frame_cam_right{}, g_frame_cam_up{};

// Камерный источник годен, если его позиция конечна и близка к корню игрока.
// Нулевой вектор конечен, поэтому одна проверка isfinite пропускала мусор: в
// логе 14.09.2026 было 11 кадров с origin=(0,0,0) — aim3d улетал на 864 и
// 1027 м при dist 0.8 м, а yaw на 158-164°, то есть камера получала команду
// развернуться почти на пол-оборота. Глаз выше корня игрока примерно на 1.5 м,
// так что 25 м запаса отсекают только явный мусор.
// Доступ к камере (esp_camera_angles / esp_local_eye_position) объявлен ниже,
// сразу за g_frame_cam_*: базис из матрицы вида — третий источник углов и
// позиции глаза, и он обязан быть объявлен раньше этих функций.
bool farm_cam_source_ok(const Vec3& p) {
    if (!vec3_is_finite(p)) return false;
    if (!g_frame_local_valid) return true;
    const float dx = p.x - g_frame_local_pos.x;
    const float dy = p.y - g_frame_local_pos.y;
    const float dz = p.z - g_frame_local_pos.z;
    return dx * dx + dy * dy + dz * dz < 625.0F;
}

// Направление оси -> углы (yaw вокруг мировой вертикали, pitch вверх).
// Общая часть всех источников: аим и фарм считают угол одинаково.
static bool angles_from_forward(const Vec3& f, float& yaw_deg, float& pitch_deg) {
    constexpr float rad2deg = 57.29577951F;
    float yaw = atan2f(f.x, f.z) * rad2deg;
    float horiz = sqrtf(f.x * f.x + f.z * f.z);
    float pitch = atan2f(f.y, horiz) * rad2deg;
    if (!std::isfinite(yaw) || !std::isfinite(pitch)) return false;
    yaw_deg = yaw; pitch_deg = pitch;
    return true;
}

// Углы камеры для ФАРМА: поза камеры (Transform), ось выстрела (LookDirection),
// а если ни то, ни другое не читается — базис из матрицы вида этого кадра.
// Фарм поворачивает камеру медленно и по экранной метке, поэтому базис (отстаёт
// на кадр) ему подходит; аимботу — нет, у него своя функция ниже.
//
// Почему базис вообще нужен. На устройстве из логов 14.09 и 15.09.2026 поза
// камеры и ось выстрела НЕ ЧИТАЮТСЯ вовсе (cam_st = 0 во всех строках аима), и
// базис из матрицы вида — единственный источник углов, который там есть: без
// него фарм висел в «нет позиции камеры».
//
// Тот же фильтр мусора, что и у точки прицела: источник с нулевой/улетевшей
// позицией не годится и для углов, иначе обучение коэффициента камеры хлебнёт
// поворот на 158° из ниоткуда (лог 14.09.2026: 11 кадров с origin=(0,0,0)).
bool esp_camera_angles(float& yaw_deg, float& pitch_deg) {
    const bool ok_ref   = g_aim_ref_valid  && farm_cam_source_ok(g_aim_ref_origin);
    const bool ok_pose  = g_cam_pose_valid && farm_cam_source_ok(g_cam_pos);
    const bool ok_frame = g_frame_cam_basis_valid && farm_cam_source_ok(g_frame_cam_pos);
    if (!ok_ref && !ok_pose && !ok_frame) return false;
    // Same reference the aim angles are measured against (firing direction
    // when available), so finger-gain learning and target lead stay consistent.
    const Vec3& f = ok_ref ? g_aim_ref_forward : ok_pose ? g_cam_forward : g_frame_cam_fwd;
    return angles_from_forward(f, yaw_deg, pitch_deg);
}

// Углы камеры ДЛЯ АИМБОТА: только НАСТОЯЩАЯ ось — поза камеры (Transform) или
// ось выстрела (LookDirection). Базис из матрицы вида здесь не участвует, хотя
// фарму он и годится.
//
// Почему. Аимбот меряет по этим углам две вещи: чувствительность (град/px,
// деление поворота камеры на свой сдвиг пальца) и «отработала ли игра прошлый
// шаг». Базис отстаёт на кадр, и оба измерения по нему врут. В логе 15.09.2026
// это видно прямо: exp/sent (он же выученный gain) скакал 0.072 -> 0.347 ->
// -0.072 — со сменой ЗНАКА, — а ход пальца доходил до 162 px за кадр, палец
// улетал в край экрана (EV «палец на краю ... перенос в центр») и камеру
// швыряло туда-обратно. Это и есть «аим дёргается».
//
// Без настоящей оси аим ведёт цель вслепую запасным коэффициентом и НЕ ждёт
// ответа камеры (s_camMovedPrev в UpdateAim) — ровно так работала сборка, где
// он вёл идеально: в том логе cam_st = 0, то есть обучать коэффициент было не
// на чем, и аим просто шёл к цели фиксированным шагом.
bool esp_aim_camera_angles(float& yaw_deg, float& pitch_deg) {
    const bool ok_ref  = g_aim_ref_valid  && farm_cam_source_ok(g_aim_ref_origin);
    const bool ok_pose = g_cam_pose_valid && farm_cam_source_ok(g_cam_pos);
    if (!ok_ref && !ok_pose) return false;
    return angles_from_forward(ok_ref ? g_aim_ref_forward : g_cam_forward, yaw_deg, pitch_deg);
}

bool esp_local_eye_position(float& x, float& y, float& z) {
    // Приоритет — точка выстрела (KCC LookDirection): она не качается от sway/
    // отдачи, поэтому производная по ней и есть реальное движение персонажа.
    if (g_aim_ref_valid && farm_cam_source_ok(g_aim_ref_origin)) {
        x = g_aim_ref_origin.x; y = g_aim_ref_origin.y; z = g_aim_ref_origin.z;
        return true;
    }
    if (g_cam_pose_valid && farm_cam_source_ok(g_cam_pos)) {
        x = g_cam_pos.x; y = g_cam_pos.y; z = g_cam_pos.z;
        return true;
    }
    // Третьим — позиция из матрицы вида. Без неё на устройстве из лога
    // 14.09.2026 mv_dps/mv_dir были -99 во всех 14499 строках: контроллер
    // движения шёл вслепую — не знал, что персонаж уже разогнался или упёрся
    // в ствол, и не мог отличить «идём» от «стоим».
    if (g_frame_cam_basis_valid && farm_cam_source_ok(g_frame_cam_pos)) {
        x = g_frame_cam_pos.x; y = g_frame_cam_pos.y; z = g_frame_cam_pos.z;
        return true;
    }
    return false;
}

// Players near us this frame (all 360 degrees, not only the ones projected
// on screen). Feeds the enemy-counter pill in the overlay.
static int  g_frame_player_count = 0;
static int      g_frame_transforms_empty_streak = 0;
// Frames in a row esp_get_boxes() gave up before publishing this frame's
// camera / local position (see the watchdog at the top of it).
static int      g_frame_publish_fail_streak = 0;
// Сколько раз подряд сторож сбрасывал кэши, не получив кадра. Три подряд —
// повод перепривязаться целиком (см. esp_get_boxes).
static int      g_frame_watchdog_resets = 0;
// Кадры подряд, в которых позиции игроков не прочитались НИ У КОГО. Одного
// такого кадра мало, чтобы объявить смещение позиции неверным: за этим идёт
// поиск смещения заново с ожиданием «поля допишутся» (0.6 с), и всё это время
// боксов нет вовсе — со стороны это «мерцание на полсекунды». Серия кадров
// отличает мигнувшее чтение от настоящей перезагрузки мира.
static int      g_local_position_fail_streak = 0;
// Кадры подряд, в которых состав игроков не пересекается с тем, по которому
// построены кэши (см. esp_get_boxes). Настоящая перезагрузка мира держится
// кадров подряд, а одиночный кадр с чужими адресами — это сбой чтения списка, и
// обнулять по нему все кэши (боксы, метки, раскладку скелета) нельзя.
// Снапшот хранит именно тот состав, под который собраны кэши: сравнивать с
// прошлым кадром нельзя — список подменяется уже на первом кадре смены, и
// следующий кадр пересекается сам с собой (смену состава это бы не заметило).
static int      g_world_change_streak = 0;
static std::vector<uint64_t> g_population_snapshot;
// Поток привязки в main.cpp читает этот флаг и переподключается к игре заново.
static std::atomic<bool> g_want_reattach{false};

// Everything derived from a particular world/session. Called when the whole
// player population is replaced (scene reload / new session) or the player
// list disappears for a while, so no stale pointers survive into the next
// world. Deliberately NOT tied to the camera object: the game swaps cameras
// while aiming, which must not disturb boxes or skeletons.
// Defined with the marker code further down (needs its caches).
static void reset_world_caches() {
    g_matrix_configuration_validated = false; g_camera_matrix_physical_match = false;
    g_player_position_validated = false;
    g_population_snapshot.clear(); g_world_change_streak = 0;
    // The bone-learned transform layout dies with the old world: after a
    // reload it reads garbage from recycled memory (finite numbers, wrong
    // places). It is relearned from the first nearby skeleton; markers use
    // the self-probing path meanwhile.
    g_skeleton_layout = {}; g_skeleton_layout_valid = false;
    g_use_direct_player_position = true; g_player_position_offset = PLAYER_POSITION;
    g_direct_position_fail_streak = 0; g_direct_position_recheck = 0;
    g_local_player = 0;
    g_aim_state = {};
    g_aim_ref_valid = false;
    g_player_aux.clear();
    g_player_text.clear();
    g_player_track.clear();
    g_player_track_pick.clear();
    g_frame_transforms_lost.clear();
    g_mount_latch.clear();
    g_skeletons.clear();
    reset_marker_caches();
}

void esp_reset() {
    g_mem.unbind();
    g_attach_state = ESP_ATTACH_OK;
    g_pid = -1; g_il2cpp_base = 0;
    g_xray_cam = 0; g_xray_saved_valid = false; // процесс ушёл — восстанавливать нечего
    g_day_tod = 0; g_day_retry = 0; g_day_cycle_addr.store(0);
    g_frame_transforms.clear(); g_frame_transforms_empty_streak = 0;
    g_frame_publish_fail_streak = 0; g_frame_watchdog_resets = 0;
    g_local_position_fail_streak = 0; g_world_change_streak = 0;
    g_population_snapshot.clear();
    g_offset_extent_fail_streak = 0;
    g_want_reattach.store(false);
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
    g_frame_transforms_lost.clear();
    g_mount_latch.clear();
    g_skeletons.clear();
    g_skeleton_layout = {}; g_skeleton_layout_valid = false;
    g_go_name_offset = 0; g_go_name_plain_pointer = false;
    g_go_name_offset_valid = false; g_go_name_retry_at = 0.0;
    g_frame_vp_valid = false; g_frame_local_valid = false;
    reset_marker_caches();
}


// Last overlay size esp_get_boxes() was called with — the camera-only frame
// fallback below needs plausible screen dimensions even when the box pipeline
// bailed out before publishing anything.
float g_last_overlay_sw = 1080.0F;
float g_last_overlay_sh = 2400.0F;

// Publish a frame (VP matrix + "local position") straight from the game
// camera, with no players involved at all. This is what keeps markers and
// the autofarm alive when the player list is empty or the box pipeline
// failed: the camera IS where the local player is.

bool publish_camera_only_frame(float sw, float sh) {
    if (g_pid <= 0 || !g_il2cpp_base) return false;
    // Resolves g_game_controller_class as a side effect — without it the
    // camera lookup below has no class to read statics from.
    resolve_local_player();
    if (!g_game_controller_class) return false;
    uint64_t gcb_sf = get_class_static_fields(g_game_controller_class);
    if (!gcb_sf) return false;
    uint64_t cam_mgr = rd_ptr(gcb_sf + GAME_CONTROLLER_CAMERA_MANAGER_FIELD);
    if (!cam_mgr) return false;
    uint64_t managed_cam = rd_ptr(cam_mgr + CAMERA_MANAGER_CAMERA_FIELD);
    if (!managed_cam) return false;
    uint64_t cam_native = rd_ptr(managed_cam + MANAGED_CACHED_PTR);
    if (!cam_native) return false;
    if (!(sw >= 100.0F) || !(sh >= 100.0F)) { sw = 1080.0F; sh = 2400.0F; }
    Mat4 solo_proj{}, solo_view{};
    xray_apply(cam_native);
    always_day_tick();
    if (!read_native_camera_matrices(cam_native, sw / sh, solo_proj, solo_view)) return false;
    Vec3 cam_pos{};
    if (!camera_position_from_view(solo_view, cam_pos)) return false;
    g_frame_vp = mat_mul(solo_proj, solo_view);
    g_frame_vp_valid = true;
    g_frame_sw = sw; g_frame_sh = sh;
    g_frame_local_pos = cam_pos;
    g_frame_local_valid = true;
    g_frame_publish_fail_streak = 0;
    g_frame_watchdog_resets = 0;
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
        ++g_frame_watchdog_resets;
        // Три сброса подряд — это ~12 с полной слепоты: сброс кэшей уже не
        // помогает, значит неверна сама привязка (база библиотеки, права,
        // перезапуск игры). Просим поток привязки подключиться заново — он
        // выберет базу заново и заново проверит доступ.
        if (g_frame_watchdog_resets >= 3) {
            g_frame_watchdog_resets = 0;
            g_want_reattach.store(true);
        }
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
        // Пересечение считаем не с прошлым кадром, а со снапшотом — составом,
        // под который собраны кэши: список подменяется уже на первом кадре
        // смены, и на следующем кадре сравнение с прошлым кадром пересекается
        // само с собой, то есть полную смену состава оно бы не заметило.
        bool overlap = g_population_snapshot.empty();
        if (!overlap) {
            for (uint64_t previous : g_population_snapshot) {
                for (uint64_t current : refreshed) if (previous == current) { overlap = true; break; }
                if (overlap) break;
            }
        }
        if (!overlap) {
            // Не с первого кадра: адреса списка иногда мигают (rd_ptr вернул
            // чужую копию объекта или мусор из недописанного массива), и такой
            // одиночный кадр раньше обнулял ВСЕ кэши — боксы, метки и раскладку
            // скелета — то есть выглядел как «всё пропало и вернулось не туда».
            // Настоящая перезагрузка мира отличается тем, что новый состав
            // держится кадров подряд. Снапшот ставим ПОСЛЕ сброса: он чистит его.
            if (++g_world_change_streak >= 3) {
                reset_world_caches();
                g_population_snapshot = refreshed;
            }
        } else {
            g_world_change_streak = 0;
            g_population_snapshot = refreshed;
        }
        // Население то же, но кого-то не досчитались: почти всегда это сбой
        // чтения одного элемента, а не уход игрока. Возвращаем пропавших в
        // список ещё на пару кадров (иначе их боксы мигают), после чего
        // отпускаем — иначе ушедший игрок остался бы в списке навсегда.
        if (overlap && refreshed.size() < s_transforms.size()) {
            for (uint64_t previous : s_transforms) {
                bool present = false;
                for (uint64_t current : refreshed) if (current == previous) { present = true; break; }
                if (present) { g_frame_transforms_lost.erase(previous); continue; }
                if (++g_frame_transforms_lost[previous] <= 2) refreshed.push_back(previous);
                else g_frame_transforms_lost.erase(previous);
            }
            if (g_frame_transforms_lost.size() > 256) g_frame_transforms_lost.clear();
        }
        s_transforms = std::move(refreshed);
        g_frame_transforms_empty_streak = 0;
    } else if (++g_frame_transforms_empty_streak > 10) {
        // List gone for a while: the world is being torn down / reloaded —
        // or we are simply alone on the server. Clear the player caches, but
        // FALL THROUGH to the empty-list branch below: it publishes the
        // camera-only frame that keeps markers and the farm alive solo.
        if (!s_transforms.empty()) {
            s_transforms.clear(); reset_world_caches();
        }
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
    prune_mount_latch(s_transforms);
    if (want_bones) prune_skeleton_cache(s_transforms);
    else if (!g_skeletons.empty()) g_skeletons.clear();
    g_skeleton_builds_this_frame = 0;

    if (!g_player_position_validated) {
        if (!discover_player_position_offset(s_transforms)) {
            // Мир ещё не отдал позиции (перезагрузка после смерти). Боксов в
            // этом кадре нет, но метки и фарм живут на камере — без публикации
            // они гаснут вместе с боксами на все секунды ожидания.
            publish_camera_only_frame(sw, sh);
            return result;
        }
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
        xray_apply(native_cam);
        always_day_tick();
        freecam_tick();
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
            // Мусорный отсчёт (нули после респауна, денормали, координаты чужого
            // объекта из пула) отбрасываем сразу: это finite-вектор, но не
            // мировая позиция, и именно он утаскивал бокс «в другую сторону».
            const bool read_ok = read_entity_position(s_transforms[index], candidate) &&
                                 position_looks_like_world_space(candidate);
            // Один сбой чтения или один мусорный кадр больше не гасят и не
            // дёргают бокс — см. filter_player_position.
            PlayerTrack& filter = g_player_track[s_transforms[index]];
            if (!filter_player_position(filter, read_ok, candidate)) continue;
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
            // Позиции не прочитались ни у одного игрока. Это бывает и на
            // одном кадре (мигнуло чтение по /proc/<pid>/mem после смерти или
            // перезагрузки мира), поэтому смещение позиции объявляем неверным
            // только по серии кадров: за этим следует поиск смещения заново с
            // ожиданием 0.6 с, и всё это время боксов нет вовсе. Метки и фарм
            // живут на кадре одной камеры — публикуем его, чтобы пропажа
            // боксов не гасила и их.
            if (++g_local_position_fail_streak >= 10) {
                g_local_position_fail_streak = 0;
                g_player_position_validated = false;
            }
            // Камера этого кадра уже прочитана — публикуем её как есть (без
            // повторного чтения и повторного xray/day-тика), иначе вместе с
            // боксами гаснут и метки, и фарм.
            if (matrix_is_finite(vp)) {
                g_frame_vp = vp; g_frame_vp_valid = true;
                g_frame_sw = sw; g_frame_sh = sh;
                Vec3 camera_only_position{};
                if (camera_position_from_view(view, camera_only_position)) {
                    g_frame_local_pos = camera_only_position;
                    g_frame_local_valid = true;
                }
                g_frame_publish_fail_streak = 0;
                g_frame_watchdog_resets = 0;
            }
            return result;
        }
        g_local_position_fail_streak = 0;
    }

    g_frame_vp = vp;
    g_frame_vp_valid = !transform_camera_mode;
    g_frame_sw = sw; g_frame_sh = sh;
    g_frame_local_pos = local;
    g_frame_local_valid = has_local_position;
    g_frame_publish_fail_streak = 0; // this frame is healthy
    g_frame_watchdog_resets = 0;

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
            bool mine_ride = player_mount_engaged(s_transforms[index]);
            bool their_ride = player_mount_engaged(s_transforms[rival]);
            bool take_mine;
            if (index == local_entity_index)      take_mine = true;
            else if (rival == local_entity_index) take_mine = false;
            else if (mine_ride != their_ride)     take_mine = mine_ride;
            else if (mine != theirs)              take_mine = mine < theirs;
            else {
                auto sticky = g_player_track_pick.find(uid);
                take_mine = sticky != g_player_track_pick.end() && sticky->second == s_transforms[index];
            }
            suppressed[take_mine ? rival : index] = 1;
            if (take_mine) found->second = index;
        }
        for (const auto& entry : chosen) g_player_track_pick[entry.first] = s_transforms[entry.second];
        // Ghost PlayerManager left at the boarding point: same frozen lastSaved
        // as a rider, even when userID on the leftover is empty.
        for (size_t index = 0; index < s_transforms.size(); ++index) {
            if (!position_ok[index] || suppressed[index]) continue;
            auto lit = g_mount_latch.find(s_transforms[index]);
            if (lit == g_mount_latch.end() || !lit->second.engaged || !lit->second.have_saved) continue;
            const Vec3& mount = lit->second.prev_saved;
            for (size_t other = 0; other < s_transforms.size(); ++other) {
                if (other == index || !position_ok[other] || suppressed[other]) continue;
                if (other == local_entity_index) continue;
                if (player_mount_engaged(s_transforms[other])) continue;
                if (vec3_horiz2(positions[other], mount) < 1.8F * 1.8F)
                    suppressed[other] = 1;
            }
        }
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
        // Мёртвый игрок. Два независимых признака: health <= 0 и respawning.
        // Оба кладём в бокс, чтобы по логу было видно, который из них живой.
        // Почему здоровье нельзя читать «как получится». rd<float>() при
        // неудачном чтении возвращает НОЛЬ (game_internal.h: «T v{}» — значение
        // просто остаётся нулевым), а ноль проходил проверку «здоровье в
        // диапазоне». То есть любой сбойный syscall помечал игрока мёртвым, и
        // сборка 829a1dc перестала брать цели ВООБЩЕ: мёртвыми оказывались все
        // подряд, а строка «мёртвых N» печаталась только из ветки «кости не
        // нашлись» — в логе не было ни одной строки, и причину жалобы 19.09
        // «аим вообще не нацеливается на игроков» стало нечем подтвердить.
        // Теперь: результат чтения проверяется (rd_exact), неудача — не
        // приговор, а признаку «здоровье = 0» мы верим, только если хотя бы
        // раз видели живое здоровье. Если смещение не то и всем читается ноль,
        // фильтр молча выключается: аим продолжает работать, а срез памяти
        // vitals в логе показывает, где здоровье лежит на самом деле.
        static float g_hp_max_seen = -1.f;   // максимум здоровья за всё время
        box.respawning = false;
        {
            uint8_t flag = 0;
            if (rd_exact(s_transforms[i] + game_offsets::PLAYER_MANAGER_RESPAWNING, flag))
                box.respawning = flag != 0;
        }
        box.health = -1.f;
        {
            const uint64_t vitals = rd_ptr(s_transforms[i] + game_offsets::PLAYER_VITALS);
            if (vitals >= 0x10000) {
                // Где здоровье на самом деле. Прошлые две сборки читали
                // GenericVitals.KQN (0xB8) — и срез памяти из лога 15:56
                // показал, что там у ВСЕХ игроков ровно 0.0, хотя соседнее
                // поле m_MaxHealth (0x88) читается как честные 100.0. Значит
                // объект найден верно, а KQN — не текущее здоровье.
                //
                // Правильная цепочка взята из дизассемблера, а не подобрана:
                //   * GenericVitals.COL() (RVA 0x65391f8) кладёт m_MaxHealth
                //       s0 = [this + 0x88]
                //     в сеттер наблюдаемой величины:
                //       x8 = [this + 0x68]        -- brS.Entity
                //       x0 = [x8 + 0x98]          -- brz.Health
                //       setter(x0, s0)
                //   * сам сеттер (RVA 0xb0f6238) пишет новое значение в
                //     [x0 + 0x20], а старое относит в [x0 + 0x24]:
                //       stp s1, s0, [x0, #0x20]
                //   * в dump.cs этому ровно соответствует
                //       public class brz : brS  ->  public ? Health;  // 0x98
                //     а 0x68 у brS — это public brz Entity.
                // Итого: здоровье = [[[vitals + 0x68] + 0x98] + 0x20].
                const uint64_t entity = rd_ptr(vitals + game_offsets::PMP_ENTITY);
                if (entity >= 0x10000) {
                    const uint64_t hv = rd_ptr(entity + game_offsets::PMK_HEALTH);
                    if (hv >= 0x10000) {
                        // Слот значения: дизассемблер сеттера показывает +0x20
                        // (старое значение при этом относит в +0x24), а по
                        // раскладке AsyncReactiveProperty<T> из дампа значение
                        // должно лежать в +0x18. Тип brz.Health в дампе не
                        // разрешён (там просто «?»), поэтому берём тот слот,
                        // где лечит правдоподобное здоровье, — и печатаем оба,
                        // чтобы следующая сборка уже знала точно.
                        const float hp20 = rd<float>(hv + game_offsets::HEALTH_VALUE);
                        const float hp18 = rd<float>(hv + game_offsets::ARP_LATEST_VALUE);
                        auto plausible = [](float v) {
                            return std::isfinite(v) && v >= 0.f && v <= 100000.f;
                        };
                        // Не прочиталось ни там ни там — здоровье НЕИЗВЕСТНО, а
                        // не «ноль». Именно подстановка нуля вместо неудачи и
                        // помечала живых игроков мёртвыми (жалоба 19.09).
                        if (plausible(hp20))      { box.health = hp20; box.health_slot = 0x20; }
                        else if (plausible(hp18)) { box.health = hp18; box.health_slot = 0x18; }
                        else                      { box.health = -1.f; box.health_slot = 0; }
                        if (box.health > g_hp_max_seen) g_hp_max_seen = box.health;
                        // Срез объекта Health больше не печатаем: слот
                        // подтверждён логом 16:32 (+0x20 = 100.0 у живых,
                        // 0.0 у части игроков, +0x24 = прошлое значение).
                        // Здоровье остаётся только для лога «поза»: решением
                        // о смерти оно больше не управляет ни при каких
                        // обстоятельствах (жалоба «смысла читать HP нет»).
                    }
                }
            }
        }
        // Мёртвый решается ПОСЛЕ чтения скелета — см. ниже, перед push_back.
        // Раньше это решение стояло здесь, но fill_skeleton_box вызывается
        // ниже по циклу, а box создаётся заново каждый кадр (EspBox box{}),
        // поэтому в этой точке костей всегда 0 и правило не срабатывало ни
        // разу: в логе 17:06 каждая строка «поза» давала костей=0, а счётчик
        // пропущенных мёртвых был 0 вместо 142.

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
        if (want_bones && !transform_camera_mode) {
            fill_skeleton_box(s_transforms[i], vp, sw, sh, box);
            // Dummy rig stays at the boarding point while the box rides the
            // vehicle — drawing both is the two-position flicker.
            auto sk = g_skeletons.find(s_transforms[i]);
            if (sk != g_skeletons.end() && box.has_skeleton &&
                sk->second.bone_world_age[BONE_HIPS] >= 1) {
                float hx = sk->second.bone_world[BONE_HIPS].x - feet.x;
                float hz = sk->second.bone_world[BONE_HIPS].z - feet.z;
                if (hx * hx + hz * hz > 3.0F * 3.0F) {
                    box.has_skeleton = false;
                    for (int b = 0; b < ESP_BONE_COUNT; ++b) box.bone_valid[b] = false;
                }
            }
        }

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

        // Запасные точки (по трансформу головы и по росту от пяток) выключены:
        // просили «аим полностью по скелету и костям». Раньше при нечитаемом
        // скелете цель бралась от роста персонажа — это давало точку выше/ниже
        // настоящей головы и вносило ещё одно расхождение с нарисованным
        // скелетом. Теперь цель либо по костям, либо её нет вовсе (это
        // видно в логе ниже).
        // (блок сохранён, чтобы вернуть запасные точки одной правкой условия)
        if (false && g_aim_bones_requested && !transform_camera_mode &&
            !(box.aim_valid[0] && box.aim_valid[1] && box.aim_valid[2])) {
            Vec3 head{};
            bool have_head = false;
            // (2) game-maintained head transform (moves with crouch/animation)
            if (player_head_world(aux, head)) {
                float dy = head.y - feet.y;
                float hx = head.x - feet.x, hz = head.z - feet.z;
                have_head = dy > 0.4F && dy < 2.4F && (hx * hx + hz * hz) < 1.5F * 1.5F;
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
        // Мёртвый — по позе, а не по здоровью.
        //
        // Здоровье цели читать бессмысленно (жалоба 19.09, и лог это
        // подтвердил): цепочка vitals->Entity->Health->+0x20 читается верно
        // (у живых 100.0, у некоторых 0.0), но нули там принадлежат СПЯЩИМ
        // игрокам, а у только что убитого здоровье на клиенте остаётся
        // прежним. Поэтому смотрим на кости: труп лежит. У стоящего игрока
        // размах скелета по вертикали 1.5-1.8 м при горизонтали 0.4 м; у
        // лежащего — высота падает до 0.3-0.5 м, а горизонталь вырастает до
        // полутора. Костей должно быть не меньше половины: по обрубку из
        // четырёх-пяти костей позу не определишь.
        //
        // Здесь, а не раньше: fill_skeleton_box уже отработал и заполнил
        // skel_bones/skel_up/skel_flat для этого кадра.
        box.dead = box.respawning;
        // Собственный флаг смерти игры — первым делом: поза (ниже) ловит
        // только лежачих, а труп может ещё стоять. Чтение сорвалось — флаг
        // неизвестен (-1) и НЕ считается смертью: пометить живого игрока
        // мёртвым хуже, чем один кадр повести по трупу.
        {
            const uint64_t ref = rd_ptr(s_transforms[i] + game_offsets::PLAYER_DEATH_HANDLER);
            if (ref >= 0x10000) {
                const uint64_t dh = rd_ptr(ref + game_offsets::DEATH_HANDLER_REF_TARGET);
                if (dh >= 0x10000) {
                    const uint32_t f = rd<uint8_t>(dh + game_offsets::DEATH_HANDLER_DEATH);
                    if (f == 1) { box.death_flag = 1; box.dead = true; }
                    else if (f == 0) box.death_flag = 0;
                }
            }
        }
        if (box.skel_bones >= 11 && box.skel_up >= 0.f && box.skel_flat >= 0.f) {
            const bool lying = box.skel_up < 0.85f || box.skel_flat > box.skel_up * 1.5f;
            if (lying) box.dead = true;
        }
        // Что именно решило судьбу игрока — в лог: без этого порог непроверяем.
        // Печатаем только тех, у кого кости РЕАЛЬНО измерены: в логе 17:06
        // первые три игрока списка оказывались спящими (костей=0, высота=-1),
        // и по такому логу порог не проверить.
        {
            static double s_pose_at = 0.0;
            static int    s_pose_left = 0;
            const double t3 = memio::now_seconds();
            if (t3 >= s_pose_at) { s_pose_at = t3 + 8.0; s_pose_left = 3; }
            if (s_pose_left > 0 && box.skel_bones >= 6) {
                --s_pose_left;
                LogLine("аим: поза 0x%llx костей=%d высота=%.2f м ширина=%.2f м respawning=%d смерть=%d мёртв=%d здоровье=%.1f",
                        (unsigned long long)box.id, (int)box.skel_bones, (double)box.skel_up,
                        (double)box.skel_flat, (int)box.respawning, (int)box.death_flag,
                        (int)box.dead, (double)box.health);
            }
        }
        result.push_back(box);
    }

    return result;
}

int esp_nearby_player_count() { return g_frame_player_count; }

bool esp_wants_reattach() { return g_want_reattach.exchange(false); }

