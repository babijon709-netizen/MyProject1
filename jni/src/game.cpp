#include "game.h"
#include "aim.h"
#include "app_state.h"
#include "player_esp_watchdog.h"
#include "silent_aim.h"
#include "silent_aim_core.h"
#include "viewmodel.h"
#include "game_viewmodel_internal.h"
#include "aim_ballistics.h"
#include "mem_io.h"                // чтение/запись памяти игры: транспорт + кэш блоков
#include "kernel_io.h"             // кернел-транспорт (сокет ядерного драйвера)
#include "game_internal.h"        // общий срез состояния/помощников для game_markers/farm.cpp
#include "maps_lookup.h"           // базовый адрес библиотеки по /proc/<pid>/maps
#include "game_offsets_active.h"   // выбранный Release/Beta профиль
#include "position_filter.h"       // временной фильтр позиций игрока (хост-проверяемый)
#include "player_motion_prediction.h" // дорисовка сетевых позиций между тиками
#include "mount_latch.h"           // удержание бокса на транспорте (хост-проверяемый)
#include "body_clip.h"             // отсечение тела по ближней плоскости (хост-проверяемый)
#include "Vector.h"
#include "lang.h"      // РУ/EN: подписи визуалов (оружие, предметы, животные)
#include "text_utf8.h"  // копия подписи в буфер без разрезания символа UTF-8

#include <errno.h>     // errno при отказе записи (сайлент: ось выстрела)
#include <stdarg.h>    // va_list: форматирование диагностики скелетов в буфер
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
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <unistd.h>
#include <sys/syscall.h>

// ---- Доступ к памяти игры ---------------------------------------------------
// Подробности — в mem_io.h: только /proc/<pid>/mem (pread/pwrite), проверка
// доступа пробой по ELF-заголовку и кэш блоков на кадр. Здесь только обёртки,
// которыми пользуется весь остальной код чтения.
memio::Reader g_mem;

// Способ доступа к памяти: экстернал (/proc/<pid>/mem), кернел FT (сокет
// bing-класса), RT-Dev (/dev/[A-GNY]{1,6} с ioctl 0x801/0x802) или RT-Hook
// (перехваченный syscall ioctl 0x259/0x25a).
// Выбирается один раз на старте (экран режима, mode_select.h) до первого
// bind; сам g_mem о деталях не знает — разница целиком в memio::Transport.
static int g_mem_backend = ESP_MEM_EXTERNAL;

void esp_set_mem_backend(int backend) {
    g_mem_backend = backend;
    memio::Transport* t = &memio::procfs_transport();
    if (backend == ESP_MEM_FT) t = &memio::kernel_transport();
    else if (backend == ESP_MEM_RT_DEV) t = &memio::rt_dev_transport();
    else if (backend == ESP_MEM_RT_HOOK) t = &memio::rt_hook_transport();
    g_mem.set_transport(t);
}

bool esp_mem_backend_kernel() { return g_mem_backend != ESP_MEM_EXTERNAL; }
int  esp_get_mem_backend() { return g_mem_backend; }

// Последняя ошибка транспорта (errno открытия/чтения): для тоста «почему
// привязка не идёт» — в кернеле это ответ драйвера, в экстернале /proc.
int esp_mem_last_errno() { return g_mem.last_error(); }

// Сайлент-аим: доступ из своего потока — только через выбранный на экране
// способ чтения. Никакого собственного /proc/<pid>/mem: в кернел-режимах
// (FT/RT-Dev/RT-Hook) он появлялся в /proc/<self>/fd чита и молча ломался там, где
// прямой доступ к памяти закрыт. Чтение — read_quiet (мимо кадрового кэша
// рендер-потока, как у viewmodel), запись — обычный путь Reader.
bool esp_silent_target(int pid) {
    return g_mem.bound() && g_mem.pid() == pid;
}
pid_t esp_target_pid() {
    return g_mem.bound() ? g_mem.pid() : -1;
}
bool esp_silent_read(uint64_t addr, void* out, size_t len) {
    return g_mem.bound() && g_mem.read_quiet(addr, out, len);
}
bool esp_silent_write(uint64_t addr, const void* in, size_t len) {
    return g_mem.bound() && g_mem.write(addr, in, len);
}

// адресами без метки тоже нельзя. Снимаем метку сразу, на входе.

// Профиль смещений выбирается до первого bind и больше не меняется.
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
// Camera resolver anchors are stable for a world. Keeping them avoids making
// every ESP frame depend on four unrelated pointer reads succeeding together.
static uint64_t g_game_controller_static_fields_cached = 0;
static uint64_t g_camera_manager_cached = 0;

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
    for (int32_t index = 0; index < count && index < 8; ++index) {
        uint64_t player = rd_ptr(items + IL2CPP_ARRAY_FIRST_ELEMENT + (uint64_t)index * sizeof(uint64_t));
        if (!player) continue;
        // One unreadable or half-destroyed entry must not invalidate the whole
        // registry. The stable snapshot below validates every accepted object
        // again and simply skips entries whose class does not match.
        uint64_t klass = rd_ptr(player);
        if (klass != player_class) continue;
        if (++checked >= 2) break;
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

// static_fields GameControllerBase — единственный вход в статики движка
// (локальный игрок, хендлер ввода, ScreenControlsManager). Резолвим класс и
// здесь, а не только по дороге чтения реестра игроков и в esp_mem_frame_begin:
// памяти-джойстику автофарма (farm/joy, см. farm_joy.cpp) статики нужны
// независимо от того, запрашивал ли кадр ESP. Повторные вызовы бесплатны —
// результат кэшируется теми же полями, что и у кадра.
uint64_t game_controller_statics() {
    if (!g_game_controller_class && g_il2cpp_base && GAME_CONTROLLER_TYPEINFO_RVA != 0) {
        const uint64_t candidate = rd_ptr(g_il2cpp_base + GAME_CONTROLLER_TYPEINFO_RVA);
        if (class_identity(candidate, "GameControllerBase", "Oxide") != 0)
            g_game_controller_class = candidate;
    }
    if (!g_game_controller_static_fields_cached && g_game_controller_class)
        g_game_controller_static_fields_cached = get_class_static_fields(g_game_controller_class);
    return g_game_controller_static_fields_cached;
}

static int resolve_player_lists(uint64_t out[2]) {
    out[0] = out[1] = 0;
    if (!g_player_manager_class && PLAYER_MANAGER_TYPEINFO_RVA != 0) {
        const uint64_t candidate = rd_ptr(g_il2cpp_base + PLAYER_MANAGER_TYPEINFO_RVA);
        if (class_identity(candidate, "PlayerManager", "Oxide") != 0)
            g_player_manager_class = candidate;
    }
    if (!g_player_manager_class) return 0;
    if (!g_player_manager_static_fields)
        g_player_manager_static_fields = get_class_static_fields(g_player_manager_class);
    if (!g_player_manager_static_fields) return 0;

    // clientPlayerList is normally complete, but during network spawn its
    // insertion can lag activePlayerList by several frames. Reading only the
    // client list permanently missed objects on builds where the insertion
    // callback was skipped. Merge both registries; object-class validation
    // keeps unrelated or half-destroyed entries out.
    const uint64_t fields[2] = {
        PLAYER_MANAGER_STATIC_FIELDS_LIST,
        PLAYER_MANAGER_STATIC_FIELDS_ACTIVE_LIST,
    };
    int count = 0;
    for (uint64_t field : fields) {
        const uint64_t list = rd_ptr(g_player_manager_static_fields + field);
        if (!validate_player_list(list, g_player_manager_class)) continue;
        bool duplicate = false;
        for (int i = 0; i < count; ++i) duplicate |= out[i] == list;
        if (!duplicate) out[count++] = list;
    }
    if (count == 0) g_player_manager_static_fields = 0;
    return count;
}

static uint64_t resolve_player_list() {
    uint64_t lists[2]{};
    return resolve_player_lists(lists) > 0 ? lists[0] : 0;
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

// Локальный PlayerManager «наружу» (для модулей, которым он нужен целиком,
// например памяти-джойстику автофарма): та же проверенная цепочка, что и
// внутри game.cpp, включая сверку класса объекта с PlayerManager.
uint64_t local_player_checked() {
    return resolve_local_player();
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
static Matrix34 g_freecam_saved_mat{};    // целая матрица до включения
static Vec3     g_freecam_pos{};          // где камера сейчас (мировые)
static float    g_freecam_yaw = 0.f;
static float    g_freecam_pitch = 0.f;
static bool     g_freecam_has_yawpitch = false;
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
static Mat4 mat_mul(const Mat4& a, const Mat4& b);
static Mat4 mat_mul(const Mat4& a, const Mat4& b);

// Transform из GameObject. Раскладка фиксирована для поддерживаемого release
// и взята не из догадок, а из libunity.so
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
static const uint64_t TRANSFORM_ACCESS_HIERARCHY = TRANSFORM_HIERARCHY_DATA;

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

// Компонент GameObject по имени класса (нативный GameObject, тот же обход
// массива пар, что у transform_from_gameobject и mouse_look_from_camera).
// Зачем отдельная функция: UI-стик движения живёт компонентом на GameObject,
// который держит ScreenControlsReferenceHolder, — добраться до него можно
// только так. Проверки те же, что у поиска MouseLook по камере: слот не
// пустой, компонент ссылается назад на свой GameObject (+0x20), и имя класса
// совпало. Именно имя, а не «похоже на указатель»: в логе 19.09 поиск по
// структуре находил на камере ЧУЖОЙ компонент (см. комментарий у
// mouse_look_from_camera).
uint64_t gameobject_component_by_class(uint64_t native_gameobject, const char* class_name) {
    if (!native_gameobject || !class_name) return 0;
    const uint64_t components = rd_ptr(native_gameobject + GAMEOBJECT_COMPONENT_ARRAY);
    if (components < 0x10000) return 0;
    const int32_t count = rd<int32_t>(native_gameobject + GAMEOBJECT_COMPONENT_COUNT);
    if (count <= 0 || count > 64) return 0;
    for (int i = 0; i < count; ++i) {
        const uint64_t component =
            rd_ptr(components + (uint64_t)i * GAMEOBJECT_COMPONENT_STRIDE + COMPONENT_PAIR_PTR);
        if (component < 0x10000 || component == native_gameobject) continue;
        if (rd_ptr(component + COMPONENT_GAMEOBJECT) != native_gameobject) continue; // чужой слот
        if (object_class_name_is(component, class_name, false)) return component;
    }
    return 0;
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
            // Которое из трёх толкований дало позицию и что вообще нашлось.
            // Без этого не видно, сломался путь GameObject или сама иерархия.
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


bool esp_freecam_active() { return g_freecam_on; }

void esp_freecam_diag(int& code, float& dist) {
    code = g_freecam_fail;
    dist = g_freecam_fail_dist;
}

// Массивы иерархии именно для трансформа камеры. Их адреса читаются по
// единственной release-раскладке и подтверждаются позицией из матрицы вида;
// это не поиск смещений и не выбор между layout-кандидатами.
struct FcSlot { uint64_t m = 0, n = 0; int32_t i = -1; float d = -1.f; };
static constexpr int kFcMaxSlots = 8;
// Насколько далеко посчитанная позиция слота может стоять от камеры, чтобы
// слот считался её собственным. 2 м, а не полметра: g_cam_pos мог прийти из
// резервной матрицы вида (она считается по другому источнику и чуть
// расходится), и из-за жёсткого порога фрикам отказывался включаться там,
// где камера найдена верно.
static constexpr float kFcSlotMaxDist = 500.0f;
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
    static const uint64_t kExt[][2] = {{0x20, 0x28}, {0x28, 0x30}, {0x30, 0x38}, {0x40, 0x48}, {0x10, 0x18}, {0x48, 0x50}};
    auto try_pair = [&](uint64_t m0, uint64_t n0) {
        if (!m0 || !n0) return;
        const uint64_t ms[2] = {m0, rd_ptr(m0)};
        const uint64_t ns[2] = {n0, rd_ptr(n0)};
        for (uint64_t m : ms) {
            for (uint64_t n : ns) {
                if (!m || !n) continue;
                // Пробуем не только точный индекс камеры, но и соседние:
                // второй буфер может лежать под соседним индексом в том же
                // массиве (лог 21:47 — слотов 1, а мерцание 50/50).
                for (int di = -2; di <= 2; ++di) {
                    const int32_t ii = idx + di;
                    if (ii < 0 || ii > 100000) continue;
                    Vec3 pos{};
                    Vec4 rot{};
                    if (!read_transform_hierarchy_arrays(m, n, ii, pos, &rot)) continue;
                    if (!vec3_is_finite(pos)) continue;
                    const float dx = pos.x - cam_pos.x, dy = pos.y - cam_pos.y, dz = pos.z - cam_pos.z;
                    const float d = sqrtf(dx * dx + dy * dy + dz * dz);
                    if (err_m < 0.f || d < err_m) err_m = d;
                    fc_slot_add(m, n, ii, d);
                    if (d <= 1.0F && !matrices) { matrices = m; indices = n; index = ii; }
                    if (!matrices && d <= 100.0F) { matrices = m; indices = n; index = ii; }
                }
            }
        }
    };
    for (const auto& pr : kPairs) {
        try_pair(rd_ptr(data + pr[0]), rd_ptr(data + pr[1]));
    }
    // Широкий перебор — второй буфер иерархии может лежать по другому смещению.
    // Лог 21:47 всё ещё слотов 1 и 50/50 мерцание, значит второй буфер не нашли.
    for (const auto& pr : kExt) {
        try_pair(rd_ptr(data + pr[0]), rd_ptr(data + pr[1]));
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
    int32_t  idx  = rd<int32_t>(transform + TRANSFORM_HIERARCHY_INDEX);
    if (!data) {
        // Данные ещё не инициализированы: Unity кладёт сюда собственный блок
        // трансформа (+0x28) и берёт индекс из +0x30 (то же видно в 0x7ED640
        // и 0x7F19E0).
        data = transform + 0x28;
        idx  = rd<int32_t>(transform + 0x30);
    }
    if (!data || idx < 0 || idx > 100000) return false;
    matrices = rd_ptr(data + TRANSFORM_HIERARCHY_MATRICES);
    indices  = rd_ptr(data + TRANSFORM_HIERARCHY_INDICES);
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
// Пишем ЦЕЛУЮ Matrix34 (48 байт), а не только трансляцию: если игра затрёт
// поворот нулями, матрица станет невалидной и камера уйдёт в ноль (лог
// 21:10: за период 60 отсчётов — у цели 37, в нуле 23).
//
// ==== Что пишет фоновый писатель (и как оно туда попадает) =================
// Писатель каждую 1 мс повторяет матрицы в памяти игры: игра пересчитывает
// их из трансформа и затирала бы разовую запись. Раньше он ПЕРЕСЧИТЫВАЛ
// матрицы сам из g_cam_*/g_freecam_pos/g_fc_mat — обычных глобалей, которые
// главный поток переписывает прямо во время чтения. Писатель получал базис,
// собранный из разных моментов времени (r новый, f старый), и писал ЧУЖУЮ
// матрицу — кадры чередовались «наша/чужная», небо дёргалось (жалоба 20.09).
//
// Теперь главный поток ПУБЛИКУЕТ готовый снимок (view+proj+w2c одним блоком +
// трансформы слотов), а писатель повторяет его как есть, ничего не считая.
// Целостность снимка — сиквенс-локом: писатель повторяет чтение, пока счётчик
// до и после не совпадёт. Писатель один: поколение g_fc_writer_gen убивает
// опоздавший поток после быстрого выкл/вкл — иначе два писателя писали бы
// разное каждую 1 мс и дрались друг с другом.
struct FcSlotPayload {
    uint64_t addr = 0;    // matrices + index*48
    float    m[12] = {};  // Matrix34 целиком
};
struct FcPayload {
    std::atomic<unsigned> seq{0};   // чётное = снимок целостен
    uint64_t cam = 0;               // native Camera на момент публикации
    bool     has_view = false;
    float    view[16] = {};
    float    proj[16] = {};
    float    w2c[16] = {};
    FcSlotPayload t[kFcMaxSlots];
    int      tn = 0;
};
// Черновик главного потока (без seq): собирается в freecam_write, публикуется
// целиком и только при успехе.
struct FcSnapshotLocal {
    uint64_t cam = 0;
    bool     has_view = false;
    float    view[16] = {};
    float    proj[16] = {};
    float    w2c[16] = {};
    FcSlotPayload t[kFcMaxSlots];
    int      tn = 0;
};
static FcPayload             g_fc_pub;
static std::atomic<bool>     g_fc_writer_running{false};
static std::atomic<unsigned> g_fc_writer_gen{0};
// Поколение последнего ЗАВЕРШИВШЕГОСЯ писателя. Выключение ждёт совпадения с
// текущим поколением: слепые 5 мс не покрывали syscall'ы к занятой игрой
// памяти (сводки 06:39 ехали на +30..70 мс), и недождавшийся писатель
// дописывал СТАРЫЙ снимок уже в новой сессии — рваный кадр на старте сессии
// плюс «расхождение 0.0 м, в памяти (прошлая цель)».
static std::atomic<unsigned> s_fc_exited_gen{0};

static void freecam_payload_publish(const FcSnapshotLocal& s) {
    g_fc_pub.seq.fetch_add(1);   // нечётное — идёт запись
    g_fc_pub.cam = s.cam;
    g_fc_pub.has_view = s.has_view;
    memcpy(g_fc_pub.view, s.view, sizeof(s.view));
    memcpy(g_fc_pub.proj, s.proj, sizeof(s.proj));
    memcpy(g_fc_pub.w2c, s.w2c, sizeof(s.w2c));
    for (int i = 0; i < kFcMaxSlots; ++i) g_fc_pub.t[i] = s.t[i];
    g_fc_pub.tn = s.tn;
    g_fc_pub.seq.fetch_add(1);   // чётное — снимок целостен
}

static void freecam_payload_clear() {
    const FcSnapshotLocal empty{};
    freecam_payload_publish(empty);
}

// Позиция камеры из матрицы вида: m[12..14] = -R^T·p. Нужно писателю, чтобы
// посчитать, насколько «затёртое игрой» отличается от нашего (лог).
// ВНИМАНИЕ: расшифровка идёт по СТОЛБЦАМ (m[0],m[1],m[2] — первый столбец),
// как в camera_position_from_view. Здесь долго стояли СТРОКИ (m[0],m[4],m[8])
// — то есть считалось p = -R·t вместо p = -R^T·t (двойной поворот R²·T):
// при вертикали в пару градусов мнимая позиция уходила на сотни метров
// строго по Y при верных X/Z — те самые «расхождение 0.2 м, в памяти
// (1020, -41, -434) при цели (1020, 15, -436)» (лог 07:05). Матрицы при этом
// были верные — врала только диагностика, и за ней не было видно, борется
// ли игра по-настоящему (тело в слоте) или просто пересчитывает наш вид.
static Vec3 freecam_pos_from_view(const float m[16]) {
    const float tx = m[12], ty = m[13], tz = m[14];
    Vec3 p = {-(m[0]*tx + m[1]*ty + m[2]*tz),
              -(m[4]*tx + m[5]*ty + m[6]*tz),
              -(m[8]*tx + m[9]*ty + m[10]*tz)};
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) p = {};
    return p;
}
static std::atomic<unsigned> g_fc_write_count{0};
static std::atomic<bool>     g_fc_write_failed{false};
// Счётчик «игра затёрла нашу запись»: сколько раз за секунду позиция камеры
// между нашей записью и чтением успела поменяться. По нему видно, кто кого
// переписывает — это главный вопрос фрикама. Тройка «счётчик+сумма+позиция»
// плюс расшифровка снимка — под сиквенс-локом БЕЗ мьютекса.
//
// Мьютекс здесь не прижился: сводки («расхождение 1.2 м, в памяти в 200 м»)
// продолжали склеиваться из разных отсчётов и с ним (лог 06:39 — сборка уже
// с мьютексом, писатель 1 мс, базис чистый, а рвань та же). То ли std::mutex
// на этой сборке не исключает, то ли гонка глубже, — разбираться дальше
// нечем: вместо замка монотонные счётчики и сиквенс-лок по учебнику
// (релиз-барьер писателя, аквайр-барьер читателя — на ARM сиквенс-лок без
// барьеров рвётся сам). Писатель один, читатель один; читатель ничего не
// сбрасывает (иначе два издателя сломали бы чётность) — периоды считаются
// дельтами от прошлого чтения, сорванное чтение лишь переносит события в
// следующий период, но не теряет их.
// Поля ps (расшифровка снимка) — судья спора «кто врёт»: pm далеко + ps
// рядом = игра написала мусор; оба далеко = мусор наш, из снимка.
struct FcOwQ {
    std::atomic<unsigned> seq{0};   // чётное — целостно (пишет только писатель)
    std::atomic<uint64_t> ow{0};    // всего затираний (монотонно)
    std::atomic<uint64_t> ev{0};    // номер последнего события
    std::atomic<double>   owm{0.0}; // сумма расхождений, м (монотонно)
    std::atomic<float>    pmx{0}, pmy{0}, pmz{0};   // расшифровка памяти
    std::atomic<float>    psx{0}, psy{0}, psz{0};   // расшифровка снимка
};
static FcOwQ g_fc_owq;
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
// Последний хороший базис вида. Поза кадра иногда приходит рваной: чтение
// 64 байт матрицы вида рвётся о нашу же запись из потока-писателя (каждую
// ~1 мс) и пересчёты игры, а camera_basis_from_view оси нормирует, но
// ортогональность не проверяет. Одной конечной-проверки мало: скошенный, но
// конечный базис уходил в снимок И в память игры — игра рисовала рваный
// кадр, а расшифровка вида (она считает базис ортонормальным) давала точку
// в сотнях метров от цели: те самые «затёрто 1 раз, расхождение 0.2 м,
// в памяти в 767 м» (лог 03:10–03:15 — память и снимок согласны между собой,
// оба врут одинаково, потому что врёт сам снимок). Кривой базис отбрасываем
// и строим вид на прошлом хорошем: позиция свежая, ориентация отстаёт не
// больше чем на тик — вместо рваного кадра игра рисует прошлый хороший.
static Vec3    s_fc_basis_r{1.f, 0.f, 0.f}, s_fc_basis_u{0.f, 1.f, 0.f}, s_fc_basis_f{0.f, 0.f, 1.f};
static bool    s_fc_basis_have = false;
static std::atomic<unsigned> g_fc_skew{0};   // отброшено кривых базисов за секунду

static bool basis_is_orthonormal(const Vec3& r, const Vec3& u, const Vec3& f) {
    const float rl = r.x * r.x + r.y * r.y + r.z * r.z;
    const float ul = u.x * u.x + u.y * u.y + u.z * u.z;
    const float fl = f.x * f.x + f.y * f.y + f.z * f.z;
    if (fabsf(rl - 1.f) > 0.02f || fabsf(ul - 1.f) > 0.02f || fabsf(fl - 1.f) > 0.02f) return false;
    // Хорошие пути (кватернион, обзор, целая матрица) дают скалярные ~1e-7,
    // рваное чтение мешает строки разных ориентаций — десятые доли. Порог
    // 0.005 (~0.3°) посередине: ни ложных отбраковок, ни видимого перекоса.
    if (fabsf(r.x * u.x + r.y * u.y + r.z * u.z) > 0.005f) return false;
    if (fabsf(r.x * f.x + r.y * f.y + r.z * f.z) > 0.005f) return false;
    if (fabsf(u.x * f.x + u.y * f.y + u.z * f.z) > 0.005f) return false;
    const float cx = r.y * u.z - r.z * u.y, cy = r.z * u.x - r.x * u.z, cz = r.x * u.y - r.y * u.x;
    if (cx * f.x + cy * f.y + cz * f.z < 0.99f) return false;   // без зеркал
    return true;
}

// Проверить базис позы перед сборкой вида; кривой заменить прошлым хорошим.
// False — только если хорошего не было ни разу (первый тик с рваной позой —
// почти невозможно: до первой нашей записи память никто не рвёт).
static bool freecam_basis_ok(Vec3& r, Vec3& u, Vec3& f) {
    if (basis_is_orthonormal(r, u, f)) {
        s_fc_basis_r = r; s_fc_basis_u = u; s_fc_basis_f = f;
        s_fc_basis_have = true;
        return true;
    }
    if (!s_fc_basis_have) return false;
    r = s_fc_basis_r; u = s_fc_basis_u; f = s_fc_basis_f;
    g_fc_skew.fetch_add(1);
    return true;
}

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
    // «Уже там» отдельным выходом НЕ делаем: ранний return оставлял local_out
    // нулевым, и вызывающий писал (0,0,0) в трансформ камеры. Каждый свайп
    // обзора (цель не двигается) обнулял слот, следующий такт писал цель
    // обратно — трансформ чередовался «цель/ноль» десятки раз в секунду,
    // игра пересчитывала вид из трансформа, и небо прыгало между двумя
    // положениями (жалоба 20.09). Падаем в общий путь: при parent<0 local и
    // так равен цели, при родителе шаг выходит ~нулевым — та же запись цели.
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
        // Если у родителя нулевой масштаб — чиним его на 1,1,1, иначе любая локальная даст 0 в мире
        if (!(fabsf(pScale.x) > 1e-6F && fabsf(pScale.y) > 1e-6F && fabsf(pScale.z) > 1e-6F)) {
            Matrix34 pm{};
            if (rd_fresh(matrices + (uint64_t)parent * sizeof(Matrix34), pm)) {
                pm.scale = {1.f,1.f,1.f,0.f};
                if (pm.rotation.x==0.f && pm.rotation.y==0.f && pm.rotation.z==0.f && pm.rotation.w==0.f) pm.rotation.w=1.f;
                wr_buf(matrices + (uint64_t)parent * sizeof(Matrix34), &pm, sizeof(Matrix34));
                pScale = {1.f,1.f,1.f};
            } else {
                return false;
            }
        }
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
            if (!(len > 1e-6F)) {
                // Кватернион родителя нулевой — ставим identity
                inv = {0.f,0.f,0.f,1.f};
            } else {
                inv = {-inv.x / len, -inv.y / len, -inv.z / len, inv.w / len};
            }
            const Vec3 turned = rotate_vector(inv, d);
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
    // Предохранитель — только для относительного шага: там local считается из
    // прочитанного cur, и мусорный отсчёт дал бы мусорную запись. При
    // абсолютной записи local выведен из цели и от cur не зависит — большой
    // шаг означает лишь протухший отсчёт, а запись всё равно верная, и
    // отбрасывать её нельзя: иначе слот останется на мусоре навсегда
    // (лог 02:25 — 2214 отброшенная поправка подряд, трансформ вечно в нуле).
    const float sl = sqrtf(step.x * step.x + step.y * step.y + step.z * step.z);
    if (sl > 200.f && !absolute) {
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
    // Поколение: быстрый выкл/вкл плодил двух писателей — старый ещё выходил,
    // новый уже стартовал, и оба писали (разное) каждую 1 мс. Опоздавший,
    // увидев чужое поколение, выходит сам.
    const unsigned my_gen = g_fc_writer_gen.fetch_add(1) + 1;
    std::thread([my_gen]() {
        // Метка выхода (RAII — накрывает и выход по поколению, и конец цикла):
        // выключение ждёт её вместо слепой паузы.
        struct ExitMark { unsigned g; ~ExitMark() { s_fc_exited_gen.store(g); } } mark{my_gen};
        float last_block[48] = {};
        bool  have_last = false;
        uint64_t last_cam = 0;
        while (g_fc_writer_running.load()) {
            if (my_gen != g_fc_writer_gen.load()) return;
            // Целостный снимок под сиквенс-локом: главный поток публикует
            // готовые матрицы, мы их только повторяем — ничего не считаем.
            uint64_t cam = 0;
            bool     has_view = false;
            float    view[16] = {}, proj[16] = {}, w2c[16] = {};
            FcSlotPayload t[kFcMaxSlots];
            int tn = 0;
            bool snapped = false;
            for (int spin = 0; spin < 64; ++spin) {
                const unsigned s0 = g_fc_pub.seq.load();
                if (s0 & 1u) continue;   // идёт публикация — повторить
                cam = g_fc_pub.cam;
                has_view = g_fc_pub.has_view;
                memcpy(view, g_fc_pub.view, sizeof(view));
                memcpy(proj, g_fc_pub.proj, sizeof(proj));
                memcpy(w2c, g_fc_pub.w2c, sizeof(w2c));
                for (int i = 0; i < kFcMaxSlots; ++i) t[i] = g_fc_pub.t[i];
                tn = g_fc_pub.tn;
                if (s0 == g_fc_pub.seq.load()) { snapped = true; break; }
            }
            if (!snapped || g_pid <= 0 || (!has_view && tn <= 0)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            if (cam != last_cam) { have_last = false; last_cam = cam; }
            if (has_view && cam) {
                // Один блок 0x70..0x130 (view+proj+w2c лежат подряд): одна
                // запись вместо трёх — игра не может вклинить свой пересчёт
                // между нашей записью view и w2c и рассогласовать пару.
                float block[48];
                memcpy(block, view, sizeof(view));
                memcpy(block + 16, proj, sizeof(proj));
                memcpy(block + 32, w2c, sizeof(w2c));
                const uint64_t vaddr = cam + CAMERA_VIEW_MATRIX;
                float cur[48] = {};
                // read_quiet: писатель вне кадра оверлея, блочный кэш ему
                // чужой — сравнение только по свежим байтам.
                const bool rd_ok = g_mem.read_quiet(vaddr, cur, sizeof(cur));
                // Чтение могло стоять десятки мс (игра занята): за это время
                // сессия успевает смениться, и наш снимок — уже прошлое.
                // Писать его в новую сессию и считать по нему затирания
                // нельзя — выходим, новое поколение уже работает.
                if (!g_fc_writer_running.load() || my_gen != g_fc_writer_gen.load()) return;
                const bool same = rd_ok && memcmp(cur, block, sizeof(cur)) == 0;
                if (!same) {
                    // В памяти не наше нынешнее. Если это и не наше прошлое —
                    // между записями писал кто-то ещё: это и есть «затёрто
                    // игрой», только без остановки писателя (прежняя «проба»
                    // молчала 24 мс каждые 2 с — и сама дёргала картинку).
                    if (rd_ok && have_last && memcmp(cur, last_block, sizeof(cur)) != 0) {
                        const Vec3 pm = freecam_pos_from_view(cur);
                        const Vec3 ps = freecam_pos_from_view(block);
                        const float dx = pm.x - ps.x, dy = pm.y - ps.y, dz = pm.z - ps.z;
                        const float dd = sqrtf(dx * dx + dy * dy + dz * dz);
                        // Событие — в сиквенс-лок (см. FcOwQ): нечётный seq,
                        // поля обычным порядком (писатель один — relaxed
                        // хватает), релиз-барьер, чётный seq. Проверка
                        // конечности тут критична: сумма монотонная и не
                        // сбрасывается — один NaN отравил бы её навсегда.
                        FcOwQ& q = g_fc_owq;
                        q.seq.fetch_add(1, std::memory_order_relaxed);
                        q.ow.store(q.ow.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
                        if (std::isfinite(dd))
                            q.owm.store(q.owm.load(std::memory_order_relaxed) + (double)dd, std::memory_order_relaxed);
                        q.pmx.store(pm.x, std::memory_order_relaxed);
                        q.pmy.store(pm.y, std::memory_order_relaxed);
                        q.pmz.store(pm.z, std::memory_order_relaxed);
                        q.psx.store(ps.x, std::memory_order_relaxed);
                        q.psy.store(ps.y, std::memory_order_relaxed);
                        q.psz.store(ps.z, std::memory_order_relaxed);
                        q.ev.store(q.ev.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
                        std::atomic_thread_fence(std::memory_order_release);
                        q.seq.fetch_add(1, std::memory_order_relaxed);
                    }
                    if (wr_buf(vaddr, block, sizeof(block))) {
                        g_fc_write_count.fetch_add(1);
                        memcpy(last_block, block, sizeof(block));
                        have_last = true;
                    } else {
                        g_fc_write_failed.store(true);
                    }
                }
                // Dirty гасим всегда, даже когда матрицы не писали: игру,
                // пометившую кэш несвежим после нашей записи, тишина не
                // остановит — она пересчитает вид из трансформа на рендере.
                // Однобайтные записи атомарны — рваных чтений они не дают.
                uint8_t zero = 0;
                wr_buf(cam + CAMERA_VIEW_DIRTY, &zero, 1);
                wr_buf(cam + CAMERA_PROJ_DIRTY, &zero, 1);
            }
            // Трансформы слотов — безусловно, как раньше: их игра
            // переписывает каждый кадр из положения тела, сравнение с памятью
            // здесь всегда «не сошлось» и только лишний syscall.
            // Та же проверка протухания, что после чтения: запись вида выше —
            // тоже syscall, сессия могла смениться уже после снимка.
            if (!g_fc_writer_running.load() || my_gen != g_fc_writer_gen.load()) return;
            for (int sl = 0; sl < tn && sl < kFcMaxSlots; ++sl) {
                if (!t[sl].addr) continue;
                if (wr_buf(t[sl].addr, t[sl].m, sizeof(t[sl].m)))
                    g_fc_write_count.fetch_add(1);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }).detach();
}

bool esp_freecam_set(bool on) {
    if (on == g_freecam_on) return g_freecam_on;
    if (!on) {
        g_fc_writer_running.store(false);
        freecam_payload_clear();   // опоздавший писатель не подхватит старье
        // Ждём РЕАЛЬНОГО выхода писателя, а не слепые 5 мс: иначе он допишет
        // старый снимок уже после восстановления трансформа ниже. Пауза была
        // 5 мс, а syscall'ы к занятой игрой памяти встают на десятки мс.
        // Не дождались за 300 мс — дальше без него: проверки поколения после
        // syscall'ов не дадут зависшему писать старье, когда доплывёт.
        {
            const unsigned wgen = g_fc_writer_gen.load();
            for (int w = 0; w < 300 && s_fc_exited_gen.load() != wgen; ++w)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        g_fc_last_m = g_fc_last_n = 0; g_fc_last_i = -1;
        if (g_freecam_saved_ok && g_freecam_matrices && g_freecam_index >= 0)
            wr_buf(g_freecam_matrices + (uint64_t)g_freecam_index * sizeof(Matrix34),
                   &g_freecam_saved_mat, sizeof(Matrix34));
        // Восстанавливаем вид: ставим dirty=1 чтобы игра пересчитала view из восстановленного трансформа
        if (g_native_camera) {
            uint8_t one=1;
            wr_buf(g_native_camera + CAMERA_VIEW_DIRTY, &one, 1);
        }
        g_freecam_on = false;
        g_freecam_saved_ok = false;
        g_freecam_has_yawpitch = false;
        return false;
    }
    if (g_pid <= 0 || !g_il2cpp_base || !g_mem.bound()) return false;
    uint64_t transform = camera_transform();
    if (!transform) {
        g_freecam_fail = g_native_camera ? 2 : 1; g_freecam_fail_dist = -1.f;
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
            g_freecam_fail = dist >= 0.f ? 4 : 3;
            g_freecam_fail_dist = dist;
            return false;
        }
    }
    Matrix34 m{};
    if (!rd_exact(matrices + (uint64_t)index * sizeof(Matrix34), m)) {
        g_freecam_fail = 3; g_freecam_fail_dist = -1.f;
        return false;
    }
    Vec3 pos{}, scale{};
    Vec4 rot{};
    if (!read_transform_world_trs(matrices, indices, index, pos, rot, scale)) return false;
    g_freecam_saved = {m.translation.x, m.translation.y, m.translation.z};
    g_freecam_saved_mat = m;
    g_freecam_saved_ok = true;
    g_freecam_transform = transform;
    g_freecam_matrices = matrices;
    g_freecam_indices = indices;
    g_freecam_index = index;
    // Запоминаем и в счётчике подмен: отсюда массивы считаются эталонными,
    // и freecam_write() больше не имеет права их подменять.
    g_fc_last_m = matrices; g_fc_last_n = indices; g_fc_last_i = index;
    // Снимок слотов, resolved through the fixed release Transform ABI.
    g_fc_own_n = g_fc_slot_n;
    for (int k = 0; k < g_fc_own_n; ++k) g_fc_own[k] = g_fc_slots[k];
    if (g_fc_own_n <= 0) { g_fc_own[0] = FcSlot{matrices, indices, index, dist}; g_fc_own_n = 1; }
    g_freecam_pos = pos;
    if (g_cam_pose_valid && vec3_is_finite(g_cam_forward)) {
        g_freecam_yaw = atan2f(g_cam_forward.x, g_cam_forward.z) * 57.29578f;
        float horiz = sqrtf(g_cam_forward.x*g_cam_forward.x + g_cam_forward.z*g_cam_forward.z);
        g_freecam_pitch = atan2f(g_cam_forward.y, horiz) * 57.29578f;
        g_freecam_has_yawpitch = true;
    } else {
        g_freecam_has_yawpitch = false;
    }
    g_freecam_on = true;
    g_freecam_fail = 0; g_freecam_fail_dist = dist;   // 0 = порядок
    // Сколько слотов ведём — по логу видно, один набор массивов у камеры или
    // два чередующихся (жалоба «мерицает»): при одном слоте второй записи нет.
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
static Mat4 mat_mul(const Mat4& a, const Mat4& b);

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
    // Фрикам теперь пишет матрицу вида и worldToClip, а не трансформ:
    // запись трансформа двигала тело (если нашли рут игрока) и не давала
    // пройти сквозь стены. Матрица вида — то, чем игра рисует кадр, но одной
    // её мало: игра может использовать worldToClip напрямую (projection *
    // view), и тогда вид остаётся на месте, а двигаются только визуалы ESP
    // (жалоба «двигаются только визуалы, а от своего лица стою на месте»).
    // Черновик снимка для писателя: публикуется целиком и только при успехе.
    // Писатель повторяет снимок как есть — матрицы здесь уже готовые, сам он
    // ничего не считает (иначе кадры чередовались бы «наша/чуждая» и небо
    // дёргалось — жалоба 20.09).
    FcSnapshotLocal pub;
    pub.cam = g_native_camera;
    // Пишем обе.
    bool ok = false;
    if (g_native_camera && g_cam_pose_valid) {
        Vec3 r = g_cam_right, u = g_cam_up, f = g_cam_forward;
        if (vec3_is_finite(r) && vec3_is_finite(u) && vec3_is_finite(f) &&
            freecam_basis_ok(r, u, f)) {
            Mat4 view{};
            view.m[0] = r.x; view.m[4] = r.y; view.m[8]  = r.z; view.m[12] = -(r.x * g_freecam_pos.x + r.y * g_freecam_pos.y + r.z * g_freecam_pos.z);
            view.m[1] = u.x; view.m[5] = u.y; view.m[9]  = u.z; view.m[13] = -(u.x * g_freecam_pos.x + u.y * g_freecam_pos.y + u.z * g_freecam_pos.z);
            view.m[2] = -f.x; view.m[6] = -f.y; view.m[10] = -f.z; view.m[14] = -(-f.x * g_freecam_pos.x + -f.y * g_freecam_pos.y + -f.z * g_freecam_pos.z);
            view.m[3] = 0.f; view.m[7] = 0.f; view.m[11] = 0.f; view.m[15] = 1.f;
            // Projection читаем строго (rd_exact): сорванное чтение давало
            // нулевой w2c — кадр с нулевой матрицей клипа, то есть чёрная
            // вспышка. Невалидный proj — пропускаем вид целиком: писатель
            // повторит прошлый хороший снимок, а не мусор.
            Mat4 proj{};
            bool proj_ok = false;
            if (rd_exact(g_native_camera + CAMERA_PROJECTION_MATRIX, proj)) {
                proj_ok = (proj.m[0] != 0.f);
                for (int k = 0; proj_ok && k < 16; ++k)
                    proj_ok = std::isfinite(proj.m[k]);
            }
            if (proj_ok) {
                Mat4 w2c = mat_mul(proj, view);
                // Один блок 0x70..0x130 (view+proj+w2c лежат подряд, см.
                // game_offsets.h): за один syscall пара view/w2c не может
                // рассогласоваться пересчётом игры между двумя записями.
                // PREV_VIEW_PROJ не трогаем (должен быть предыдущим кадром),
                // а dirty сбрасываем в 0 чтобы игра не пересчитывала вид.
                float block[48];
                memcpy(block, view.m, sizeof(view.m));
                memcpy(block + 16, proj.m, sizeof(proj.m));
                memcpy(block + 32, w2c.m, sizeof(w2c.m));
                if (wr_buf(g_native_camera + CAMERA_VIEW_MATRIX, block, sizeof(block))) ok = true;
                {
                    uint8_t zero = 0;
                    wr_buf(g_native_camera + CAMERA_VIEW_DIRTY, &zero, 1);
                    wr_buf(g_native_camera + CAMERA_PROJ_DIRTY, &zero, 1);
                }
                memcpy(pub.view, view.m, sizeof(view.m));
                memcpy(pub.proj, proj.m, sizeof(proj.m));
                memcpy(pub.w2c, w2c.m, sizeof(w2c.m));
                pub.has_view = true;
                g_fc_gap.store(0.f);
            }
        }
    }
    // Трансформы слотов пишем всегда (и в снимок, и сразу): если игра всё же
    // пересчитает вид из трансформа, он возьмётся уже из цели, а не из тела.
    // Это же закрывает «двигаются только визуалы, а от первого лица стою».
    {
        bool first_gap = true;
        for (int sl = 0; sl < g_fc_own_n && pub.tn < kFcMaxSlots; ++sl) {
            const FcSlot& S = g_fc_own[sl];
            if (!S.m || !S.n || S.i < 0) continue;
            Vec3 local{}; float gap = -1.f;
            if (!freecam_local_for_target(S.m, S.n, S.i, g_freecam_pos, local, gap)) continue;
            Matrix34 cur_m{};
            if (!rd_fresh(S.m + (uint64_t)S.i * sizeof(Matrix34), cur_m)) continue;
            if (!matrix34_is_valid(cur_m)) { cur_m.rotation={0.f,0.f,0.f,1.f}; cur_m.scale={1.f,1.f,1.f,0.f}; }
            cur_m.translation.x=local.x; cur_m.translation.y=local.y; cur_m.translation.z=local.z;
            const uint64_t addr = S.m + (uint64_t)S.i * sizeof(Matrix34);
            FcSlotPayload& P = pub.t[pub.tn++];
            P.addr = addr;
            P.m[0]=cur_m.translation.x; P.m[1]=cur_m.translation.y; P.m[2]=cur_m.translation.z; P.m[3]=cur_m.translation.w;
            P.m[4]=cur_m.rotation.x;    P.m[5]=cur_m.rotation.y;    P.m[6]=cur_m.rotation.z;    P.m[7]=cur_m.rotation.w;
            P.m[8]=cur_m.scale.x;       P.m[9]=cur_m.scale.y;       P.m[10]=cur_m.scale.z;      P.m[11]=cur_m.scale.w;
            if (wr_buf(addr, &cur_m, sizeof(Matrix34))) ok = true;
            if (first_gap && gap >= 0.f) { if (!pub.has_view) g_fc_gap.store(gap); first_gap = false; }
        }
    }
    if (ok) freecam_payload_publish(pub);
    // Без успеха снимок не трогаем: писатель повторит прошлый хороший, а не
    // полупустой черновик.
    if (!ok) return false;
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
    Vec3 f, r, u;
    if (g_freecam_has_yawpitch) {
        float yaw_rad = g_freecam_yaw * 0.0174532925f;
        float pitch_rad = g_freecam_pitch * 0.0174532925f;
        float cy = cosf(yaw_rad), sy = sinf(yaw_rad);
        float cp = cosf(pitch_rad), sp = sinf(pitch_rad);
        f = {sy * cp, sp, cy * cp};
        r = {cy, 0.f, -sy};
        u = {0.f, 1.f, 0.f};
    } else {
        f = g_cam_forward; r = g_cam_right; u = g_cam_up;
        if (!g_cam_pose_valid || !vec3_is_finite(f) || !vec3_is_finite(r)) { f = {0, 0, 1}; r = {1, 0, 0}; u = {0,1,0}; }
    }
    float fl = sqrtf(f.x*f.x + f.y*f.y + f.z*f.z);
    if (fl > 1e-6f) { f.x/=fl; f.y/=fl; f.z/=fl; }
    float rl = sqrtf(r.x*r.x + r.y*r.y + r.z*r.z);
    if (rl > 1e-6f) { r.x/=rl; r.y/=rl; r.z/=rl; }
    g_freecam_pos.x += f.x * forward + r.x * right + u.x * up;
    g_freecam_pos.y += f.y * forward + r.y * right + u.y * up;
    g_freecam_pos.z += f.z * forward + r.z * right + u.z * up;
    if (!vec3_is_finite(g_freecam_pos)) { g_freecam_pos = {}; return false; }
    return freecam_write();
}


bool esp_freecam_look(float yaw_delta, float pitch_delta) {
    if (!g_freecam_on) return false;
    if (!std::isfinite(yaw_delta) || !std::isfinite(pitch_delta)) return false;
    if (!g_freecam_has_yawpitch) {
        float yaw=0.f, pitch=0.f;
        if (g_cam_pose_valid && vec3_is_finite(g_cam_forward)) {
            yaw = atan2f(g_cam_forward.x, g_cam_forward.z) * 57.29578f;
            float horiz = sqrtf(g_cam_forward.x*g_cam_forward.x + g_cam_forward.z*g_cam_forward.z);
            pitch = atan2f(g_cam_forward.y, horiz) * 57.29578f;
        }
        g_freecam_yaw = yaw;
        g_freecam_pitch = pitch;
        g_freecam_has_yawpitch = true;
    }
    g_freecam_yaw += yaw_delta;
    g_freecam_pitch += pitch_delta;
    if (g_freecam_pitch > 85.f) g_freecam_pitch = 85.f;
    if (g_freecam_pitch < -85.f) g_freecam_pitch = -85.f;
    float yaw_rad = g_freecam_yaw * 0.0174532925f;
    float pitch_rad = g_freecam_pitch * 0.0174532925f;
    float cy = cosf(yaw_rad), sy = sinf(yaw_rad);
    float cp = cosf(pitch_rad), sp = sinf(pitch_rad);
    Vec3 fwd = {sy * cp, sp, cy * cp};
    Vec3 right = {cy, 0.f, -sy};
    Vec3 up = {-sy * sp, cp, -cy * sp};
    float fl = sqrtf(fwd.x*fwd.x + fwd.y*fwd.y + fwd.z*fwd.z);
    if (fl > 1e-6f) { fwd.x/=fl; fwd.y/=fl; fwd.z/=fl; }
    float rl = sqrtf(right.x*right.x + right.y*right.y + right.z*right.z);
    if (rl > 1e-6f) { right.x/=rl; right.y/=rl; right.z/=rl; }
    float ul = sqrtf(up.x*up.x + up.y*up.y + up.z*up.z);
    if (ul > 1e-6f) { up.x/=ul; up.y/=ul; up.z/=ul; }
    g_cam_forward = fwd;
    g_cam_right = right;
    g_cam_up = up;
    g_cam_pose_valid = true;
    return freecam_write();
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
            // Чтение сиквенс-лока (см. FcOwQ): чётный seq, поля, аквайр-барьер
            // (без него на ARM чтения полей уплывают за вторую проверку seq и
            // снимок рвётся), повторная проверка. Сорвалось — не беда: база
            // не двигается, и события периода доберутся следующей дельтой.
            uint64_t ow_t = 0, ev_t = 0; double owm_t = 0.0;
            float pmx = 0, pmy = 0, pmz = 0, psx = 0, psy = 0, psz = 0;
            bool ow_snap = false;
            for (int spin = 0; spin < 64; ++spin) {
                const unsigned s0 = g_fc_owq.seq.load(std::memory_order_acquire);
                if (s0 & 1u) continue;
                ow_t = g_fc_owq.ow.load(std::memory_order_relaxed);
                owm_t = g_fc_owq.owm.load(std::memory_order_relaxed);
                pmx = g_fc_owq.pmx.load(std::memory_order_relaxed);
                pmy = g_fc_owq.pmy.load(std::memory_order_relaxed);
                pmz = g_fc_owq.pmz.load(std::memory_order_relaxed);
                psx = g_fc_owq.psx.load(std::memory_order_relaxed);
                psy = g_fc_owq.psy.load(std::memory_order_relaxed);
                psz = g_fc_owq.psz.load(std::memory_order_relaxed);
                ev_t = g_fc_owq.ev.load(std::memory_order_relaxed);
                std::atomic_thread_fence(std::memory_order_acquire);
                if (s0 == g_fc_owq.seq.load(std::memory_order_acquire)) { ow_snap = true; break; }
            }
            static uint64_t s_ow_prev = 0, s_ev_prev = 0;
            static double   s_owm_prev = 0.0;
            unsigned ow = 0; float owm = 0.f;
            float owx = 0.f, owy = 0.f, owz = 0.f, osx = 0.f, osy = 0.f, osz = 0.f;
            if (ow_snap) {
                ow = (unsigned)(ow_t - s_ow_prev);
                owm = (float)(owm_t - s_owm_prev);
                owx = pmx; owy = pmy; owz = pmz; osx = psx; osy = psy; osz = psz;
                s_ow_prev = ow_t; s_owm_prev = owm_t; s_ev_prev = ev_t;
            }
            const unsigned sk = g_fc_skew.exchange(0);
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
                const unsigned sf = g_fc_samp.exchange(0), hf = g_fc_hit.exchange(0),
                               zf = g_fc_zero.exchange(0), of = g_fc_other.exchange(0);
            }
            // Чем именно оказалась затёрта наша запись. По этому значению
            // видно, кто второй пишет в ту же ячейку: если это текущая позиция
            // головы — запись перебивает игра, и бороться надо её кадром, а не
            // частотой нашего писателя.
            if (ow)
                ;
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

// Перебор раскладок иерархии — самая тяжёлая работа во всём чите: три сида на
// 61 смещение данных на 4 смещения индекса на 33 пары массивов на 4 варианта
// разыменования, и КАЖДЫЙ кандидат, который вообще прочитался, ещё и
// проверяется по всем известным трансформам с подъёмом по родителям. Целиком
// это сотни миллисекунд в одном кадре — те самые «esp 218 мс» из кадргиста.
//
// Перебор не урезан: порядок обхода, отбор лучшего и результат в точности те
// же. Изменено ровно одно — за один вызов делается не больше kProbeBudget
// проверок, а состояние обхода сохраняется и продолжается со следующего
// кадра. Раскладка находится за несколько кадров вместо одного залипшего.
namespace {
struct LayoutScan {
    uint64_t signature = 0;      // по какому набору трансформов идёт обход
    size_t   seed_index = 0;
    uint64_t data_offset = 0x10;
    int      delta_index = 0;
    uint64_t matrices_offset = 0;
    bool     in_progress = false;
    TransformHierarchyLayout best_layout{};
    double   best_score = 0.0;
    size_t   best_position_count = 0;
    size_t   candidate_count = 0;
};
LayoutScan g_layout_scan;

uint64_t layout_scan_signature(const std::vector<uint64_t>& transforms) {
    uint64_t signature = 0x9E3779B97F4A7C15ULL ^ (uint64_t)transforms.size();
    for (uint64_t transform : transforms) signature = signature * 1099511628211ULL ^ transform;
    return signature ? signature : 1;
}
}  // namespace

// Обход раскладок ещё не закончен: вызывающий обязан позвать снова на
// следующем кадре, а не ждать своего обычного интервала.
bool layout_scan_in_progress() { return g_layout_scan.in_progress; }

bool discover_layout_from_native_transforms(const std::vector<uint64_t>& native_transforms,
                                                   size_t& best_position_count, size_t& candidate_count) {
    if (native_transforms.size() < 2) return false;

    static constexpr int64_t kIndexDeltas[] = {-8, 8, 16, 24};
    // Сколько кандидатов проверяем за один вызов. Каждая проверка — несколько
    // чтений через кэш блоков плюс подъём по родителям, то есть единицы
    // микросекунд; 1500 штук укладываются примерно в миллисекунду.
    static constexpr int kProbeBudget = 1500;

    LayoutScan& scan = g_layout_scan;
    const uint64_t signature = layout_scan_signature(native_transforms);
    if (!scan.in_progress || scan.signature != signature) {
        // Состав трансформов сменился — обход начинается заново, иначе лучший
        // кандидат сравнивался бы с оценками от другого набора.
        scan = LayoutScan{};
        scan.signature = signature;
        scan.in_progress = true;
    }

    const size_t seed_count = std::min<size_t>(native_transforms.size(), 3);
    int probes = 0;

    for (; scan.seed_index < seed_count; ++scan.seed_index, scan.data_offset = 0x10,
                                         scan.delta_index = 0, scan.matrices_offset = 0) {
        const uint64_t seed = native_transforms[scan.seed_index];
        for (; scan.data_offset <= 0x200; scan.data_offset += 8, scan.delta_index = 0,
                                          scan.matrices_offset = 0) {
            const uint64_t transform_data = rd_ptr(seed + scan.data_offset);
            if (!likely_native_pointer(transform_data)) continue;
            for (; scan.delta_index < (int)(sizeof(kIndexDeltas) / sizeof(kIndexDeltas[0]));
                 ++scan.delta_index, scan.matrices_offset = 0) {
                const int64_t signed_index_offset =
                    (int64_t)scan.data_offset + kIndexDeltas[scan.delta_index];
                if (signed_index_offset < 0x10 || signed_index_offset > 0x220) continue;
                const uint64_t index_offset = (uint64_t)signed_index_offset;
                const int32_t transform_index = rd<int32_t>(seed + index_offset);
                if (transform_index < 0 || transform_index > 100000) continue;
                for (; scan.matrices_offset <= 0x100; scan.matrices_offset += 8) {
                    if (probes >= kProbeBudget) {
                        // Бюджет кадра исчерпан: отдаём накопленное и
                        // продолжаем ровно отсюда на следующем вызове.
                        best_position_count = scan.best_position_count;
                        candidate_count = scan.candidate_count;
                        return false;
                    }
                    const uint64_t indices_offset = scan.matrices_offset + 8;
                    const uint64_t matrices = rd_ptr(transform_data + scan.matrices_offset);
                    const uint64_t indices_ptr = rd_ptr(transform_data + indices_offset);
                    if (!likely_native_pointer(matrices) || !likely_native_pointer(indices_ptr)) continue;
                    for (int matrices_indirect = 0; matrices_indirect < 2; ++matrices_indirect) {
                        for (int indices_indirect = 0; indices_indirect < 2; ++indices_indirect) {
                            TransformHierarchyLayout layout{};
                            layout.data_offset = scan.data_offset; layout.index_offset = index_offset;
                            layout.matrices_offset = scan.matrices_offset; layout.indices_offset = indices_offset;
                            layout.matrices_indirect = matrices_indirect != 0; layout.indices_indirect = indices_indirect != 0;
                            ++probes;
                            Vec3 seed_position{};
                            if (!read_transform_hierarchy_layout(seed, layout, seed_position)) continue;
                            ++scan.candidate_count;
                            size_t position_count = 0; double extent = 0.0;
                            bool valid = evaluate_transform_hierarchy_layout(native_transforms, layout, position_count, extent);
                            scan.best_position_count = std::max(scan.best_position_count, position_count);
                            if (!valid) continue;
                            double score = (double)position_count * 1000000.0 + std::min(extent, 999999.0);
                            if (score > scan.best_score) { scan.best_score = score; scan.best_layout = layout; }
                        }
                    }
                }
            }
        }
    }

    // Полный проход закончен: решение принимается по тем же правилам, что и
    // раньше, — лучший кандидат за весь перебор.
    best_position_count = scan.best_position_count;
    candidate_count = scan.candidate_count;
    const bool found = scan.best_score > 0.0;
    TransformHierarchyLayout found_layout = scan.best_layout;
    scan = LayoutScan{};
    if (!found) return false;
    g_transform_hierarchy_layout = found_layout;
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

// Сентинел пулового мусора: свежий объект (игра пересоздаёт PlayerManager
// каждые ~10 с, см. лог «сменили победителя») пока недоинициализирован отдаёт
// в поле позиции вектор ВИДА (x_любой, 0.0, 0.0) — в логе x принимал 0.1,
// 0.8, 3.1, 4.8, 9.5, 11.8, а y и z всегда ровный ноль. Такая точка проходит
// world-space-гейт и рисует бокс под картой (нырок y 10.4 -> 0.0 за кадр).
// Ловим ПОДПИСЬ этого мусора, а не «две почти нулевые оси из трёх». Пара
// ровных нулей у y и z — всегда недоделанный объект и почти никогда живая
// позиция; а вот x у мусора произвольный (0.1 ... 11.8), поэтому требовать
// нуля по трём осям нельзя — подпись (x любой, y0, z0) тогда проходит. Порог
// по y/z узкий (2 см), потому что широкие 0.25 м выкидывали живого игрока на
// спавне в центре карты: там x и z тоже стоят в нуле, и трек не подтверждался
// НИКОГДА. Дополнительно гейт применяется только к первой точке трека
// (см. вызовы): подтверждённая позиция нулём быть не перестаёт.
static bool position_near_world_origin(const Vec3& v) {
    return fabsf(v.y) < 0.02F && fabsf(v.z) < 0.02F;
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
        // Сэмпл вида (x, 0, 0) — не живая позиция, а неинициализированный
        // пул. Раньше такой сэмпл шёл в valid/non_zero, и смещение могло быть
        // принято на мусоре.
        if (position_near_world_origin(position)) continue;
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

// Direct (PlayerManager field) position offsets, most trusted first. 0x260 is
// the field the live client actually uses now: the log at 19:52 confirmed it
// against kcc (8 of 9 players) while the profile's 0x1F4 read invalid-world
// for every player. The canonical field is lastSavedPosition (0x1D0);
// lastTickPosition (0x1C8) is equivalent. The rest are legacy guesses kept as
// a last resort only.
static const uint64_t k_known_position_offsets[] = {0x260, 0x1D0, 0x1C8, 0x1E0, 0x2D0, 0x2DC, 0x1D4, 0x1DC, 0x1E8};

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
// Поле публичное и проверено по release dump 205619:
// PlayerManager.mouseLook находится по +0x70.
static constexpr uint64_t PLAYER_MOUSE_LOOK_OFFSET      = 0x70;
static constexpr uint64_t MOUSE_LOOK_SENSITIVITY_OFFSET = 0x34;

bool esp_read_look_sensitivity(float& out) {
    if (g_pid <= 0 || !g_il2cpp_base || !g_mem.bound()) return false;

    uint64_t player = resolve_local_player();
    if (!player) {
        // Своей PlayerManager ещё нет (загрузка, только что респавнулись):
        // настройка клиентская, у любой PlayerManager она одна и та же.
        uint64_t list = resolve_player_list();
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
// Остальные поля Oxide.MouseLook, которыми аим вертит камерой, сверены по
// release dump (класс Oxide.MouseLook, строка 205611).
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
// Поля MouseLook проверены по release-дампу билда 62a8534; обфусцированные
// имена при следующих обновлениях могут измениться.
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
// Mirror помечает игрока, которым управляет это устройство. Оба поля берутся
// только из выбранного профиля оффсетов.

// -1 — прочитать не удалось, 0 — чужой, 1 — локальный.
static int player_is_local(uint64_t player) {
    if (player < 0x10000) return -1;
    const uint64_t identity = rd_ptr(player + NETWORK_BEHAVIOUR_IDENTITY);
    if (identity < 0x10000) return -1;
    const uint8_t flag = rd<uint8_t>(identity + NETWORK_IDENTITY_IS_LOCAL_PLAYER);
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
// Мировая позиция трансформа по фиксированным release-смещениям +0x38/+0x40,
// подтверждённым кодом Unity. Результат дополнительно сверяется с камерой.
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
            return true;
        }
        return false;
    }
    if (g_player_manager_class && rd_ptr(player) != g_player_manager_class) {
        if (why) *why = LookFail::wrongPlayer;
        if (mouse_look_from_cache(player, out)) { return true; }
        return false;
    }
    uint64_t mouse_look = rd_ptr(player + PLAYER_MOUSE_LOOK_OFFSET);
    if (mouse_look < 0x10000) {  // ноль или мусор вместо указателя
        if (why) *why = LookFail::noObject;
        if (mouse_look_from_cache(player, out)) { return true; }
        return false;
    }
    const uint64_t klass = rd_ptr(mouse_look);
    const uint64_t known = g_mouse_look_klass.load();
    if (known ? (klass != known) : (klass < 0x10000)) {
        if (why) *why = LookFail::noObject;
        if (mouse_look_from_cache(player, out)) { return true; }
        return false;
    }
    // Свежий объект проверяем строже кеша: он только что найден по указателю,
    // который мог остаться от умершего игрока. Владелец известен и не совпал —
    // это чужой MouseLook, писать в него углы нельзя (повернём камеру не тому).
    const uint64_t owner = mouse_look_owner(mouse_look);
    if (owner && player && owner != player) {
        if (why) *why = LookFail::wrongOwner;
        look_mismatch(true);
        if (mouse_look_from_cache(player, out)) { return true; }
        return false;
    }
    // Признак Mirror: isLocalPlayer. В отличие от владельца он читается у
    // любого игрока, поэтому ловит как раз тот случай, от которого проверка
    // владельца молчала: чужой MouseLook с пустым m_EventHandler.
    const int local = player_is_local(player);
    if (local == 0) {
        if (why) *why = LookFail::wrongOwner;
        look_mismatch(true);
        if (mouse_look_from_cache(player, out)) { return true; }
        return false;
    }
    // И последняя сверка — геометрией: m_LookRoot обязан стоять у камеры.
    float d = -1.f;
    const int drives = mouse_look_camera_distance(mouse_look, d);
    if (drives == 0) {
        if (why) *why = LookFail::wrongOwner;
        look_mismatch(true);
        if (mouse_look_from_cache(player, out)) { return true; }
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
        return false;
    }
    const float value = rd<float>(mouse_look + MOUSE_LOOK_SENSITIVITY_OFFSET);
    if (!std::isfinite(value) || value < 0.05F || value > 100.0F) {
        look_mismatch(true);
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
        }
    }
    if (mouse_look != g_look_addr_logged.exchange(mouse_look))
        ;
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

// PlayerEventHandler локального игрока «наружу» (память-джойстик автофарма
// пишет в его канал MovementInput рядом с LookDirection). Владелец сверен по
// обратной ссылке handler.manager (+0xD0) == PlayerManager, как и везде, где
// мы пишем в этот объект.
uint64_t local_event_handler_checked() {
    uint64_t handler = 0;
    return resolve_event_handler(handler) ? handler : uint64_t{0};
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


bool esp_mem_aim_read_fire_dir(float& x, float& y, float& z) {
    uint64_t addr = 0;
    if (!resolve_look_direction(addr)) { return false; }
    const Vec3 dir = rd_v3(addr);
    if (!vec3_is_finite(dir)) return false;
    const float len = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    // Игра держит тут единичный вектор; всё остальное — не ось, а мусор.
    if (!(len > 0.5F && len < 2.0F)) return false;
    x = dir.x / len; y = dir.y / len; z = dir.z / len;
    {
        const float d[3] = {x, y, z};
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
    const bool ok = wr_buf(addr, dir, sizeof(dir));
    if (!ok) {
        // Отказ записи — редкость (доступ отобрали или адрес умер); пишем, но
        // не чаще раза в секунду, чтобы не забить лог.
        static double last = 0.0;
        const double now = memio::now_seconds();
        if (now - last > 1.0) {
            last = now;
        }
    }
    return ok;
}

// Сайлент использует выбранный транспорт без кадрового кэша; worker получает
// immutable snapshot и повторно проверяет цепочку владельцев перед записью.
// Shared snapshot, refreshed by the frame reader before target selection.
static float g_aim_lead_seconds = 0.0F;
static Vec3 g_aim_inherited_velocity{};
static aim_ballistics::Parameters g_aim_ballistic_params{};
static bool g_aim_is_instant_ray = false; // FPHitscan fires a Ray, not a travelling ShaftedProjectile
namespace { bool aim_ballistic_origin(Vec3& origin); }

bool esp_silent_publish(uint64_t target, const float aim[3],
                        const float velocity[3], float sample_age) {
    auto fail = [] { silent_aim::clear(); return false; };
    if (g_pid <= 0 || !g_mem.bound() || !g_cam_pose_valid || esp_freecam_active()) return fail();
    silent_aim::Command c;
    c.pid = g_pid;
    c.player = resolve_local_player();
    if (!c.player) return fail();
    c.player_handler_offset = PLAYER_EVENT_HANDLER;
    c.handler_player_offset = EVENT_HANDLER_MANAGER_BACKREF;
    c.look_offset = EVENT_HANDLER_LOOK_DIRECTION;
    c.value_offset = SYNC_VALUE_OFFSET;
    auto ptr = [](uint64_t address) {
        uint64_t value = 0;
        return rd_fresh(address, value) ? memio::untag(value) : uint64_t{0};
    };
    c.handler = ptr(c.player + c.player_handler_offset);
    if (!c.handler || ptr(c.handler + c.handler_player_offset) != c.player) return fail();
    c.look = ptr(c.handler + c.look_offset);
    if (!c.look) return fail();
    c.player_class = ptr(c.player);
    c.handler_class = ptr(c.handler);
    c.look_class = ptr(c.look);
    c.target = target;
    Vec3 origin{};
    if (!aim_ballistic_origin(origin) || !aim || !velocity ||
        !silent_aim::solve_axis(origin, Vec3{aim[0], aim[1], aim[2]},
                                Vec3{velocity[0], velocity[1], velocity[2]},
                                g_aim_inherited_velocity,
                                g_aim_is_instant_ray ? aim_ballistics::Parameters{}
                                                     : g_aim_ballistic_params,
                                sample_age, g_aim_lead_seconds, c)) return fail();
    // Shot axis is not the camera view axis. Both the synchronous first write
    // and the worker guard the Player -> Handler -> Look chain before writing.
    struct LiveMemory {
        bool read(uint64_t addr, void* out, size_t len) { return esp_silent_read(addr, out, len); }
        bool write(uint64_t addr, const void* in, size_t len) { return esp_silent_write(addr, in, len); }
    } live;
    if (silent_aim::tick(live, c) != silent_aim::Result::written) return fail();
    static uint64_t previous_player = 0;
    static float previous_shot = 0.f;
    static double hot_until = 0.0;
    if (previous_player != c.player) {
        previous_player = c.player;
        previous_shot = 0.f; hot_until = 0.0;
    }
    float shot = 0.f;
    const double now = memio::now_seconds();
    if (esp_mem_aim_last_shot(shot) && shot > 0.f && shot != previous_shot) {
        if (previous_shot > 0.f) hot_until = now + 0.280;
        previous_shot = shot;
    }
    c.recent_shot = now < hot_until;
    return silent_aim::publish(c);
}


bool esp_silent_wants_no_spread() {
    // Writer::active is deliberately target-scoped and falls to false as soon
    // as AimSelectTarget finds no player in the FOV.  Weapon tuning is a mode
    // setting, so bind it to the user's enabled silent mode instead.
    return silent_aim::wants_no_spread(
        g_state.aim_touch, g_state.aim_mode == AIM_MODE_SILENT, g_state.freecam_on);
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

enum class W2SProjectionState : unsigned char {
    NotRun,
    NonFiniteClip,
    BehindCamera,
    NonFiniteScreen,
    OutsideScreen,
    Projected,
    InvalidViewport,
    NonFiniteCamera,
    CameraBehind
};

struct W2SProjectionTrace {
    bool camera_space = false;
    float x = 0.0F, y = 0.0F, z = 0.0F, w = 0.0F;
    float screen_x = 0.0F, screen_y = 0.0F;
    W2SProjectionState state = W2SProjectionState::NotRun;
};

static const char* w2s_projection_state_name(W2SProjectionState state) {
    switch (state) {
        case W2SProjectionState::NonFiniteClip: return "nonfinite-clip";
        case W2SProjectionState::BehindCamera: return "behind-camera";
        case W2SProjectionState::NonFiniteScreen: return "nonfinite-screen";
        case W2SProjectionState::OutsideScreen: return "outside-screen";
        case W2SProjectionState::Projected: return "projected";
        case W2SProjectionState::InvalidViewport: return "invalid-viewport";
        case W2SProjectionState::NonFiniteCamera: return "nonfinite-camera";
        case W2SProjectionState::CameraBehind: return "camera-behind";
        default: return "not-run";
    }
}

static bool w2s_with_trace(const Mat4& vp, const Vec3& world, float sw, float sh,
                           Vec2& out, bool clip_to_screen, W2SProjectionTrace* trace) {
    if (trace) *trace = {};
    // clip = VP * float4(world, 1) with column-major VP
    const float clip_x = mat_get(vp, 0, 0) * world.x + mat_get(vp, 0, 1) * world.y + mat_get(vp, 0, 2) * world.z + mat_get(vp, 0, 3);
    const float clip_y = mat_get(vp, 1, 0) * world.x + mat_get(vp, 1, 1) * world.y + mat_get(vp, 1, 2) * world.z + mat_get(vp, 1, 3);
    const float clip_w = mat_get(vp, 3, 0) * world.x + mat_get(vp, 3, 1) * world.y + mat_get(vp, 3, 2) * world.z + mat_get(vp, 3, 3);
    if (trace) {
        trace->x = clip_x;
        trace->y = clip_y;
        trace->z = mat_get(vp, 2, 0) * world.x + mat_get(vp, 2, 1) * world.y + mat_get(vp, 2, 2) * world.z + mat_get(vp, 2, 3);
        trace->w = clip_w;
    }
    if (!std::isfinite(clip_x) || !std::isfinite(clip_y) || !std::isfinite(clip_w)) {
        if (trace) trace->state = W2SProjectionState::NonFiniteClip;
        return false;
    }
    if (clip_w <= 0.001F) {
        if (trace) trace->state = W2SProjectionState::BehindCamera;
        return false;
    }
    out.x = ((clip_x / clip_w) + 1.0F) * 0.5F * sw;
    out.y = ((-clip_y / clip_w) + 1.0F) * 0.5F * sh;
    if (trace) {
        trace->screen_x = out.x;
        trace->screen_y = out.y;
    }
    if (!std::isfinite(out.x) || !std::isfinite(out.y)) {
        if (trace) trace->state = W2SProjectionState::NonFiniteScreen;
        return false;
    }
    if (clip_to_screen && (out.x < 0.0F || out.x > sw || out.y < 0.0F || out.y > sh)) {
        if (trace) trace->state = W2SProjectionState::OutsideScreen;
        return false;
    }
    if (trace) trace->state = W2SProjectionState::Projected;
    return true;
}

bool w2s(const Mat4& vp, const Vec3& world, float sw, float sh, Vec2& out, bool clip_to_screen) {
    return w2s_with_trace(vp, world, sw, sh, out, clip_to_screen, nullptr);
}


// Массивы камеры, найденные проверенным путём (resolve_camera_arrays). Держим
// их: пара «данные/индекс» у трансформа камеры постоянна, пока жив мир, а
// искать её заново каждый кадр — это четыре чтения и разбор иерархии на каждом
// кадре оверлея.
static uint64_t s_cam_arr_m = 0, s_cam_arr_n = 0;
static int32_t  s_cam_arr_idx = -1;
static bool     s_cam_arr_valid = false;
static double   s_cam_arr_at = -1e9;

// Поза камеры, сверенная с матрицей вида. Адреса массивов могут устареть при
// перестройке иерархии, поэтому при расхождении они перечитываются по тем же
// фиксированным release-смещениям. Во фрикаме сверку отключаем: там камера
// намеренно не совпадает с положением, которое только что оставила игра.
//
// Сверяем И ПОВОРОТ, не только позицию. Позиция сходится и у чужого трансформа
// (риг камеры, соседний узел её GameObject, полузаписанный, но нормализуемый
// кватернион): у такого источника позиция честная, а поворот сдвинут на
// градусы — и все визуалы висят рядом с объектом, «дыша» на панораме, потому
// что угловая ошибка на экране растёт с расстоянием и меняется вместе с
// направлением взгляда. Кэш +0x70 — это ровно то, чем игра рисует кадр, поэтому
// оси позы обязаны лежать в его базисе; разошлись — веры позе нет.
//
// Про порог. 0.999 (~2.6°) вместо 0.995 (~5.7°): пять градусов на панораме —
// это заметный сдвиг визуалов у всего, что дальше 20-30 м, и он проходит
// проверку «на глаз» незамеченным только на близких целях. Расхождение больше
// 2.6° между позой и кадровой матрицей — это уже разные кадры, а не шум.
enum class PoseCheckResult {
    Ok,
    Unavailable,        // позу не прочитали (или она не нормализуется)
    PositionMismatch,   // позиция ушла больше чем на 2 м от эталона
    RotationMismatch,   // поворот разошёлся с эталоном больше чем на 0.999
};

static PoseCheckResult camera_pose_checked(Vec3& position, Vec4& rotation, const Vec3& ref,
                                          const Vec3& ref_forward, bool verify) {
    if (!s_cam_arr_valid) {
        Vec3 cp{};
        float err = -1.f;
        if (!resolve_camera_arrays(s_cam_arr_m, s_cam_arr_n, s_cam_arr_idx, cp, err)) return PoseCheckResult::Unavailable;
        s_cam_arr_valid = true;
        s_cam_arr_at = memio::now_seconds();
    }
    Vec3 p{};
    Vec4 r{};
    if (!read_transform_hierarchy_arrays(s_cam_arr_m, s_cam_arr_n, s_cam_arr_idx, p, &r)) {
        s_cam_arr_valid = false;
        return PoseCheckResult::Unavailable;
    }
    if (!vec3_is_finite(p)) { s_cam_arr_valid = false; return PoseCheckResult::Unavailable; }
    position = p;
    rotation = r;
    if (!verify) return PoseCheckResult::Ok;
    {
        const float dx = p.x - ref.x, dy = p.y - ref.y, dz = p.z - ref.z;
        const float d2 = dx * dx + dy * dy + dz * dz;
        // Два метра: камера успевает сдвинуться между двумя чтениями, а
        // подмена массивов даёт десятки метров.
        if (d2 > 2.0F * 2.0F) {
            s_cam_arr_valid = false;
            return PoseCheckResult::PositionMismatch;
        }
        // Кватернион нормируем ДО проверки — иначе знак/длина не влияют на
        // направление, а вот на dot влияют.
        Vec4 q = r;
        if (!normalize_quaternion(q)) { s_cam_arr_valid = false; return PoseCheckResult::Unavailable; }
        const Vec3 pose_forward = rotate_vector(q, {0.0F, 0.0F, 1.0F});
        if (!vec3_is_finite(pose_forward)) { s_cam_arr_valid = false; return PoseCheckResult::Unavailable; }
        const float dot = pose_forward.x * ref_forward.x + pose_forward.y * ref_forward.y +
                          pose_forward.z * ref_forward.z;
        if (!(dot > 0.999F)) {
            s_cam_arr_valid = false;
            return PoseCheckResult::RotationMismatch;
        }
    }
    return PoseCheckResult::Ok;
}

// Кадровый барьер матрицы вида. Все визуалы (боксы, маркеры, скелеты, линии)
// рисуются через единый vp = projection * view, поэтому ОДИН кадр с недописанной
// игрой матрицей разворачивает всю сцену в случайную сторону на миллисекунды —
// это и было «визуалы телепортируются в рандомное место экрана». Раньше скачок
// отсекался только в ветке живой позы камеры; кеш камеры и запасной путь шли в
// кадр без проверок вовсе.
//
// Физика такова, что лечится дёшево:
//   1. worldToCamera у Unity — чистый поворот + перенос, без масштаба: строки
//      поворота обязаны быть ортонормальными. Смесь двух кадров, прочитанная
//      на половине записи, таким свойством не обладает.
//  2. Камера не прыгает на 30+ м и не разворачивается на 60+ градусов за кадр.
//      Настоящий телепорт/респаун держится в памяти — повтор скачка на следующем
//      кадре подтвердит его и пропустит, а одиночный мусор отбрасывается, и
//      кадр дорисовывается прошлой принятой матрицей.
//
// Подтверждение — РОВНО один пропущенный кадр. Раньше скачок отклонялся до трёх
// кадров подряд, и быстрый доворот в прицеле (или настоящий респаун) отдавал
// экрану три кадра прежней камеры: мир под визуалами уже уехал, а они стоят —
// это то же «плавают при движении камеры», только с другой стороны. Мусорный
// кадр всё равно отсекается: полузаписанная матрица не проходит view_basis_sane
// выше, а одиночный скачок — этой проверкой.
struct FrameViewGuard {
    Mat4 view{};
    Vec3 pos{};
    Vec3 fwd{};
    bool ok = false;
    int  pos_streak = 0;
    int  rot_streak = 0;
};
static FrameViewGuard s_fv_guard;

// Ортонормальность поворота: camera_basis_from_view нормирует оси, дальше они
// обязаны быть попарно ортогональными. 0.45 (~63°) — запас против почти
// прямых углов игровой камеры; серьёзной половинчатой записи сюда не пройти.
static bool view_basis_sane(const Mat4& view) {
    if (!matrix_is_finite(view)) return false;
    Vec3 right{}, up{}, forward{};
    if (!camera_basis_from_view(view, right, up, forward)) return false;
    const float d_ru = right.x * up.x + right.y * up.y + right.z * up.z;
    const float d_rf = right.x * forward.x + right.y * forward.y + right.z * forward.z;
    const float d_uf = up.x * forward.x + up.y * forward.y + up.z * forward.z;
    return fabsf(d_ru) < 0.45F && fabsf(d_rf) < 0.45F && fabsf(d_uf) < 0.45F;
}

// Единый фильтр кадровых скачков для финального view кадра, из какого бы
// источника он ни пришёл. false — образец отклонён; вызывающий дорисовывает
// кадр через s_fv_guard.view.
static bool frame_view_accepted(const Mat4& view) {
    Vec3 pos{};
    Vec3 right{}, up{}, fwd{};
    if (!camera_position_from_view(view, pos)) return false;
    if (!camera_basis_from_view(view, right, up, fwd)) return false;
    if (!s_fv_guard.ok) {
        s_fv_guard.view = view; s_fv_guard.pos = pos; s_fv_guard.fwd = fwd;
        s_fv_guard.ok = true;
        return true;
    }
    const float dx = pos.x - s_fv_guard.pos.x, dy = pos.y - s_fv_guard.pos.y, dz = pos.z - s_fv_guard.pos.z;
    const bool pos_jump = dx * dx + dy * dy + dz * dz > 30.0F * 30.0F;
    const float dot = fwd.x * s_fv_guard.fwd.x + fwd.y * s_fv_guard.fwd.y + fwd.z * s_fv_guard.fwd.z;
    const bool rot_jump = dot < 0.5F;  // ~60° за кадр
    if (pos_jump && s_fv_guard.pos_streak < 1) { ++s_fv_guard.pos_streak; return false; }
    if (pos_jump) s_fv_guard.pos_streak = 0;
    if (rot_jump && s_fv_guard.rot_streak < 1) { ++s_fv_guard.rot_streak; return false; }
    if (rot_jump) s_fv_guard.rot_streak = 0;
    s_fv_guard.view = view; s_fv_guard.pos = pos; s_fv_guard.fwd = fwd;
    return true;
}

static bool read_native_camera_matrices(uint64_t native_cam, float screen_aspect, Mat4& projection,
                                        Mat4& view, float force_aspect = 0.0F) {
    if (!native_cam) return false;

    static Mat4 s_last_view{};
    static Mat4 s_last_proj{};
    static bool s_last_ok = false;

    // Матрица вида из кеша камеры (+0x70) — эталон: по ней игра и рисует кадр.
    // Живая поза берётся только если она с этим эталоном согласна.
    const Mat4 cached_view = rd_m4(native_cam + CAMERA_VIEW_MATRIX);
    Vec3 ref_pos{};
    Vec3 ref_right{}, ref_up{}, ref_forward{};
    // Оси эталона нужны сверке ПОВОРОТА позы (см. camera_pose_checked): по одной
    // позиции чужой трансформ проходит проверку, а его поворот сдвинут — и все
    // визуалы висят рядом с объектом, «дыша» на панораме.
    const bool have_ref = matrix_is_finite(cached_view) &&
                          camera_position_from_view(cached_view, ref_pos) &&
                          vec3_is_finite(ref_pos) &&
                          camera_basis_from_view(cached_view, ref_right, ref_up, ref_forward);

    // Откуда брать вид. Байты свежести кеша ставит САМА игра, когда +0x70
    // разошёлся с трансформом камеры, и снимает, пересчитав его; мы их же
    // используем во фрикаме, чтобы заставить игру пересчитать вид.
    //   кеш свежий   — +0x70 и есть кадровая матрица: берём её, а живой позой
    //                  только контролируем (см. 0.999 в camera_pose_checked).
    //                  Расхождение при свежем кеше — это либо полузапись кеша,
    //                  либо чужая колонка позы: кадру верим всё равно, но
    //                  массивы позы перечитываем (self-heal адресов);
    //   кеш несвежий — эталон отстал от трансформа, и сверять с ним позу
    //                  нельзя (расхождение и есть ошибка кадра): берём позу;
    //   байты не читаются — как раньше: поза, если сходится с кешем.
    // Во фрикаме +0x70 пишем мы сами, там «свежесть» наша собственная: не лезем.
    uint8_t view_dirty = 0, proj_dirty = 0;
    const bool dirty_known = rd_exact(native_cam + CAMERA_VIEW_DIRTY, view_dirty) &&
                             rd_exact(native_cam + CAMERA_PROJ_DIRTY, proj_dirty);
    const bool cache_stale = dirty_known && (view_dirty != 0 || proj_dirty != 0);
    const bool cache_fresh = dirty_known && !cache_stale && !g_freecam_on;
    bool have_live_view = false;
    uint64_t native_transform = camera_transform_of(native_cam);
    if (native_transform && have_ref) {
        Vec3 cam_pos{};
        Vec4 cam_rot{};
        const PoseCheckResult check =
            camera_pose_checked(cam_pos, cam_rot, ref_pos, ref_forward, !cache_stale && !g_freecam_on);
        bool fin_ok = vec3_is_finite(cam_pos);
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
            // Один кадр подтверждения, не четыре: настоящий телепорт камеры
            // (респаун, смерть, смена вида) подтверждается следующим кадром, а
            // четыре кадра прежнего вида — это уже видимая пауза визуалов
            // посреди движения.
            if (j2 > 30.0F * 30.0F && s_pose_jump_streak < 1) {
                ++s_pose_jump_streak;
                quat_ok = false; // fall through to the cached view below
            } else {
                s_pose_jump_streak = 0;
            }
        }
        // Позу берём по состоянию байтов свежести:
        //   кеш несвежий — эталон отстал от трансформа: поза первична (сверку с
        //                  эталоном выше отключили — расхождение и есть ошибка);
        //   кеш свежий  — +0x70 и есть кадровая матрица кадра: оставляем её, но
        //                  только пока поза с ней согласна. Расхождение при
        //                  «свежем» кеше — доказательство, что свежесть соврала
        //                  (полузапись кеша или чужая колонка позы): берём позу;
        //   байты не читаются — как раньше: поза, если сошлась, иначе кеш.
        bool pose_usable = false;
        if (cache_stale || g_freecam_on)
            pose_usable = quat_ok;
        else if (cache_fresh)
            pose_usable = quat_ok && check != PoseCheckResult::Ok;
        else
            pose_usable = quat_ok && check == PoseCheckResult::Ok;
        if (pose_usable) {
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
        g_cam_pose_derived = true;
    } else {
        g_cam_pose_derived = false;
        s_last_view = view;
    }
    // Кадровый барьер (см. FrameViewGuard): структурная проверка и скачки.
    // Отклонённый образец заменяется прошлой принятой матрицей — один кадр
    // прежней камеры незаметен, а мусорный был бы виден сразу на всех визуалах.
    if (!view_basis_sane(view) || !frame_view_accepted(view)) {
        if (!s_fv_guard.ok) return false;
        view = s_fv_guard.view;
        g_cam_pose_derived = true;
    }
    // Поза и базис — из финальной матрицы вида, единственной, по которой
    // рисуется кадр: иначе в g_cam_pos оставалась бы поза прошлого мира (её
    // писал живой путь), и по ней строились и боксы, и углы цели.
    {
        Vec3 r{}, u{}, f{}, p{};
        if (camera_basis_from_view(view, r, u, f) && camera_position_from_view(view, p)) {
            g_cam_pos = p; g_cam_right = r; g_cam_up = u; g_cam_forward = f;
            g_cam_pose_valid = true;
        }
    }

    // Projection params (FOV/aspect/clip) are stored as plain floats and stay hot.
    float fov = rd<float>(native_cam + CAMERA_FOV_DEGREES);
    float aspect = rd<float>(native_cam + CAMERA_ASPECT);
    float z_near = rd<float>(native_cam + CAMERA_NEAR_CLIP);
    float z_far = rd<float>(native_cam + CAMERA_FAR_CLIP);
    // Запрошенный растяг перебивает и поле камеры, и его клампинг: проекция
    // этого кадра обязана быть посчитана ровно тем значением, которое кадр
    // кладёт в камеру (иначе ESP и картинка игры расходятся на кадр на каждом
    // изменении слайдера, и вся сцена прыгает по X).
    const bool aspect_forced = force_aspect > 0.1F && force_aspect < 10.0F;
    if (aspect_forced) aspect = force_aspect;
    if (!(aspect > 0.1F && aspect < 10.0F))
        aspect = (screen_aspect > 0.1F && screen_aspect < 10.0F) ? screen_aspect : (9.0F / 16.0F);
    if (!(z_near > 0.001F && z_near < 100.0F)) z_near = 0.1F;
    if (!(z_far > z_near && z_far < 100000.0F)) z_far = 1000.0F;
    // Aspect, FOV и дистанции клипа лежат в тех же float-полях и попадают в тот
    // же полузаписанный кадр разом с видом. Раньше любой скачок больше порога
    // отбрасывался и кадр рисовался прошлым значением ДО ТРЁХ кадров подряд:
    // на настоящем зуме (FOV уходит больше чем на 25° за кадр) это три кадра
    // чужого масштаба, и все визуалы уезжают от объектов, пока прицел едет.
    // Теперь у значений два класса: правдоподобный шаг ОГРАНИЧИВАЕТСЯ (зум
    // доходит за пару кадров, вместо паузы и щелчка), а физически невозможный
    // отбрасывается на ОДИН кадр — дальше принимается, потому что два кадра
    // подряд одинакового «невозможного» значения — это уже не помеха чтению.
    static float s_fv_aspect = 0.0F;
    static int   s_fv_aspect_streak = 0;
    if (aspect_forced) {
        // Запрошенный растяг — это наше собственное значение, а не отсчёт игры:
        // подтверждать его серией нечего.
        s_fv_aspect_streak = 0;
        s_fv_aspect = aspect;
    } else if (s_fv_aspect > 0.001F) {
        // Шаг ограничиваем МНОЖИТЕЛЕМ (0.65..1.35 за кадр), а не абсолютной
        // дельтой: 0.3 «в лоб» — это +30% на широком экране и +60% на узком, и
        // вся сцена прыгала по X сильнее, чем при живом повороте устройства.
        // Множитель одинаково ограничивает и 9:16, и 21:9.
        const float ratio = aspect / s_fv_aspect;
        if ((ratio > 2.5F || ratio < 0.4F) && s_fv_aspect_streak < 1) {
            // Полукадр смены ориентации/чужое значение: один кадр прошлого
            // аспекта, дальше принимаем — два одинаковых «невозможных» чтения
            // подряд это уже настоящее значение.
            ++s_fv_aspect_streak;
            aspect = s_fv_aspect;
        } else {
            s_fv_aspect_streak = 0;
            const float lower = s_fv_aspect * 0.65F;
            const float upper = s_fv_aspect * 1.35F;
            if (aspect < lower) aspect = lower;
            else if (aspect > upper) aspect = upper;
            s_fv_aspect = aspect;
        }
    } else {
        s_fv_aspect = aspect;
    }
    static float s_fv_zn = 0.0F, s_fv_zf = 0.0F;
    static int   s_fv_clip_streak = 0;
    if (s_fv_zn > 0.0F && s_fv_zf > 0.0F) {
        const bool clip_jump = z_near > s_fv_zn * 4.0F || z_near < s_fv_zn * 0.25F ||
                               z_far  > s_fv_zf * 4.0F || z_far  < s_fv_zf * 0.25F;
        if (clip_jump && s_fv_clip_streak < 1) {
            ++s_fv_clip_streak;
            z_near = s_fv_zn; z_far = s_fv_zf;
        } else {
            s_fv_clip_streak = 0;
            s_fv_zn = z_near; s_fv_zf = z_far;
        }
    } else {
        s_fv_zn = z_near; s_fv_zf = z_far;
    }
    // FOV: тот же множительный шаг 0.65..1.35 за кадр. Градусный шаг (+25°)
    // ведёт себя противоположно на разных краях: на 20° прицела это мгновенный
    // скачок масштаба вдвое, а на 90° обзора — незаметное движение. 0.65..1.35
    // покрывает любой настоящий зум (он идёт 3-4 кадра) и не пускает в кадр
    // чужой масштаб. Невозможный множитель (полузаписанный float) отдаёт один
    // кадр прошлого значения; кадр с нечитаемым FOV рисуем прошлым.
    static float s_fv_fov = 0.0F;
    static int   s_fv_fov_streak = 0;
    if (std::isfinite(fov) && fov > 1.0F && fov < 179.0F) {
        if (s_fv_fov > 0.0F) {
            const float ratio = fov / s_fv_fov;
            if ((ratio > 2.5F || ratio < 0.4F) && s_fv_fov_streak < 1) {
                ++s_fv_fov_streak;
                fov = s_fv_fov;
            } else {
                s_fv_fov_streak = 0;
                const float lower = s_fv_fov * 0.65F;
                const float upper = s_fv_fov * 1.35F;
                if (fov < lower) fov = lower;
                else if (fov > upper) fov = upper;
                s_fv_fov = fov;
            }
        } else {
            s_fv_fov = fov;
        }
    } else if (s_fv_fov > 0.0F) {
        fov = s_fv_fov;
    }
    if (std::isfinite(fov) && fov > 1.0F && fov < 179.0F) g_cam_fov_deg = fov;
    projection = mat_perspective(fov, aspect, z_near, z_far);
    if (!matrix_is_finite(projection)) {
        if (s_last_ok && matrix_is_finite(s_last_proj)) {
            projection = s_last_proj;
        } else {
            projection = rd_m4(native_cam + CAMERA_PROJECTION_MATRIX);
            if (!matrix_is_finite(projection)) return false;
            // Перспектура Unity: последняя строка (0,0,-1,0). Полузаписанная
            // матрица нарушает форму и бросает весь кадр — такую не берём.
            if (fabsf(mat_get(projection, 3, 2) + 1.0F) > 0.05F ||
                fabsf(mat_get(projection, 3, 0)) > 0.001F ||
                fabsf(mat_get(projection, 3, 1)) > 0.001F ||
                fabsf(mat_get(projection, 3, 3)) > 0.001F ||
                !(mat_get(projection, 0, 0) > 0.0F) ||
                !(mat_get(projection, 1, 1) > 0.0F)) return false;
        }
    }
    if (matrix_is_finite(projection)) s_last_proj = projection;
    if (have_live_view && matrix_is_finite(view)) {
        s_last_view = view;
        s_last_ok = true;
    }
    return true;
}

static bool w2s_transform_camera(const Vec3& camera_position, const Vec4& camera_rotation,
                                 const Vec3& world, float screen_width, float screen_height,
                                 Vec2& output, bool clip_to_screen = true,
                                 W2SProjectionTrace* trace = nullptr) {
    if (trace) {
        *trace = {};
        trace->camera_space = true;
    }
    if (screen_width < 100.0F || screen_height < 100.0F) {
        if (trace) trace->state = W2SProjectionState::InvalidViewport;
        return false;
    }
    Vec3 relative = {world.x - camera_position.x, world.y - camera_position.y, world.z - camera_position.z};
    Vec4 inverse_rotation = {-camera_rotation.x, -camera_rotation.y, -camera_rotation.z, camera_rotation.w};
    Vec3 camera_space = rotate_vector(inverse_rotation, relative);
    if (trace) {
        trace->x = camera_space.x;
        trace->y = camera_space.y;
        trace->z = camera_space.z;
    }
    if (!vec3_is_finite(camera_space)) {
        if (trace) trace->state = W2SProjectionState::NonFiniteCamera;
        return false;
    }
    if (camera_space.z <= 0.05F) {
        if (trace) trace->state = W2SProjectionState::CameraBehind;
        return false;
    }
    constexpr float vertical_fov_radians = 1.0471975512F;
    float tangent = tanf(vertical_fov_radians * 0.5F);
    float aspect = screen_width / screen_height;
    float normalized_x = camera_space.x / (camera_space.z * tangent * aspect);
    float normalized_y = camera_space.y / (camera_space.z * tangent);
    if (trace) {
        trace->screen_x = (normalized_x + 1.0F) * 0.5F * screen_width;
        trace->screen_y = (1.0F - normalized_y) * 0.5F * screen_height;
    }
    if (!std::isfinite(normalized_x) || !std::isfinite(normalized_y)) {
        if (trace) trace->state = W2SProjectionState::NonFiniteScreen;
        return false;
    }
    if (clip_to_screen && (fabsf(normalized_x) > 1.0F || fabsf(normalized_y) > 1.0F)) {
        if (trace) trace->state = W2SProjectionState::OutsideScreen;
        return false;
    }
    output.x = (normalized_x + 1.0F) * 0.5F * screen_width;
    output.y = (1.0F - normalized_y) * 0.5F * screen_height;
    if (trace) {
        trace->screen_x = output.x;
        trace->screen_y = output.y;
    }
    const bool projected = std::isfinite(output.x) && std::isfinite(output.y);
    if (trace) trace->state = projected ? W2SProjectionState::Projected
                                        : W2SProjectionState::NonFiniteScreen;
    return projected;
}

// Кадры подряд, в которых выборка позиций игроков сошлась в одну точку (см.
// optimize_matrix_configuration). Один transient sample must not invalidate
// the fixed release position path.
static int g_offset_extent_fail_streak = 0;

static bool optimize_matrix_configuration(uint64_t native_camera, const std::vector<uint64_t>& transforms) {
    std::vector<Vec3> samples;
    for (uint64_t source : transforms) {
        Vec3 position{};
        if (!read_entity_position(source, position)) continue;
        // Нули после респауна и мусор из пула — не мировые позиции. Они не
        // должны инвалидировать уже подтверждённый release position path.
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
    // fixed release path revalidation starts only after a short failure streak.
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

static std::vector<uint64_t> read_configured_player_transforms(bool* player_list_ok) {
    if (player_list_ok) *player_list_ok = false;
    std::vector<uint64_t> transforms;
    uint64_t lists[2]{};
    int list_count = resolve_player_lists(lists);
    if (list_count == 0) return transforms;

    uint64_t local_player = resolve_local_player();
    if (local_player) {
        bool contained = false;
        bool any_nonempty = false;
        for (int i = 0; i < list_count; ++i) {
            contained |= player_list_contains(lists[i], local_player);
            const int32_t count = rd<int32_t>(lists[i] + IL2CPP_LIST_SIZE);
            any_nonempty |= count > 0 && count <= 512;
        }
        if (!contained && any_nonempty) {
            if (local_player == g_local_player) g_local_player = 0;
            local_player = 0;
        }
    }
    const uint64_t local_source = local_player;

    for (int attempt = 0; attempt < 3; ++attempt) {
        if (attempt > 0) {
            const int refreshed_count = resolve_player_lists(lists);
            if (refreshed_count > 0) list_count = refreshed_count;
        }

        std::vector<uint64_t> snapshot;
        snapshot.reserve(32);
        if (local_source) snapshot.push_back(local_source);
        bool stable = true;
        bool healthy = false;

        for (int list_index = 0; list_index < list_count; ++list_index) {
            const uint64_t list = lists[list_index];
            const uint64_t items = rd_ptr(list + IL2CPP_LIST_ITEMS);
            const int32_t count = rd<int32_t>(list + IL2CPP_LIST_SIZE);
            if (count < 0 || count > 512 || (count > 0 && !items)) {
                stable = false;
                break;
            }
            healthy = true;

            for (int32_t index = 0; index < count; ++index) {
                const uint64_t player = rd_ptr(
                    items + IL2CPP_ARRAY_FIRST_ELEMENT + (uint64_t)index * sizeof(uint64_t));
                if (!player || player == local_source) continue;
                // The containing registry is already validated and its
                // pointer/count are rechecked below. A failed class read must
                // not make one live player disappear from an otherwise stable
                // snapshot.
                bool duplicate = false;
                for (uint64_t known : snapshot) duplicate |= known == player;
                if (!duplicate) snapshot.push_back(player);
            }

            const uint64_t confirmed_items = rd_ptr(list + IL2CPP_LIST_ITEMS);
            const int32_t confirmed_count = rd<int32_t>(list + IL2CPP_LIST_SIZE);
            if (items != confirmed_items || count != confirmed_count) {
                stable = false;
                break;
            }
        }

        if (healthy && stable) {
            if (player_list_ok) *player_list_ok = true;
            return snapshot;
        }
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
    double   bone_world_at[ESP_BONE_COUNT] = {}; // monotonic last successful read
    uint64_t model_root = 0;
    int      bone_count = 0;
    bool     valid = false;
    int      retry_cooldown = 0;
    int      fail_streak = 0;
    int      build_fail = 0;         // пересборок подряд, закончившихся провалом
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

// Сиды для широкого перебора раскладки (см. resolve_skeleton_layout): сэмпл и
// его дети, набранные один раз, чтобы перебор не начинался заново каждый кадр.
static std::vector<uint64_t> g_skeleton_layout_seeds;

// Слот characterModel в PlayerManager: вшитое значение переезжает между
// сборками игры. 0 — «ещё не искали»; найденное смещение запоминается на всю
// сессию (раскладка полей одна для всех игроков и не меняется до обновления).
static uint64_t g_character_model_offset = 0;
static bool     g_character_model_probed = false;
// Защита от рекурсии: поиск модели читает имена, а поиск смещения имени сам
// зовёт skeleton_model_root().
static bool     g_character_model_probing = false;
static int      g_character_model_probe_fail = 0;   // промахи перебора: отступ

void esp_set_skeleton_enabled(bool enabled) { g_skeleton_enabled = enabled; }
void esp_set_aim_bones_enabled(bool enabled) { g_aim_bones_requested = enabled; }

// characterModel (managed GameObject) -> native GameObject -> its Transform.
static uint64_t model_root_at_offset(uint64_t player, uint64_t offset) {
    if (!player) return 0;
    uint64_t managed_go = rd_ptr(player + offset);
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

// Определена ниже: имя кости нужно до того, как найдено само имя GameObject.
static int  match_bone_name(const char* raw_name);

// Перебор полей PlayerManager в поисках модели: у модели под трансформом лежит
// риг, то есть поддерево, в котором читаются знакомые имена костей. У любого
// другого GameObject игрока (оружие, метка, коллайдеры) таких имён нет.
// Дорого (до ~90 полей на поддерево), поэтому ровно один раз за сессию.
static uint64_t discover_character_model_offset(uint64_t player) {
    if (!g_go_name_offset_valid) return 0;   // без имени кость не опознать
    static std::vector<uint64_t> nodes;
    for (uint64_t offset = 0x40; offset <= 0x300; offset += 8) {
        uint64_t transform = model_root_at_offset(player, offset);
        if (!transform) continue;
        collect_transform_subtree(transform, nodes, 128);
        if (nodes.size() < 8) continue;
        int hits = 0;
        for (uint64_t node : nodes) {
            char name[48];
            if (!read_transform_name(node, name, sizeof(name))) continue;
            if (match_bone_name(name) >= 0 && ++hits >= 3) break;
        }
        if (hits >= 3) return offset;
    }
    return 0;
}

// Когда вшитый слот перестал работать, перебор повторяется не чаще раза в
// kModelProbePeriod: игроки появляются и исчезают, и у свежего объекта поля
// могут быть ещё не заполнены.
static double   g_character_model_retry_at = 0.0;
static constexpr double kModelProbePeriod = 5.0;

static uint64_t character_model_offset(uint64_t player) {
    if (!player) return go_active::PLAYER_CHARACTER_MODEL;
    // 1) вшитый слот: на своей сборке игры он работает у всех игроков.
    if (model_root_at_offset(player, go_active::PLAYER_CHARACTER_MODEL))
        return go_active::PLAYER_CHARACTER_MODEL;
    // 2) найденный перебором для другой сборки.
    if (g_character_model_offset && g_character_model_offset != go_active::PLAYER_CHARACTER_MODEL &&
        model_root_at_offset(player, g_character_model_offset))
        return g_character_model_offset;
    // 3) перебор: только когда имена уже читаются (по именам костей и ищем).
    if (g_character_model_probing || !g_go_name_offset_valid) return go_active::PLAYER_CHARACTER_MODEL;
    const double now = mono_seconds();
    if (now < g_character_model_retry_at) return go_active::PLAYER_CHARACTER_MODEL;
    g_character_model_probing = true;
    g_character_model_probed = true;
    g_character_model_offset = discover_character_model_offset(player);
    g_character_model_probing = false;
    // Неудача — это не «смещение не там», а «у этого игрока модели нет» (труп,
    // спектейтор, недосозданный PlayerManager). Перебор читает до 90 полей с
    // обходом поддерева на каждом, поэтому после промаха отступаем всё дальше:
    // 5 с -> 20 -> 60 -> 300. Без отступа такие игроки сжирали кадр целиком
    // (лог 03.10: 500–600 мс на кадре, 783 чтений).
    const double period = g_character_model_probe_fail == 0 ? kModelProbePeriod
                        : g_character_model_probe_fail == 1 ? 20.0
                        : g_character_model_probe_fail == 2 ? 60.0 : 300.0;
    if (g_character_model_offset) {
        g_character_model_probe_fail = 0;
    } else if (g_character_model_probe_fail < 3) {
        ++g_character_model_probe_fail;
    }
    g_character_model_retry_at = now + period;
    return g_character_model_offset ? g_character_model_offset
                                    : go_active::PLAYER_CHARACTER_MODEL;
}

static uint64_t skeleton_model_root(uint64_t player) {
    return model_root_at_offset(player, character_model_offset(player));
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
    // Вшитая догадка — первой (на проверенных сборках имя лежит по ней),
    // дальше весь диапазон полей GameObject. Прежний список из девяти
    // кандидатов не накрывал сборки, где имя уехало на 0x68/0x70/0x88: имя не
    // читалось вообще, а без него не находится ни одна кость (скелеты молчали
    // на части устройств при живых боксах).
    uint64_t candidates[40];
    int candidate_count = 0;
    candidates[candidate_count++] = GAMEOBJECT_NAME_GUESS;
    for (uint64_t offset = 0x10; offset <= 0xB0 && candidate_count < 40; offset += 8) {
        bool duplicate = false;
        for (int i = 0; i < candidate_count; ++i) duplicate |= candidates[i] == offset;
        if (!duplicate) candidates[candidate_count++] = offset;
    }
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
        for (int ci = 0; ci < candidate_count; ++ci) {
            const uint64_t offset = candidates[ci];
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

// Раскладка прочитала трансформ И дала правдоподобную мировую позицию.
// Одного чтения мало: комбинация может читать чужую/протухшую иерархию —
// конечные числа не из того места. Сэмпл здесь — кость игрока, она обязана
// лежать в мире.
static bool skeleton_layout_reads_plausible(const TransformHierarchyLayout& layout,
                                             uint64_t sample_transform) {
    Vec3 probe{};
    return read_transform_hierarchy_layout(sample_transform, layout, probe) &&
           vec3_is_finite(probe) && position_looks_like_world_space(probe);
}

// Отказов подряд действующей раскладке, прежде чем перебирать заново.
// Раскладка одна на сборку игры (смещения структур Unity одинаковы у всех
// сущностей), поэтому одиночный отказ — это рваное чтение (спавн как раз
// перестраивает иерархию), а не смена раскладки. Прежний код сносил общую
// раскладку по первому же такому сэмплу и перенимал первую читающуюся
// комбинацию — после каждого нового игрока рядом все чужие скелеты прыгали
// и гасли на несколько кадров, пока следующий билд не возвращал раскладку
// обратно (в логе: новый адрес + 3-4 кадра без костей без тормоза).
static int      g_skeleton_layout_miss = 0;
static constexpr int kSkeletonLayoutMissLimit = 4;
// Последний трансформ, успешно прочитанный действующей раскладкой: по нему
// отличаем «сэмплы рваные» (якорь читается — раскладку держим) от «раскладка
// правда умерла» (якорь тоже не читается — переучиваем).
static uint64_t g_skeleton_layout_anchor = 0;

static bool resolve_skeleton_layout(uint64_t sample_transform) {
    if (g_skeleton_layout_valid) {
        if (skeleton_layout_reads_plausible(g_skeleton_layout, sample_transform)) {
            g_skeleton_layout_anchor = sample_transform;
            g_skeleton_layout_miss = 0;
            return true;
        }
        // Действующая не прочитала сэмпл — но сносить её по одному сэмплу
        // нельзя: этот билд скелета не состоится (пересборка через 20 кадров,
        // когда иерархия устоится), а остальные игроки продолжают читать по
        // действующей.
        if (++g_skeleton_layout_miss < kSkeletonLayoutMissLimit) return false;
        g_skeleton_layout_miss = 0;
        if (g_skeleton_layout_anchor &&
            skeleton_layout_reads_plausible(g_skeleton_layout, g_skeleton_layout_anchor))
            return false;   // раскладка жива, сэмплы рваные — держим её
        g_skeleton_layout_valid = false;
        g_skeleton_layout_anchor = 0;
        static double s_layout_relearn_at = -1e9;
        const double now = memio::now_seconds();
        if (now - s_layout_relearn_at >= 2.0) {
            s_layout_relearn_at = now;
        }
    }
    if (g_transform_hierarchy_layout_valid &&
        skeleton_layout_reads_plausible(g_transform_hierarchy_layout, sample_transform)) {
        g_skeleton_layout = g_transform_hierarchy_layout;
        g_skeleton_layout_valid = true;
        g_skeleton_layout_anchor = sample_transform;
        return true;
    }
    // 0x38/0x40 — основная пара, 0x18/0x20 — старая, 0x28/0x30 — вариант, на
    // котором TransformHierarchy лежит в запасном слоте Transform (её подобрал
    // широкий перебор на сборке из лога 03.10: +0x28 данные, +0x30 индекс).
    const uint64_t base_offsets[][2] = {{0x38, 0x40}, {0x18, 0x20}, {0x28, 0x30}};
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
                    if (skeleton_layout_reads_plausible(layout, sample_transform)) {
                        g_skeleton_layout = layout;
                        g_skeleton_layout_valid = true;
                        g_skeleton_layout_anchor = sample_transform;
                        return true;
                    }
                }
            }
        }
    }
    // Вшитые комбинации не сошлись: на части сборок данные иерархии лежат не
    // по 0x38/0x40 и не по 0x18/0x20 (на одном из устройств имя GameObject
    // тоже оказалось не на вшитом 0x48, а сырым указателем на 0x50 — того же
    // порядка различие). Запускаем широкий перебор — тот самый, что учит
    // раскладку по игрокам; он закрывает data 0x10..0x200 и все пары массивов
    // 0x00..0x100 с обоими вариантами разыменования.
    // Перебор дорогой, а вызываемся мы раз в кадр на каждого игрока: гоним его
    // не чаще раза в 2 с. Заодно обновляем протухшие сиды — первый сэмпл мог
    // быть рваным (игрок как раз вышел), и на нём перебор бы не сошёлся.
    static double s_sweep_at = -1e9;
    static double s_seeds_at = -1e9;
    const double sweep_now = memio::now_seconds();
    if (g_skeleton_layout_seeds.empty() || sweep_now - s_seeds_at >= 30.0) {
        // Сид — сам сэмпл и его дети: перебору нужны минимум две точки на
        // расстоянии, иначе оценка «несколько позиций с разбросом» не пройдёт.
        g_skeleton_layout_seeds.clear();
        s_seeds_at = sweep_now;
        g_skeleton_layout_seeds.push_back(sample_transform);
        uint64_t children[64];
        const int count = read_transform_children(sample_transform, children, 64);
        for (int i = 0; i < count && g_skeleton_layout_seeds.size() < 6; ++i)
            if (children[i]) g_skeleton_layout_seeds.push_back(children[i]);
    }
    if (g_skeleton_layout_seeds.size() >= 2 && sweep_now >= s_sweep_at) {
        s_sweep_at = sweep_now + 2.0;
        size_t position_count = 0, candidate_count = 0;
        const bool learned = discover_layout_from_native_transforms(g_skeleton_layout_seeds,
                                                                   position_count, candidate_count);
        if (learned && g_transform_hierarchy_layout_valid) {
            g_skeleton_layout = g_transform_hierarchy_layout;
            g_skeleton_layout_valid = true;
            g_skeleton_layout_anchor = sample_transform;
            g_skeleton_layout_seeds.clear();
            return true;
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

// Мягкая проверка «это нативный Transform» в отличие от
// skeleton_transform_ptr_valid(): Transform не обязан быть компонентом[0]
// своего GameObject. Признак — GameObject действительно держит этот компонент
// в первой четвёрке, а обратная ссылка компонента смотрит на него же.
static bool transform_belongs_to_gameobject(uint64_t transform) {
    if (!transform) return false;
    const uint64_t go = rd_ptr(transform + COMPONENT_GAMEOBJECT);
    if (!go) return false;
    const uint64_t pairs = rd_ptr(go + GAMEOBJECT_COMPONENT_ARRAY);
    if (!pairs) return false;
    for (int i = 0; i < 4; ++i) {
        if (rd_ptr(pairs + (uint64_t)i * GAMEOBJECT_COMPONENT_STRIDE + COMPONENT_PAIR_PTR) != transform)
            continue;
        return true;
    }
    return false;
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
    //
    // Вшитое смещение поля transform в BodyPart переезжает между сборками: на
    // одном из устройств все 11 элементов m_Bones дали ноль валидных
    // трансформов при живом массиве. Тогда смещение ищется перебором полей
    // первого элемента (у настоящего Transform компонент[0] его GameObject —
    // он сам) и запоминается на сессию: раскладка класса одна для всех.
    static uint64_t g_bodypart_transform_offset = RAGDOLL_BODYPART_TRANSFORM;
    constexpr int kMaxSet = 24;
    uint64_t set_transform[kMaxSet];
    int set_count = 0;
    auto collect = [&](uint64_t transform_offset) {
        set_count = 0;
        for (int32_t i = 0; i < element_count && set_count < kMaxSet; ++i) {
            uint64_t element = rd_ptr(bones_array + IL2CPP_ARRAY_FIRST_ELEMENT + (uint64_t)i * 8);
            if (!element) continue;
            uint64_t transform = managed_object_native(rd_ptr(element + transform_offset));
            if (!skeleton_transform_ptr_valid(transform))
                transform = native_component_transform(managed_object_native(element));
            if (!skeleton_transform_ptr_valid(transform)) continue;
            bool duplicate = false;
            for (int j = 0; j < set_count; ++j)
                if (set_transform[j] == transform) { duplicate = true; break; }
            if (!duplicate) set_transform[set_count++] = transform;
        }
    };
    collect(g_bodypart_transform_offset);
    if (set_count == 0 && g_bodypart_transform_offset == RAGDOLL_BODYPART_TRANSFORM) {
        // Перебором ищем поле transform в первом живом элементе.
        uint64_t element = 0;
        for (int32_t i = 0; i < element_count && !element; ++i)
            element = rd_ptr(bones_array + IL2CPP_ARRAY_FIRST_ELEMENT + (uint64_t)i * 8);
        for (uint64_t offset = 0x08; offset <= 0x40 && element; offset += 8) {
            if (offset == RAGDOLL_BODYPART_TRANSFORM) continue;
            uint64_t candidate = managed_object_native(rd_ptr(element + offset));
            if (!transform_belongs_to_gameobject(candidate)) continue;
            collect(offset);
            if (set_count > 0) {
                g_bodypart_transform_offset = offset;
                break;
            }
        }
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

    // Смещение имени — первым: без него не опознать кости, а значит не найти
    // и саму модель, если вшитый слот characterModel уже не тот.
    if (!g_go_name_offset_valid) ensure_gameobject_name_offset(player);

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
    // Последнее кадровое решение «мёртв» для ЭТОГО объекта и когда оно принято.
    // Труп остаётся в реестре ещё несколько секунд после респауна, и без этого
    // флага объект-трупа забирал uid себе: свежий объект того же игрока
    // подавлялся как duplicate-uid, и живой игрок оставался без бокса. В логе
    // 11:52:50 так пропал игрок на 0.5 с, а в 11:53:32 — игрок на 7.6 м.
    bool corpse = false;
    double corpse_at = 0.0;
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
    // Последняя ПРИНЯТАЯ позиция и состояние временного фильтра (см.
    // filter_player_position): бокс рисуется по ним, а не по сырому чтению.
    posfilter::State filt{};
};

double mono_seconds() {
    using namespace std::chrono;
    return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
}
static std::unordered_map<uint64_t, PlayerTrack> g_player_track;
// Сколько игроков в кадре получили позицию из запасного источника (KCC/
// characterModel), потому что синхронизируемое поле ещё не записано игрой —
// наблюдение за жалобой «нет боксов на свежеспавнившихся».
static int g_position_fallback_this_frame = 0;
static int g_position_fallback_rejected = 0;  // отклонённые как далёкие (чужая база)
static int g_position_origin_rejected = 0;   // отсчёты вида (≈0, 0, ≈0) из недоинициализированного пула
static int g_position_origin_rejected_logged = 0;
static double g_position_fallback_log_at = 0.0;
static int g_position_fallback_logged = 0;
static int g_position_fallback_rejected_logged = 0;
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
    return vec3_is_finite(out) && position_looks_like_world_space(out) &&
           !position_near_world_origin(out);
}

// World position of the player's own transform (worldCameraRoot). While he is
// mounted this is the only source that moves with the vehicle.
static bool player_rendered_position(uint64_t player, Vec3& out) {
    uint64_t native = resolve_player_native_transform(player);
    if (!native) return false;
    if (g_skeleton_layout_valid && read_transform_hierarchy_layout(native, g_skeleton_layout, out)
        && vec3_is_finite(out) && position_looks_like_world_space(out)
        && !position_near_world_origin(out))
        return true;
    return read_transform_hierarchy_position(native, out) && vec3_is_finite(out)
        && position_looks_like_world_space(out) && !position_near_world_origin(out);
}

// Seated body: characterModel is parented to the seat even when worldCameraRoot
// is left behind at the boarding point.
static bool player_model_position(uint64_t player, Vec3& out) {
    uint64_t root = skeleton_model_root(player);
    if (!root) return false;
    if (g_skeleton_layout_valid && read_transform_hierarchy_layout(root, g_skeleton_layout, out)
        && vec3_is_finite(out) && position_looks_like_world_space(out)
        && !position_near_world_origin(out))
        return true;
    return read_transform_hierarchy_position(root, out) && vec3_is_finite(out)
        && position_looks_like_world_space(out) && !position_near_world_origin(out);
}

// worldCameraRoot sits at eye level, the box is built from the feet.
static constexpr float kCameraRootHeight = 1.60F;

// Mounted-state latch. lastSavedPosition freezes at the boarding point while
// the rendered transform (and/or the character model) rides with the vehicle.
// vehicleID / the transform also flicker mid-tick. Engage on seat/vehicle OR
// on lastSaved freeze + a live visual that has left the mount; never snap the
// box back to that mount mid-ride.
// Состояние защёлки и само решение об удержании — в mount_latch.h, там же
// host-тест на сценарий из лога 08.10 23:09:07.
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
    const char* live_source = "нет";
    if (have_cam) {
        Vec3 p = cam;
        if (ground) p.y -= kCameraRootHeight;
        if (position_looks_like_world_space(p) || position_looks_like_world_space(cam)) {
            live = p;
            have_live = true;
            live_h2 = vec3_horiz2(p, saved);
            live_source = "трансформ";
        }
    }
    if (have_model && !id_mounted && !latch.engaged) {
        // While riding, characterModel can remain parented to the seat's old
        // PlayerManager copy for a tick. Selecting whichever sample is farther
        // from lastSaved made the box alternate between the boarding point and
        // the vehicle every frame. The rendered/worldCameraRoot transform is
        // the authoritative moving sample; use the model only on foot or when
        // the rendered transform is unavailable.
        Vec3 p = model;
        if (!ground) p.y += 0.90F; // hips -> roughly eye, matches hierarchy boxes
        float h2 = vec3_horiz2(p, saved);
        if (!have_live || h2 > live_h2 + 0.25F) {
            live = p;
            have_live = true;
            live_h2 = h2;
            live_source = "модель";
        }
    } else if (!have_live && have_model) {
        Vec3 p = model;
        if (!ground) p.y += 0.90F;
        live = p;
        have_live = true;
        live_h2 = vec3_horiz2(p, saved);
        live_source = "модель";
    }
    if (!ground && !have_live && position_looks_like_world_space(feet)) {
        live = feet;
        have_live = true;
        live_h2 = vec3_horiz2(feet, saved);
        live_source = "ноги";
    }

    const bool was_engaged = latch.engaged;
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
            latch.hold_count = 0;
            latch.hold_streak = 0;
            latch.miss_streak = 0;
        }
    }
    if (latch.engaged != was_engaged) {
        const uint32_t vid = rd<uint32_t>(player + PLAYER_VEHICLE_ID);
        const uint32_t sid = rd<uint32_t>(player + PLAYER_SEAT_ID);
        if (latch.engaged)
            ;
        else
            ;
    }
    if (!latch.engaged) return;

    const char* source = "нет";
    bool held = false;
    if (have_live) {
        source = live_source;
        held = mountlatch::apply_hold(latch, saved, live, live_h2, latch.saved_still, feet);
    } else {
        ++latch.miss_streak;
        if (latch.last_ok) feet = latch.last;
    }

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
                    if (std::isfinite(speed) && speed < 12.0F) {
                        track.vel = playermotion::blend_velocity(track.vel, v);
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

// Объект в прошлом кадре признали мёртвым. Решение живёт один кадр, поэтому
// staleness ограничиваем секундой: объекты пула меняют владельца, и вчерашний
// труп не должен распоряжаться uid сегодня.
static bool player_track_is_corpse(uint64_t player) {
    auto it = g_player_track.find(player);
    if (it == g_player_track.end() || !it->second.corpse) return false;
    return mono_seconds() - it->second.corpse_at < 1.0;
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
// Пассажиры коптера — отдельный случай «настоящего» скачка: подтверждение по
// форме ломаной, а не по той же точке (см. position_filter.h, покрыт тестом).
// (сырое чтение, при read_ok) и выход (то, что рисуем).
static double s_filt_sec = -1e9;
static posfilter::Obs s_filt_obs;

static bool filter_player_position(PlayerTrack& track, bool read_ok, Vec3& pos, uint64_t player) {
    const double now = mono_seconds();
    // Наблюдение (на решения не влияет): сколько рваных отсчётов в секунду и
    // сколько из них подтвердилось как настоящий скачок. Строка — не чаще раза
    // в секунду и только когда было что считать.
    if (now - s_filt_sec >= 1.0) {
        s_filt_sec = now;
        if (s_filt_obs.susp + s_filt_obs.conf > 0) {
            s_filt_obs.susp = 0; s_filt_obs.conf = 0;
        }
    }

    const bool ok = posfilter::filter(track.filt, read_ok, pos, track.vel, now, s_filt_obs);
    if (s_filt_obs.vel_reset) { track.vel = {}; track.have_vel_ref = false; s_filt_obs.vel_reset = false; }
    if (ok && s_filt_obs.conf) s_filt_obs.last = player;
    return ok;
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
    if (!root) {
        // Вшитый слот characterModel не дал модель (поле переехало). Имя
        // ищем по поддереву самого игрока: модель обычно лежит под ним, а для
        // поиска смещения имени хватит любых GameObject с известными именами.
        root = resolve_player_native_transform(player);
    }
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
// (items.txt, /storage/emulated/0/benzware). The id comes from the synced
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
// A small control-latency floor is applied by esp_draw.cpp for memory/touch;
// the user setting may raise it. This shifts only the aim point, not the ESP
// box. Projectile flight is solved separately from weapon speed and profile;
// silent uses the same model for the shot axis even with zero gravity/drag.
static Vec3 g_aim_lead{};
static Vec3 g_aim_velocity{};

namespace {
constexpr const char* kAimWeaponNamespace = "HyperHug.Games.Oxide.Features.Weapons";
constexpr const char* kAimFirstPersonNamespace =
    "HyperHug.Games.Oxide.By_Namespace.Oxide.Player.First_Person";
constexpr const char* kAimBallisticsNamespace =
    "HyperHug.Games.Oxide.Features.Weapons.Ballistics";

struct AimBallisticRead {
    aim_ballistics::Parameters params{};
    bool valid_chain = false;
};

bool aim_read_pointer_exact(uint64_t address, uint64_t& value) {
    value = 0;
    uint64_t raw = 0;
    if (!rd_exact(address, raw)) return false;
    value = memio::untag(raw);
    return raw == 0 || valid_obj(value);
}

int aim_object_identity(uint64_t object, const char* name, const char* name_space) {
    if (!valid_obj(object)) return 0;
    uint64_t klass = 0;
    if (!aim_read_pointer_exact(object, klass) || !valid_obj(klass)) return 0;
    return class_identity(klass, name, name_space);
}

bool aim_read_float(uint64_t address, float minimum, float maximum, float& value) {
    value = 0.0f;
    float read_value = 0.0f;
    if (!rd_exact(address, read_value) || !aim_ballistics::finite(read_value) ||
        read_value < minimum || read_value > maximum) return false;
    value = read_value;
    return true;
}

bool aim_read_profile(uint64_t profile, AimBallisticRead& read) {
    if (aim_object_identity(profile, "BallisticProfile", kAimBallisticsNamespace) == 0)
        return false;
    float gravity = 0.0f, air_drag = 0.0f, underwater_drag = 0.0f;
    if (!aim_read_float(profile + BALLISTICPROFILE_GRAVITY, -20.0f, 20.0f, gravity) ||
        !aim_read_float(profile + BALLISTICPROFILE_AIR_DRAG, 0.0f, 100.0f, air_drag) ||
        !aim_read_float(profile + BALLISTICPROFILE_UNDERWATER_DRAG, 0.0f, 100.0f,
                        underwater_drag)) return false;
    read.params.gravity = gravity;
    read.params.air_drag = air_drag;
    read.params.underwater_drag = underwater_drag;
    return true;
}

bool aim_read_weapon_config(uint64_t weapon, uint64_t offset, bool allow_structural_fallback,
                            AimBallisticRead& read) {
    read = {};
    uint64_t config = 0;
    if (!aim_read_pointer_exact(weapon + offset, config) || !valid_obj(config)) return false;
    const int identity = aim_object_identity(config, "HitscanWeaponConfig", kAimWeaponNamespace);
    if (identity == 0) return false;

    float max_distance = 0.0f, bullet_speed = 0.0f;
    if (!aim_read_float(config + HITSCANCONFIG_MAX_DISTANCE, 0.0f, 5000.0f, max_distance) ||
        !aim_read_float(config + HITSCANCONFIG_BULLET_SPEED, 0.0f, 2000.0f, bullet_speed))
        return false;

    uint64_t profile = 0;
    if (!aim_read_pointer_exact(config + HITSCANCONFIG_BALLISTIC_PROFILE, profile)) return false;
    if (profile && !aim_read_profile(profile, read)) return false;

    if (identity == 2 && allow_structural_fallback) {
        float fire_rate = 0.0f, dispersion = 0.0f;
        int32_t projectiles = 0;
        uint64_t dispersion_config = 0;
        if (!aim_read_float(config + HITSCANCONFIG_FIRE_RATE, 0.0f, 1000.0f, fire_rate) ||
            !aim_read_float(config + HITSCANCONFIG_BASE_DISPERSION, 0.0f, 1000.0f,
                            dispersion) ||
            !rd_exact(config + HITSCANCONFIG_PROJECTILES_PER_SHOT, projectiles) ||
            !aim_read_pointer_exact(config + HITSCANCONFIG_DISPERSION_CONFIG,
                                    dispersion_config) ||
            !valid_obj(dispersion_config) ||
            aim_object_identity(dispersion_config, "DispersionConfig", kAimWeaponNamespace) == 0 ||
            projectiles < 1 || projectiles > 32 || !(fire_rate > 0.0f) ||
            !(max_distance > 0.0f) || !(bullet_speed > 1.0f) || dispersion > 30.0f)
            return false;
    }

    read.params.muzzle_speed = bullet_speed;
    read.params.max_range = max_distance;
    read.params.valid = bullet_speed >= 1.0f;
    read.valid_chain = true;
    return true;
}

bool aim_read_projectile(uint64_t weapon, uint64_t offset, bool allow_structural_fallback,
                         AimBallisticRead& read) {
    read = {};
    uint64_t projectile = 0;
    if (!aim_read_pointer_exact(weapon + offset, projectile) || !valid_obj(projectile))
        return false;
    const int identity = aim_object_identity(projectile, "ShaftedProjectile", "Oxide");
    if (identity == 0) return false;

    float max_distance = 0.0f, launch_speed = 0.0f;
    float gravity = 0.0f, air_drag = 0.0f, underwater_drag = 0.0f;
    if (!aim_read_float(projectile + SHAFTEDPROJECTILE_MAX_DISTANCE, 0.0f, 5000.0f,
                        max_distance) ||
        !aim_read_float(projectile + SHAFTEDPROJECTILE_LAUNCH_SPEED, 0.0f, 2000.0f,
                        launch_speed) ||
        !aim_read_float(projectile + SHAFTEDPROJECTILE_GRAVITY, -20.0f, 20.0f, gravity) ||
        !aim_read_float(projectile + SHAFTEDPROJECTILE_AIR_DRAG, 0.0f, 100.0f, air_drag) ||
        !aim_read_float(projectile + SHAFTEDPROJECTILE_UNDERWATER_DRAG, 0.0f, 100.0f,
                        underwater_drag)) return false;
    if (identity == 2 && allow_structural_fallback &&
        (!(max_distance > 0.0f) || !(launch_speed > 1.0f))) return false;

    read.params.muzzle_speed = launch_speed;
    read.params.max_range = max_distance;
    read.params.gravity = gravity;
    read.params.air_drag = air_drag;
    read.params.underwater_drag = underwater_drag;
    read.params.valid = launch_speed >= 1.0f;
    read.valid_chain = true;
    return true;
}

void aim_merge_ballistics(const AimBallisticRead& config, const AimBallisticRead& projectile,
                          aim_ballistics::Parameters& out) {
    out = {};
    if (projectile.valid_chain && projectile.params.valid) {
        out = projectile.params;
        if (!(out.max_range > 0.0f) && config.valid_chain && config.params.valid)
            out.max_range = config.params.max_range;
    } else if (config.valid_chain && config.params.valid) {
        out = config.params;
    }
    out.valid = out.muzzle_speed >= 1.0f && aim_ballistics::parameters_valid(out);
}

void update_aim_ballistics(uint64_t player) {
    static uint64_t cached_player = 0;
    static uint64_t cached_weapon = 0;
    static uint64_t cached_weapon_class = 0;
    static double cached_at = 0.0;
    static aim_ballistics::Parameters cached_params{};
    static bool cached_instant_ray = false;
    constexpr double kCacheSeconds = 0.35;
    const double now = mono_seconds();

    g_aim_ballistic_params = {};
    g_aim_is_instant_ray = false;
    g_aim_inherited_velocity = {};
    if (valid_obj(player)) {
        auto local_track = g_player_track.find(player);
        if (local_track != g_player_track.end() && local_track->second.have_vel_ref) {
            const double age = now - local_track->second.vel_ref_t;
            const Vec3 velocity = local_track->second.vel;
            const float speed = aim_ballistics::length(velocity);
            if (age >= 0.0 && age <= 0.35 && aim_ballistics::finite(velocity) &&
                aim_ballistics::finite(speed) && speed <= 12.0f)
                g_aim_inherited_velocity = velocity;
        }
    }

    auto clear_cache = [&]() {
        cached_weapon = 0;
        cached_weapon_class = 0;
        cached_at = 0.0;
        cached_params = {};
        cached_instant_ray = false;
    };
    auto restore_cache = [&]() {
        if (cached_params.valid && now >= cached_at && now - cached_at <= kCacheSeconds) {
            g_aim_ballistic_params = cached_params;
            g_aim_is_instant_ray = cached_instant_ray;
        } else {
            g_aim_ballistic_params = {};
            g_aim_is_instant_ray = false;
            cached_params = {};
            cached_at = 0.0;
        }
    };

    if (valid_obj(player) && cached_player != player) {
        clear_cache();
        cached_player = player;
    }
    if (!g_mem.bound() || !valid_obj(player) ||
        aim_object_identity(player, "PlayerManager", "Oxide") == 0) {
        restore_cache();
        return;
    }

    uint64_t fp = 0;
    if (!aim_read_pointer_exact(player + PLAYER_FP_MANAGER, fp)) {
        restore_cache();
        return;
    }
    if (!valid_obj(fp)) {
        clear_cache();
        return;
    }
    if (aim_object_identity(fp, "FPManager", "Oxide") == 0) {
        restore_cache();
        return;
    }

    uint64_t weapon = 0;
    if (!aim_read_pointer_exact(fp + FPMANAGER_CURRENT_OBJECT, weapon)) {
        restore_cache();
        return;
    }
    if (!valid_obj(weapon)) {
        clear_cache();
        return;
    }
    uint64_t weapon_class = 0;
    if (!aim_read_pointer_exact(weapon, weapon_class) || !valid_obj(weapon_class)) {
        restore_cache();
        return;
    }
    if (cached_weapon != weapon || cached_weapon_class != weapon_class) {
        clear_cache();
        cached_weapon = weapon;
        cached_weapon_class = weapon_class;
    }

    uint64_t owner = 0;
    if (!aim_read_pointer_exact(weapon + FPOBJECT_PLAYER_BACKREF, owner)) {
        restore_cache();
        return;
    }
    if (owner != player) {
        clear_cache();
        return;
    }

    enum class Kind { None, Hitscan, Bow, Crossbow, Rocket, Explosive, Snowball, Spear };
    Kind kind = Kind::None;
    if (aim_object_identity(weapon, "FPHitscan", "Oxide") == 1) kind = Kind::Hitscan;
    else if (aim_object_identity(weapon, "FPBow", "Oxide") == 1) kind = Kind::Bow;
    else if (aim_object_identity(weapon, "FPCrossbow", kAimFirstPersonNamespace) == 1)
        kind = Kind::Crossbow;
    else if (aim_object_identity(weapon, "FPRocketLauncher", "Oxide") == 1)
        kind = Kind::Rocket;
    else if (aim_object_identity(weapon, "FPExplosiveCharge", "Oxide") == 1)
        kind = Kind::Explosive;
    else if (aim_object_identity(weapon, "FPSnowball", kAimFirstPersonNamespace) == 1)
        kind = Kind::Snowball;
    else if (aim_object_identity(weapon, "FPSpear", "Oxide") == 1)
        kind = Kind::Spear;

    uint64_t config_offset = 0, projectile_offset = 0;
    switch (kind) {
        case Kind::Hitscan: config_offset = FPHITSCAN_CONFIG; break;
        case Kind::Bow: projectile_offset = FPBOW_PROJECTILE_PREFAB; break;
        case Kind::Crossbow:
            config_offset = FPCROSSBOW_CONFIG;
            projectile_offset = FPCROSSBOW_PROJECTILE_PREFAB;
            break;
        case Kind::Rocket:
            config_offset = FPROCKETLAUNCHER_CONFIG;
            projectile_offset = FPROCKETLAUNCHER_PROJECTILE_PREFAB;
            break;
        case Kind::Explosive: projectile_offset = FPEXPLOSIVECHARGE_PROJECTILE_PREFAB; break;
        case Kind::Snowball: projectile_offset = FPSNOWBALL_PROJECTILE_PREFAB; break;
        case Kind::Spear: projectile_offset = FPSPEAR_PROJECTILE_PREFAB; break;
        default: break;
    }

    AimBallisticRead config_read{}, projectile_read{};
    bool config_ok = false, projectile_ok = false;
    if (config_offset)
        config_ok = aim_read_weapon_config(weapon, config_offset, false, config_read);
    if (projectile_offset)
        projectile_ok = aim_read_projectile(weapon, projectile_offset, false, projectile_read);

    bool structural_hitscan = false;
    if (kind == Kind::None) {
        const uint64_t config_candidates[] = {
            FPHITSCAN_CONFIG, FPCROSSBOW_CONFIG, FPROCKETLAUNCHER_CONFIG
        };
        for (uint64_t offset : config_candidates) {
            if (aim_read_weapon_config(weapon, offset, true, config_read) &&
                config_read.params.valid) {
                config_ok = true;
                structural_hitscan = offset == FPHITSCAN_CONFIG;
                break;
            }
            config_read = {};
        }
        const uint64_t projectile_candidates[] = {
            FPBOW_PROJECTILE_PREFAB, FPCROSSBOW_PROJECTILE_PREFAB,
            FPROCKETLAUNCHER_PROJECTILE_PREFAB, FPEXPLOSIVECHARGE_PROJECTILE_PREFAB,
            FPSPEAR_PROJECTILE_PREFAB
        };
        for (uint64_t offset : projectile_candidates) {
            if (aim_read_projectile(weapon, offset, true, projectile_read) &&
                projectile_read.params.valid) { projectile_ok = true; break; }
            projectile_read = {};
        }
    }

    // FPHitscan casts a Ray along LookDirection (game_offsets.h). Its config
    // bulletSpeed is not the Ray's hit time: distance / bulletSpeed overleads
    // a runner by metres. If metadata names cannot be read, the confirmed
    // config at the hitscan offset (and no projectile) identifies this path.
    g_aim_is_instant_ray = kind == Kind::Hitscan ||
                           (kind == Kind::None && structural_hitscan && !projectile_ok);
    aim_ballistics::Parameters current{};
    aim_merge_ballistics(config_ok ? config_read : AimBallisticRead{},
                         projectile_ok ? projectile_read : AimBallisticRead{}, current);
    if (current.valid) {
        g_aim_ballistic_params = current;
        cached_params = current;
        cached_instant_ray = g_aim_is_instant_ray;
        cached_at = now;
    } else if (!g_aim_is_instant_ray) {
        restore_cache();
    }
}

bool aim_ballistic_origin(Vec3& origin) {
    if (g_aim_ref_valid && aim_ballistics::finite(g_aim_ref_origin)) {
        origin = g_aim_ref_origin;
        return true;
    }
    if (g_cam_pose_valid && aim_ballistics::finite(g_cam_pos)) {
        origin = g_cam_pos;
        return true;
    }
    if (g_frame_cam_basis_valid && aim_ballistics::finite(g_frame_cam_pos)) {
        origin = g_frame_cam_pos;
        return true;
    }
    return false;
}
} // namespace

// Растяг экрана (Aspect Ratio), множитель к aspect камеры игры; 1.0 = выкл.
// Поставляется из меню через esp_set_screen_stretch (см. esp_draw.cpp).
static float g_screen_stretch_x = 1.0F;
void esp_set_screen_stretch(float factor) {
    // Кламп к допустимому диапазону слайдера (1.00–2.00; верх поднят по заказу 04.10).
    if (!(factor >= 0.75F && factor <= 2.0F)) factor = 1.0F;
    g_screen_stretch_x = factor;
}
void esp_set_aim_lead(float seconds) {
    // Без isfinite (-ffast-math): значение приходит со слайдера 0..150 мс
    // и проверяется при загрузке конфига, сюда мусор не попадает — обычный
    // кламп достаточен и безопасен.
    if (seconds < 0.0F) seconds = 0.0F;
    if (seconds > 0.15F) seconds = 0.15F;
    g_aim_lead_seconds = seconds;
}

static bool set_aim_point(EspBox& box, int slot, const Vec3& world_in, const Mat4& vp, float sw, float sh) {
    Vec3 world{world_in.x + g_aim_lead.x,
               world_in.y + g_aim_lead.y,
               world_in.z + g_aim_lead.z};
    if (slot == 0) world.y += head_lift_for_range(world_in);

    Vec3 origin{};
    const bool haveOrigin = aim_ballistic_origin(origin);
    bool ballisticSolved = false;
    if (g_state.aim_touch && g_state.aim_mode != AIM_MODE_SILENT &&
        g_aim_ballistic_params.valid && haveOrigin) {
        aim_ballistics::Solution solution{};
        if (aim_ballistics::solve(origin, world, g_aim_velocity,
                                  g_aim_inherited_velocity,
                                  g_aim_ballistic_params, solution)) {
            float range = aim_ballistics::length(aim_ballistics::sub(world, origin));
            if (aim_ballistics::finite(range) && range < 1.0f) range = 1.0f;
            if (aim_ballistics::finite(range)) {
                world = aim_ballistics::add(origin,
                    aim_ballistics::scale(solution.direction, range));
                ballisticSolved = true;
            }
        }
    }

    // Camera aim must stay on the selected bone when weapon data is missing;
    // the legacy range-only drop has no velocity or weapon model to justify it.
    if (!ballisticSolved && haveOrigin && g_state.aim_mode == AIM_MODE_SILENT) {
        const float range = aim_ballistics::length(aim_ballistics::sub(world_in, origin));
        if (aim_ballistics::finite(range) && range > 0.0f)
            world.y -= 0.05F * (range / 20.0F);
    }
    Vec2 screen{};
    if (!w2s(vp, world, sw, sh, screen, false)) return false;
    if (fabsf(screen.x) > sw * 4.0F || fabsf(screen.y) > sh * 4.0F) return false;
    float yaw = 0.0F, pitch = 0.0F;
    if (!aim_angles_for(world, screen, sw, sh, yaw, pitch)) return false;
    box.aim_pts[slot][0] = screen.x;
    box.aim_pts[slot][1] = screen.y;
    box.aim_yaw[slot] = yaw;
    box.aim_pitch[slot] = pitch;
    // Silent must not inherit the range-drop/head-lift compensation applied
    // above for camera-driving modes: publish the exact world-space bone.
    box.aim_world[slot][0] = world_in.x;
    box.aim_world[slot][1] = world_in.y;
    box.aim_world[slot][2] = world_in.z;
    box.aim_world_valid[slot] = true;
    box.aim_velocity[0] = g_aim_velocity.x;
    box.aim_velocity[1] = g_aim_velocity.y;
    box.aim_velocity[2] = g_aim_velocity.z;
    box.aim_sample_at = mono_seconds();
    box.aim_sample_age[slot] = 0.f;
    box.aim_valid[slot] = true;
    return true;
}

// Возраст мировой кости (в кадрах), при котором она ещё годится.
//   * kSkeletonDrawMaxAge — на экран: переиспользованная точка отстаёт от тела,
//     и две-три кости «уезжают» вперёд/назад (видно на панораме и в беге);
//   * kSkeletonAimMaxAge — в прицел: там нужна мировая ТОЧКА и упреждение, и
//     короткий перебой чтения лучше пропущенного кадра аима.
constexpr uint8_t kSkeletonDrawMaxAge = 2;
constexpr uint8_t kSkeletonAimMaxAge  = 9;

static bool fill_skeleton_box(uint64_t player, const Mat4& view_projection, float sw, float sh, EspBox& box) {
    box.has_skeleton = false;
    for (int bone = 0; bone < ESP_BONE_COUNT; ++bone) box.bone_valid[bone] = false;
    for (int i = 0; i < 3; ++i) {
        box.aim_valid[i] = false;
        box.aim_world_valid[i] = false;
    }

    CachedSkeleton& skeleton = g_skeletons[player];
    if (!skeleton.valid) {
        if (skeleton.retry_cooldown > 0) { --skeleton.retry_cooldown; return false; }
        // Building walks the whole model; allow at most one rebuild per frame
        // so multiple new players never stall the overlay.
        if (g_skeleton_builds_this_frame >= 1) return false;
        ++g_skeleton_builds_this_frame;
        // build_skeleton обнуляет структуру, поэтому счётчик провалов держим
        // здесь: он переживает пересборку и копит отступ.
        const int build_fails = skeleton.build_fail;
        const bool built = build_skeleton(player, skeleton);
        if (!built) {
            skeleton.valid = false;
            // Объект без модели (манекен, спектейтор, пустое кресло) не
            // обзаведётся ей от того, что мы будем пересобирать его каждые
            // 20 кадров. Отступаем всё дальше — иначе такие объекты занимают
            // единственный слот пересборки в кадре, и настоящий новый игрок
            // ждёт скелета несколько кадров (лог 10.10: двое таких висят всё
            // время — это разрыв «игроков 5, со скелетом 4»).
            skeleton.retry_cooldown = build_fails < 3 ? 20 : (build_fails < 6 ? 120 : 600);
            skeleton.build_fail = build_fails + 1;
            return false;
        }
        skeleton.build_fail = 0;
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
        if (++skeleton.fail_streak > 30) {
            skeleton = CachedSkeleton{};
            skeleton.retry_cooldown = 30;
        }
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
        // Раскладка, у которой matrices/indices лежат не в самом слоте, а по
        // указателю из него: без разыменования читается указатель вместо
        // массива, пакетный проход срывается и скелет гаснет (на сборках с
        // косвенными слотами — то есть «на части устройств»).
        if (g_skeleton_layout.matrices_indirect) matrices = rd_ptr(matrices);
        if (g_skeleton_layout.indices_indirect) indices = rd_ptr(indices);
        if (!matrices || !indices) continue;
        local_matrices[g].resize((size_t)needed);
        local_parents[g].resize((size_t)needed);
        if (!rd_buf(matrices, local_matrices[g].data(), (size_t)needed * sizeof(Matrix34)) ||
            !rd_buf(indices, local_parents[g].data(), (size_t)needed * sizeof(int32_t))) {
            continue;
        }
        group_matrices[g] = matrices;
        group_indices[g] = indices;
        group_ok[g] = true;
    }

    int projected = 0;
    int bones_world = 0;      // костей с мировой позицией, прочитанных сейчас
    int remote_fallbacks = 0;
    // Мировые кости этого кадра — отдельно для аима и для рисования. Возраст
    // годности у них разный: аиму важна ТОЧКА (и он сам держит упреждение), а
    // на экране переиспользованная кость отстаёт от тела, и скелет «плывёт»
    // относительно бокса. Отсюда два порога: 9 — аим, 2 — рисование.
    bool aim_bone_ok[ESP_BONE_COUNT] = {};
    Vec3 aim_bone_world[ESP_BONE_COUNT] = {};
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
                ++bones_world;
                skeleton.bone_world[bone] = world;
                skeleton.bone_world_age[bone] = 1;
                skeleton.bone_world_at[bone] = mono_seconds();
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
        const uint8_t age = skeleton.bone_world_age[bone];
        if (age >= 1 && age <= kSkeletonAimMaxAge) {
            aim_bone_ok[bone] = true;
            aim_bone_world[bone] = world;
        }
        // Рисуется только свежая кость (возраст <= 2). Переиспользованная точка
        // (её держат до 8 кадров, пока чтение срывается) на экране отстаёт от
        // тела: рука уезжает вперёд, нога назад — на панораме это и читается
        // как «скелет плавает рядом с игроком». Аиму старость не мешает: он
        // ведёт по точке и упреждение держит сам (см. kSkeletonAimMaxAge).
        if (age < 1 || age > kSkeletonDrawMaxAge) continue;
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
        // Для аима кость годна и тогда, когда она уже не рисуется (возраст до
        // kSkeletonAimMaxAge): точка живёт на экране своей жизнью, а прицелу
        // нужна мировая позиция.
        auto bone_world_ok = [&](int bone, Vec3& out) -> bool {
            if (!aim_bone_ok[bone]) return false;
            out = aim_bone_world[bone];
            return vec3_is_finite(out);
        };
        Vec3 target[3]{};
        bool have[3] = {false, false, false};
        double sample_at[3] = {};

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
            sample_at[0] = skeleton.bone_world_at[BONE_HEAD];
            have[0] = true;
        }
        if (have_neck_bone && have_head_bone) {
            target[1] = {(neck.x + head.x) * 0.5F, (neck.y + head.y) * 0.5F, (neck.z + head.z) * 0.5F};
            sample_at[1] = std::min(skeleton.bone_world_at[BONE_HEAD],
                                    skeleton.bone_world_at[BONE_NECK]);
            have[1] = true;
        } else if (have_neck_bone) {
            target[1] = {neck.x, neck.y + 0.05F, neck.z};
            sample_at[1] = skeleton.bone_world_at[BONE_NECK]; have[1] = true;
        } else if (have_head_bone) {
            target[1] = {head.x, head.y - 0.06F, head.z};
            sample_at[1] = skeleton.bone_world_at[BONE_HEAD]; have[1] = true;
        }
        Vec3 chest{};
        for (int bone : {BONE_SPINE2, BONE_SPINE1, BONE_SPINE}) {
            if (!bone_world_ok(bone, chest)) continue;
            target[2] = chest; sample_at[2] = skeleton.bone_world_at[bone];
            have[2] = true; break;
        }

        for (int i = 0; i < 3; ++i) {
            if (!have[i]) continue;
            if (set_aim_point(box, i, target[i], view_projection, sw, sh)) {
                box.aim_source = 1;
                const double age = box.aim_sample_at - sample_at[i];
                box.aim_sample_age[i] = sample_at[i] > 0.0 && age >= 0.0
                    ? static_cast<float>(age) : 0.f;
            }
        }
    }

    // Данные целы — мировые позиции костей читаются, просто игрок за спиной
    // или за краем экрана. Кэш не трогаем: раньше не спроецировавшаяся кость
    // крутила fail_streak, и через 30 кадров скелет игрока за спиной
    // выбрасывался и пересобирался заново — обход модели на ровном месте.
    if (bones_world >= 4) skeleton.fail_streak = 0;
    if (projected < 4) {
        if (bones_world < 4 && ++skeleton.fail_streak > 30) {
            skeleton = CachedSkeleton{};
            skeleton.retry_cooldown = 30;
        }
        return false;
    }
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
    const uint64_t local = resolve_local_player();
    if (!local) { g_aim_state = {}; return false; }

    // ADS — короткоживущий флаг. После респавна старый PlayerManager и его Aim
    // activity могут оставаться доступными ещё несколько кадров, поэтому здесь
    // нет кэша: каждый тик читается цепочка именно текущего локального игрока.
    LocalAimState fresh{};

    // Primary: PlayerManager.playerEventHandler.Aim.Active.
    const uint64_t handler = rd_ptr(local + PLAYER_EVENT_HANDLER);
    if (handler && rd_ptr(handler + EVENT_HANDLER_MANAGER_BACKREF) == local) {
        const uint64_t activity = rd_ptr(handler + EVENT_HANDLER_AIM_ACTIVITY);
        uint8_t active = 0;
        if (activity && rd_exact(activity + ACTIVITY_ACTIVE_FLAG, active) && active <= 1) {
            fresh.event_handler = handler;
            fresh.aim_activity = activity;
            fresh.aiming = active != 0;
            fresh.source = 1;
            g_aim_state = fresh;
            return true;
        }
    }

    // Fallback: current first-person weapon isAiming flag. The visual FOV blend
    // deliberately is not used: it decays after leaving ADS and would keep the
    // aimbot active through that transition.
    const uint64_t fp_manager = rd_ptr(local + PLAYER_FP_MANAGER);
    if (fp_manager) {
        const uint64_t weapon = rd_ptr(fp_manager + FPMANAGER_CURRENT_WEAPON);
        if (weapon && rd_ptr(weapon + FPOBJECT_PLAYER_BACKREF) == local) {
            uint8_t is_aiming = 0;
            if (rd_exact(weapon + FPWEAPON_IS_AIMING, is_aiming) && is_aiming <= 1) {
                fresh.fp_manager = fp_manager;
                fresh.weapon = weapon;
                fresh.aiming = is_aiming != 0;
                fresh.source = 2;
                g_aim_state = fresh;
                return true;
            }
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
    return (g_pid > 0 && g_il2cpp_base) ? g_aim_state.source : 0;
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
            const uint64_t fallback = rd_ptr(handler + GUM_AIM_RAYCAST);
            out.ray_primary_wrapper = valid_obj(primary) ? primary : 0;
            out.ray_fallback_wrapper = valid_obj(fallback) ? fallback : 0;
            if (valid_obj(primary)) data = rd_ptr(primary + GUI_VALUE);
            if (!valid_obj(data) && valid_obj(fallback))
                data = rd_ptr(fallback + GUI_VALUE);
            if (valid_obj(data)) {
                out.ray_valid = true;
                out.ray_data = data;
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

// ==== Пин крестика: клей крестика к точке удара ==============================
//
// Прежний «Magic-Farm» пытался перенаправить САМ удар — подменял точку
// попадания в GKo (RaycastData) перед тапом. На живой сборке игра успевает
// пересчитать свой луч в момент замаха, и удар всё равно проходил туда, куда
// смотрела камера, — по корпусу узла, мимо крестика. Поэтому логика
// развёрнута: не удар тащим к крестику, а КРЕСТИК К ТОЧКЕ УДАРА.
//
// Проверка серии у самой игры меряет дистанцию от точки попадания до
// крестика:
//   руда   — OreHitstreaks.os() читает мировую позицию transform'а живого
//            маркера (MoW);
//   дерево — дистанцию до отрезка MTQ..MTu (TREEHS_SPOT_A/B) радиусом 0.15 м
//            в локальных координатах узла.
// Оба места доступны на запись:
//   руда   — трансляция маркера в кеше иерархии трансформов Unity: позиция
//            геттера Transform.position собирается из тех же массивов, из
//            которых читаем мы (read_transform_hierarchy_arrays);
//   дерево — MTQ/MTu это обычные managed-поля экстеншена: пишем точку,
//            переведённую из мировой в локальную узла через его TRS
//            (собираем TRS из той же иерархии). Декаль-визуал дерева
//            двигаем тем же способом, что и маркер руды, — чтобы X на
//            экране стоял ровно там, куда падают удары.
//
// Пин живёт в фоновом потоке (по образцу сайлент-аима: только
// esp_silent_read/esp_silent_write, привязка проверяется каждый цикл),
// раз в ~3 мс перечитывает живой луч прицела игры (GKo из обёрток GuI) и
// клеит крестик ровно в точку попадания. Камера при этом всегда держит
// центр узла: серия растёт без какого-либо наведения на крестик.
namespace {

struct CrossPinTRS {
    Vec3 t{};
    Vec4 q{0.0F, 0.0F, 0.0F, 1.0F};
    Vec3 s{1.0F, 1.0F, 1.0F};
};

// world = parent (∘) child — композиция одного уровня иерархии.
static void cross_trs_compose(const CrossPinTRS& parent, const CrossPinTRS& child, CrossPinTRS& out) {
    const Vec3 scaled = { child.t.x * parent.s.x, child.t.y * parent.s.y, child.t.z * parent.s.z };
    const Vec3 rotated = rotate_vector(parent.q, scaled);
    CrossPinTRS result;
    result.t = { parent.t.x + rotated.x, parent.t.y + rotated.y, parent.t.z + rotated.z };
    result.q = multiply_quaternion(parent.q, child.q);
    result.s = { parent.s.x * child.s.x, parent.s.y * child.s.y, parent.s.z * child.s.z };
    out = result;
}

// Мир -> локаль для TRS. Кватернионы иерархии единичные: обратный =
// сопряжённый. Масштаб делим покомпонентно (как и композиция выше).
static bool cross_trs_world_to_local(const CrossPinTRS& world, const Vec3& p, Vec3& out) {
    if (fabsf(world.s.x) < 1e-4F || fabsf(world.s.y) < 1e-4F || fabsf(world.s.z) < 1e-4F) return false;
    const Vec3 d = { p.x - world.t.x, p.y - world.t.y, p.z - world.t.z };
    const Vec3 unscaled = { d.x / world.s.x, d.y / world.s.y, d.z / world.s.z };
    const Vec4 inv = { -world.q.x, -world.q.y, -world.q.z, world.q.w };
    const Vec3 local = rotate_vector(inv, unscaled);
    if (!vec3_is_finite(local)) return false;
    out = local;
    return true;
}

struct CrossPinHierarchy {
    uint64_t matrices = 0, indices = 0;
    int32_t index = -1;
};

// Найти массивы иерархии и индекс трансформа. Те же пробы, что у
// read_transform_hierarchy_position: выученная раскладка, потом +0x38/+0x40
// и +0x18/+0x20 с разыменованием кандидатов. Для записи кандидат обязан
// ещё и компоноваться в осмысленную мировую позицию.
static bool cross_pin_resolve_arrays(uint64_t native_transform, CrossPinHierarchy& out) {
    if (!native_transform) return false;
    if (g_transform_hierarchy_layout_valid) {
        const uint64_t data = rd_ptr(native_transform + g_transform_hierarchy_layout.data_offset);
        const int32_t index = rd<int32_t>(native_transform + g_transform_hierarchy_layout.index_offset);
        if (data && index >= 0 && index <= 100000) {
            uint64_t matrices = rd_ptr(data + g_transform_hierarchy_layout.matrices_offset);
            uint64_t indices = rd_ptr(data + g_transform_hierarchy_layout.indices_offset);
            if (g_transform_hierarchy_layout.matrices_indirect) matrices = rd_ptr(matrices);
            if (g_transform_hierarchy_layout.indices_indirect) indices = rd_ptr(indices);
            if (matrices && indices) {
                Matrix34 probe{};
                if (rd_exact(matrices + (uint64_t)index * sizeof(Matrix34), probe) &&
                    matrix34_is_valid(probe)) {
                    out.matrices = matrices; out.indices = indices; out.index = index;
                    return true;
                }
            }
        }
    }
    uint64_t transform_data = rd_ptr(native_transform + 0x38);
    int32_t transform_index = rd<int32_t>(native_transform + 0x40);
    if (!transform_data || transform_index < 0 || transform_index > 100000) {
        transform_data = rd_ptr(native_transform + 0x18);
        transform_index = rd<int32_t>(native_transform + 0x20);
    }
    if (!transform_data || transform_index < 0 || transform_index > 100000) return false;
    const uint64_t pairs[2][2] = {{0x18, 0x20}, {0x08, 0x10}};
    for (int pi = 0; pi < 2; ++pi) {
        const uint64_t matrix_pointer = rd_ptr(transform_data + pairs[pi][0]);
        const uint64_t index_pointer = rd_ptr(transform_data + pairs[pi][1]);
        if (!matrix_pointer || !index_pointer) continue;
        const uint64_t matrix_candidates[2] = {matrix_pointer, rd_ptr(matrix_pointer)};
        const uint64_t index_candidates[2] = {index_pointer, rd_ptr(index_pointer)};
        for (uint64_t matrices : matrix_candidates) {
            if (!matrices) continue;
            for (uint64_t indices : index_candidates) {
                if (!indices) continue;
                Matrix34 probe{};
                if (!rd_exact(matrices + (uint64_t)transform_index * sizeof(Matrix34), probe) ||
                    !matrix34_is_valid(probe))
                    continue;
                Vec3 composed{};
                if (read_transform_hierarchy_arrays(matrices, indices, transform_index, composed)) {
                    out.matrices = matrices; out.indices = indices; out.index = transform_index;
                    return true;
                }
            }
        }
    }
    return false;
}

// Мировая TRS трансформа (include_self=false — только предки: нужна для
// перевода world->local самого трансформа). Цепочка обязана дойти до корня,
// каждая запись — валидная Matrix34.
static bool cross_pin_read_trs(const CrossPinHierarchy& h, bool include_self, CrossPinTRS& out) {
    if (!h.matrices || !h.indices || h.index < 0) return false;
    Matrix34 chain[64];
    int count = 0;
    if (include_self) {
        if (!rd_exact(h.matrices + (uint64_t)h.index * sizeof(Matrix34), chain[0]) ||
            !matrix34_is_valid(chain[0]))
            return false;
        count = 1;
    }
    int32_t parent = -2;
    if (!rd_exact(h.indices + (uint64_t)h.index * sizeof(int32_t), parent)) return false;
    int depth = 0;
    int32_t previous = h.index;
    while (parent >= 0 && depth++ < 64) {
        if (parent > 100000 || parent == previous || count >= 64) return false;
        if (!rd_exact(h.matrices + (uint64_t)parent * sizeof(Matrix34), chain[count]) ||
            !matrix34_is_valid(chain[count]))
            return false;
        ++count;
        previous = parent;
        if (!rd_exact(h.indices + (uint64_t)parent * sizeof(int32_t), parent)) return false;
    }
    if (parent != -1) return false;
    CrossPinTRS acc;
    for (int i = count - 1; i >= 0; --i) {
        CrossPinTRS link;
        link.t = { chain[i].translation.x, chain[i].translation.y, chain[i].translation.z };
        link.q = chain[i].rotation;
        link.s = { chain[i].scale.x, chain[i].scale.y, chain[i].scale.z };
        cross_trs_compose(acc, link, acc);
    }
    out = acc;
    return true;
}

// Записываемый трансформ: адрес его ЛОКАЛЬНОЙ трансляции в массиве иерархии
// (translation — первое поле Matrix34), снапшот для отката и мировая TRS
// предков. Адрес валидируется дважды: композицией из найденных массивов и
// независимым чтением мировой позиции (marker_world_position идёт другими
// путями — скелетная раскладка и собственные пробы). Совпадение двух путей
// = мы нашли те же данные, из которых позицию читает сама игра.
struct CrossPinEntry {
    uint64_t native_transform = 0;
    uint64_t local_addr = 0;      // адрес translation (пишем 12 байт: x,y,z)
    Vec3 snapshot{};              // исходная трансляция — откат при несходимости
    Vec4 self_r{};                // собственные rotation/scale (для контроля)
    Vec3 self_s{};
    CrossPinTRS parent{};         // мировая TRS предков
    bool valid = false;
};

static bool cross_pin_prepare_entry(uint64_t native_transform, CrossPinEntry& entry) {
    entry.valid = false;
    CrossPinHierarchy h;
    if (!cross_pin_resolve_arrays(native_transform, h)) return false;
    Vec3 independent{};
    if (!marker_world_position(native_transform, independent)) return false;
    Vec3 composed{};
    if (!read_transform_hierarchy_arrays(h.matrices, h.indices, h.index, composed)) return false;
    const float dx = composed.x - independent.x, dy = composed.y - independent.y,
                dz = composed.z - independent.z;
    if (dx * dx + dy * dy + dz * dz > 0.05F * 0.05F) return false;
    Matrix34 local{};
    if (!rd_exact(h.matrices + (uint64_t)h.index * sizeof(Matrix34), local) ||
        !matrix34_is_valid(local))
        return false;
    CrossPinTRS parent;
    if (!cross_pin_read_trs(h, false, parent)) return false;
    entry.native_transform = native_transform;
    entry.local_addr = h.matrices + (uint64_t)h.index * sizeof(Matrix34);  // translation в начале
    entry.snapshot = { local.translation.x, local.translation.y, local.translation.z };
    entry.self_r = local.rotation;
    entry.self_s = { local.scale.x, local.scale.y, local.scale.z };
    entry.parent = parent;
    entry.valid = true;
    return true;
}

// Записать мировую позицию трансформа. Вызывается из фонового потока:
// только esp_silent_*. Контроль — перечитать трансляцию, скомпоновать мир и
// сравнить с желаемым; не сошлось (родители сдвинулись/адрес чужой) — откат
// снапшота и false.
static bool cross_pin_write_world_pos(const CrossPinEntry& entry, const Vec3& world) {
    if (!entry.valid || !entry.local_addr) return false;
    Vec3 local{};
    if (!cross_trs_world_to_local(entry.parent, world, local)) return false;
    if (!esp_silent_write(entry.local_addr, &local, sizeof(local))) return false;
    Vec3 back{};
    if (!esp_silent_read(entry.local_addr, &back, sizeof(back))) return false;
    CrossPinTRS self;
    self.t = back;
    self.q = entry.self_r;
    self.s = entry.self_s;
    CrossPinTRS composed;
    cross_trs_compose(entry.parent, self, composed);
    const float dx = composed.t.x - world.x, dy = composed.t.y - world.y,
                dz = composed.t.z - world.z;
    if (dx * dx + dy * dy + dz * dz > 0.06F * 0.06F) {
        esp_silent_write(entry.local_addr, &entry.snapshot, sizeof(entry.snapshot));
        return false;
    }
    return true;
}

struct CrossPin {
    bool tree = false;
    uint64_t ext = 0;                 // MineableObjectExtension (дерево: MTQ/MTu)
    bool node_trs_valid = false;      // мировая TRS узла (дерево: world->local)
    CrossPinTRS node_trs{};
    CrossPinEntry marker;             // руда: маркер — его позицию читает os()
    CrossPinEntry mark;               // дерево: декаль-визуал X
    uint64_t primary = 0, fallback_wrapper = 0;   // GuI<GKo> — живой луч игры
    Vec3 pivot{};                     // пивот узла (санити точки луча)
    Vec3 fallback{};                  // куда смотрит камера (клей без луча)
    unsigned long long node_id = 0;
    int pid = 0;
};
std::mutex g_cross_pin_mutex;
CrossPin g_cross_pin;
std::atomic<bool> g_cross_pin_active{false};
// Последний цикл пина не смог записаться (сбой транспорта/раскладки).
// esp_farm_cross_healthy() отдаёт это контроллеру — тот переподключит пин
// через пару секунд. Удары пин не блокирует: не включился — бьём как били.
std::atomic<bool> g_cross_pin_fail{false};
std::once_flag g_cross_pin_once;

// Точка луча обязана лежать на узле: иначе это чужая геометрия или мусор
// чтения, и клеить крестик туда нельзя.
static bool cross_pin_point_on_node(const Vec3& p, const Vec3& pivot, bool tree) {
    const float dx = p.x - pivot.x, dz = p.z - pivot.z;
    const float dy = p.y - pivot.y;
    if (!std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(dz)) return false;
    const float horiz = sqrtf(dx * dx + dz * dz);
    return horiz <= 2.5F && dy >= -2.5F && dy <= (tree ? 26.0F : 6.0F);
}

void cross_pin_worker() {
    // Луч гаснет на кадр-другой (рескан, сбой чтения) — помним последнюю
    // валидную точку на этом узле и держим клей на ней. Декаль, которую не
    // удалось записать, больше не трогаем до смены узла (косметика).
    static unsigned long long s_ray_node = 0;
    static Vec3 s_ray_last{};
    static bool s_ray_valid = false;
    static unsigned long long s_mark_skip = 0;
    for (;;) {
        if (!g_cross_pin_active.load(std::memory_order_acquire)) {
            usleep(2000);
            continue;
        }
        CrossPin pin;
        {
            std::lock_guard<std::mutex> lock(g_cross_pin_mutex);
            pin = g_cross_pin;
        }
        if (!pin.pid || !esp_silent_target(pin.pid)) {
            g_cross_pin_active.store(false, std::memory_order_release);
            continue;
        }
        if (pin.node_id != s_ray_node) {
            s_ray_node = pin.node_id;
            s_ray_valid = false;
            s_mark_skip = 0;
        }

        // Точка, куда реально падает удар: живой луч прицела игры из обёрток
        // GuI<GKo> (тот же GKo, из которого FPMelee берёт дистанцию удара).
        Vec3 point = pin.fallback;
        {
            uint64_t data = 0;
            if (pin.primary) {
                uint64_t value = 0;
                if (esp_silent_read(pin.primary + GUI_VALUE, &value, sizeof(value)))
                    data = memio::untag(value);
            }
            if (!valid_obj(data) && pin.fallback_wrapper) {
                uint64_t value = 0;
                if (esp_silent_read(pin.fallback_wrapper + GUI_VALUE, &value, sizeof(value)))
                    data = memio::untag(value);
            }
            if (valid_obj(data)) {
                Vec3 ray{};
                if (esp_silent_read(data + GKO_RAYCAST_HIT + RAYCASTHIT_POINT, &ray, sizeof(ray)) &&
                    vec3_is_finite(ray) && position_looks_like_world_space(ray) &&
                    cross_pin_point_on_node(ray, pin.pivot, pin.tree)) {
                    point = ray;
                    s_ray_last = ray;
                    s_ray_valid = true;
                } else if (s_ray_valid) {
                    point = s_ray_last;
                }
            } else if (s_ray_valid) {
                point = s_ray_last;
            }
        }

        // Клей крестика. Для дерева сегмент MTQ/MTu — критично (это проверка
        // серии); декаль — косметика, её неудача пин не роняет. Для руды
        // маркер и есть проверка — его неудача роняет пин.
        bool critical_ok = true;
        if (pin.tree) {
            if (pin.node_trs_valid && pin.ext) {
                Vec3 local{};
                if (cross_trs_world_to_local(pin.node_trs, point, local)) {
                    critical_ok = esp_silent_write(pin.ext + TREEHS_SPOT_A, &local, sizeof(local));
                    critical_ok = esp_silent_write(pin.ext + TREEHS_SPOT_B, &local, sizeof(local)) && critical_ok;
                } else {
                    critical_ok = false;
                }
            }
            if (pin.mark.valid && s_mark_skip != pin.node_id &&
                !cross_pin_write_world_pos(pin.mark, point))
                s_mark_skip = pin.node_id;   // до смены узла декаль не трогаем
        } else if (pin.marker.valid) {
            critical_ok = cross_pin_write_world_pos(pin.marker, point);
        }
        if (!critical_ok) {
            g_cross_pin_active.store(false, std::memory_order_release);
            g_cross_pin_fail.store(true, std::memory_order_release);
            continue;
        }
        usleep(3000);   // ~330 Гц: проверка серии читает поля в момент удара
    }
}
} // namespace

bool esp_farm_cross_pin(const FarmTarget& target, bool enabled) {
    if (!enabled) {
        g_cross_pin_active.store(false, std::memory_order_release);
        return true;
    }
    if (g_pid <= 0 || !target.id || !target.valid) return false;
    if (g_cross_pin_active.load(std::memory_order_acquire) &&
        !g_cross_pin_fail.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(g_cross_pin_mutex);
        if (g_cross_pin.node_id == target.id) {
            // Живое обновление: точка-запас, обёртки луча и пересозданные
            // маркеры (крестик гаснет по таймеру и появляется новым объектом).
            g_cross_pin.fallback = {target.aim_x, target.aim_y, target.aim_z};
            g_cross_pin.primary = memio::untag((uint64_t)target.ray_wrapper_primary);
            g_cross_pin.fallback_wrapper = memio::untag((uint64_t)target.ray_wrapper_fallback);
            if (g_cross_pin.tree) {
                if (target.cross_mark_transform &&
                    target.cross_mark_transform != g_cross_pin.mark.native_transform) {
                    CrossPinEntry entry;
                    if (cross_pin_prepare_entry(target.cross_mark_transform, entry))
                        g_cross_pin.mark = entry;
                    else
                        g_cross_pin.mark.valid = false;
                }
            } else if (target.cross_marker_transform &&
                       target.cross_marker_transform != g_cross_pin.marker.native_transform) {
                CrossPinEntry entry;
                if (cross_pin_prepare_entry(target.cross_marker_transform, entry))
                    g_cross_pin.marker = entry;
                else
                    g_cross_pin.marker.valid = false;
            }
            return g_cross_pin.tree ? (g_cross_pin.node_trs_valid || g_cross_pin.mark.valid)
                                    : g_cross_pin.marker.valid;
        }
    }
    CrossPin pin;
    pin.pid = g_pid;
    pin.node_id = target.id;
    pin.tree = (target.kind == 0);
    pin.ext = memio::untag((uint64_t)target.cross_ext);
    pin.primary = memio::untag((uint64_t)target.ray_wrapper_primary);
    pin.fallback_wrapper = memio::untag((uint64_t)target.ray_wrapper_fallback);
    pin.pivot = {target.node_x, target.node_y, target.node_z};
    pin.fallback = {target.aim_x, target.aim_y, target.aim_z};
    if (pin.tree) {
        if (!valid_obj(pin.ext) || !target.cross_node_transform) return false;
        CrossPinHierarchy h;
        if (!cross_pin_resolve_arrays(target.cross_node_transform, h)) return false;
        if (!cross_pin_read_trs(h, true, pin.node_trs)) return false;
        pin.node_trs_valid = true;
        if (target.cross_mark_transform)
            cross_pin_prepare_entry(target.cross_mark_transform, pin.mark);   // косметика
    } else {
        if (!target.cross_marker_transform) return false;
        if (!cross_pin_prepare_entry(target.cross_marker_transform, pin.marker)) return false;
    }
    std::call_once(g_cross_pin_once, [] {
        std::thread(cross_pin_worker).detach();
    });
    {
        std::lock_guard<std::mutex> lock(g_cross_pin_mutex);
        g_cross_pin = pin;
    }
    g_cross_pin_fail.store(false, std::memory_order_release);
    g_cross_pin_active.store(true, std::memory_order_release);
    return true;
}

bool esp_farm_cross_healthy() {
    return !g_cross_pin_fail.load(std::memory_order_acquire);
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

bool esp_init(pid_t pid, bool package_verified_release) {
    std::lock_guard<std::recursive_mutex> viewmodel_guard(viewmodel_session_mutex());
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
    // any_bound различает две разные неудачи: ни один кандидат даже не
    // привязался (NO_ACCESS — в кернеле отказ драйвера, в экстернале /proc)
    // и привязались, но класс игры не резолвится (BAD_IMAGE — не тот образ).
    bool any_bound = false;
    uint64_t chosen = 0;
    for (int i = 0; i < candidate_count; ++i) {
        if (!g_mem.bind(pid, candidates[i])) continue;
        any_bound = true;
        if (base_resolves_game(candidates[i])) { chosen = candidates[i]; break; }
        g_mem.unbind();
    }
    if (!chosen && package_verified_release) {
        // old_source attached to the first readable libil2cpp mapping while
        // TypeInfo statics were still null during loading, then resolved the
        // game data once Unity initialized them. Preserve that working path
        // only for the exact official release package. Renamed/cloned Unity
        // candidates still have to pass base_resolves_game(), so this cannot
        // silently reintroduce a build selector or alternate-client support.
        for (int i = 0; i < candidate_count; ++i) {
            if (g_mem.bind(pid, candidates[i])) {
                any_bound = true;
                chosen = candidates[i];
                break;
            }
        }
    }
    if (!chosen) {
        g_attach_state = any_bound ? ESP_ATTACH_BAD_IMAGE : ESP_ATTACH_NO_ACCESS;
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

// Счётчики чтений памяти для диагностики штормов (см. кадргист в main.cpp):
// отличают «читаем больше» (аим раздул объём) от «читаем так же, но каждое
// чтение ждёт» (ядро/память устройства под давлением).
unsigned long long esp_mem_reads_total() { return g_mem.reads_total(); }
unsigned long long esp_mem_read_fails_total() { return g_mem.read_fails_total(); }

// KCC.Move.Position: the simulated character position (capsule bottom) the
// game itself moves the character with. Independent of the discovered
// PlayerManager position field, and validated by the KCC back-reference.
static bool player_kcc_position(const PlayerAux& aux, Vec3& out, bool gate_origin = true) {
    if (!aux.kcc) return false;
    Vec3 p = rd_v3(aux.kcc + KCC_MOVE + 0x0C);
    if (!vec3_is_finite(p)) return false;
    float magnitude = fabsf(p.x) + fabsf(p.y) + fabsf(p.z);
    if (magnitude < 0.01F || magnitude > 100000.0F) return false;
    // Тот же пуловой мусор, что и в поле позиции: свежий CharacterController
    // отдаёт (x, 0, 0), и бокс молча уезжает под карту. Гейт — только на
    // первой точке трека (см. have_confirmed в esp_get_boxes): у карты, где
    // игрок реально стоит в нуле, подтверждённая точка нулём не перестаёт.
    if (gate_origin && position_near_world_origin(p)) return false;
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
static int      g_frame_player_count = 0;
static int      g_frame_bot_count = 0;
static int      g_frame_transforms_empty_streak = 0;
// Frames in a row esp_get_boxes() gave up before publishing this frame's
// camera / local position (see the watchdog at the top of it).
static int      g_frame_publish_fail_streak = 0;
// True when the current esp_get_boxes result ended on an unreadable/transient
// path. The renderer may briefly retain the previous boxes instead of flashing
// an empty frame. A readable empty player registry is not transient.
static bool     g_box_frame_transient = true;
bool esp_box_frame_transient() { return g_box_frame_transient; }
// Independent from camera/marker publication: successful ore frames must not
// conceal a dead player list or position pipeline.
static PlayerEspWatchdog g_player_esp_watchdog;
// Кадры подряд, в которых позиции игроков не прочитались НИ У КОГО. Серия
// отличает transient read failure от настоящей перезагрузки мира; одиночный
// кадр не инвалидирует подтверждённый fixed release path.
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
// Кандидат на смену мира: последний не пересёкшийся со снапшотом состав и
// сколько кадров подряд он идёт идентичным (по членству). Рваные кадры тоже
// идут сериями по 3+ в глубоких штормах, но каждый кадр с РАЗНЫМ мусором —
// считать «3 любых» сменой мира нельзя (лог 09-20: +0-9 прорывались).
static std::vector<uint64_t> s_adopt_cand;
static int s_adopt_ident_streak = 0;
// Everything derived from concrete objects in a world/session. Called when the
// whole player population is replaced.  Structural choices (position field,
// matrix convention and Transform hierarchy offsets) belong to the release
// build, not to one PlayerManager lifetime, so an ordinary respawn keeps them.
static void reset_world_caches() {
    g_matrix_configuration_validated = false; g_camera_matrix_physical_match = false;
    g_player_position_validated = false;
    g_population_snapshot.clear(); g_world_change_streak = 0;
    s_adopt_cand.clear(); s_adopt_ident_streak = 0;
    // Раскладка, выученная по костям, умирает вместе со старым миром: после
    // перезагрузки она читает мусор из переиспользованной памяти (конечные
    // числа, но не оттуда). Переучивается с первого же скелета рядом, а метки
    // до тех пор живут на самопроверяющемся пути.
    g_skeleton_layout = {}; g_skeleton_layout_valid = false;
    g_use_direct_player_position = true; g_player_position_offset = PLAYER_POSITION;
    g_direct_position_fail_streak = 0; g_direct_position_recheck = 0;
    // Keep g_local_player until the normal resolver confirms its replacement.
    // If the old object is absent from the new population, the camera-local
    // fallback below is used instead of misidentifying a nearby enemy.
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
    // Камерный страж нового мира ничему не следит: прошлая планета тётушка
    // задержала бы новую камеру на 3 кадра подтверждения «телепорта».
    s_fv_guard = {};
}

void esp_reset() {
    std::lock_guard<std::recursive_mutex> viewmodel_guard(viewmodel_session_mutex());
    silent_aim::clear();
    esp_viewmodel_reset(false); // process lost: never restore to a reused address
    g_mem.unbind();
    g_attach_state = ESP_ATTACH_OK;
    g_pid = -1; g_il2cpp_base = 0;
    g_native_camera = 0;
    g_game_controller_static_fields_cached = 0;
    g_camera_manager_cached = 0;
    g_xray_cam = 0; g_xray_saved_valid = false; // процесс ушёл — восстанавливать нечего
    g_day_tod = 0; g_day_retry = 0; g_day_cycle_addr.store(0);
    g_frame_transforms.clear(); g_frame_transforms_empty_streak = 0;
    g_frame_publish_fail_streak = 0;
    g_player_esp_watchdog.reset();
    g_local_position_fail_streak = 0; g_world_change_streak = 0;
    g_population_snapshot.clear();
    g_offset_extent_fail_streak = 0;
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
    // Смещение имени и слот модели переучиваются заново: они найдены по
    // живой памяти прошлого процесса и к новому не относятся.
    g_character_model_offset = 0; g_character_model_probed = false;
    g_character_model_retry_at = 0.0;
    g_character_model_probe_fail = 0;
    // Сиды были набраны в прошлом процессе: их адреса в новом ничего не значат.
    g_skeleton_layout_seeds.clear();
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

static uint64_t resolve_native_camera_pointer(bool allow_last_confirmed) {
    // Класс GameControllerBase разрешается ЗДЕСЬ, а не только по дороге чтения
    // реестра игроков.
    //
    // До этого g_game_controller_class выставлялся исключительно внутри
    // read_configured_player_transforms, и при неразобранном списке игроков
    // (resolve_player_lists вернул 0) до резолвера локального игрока дело не
    // доходило вовсе. Камера в этом состоянии не находилась никогда: статики
    // читать неоткуда, g_camera_manager_cached пуст, запасной g_native_camera
    // тоже пуст на первом кадре — read_camera_frame падал, esp_get_boxes
    // выходил по transient_return(5) на каждом кадре. Снаружи это выглядело
    // ровно как «у некоторых юзеров еспы вообще не работают»: боксы не
    // появлялись ни разу за сессию, а маркеры жили на camera-only кадре.
    if (!g_game_controller_class && GAME_CONTROLLER_TYPEINFO_RVA != 0) {
        const uint64_t candidate = rd_ptr(g_il2cpp_base + GAME_CONTROLLER_TYPEINFO_RVA);
        if (class_identity(candidate, "GameControllerBase", "Oxide") != 0)
            g_game_controller_class = candidate;
    }
    if (!g_game_controller_static_fields_cached && g_game_controller_class)
        g_game_controller_static_fields_cached = get_class_static_fields(g_game_controller_class);

    if (g_game_controller_static_fields_cached) {
        const uint64_t manager = rd_ptr(g_game_controller_static_fields_cached +
                                        GAME_CONTROLLER_CAMERA_MANAGER_FIELD);
        if (manager) g_camera_manager_cached = manager;
    }

    if (g_camera_manager_cached) {
        const uint64_t managed = rd_ptr(g_camera_manager_cached + CAMERA_MANAGER_CAMERA_FIELD);
        if (managed) {
            const uint64_t native = rd_ptr(managed + MANAGED_CACHED_PTR);
            if (native) return native;
        }
    }
    return allow_last_confirmed ? g_native_camera : 0;
}

// Растяг «Aspect Ratio» (меню «Визуалы → Ещё настройки»): значение, которое
// кадр обязан положить в камеру игры, чтобы её картинка и наши визуалы были
// посчитаны по одному aspect. 0 — не трогать камеру, своё значение она даст
// сама. Слайдер вернулся в 1.00 — один раз восстанавливаем ровный sw/sh.
static float stretched_camera_aspect(float sw, float sh) {
    if (!(sh > 1.0F)) return 0.0F;
    // Восстановление ровного aspect делается РОВНО один раз (флаг, а не
    // сравнение с прошлым значением): функция зовётся и из конвейера боксов, и
    // из camera-only кадра, и по одному только «прошлому» значению второй вызов
    // в том же кадре счёл бы, что восстанавливать больше нечего.
    static bool s_restore_pending = false;
    const float stretch = g_screen_stretch_x;
    const float base = sw / sh;
    if (stretch > 1.005F) {
        s_restore_pending = true;
        return base / stretch;
    }
    if (s_restore_pending) {
        s_restore_pending = false;
        return base;
    }
    return 0.0F;
}

static bool read_camera_frame(float aspect, uint64_t& native, Mat4& projection, Mat4& view,
                              float force_aspect = 0.0F) {
    native = resolve_native_camera_pointer(true);
    if (native && read_native_camera_matrices(native, aspect, projection, view, force_aspect)) {
        xray_apply(native);
        always_day_tick();
        return true;
    }

    // A stale anchor after a camera/world replacement must not pin recovery to
    // the old object. Re-resolve once from the class, but retain the last native
    // pointer across ordinary EIO bursts.
    g_game_controller_static_fields_cached = 0;
    g_camera_manager_cached = 0;
    const uint64_t refreshed = resolve_native_camera_pointer(false);
    if (refreshed && refreshed != native &&
        read_native_camera_matrices(refreshed, aspect, projection, view, force_aspect)) {
        native = refreshed;
        xray_apply(native);
        always_day_tick();
        return true;
    }
    return false;
}

bool publish_camera_only_frame(float sw, float sh) {
    if (g_pid <= 0 || !g_il2cpp_base) return false;
    // Resolves g_game_controller_class as a side effect — without it the
    // camera lookup below has no class to read statics from.
    resolve_local_player();
    if (!g_game_controller_class) return false;
    if (!(sw >= 100.0F) || !(sh >= 100.0F)) { sw = 1080.0F; sh = 2400.0F; }
    Mat4 solo_proj{}, solo_view{};
    uint64_t cam_native = 0;
    // Растяг экрана действует и на camera-only кадре: иначе маркеры на кадре без
    // игроков живут в другом aspect, чем картинка игры.
    const float stretched = stretched_camera_aspect(sw, sh);
    if (!read_camera_frame(sw / sh, cam_native, solo_proj, solo_view, stretched)) return false;
    if (stretched > 0.0F && cam_native)
        wr_buf(cam_native + CAMERA_ASPECT, &stretched, sizeof(stretched));
    Vec3 cam_pos{};
    if (!camera_position_from_view(solo_view, cam_pos)) return false;
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

static void reset_player_pipeline_resolvers(bool deep) {
    // Refresh only the resolver anchors. The last confirmed population and its
    // position tracks are the fallback that prevents read storms from becoming
    // visible flicker; clearing them every 1.5 seconds was self-sabotage.
    g_player_manager_static_fields = 0;
    g_local_position_fail_streak = 0;
    if (deep) {
        // Keep the established position path. Re-discovering it during an EIO
        // burst removed every box even though each track still had a position.
        g_direct_position_fail_streak = 0;
        g_direct_position_recheck = 0;
    }
}

static void note_player_pipeline_health(bool player_data_ok) {
    const PlayerEspWatchdogAction action =
        g_player_esp_watchdog.observe(player_data_ok, mono_seconds());
    if (action.reset_resolvers) {
        reset_player_pipeline_resolvers(action.deep_recovery);
    }
    if (action.deep_recovery) {
        // A live camera/local-memory pipeline proves that the process binding is
        // still valid. Reattaching here used to drop every memory feature and
        // the FOV circle in a loop. Keep retrying player-only resolvers instead.
    }
}

// Боты в списке боксов: только данные для аима (is_bot), оверлей их не рисует.
// Углы считает тот же set_aim_point, что у игроков. Точка прицела — кость
// головы модели (NPCBrainHumanoid.modelInfo.head), когда её удалось прочитать;
// иначе — оценка от корня маркера: начало координат префаба бота лежит в ногах
// (конвенция Unity для персонажей), и если тест покажет, что аим уходит
// выше/ниже, двигать kBot*H.
static constexpr float kBotHeadH = 1.60F, kBotNeckH = 1.42F, kBotChestH = 1.18F;
// Запасные слоты, когда голова прочитана по кости: шея и грудь вниз от неё.
static constexpr float kBotNeckDrop = 0.17F, kBotChestDrop = 0.45F;
// Потолок добавки за возраст отсчёта позиции бота (секунды, см. append_bot_boxes).
static constexpr float kBotPosLeadMax = 0.25F;
static void append_bot_boxes(std::vector<EspBox>& out) {
    if (!g_aim_bones_requested) return; // без аима боксы ботов не нужны никому
    if (!g_frame_vp_valid || !g_frame_local_valid) return;
    esp_bot_aim_maintain(); // скан по расписанию + свежие позиции
    for (const BotAimEnt& src : esp_bot_aim_list()) {
        Vec3 feet{}; feet.x = src.x; feet.y = src.y; feet.z = src.z;
        EspBox box{};
        box.id = src.id;
        box.is_bot = true;
        float dx = feet.x - g_frame_local_pos.x;
        float dy = feet.y - g_frame_local_pos.y;
        float dz = feet.z - g_frame_local_pos.z;
        box.distance = sqrtf(dx * dx + dy * dy + dz * dz);
        // Упреждение — по скорости САМОГО бота. Раньше здесь оставался
        // g_aim_lead последнего обработанного игрока кадра (в мемори-режиме не
        // меньше 50 мс), и бегущий игрок уводил прицел на бота в свою сторону.
        // К задержке контура прибавляем возраст отсчёта позиции: далёкие боты
        // перечитываются раз в 6/15 кадров, и видимый бот за это время уходит
        // вперёд от той точки, которую мы ещё держим в кэше.
        if (src.vel_valid) {
            g_aim_velocity = {src.vx, src.vy, src.vz};
            float lead = g_aim_lead_seconds;
            if (src.pos_age > 0.0F)
                lead += (src.pos_age < kBotPosLeadMax) ? src.pos_age : kBotPosLeadMax;
            g_aim_lead.x = src.vx * lead;
            g_aim_lead.y = src.vy * lead;
            g_aim_lead.z = src.vz * lead;
        } else {
            g_aim_velocity = {};
            g_aim_lead = {};
        }
        // Keep bot slots aligned with player slots: [0] head, [1] neck, [2] chest.
        const float bot_sample_age = src.vel_valid && src.pos_age > 0.f
            ? std::min(src.pos_age, kBotPosLeadMax) : 0.f;
        Vec3 head, neck, chest;
        if (src.head_valid) {
            // Точная кость головы модели: без оценки по росту от ног.
            head = {src.hx, src.hy, src.hz};
            neck = head;  neck.y -= kBotNeckDrop;
            chest = head; chest.y -= kBotChestDrop;
        } else {
            head = feet;  head.y += kBotHeadH;
            neck = feet;  neck.y += kBotNeckH;
            chest = feet; chest.y += kBotChestH;
        }
        set_aim_point(box, 0, head, g_frame_vp, g_frame_sw, g_frame_sh);
        set_aim_point(box, 1, neck, g_frame_vp, g_frame_sw, g_frame_sh);
        set_aim_point(box, 2, chest, g_frame_vp, g_frame_sw, g_frame_sh);
        for (float& age : box.aim_sample_age) age = bot_sample_age;
        if (!box.aim_valid[0] && !box.aim_valid[1] && !box.aim_valid[2]) continue;
        box.aim_source = src.head_valid ? 1 : 3; // 1 — кость, 3 — оценка от ног
        // Габариты для приличия: ноги и макушка в экранных координатах.
        Vec3 top = feet; top.y += 1.8F;
        Vec2 s_feet{}, s_top{};
        if (!w2s(g_frame_vp, feet, g_frame_sw, g_frame_sh, s_feet, false)) continue;
        if (!w2s(g_frame_vp, top, g_frame_sw, g_frame_sh, s_top, false)) continue;
        float h = fabsf(s_feet.y - s_top.y);
        float w = h * 0.45F;
        float cx = (s_feet.x + s_top.x) * 0.5F;
        box.x1 = cx - w * 0.5F; box.x2 = cx + w * 0.5F;
        box.y1 = s_feet.y < s_top.y ? s_feet.y : s_top.y;
        box.y2 = s_feet.y < s_top.y ? s_top.y : s_feet.y;
        out.push_back(box);
        ++g_frame_bot_count;
    }
    // Per-target velocity must not leak into the next frame's aim point.
    g_aim_lead = {};
    g_aim_velocity = {};
}


// ---- Наблюдатель мерцаний визуалов (только фиксирует, ничего не меняет) ----
// Бокс, нарисованный в прошлом кадре и пропущенный в этом (или наоборот),
// записывается одной строкой: адрес, причина пропуска, дельта отказов чтения
// за кадр, смена локального. Так видно, ГДЕ именно пропадает бокс: фильтр
// позиции, дедупликация, проецирование, смена локального или рваное чтение.
// Бюджет — 4 строки в секунду, остальное считается молча.
static std::unordered_map<uint64_t, int> s_watch_gap;   // addr -> кадров с прошлого бокса
static std::vector<uint64_t> s_watch_pop;
static uint64_t s_watch_local_addr = 0;
static unsigned long long s_watch_fails = 0;
static double s_watch_sec = -1e9;
static int s_watch_budget = 0;
static int s_watch_hidden = 0;

struct BoxWatchPose {
    float cx = 0.0F, cy = 0.0F, distance = -1.0F;
    unsigned long long frame = 0;
};
static std::unordered_map<uint64_t, BoxWatchPose> s_watch_box_pose;
static unsigned long long s_watch_frame = 0;
static double s_watch_summary_at = -1e9;
static int s_watch_summary_frames = 0;
static int s_watch_summary_empty = 0;
static int s_watch_summary_min = 1000000;
static int s_watch_summary_max = 0;
static int s_watch_summary_returns = 0;
static int s_watch_summary_losses = 0;
static int s_watch_summary_jumps = 0;
static int s_watch_summary_local_jumps = 0;
static int s_watch_summary_camera_jumps = 0;
static int s_watch_summary_camera_mismatch = 0;
static int s_watch_summary_early[16]{};
static unsigned long long s_watch_summary_fails = 0;
static Vec3 s_watch_prev_local{};
static bool s_watch_prev_local_valid = false;
static double s_watch_prev_local_at = 0.0;

static bool watch_allow() {
    const double now = memio::now_seconds();
    if (now - s_watch_sec >= 1.0) {
        s_watch_sec = now;
        s_watch_budget = 6;
        if (s_watch_hidden > 0) {
            s_watch_hidden = 0;
            --s_watch_budget;
        }
    }
    if (s_watch_budget <= 0) { ++s_watch_hidden; return false; }
    --s_watch_budget;
    return true;
}

static void watch_camera_and_local(const Mat4& view, const Vec3& local) {
    static Vec3 previous_camera{};
    static bool have_previous_camera = false;
    static double previous_at = 0.0;
    Vec3 camera{};
    if (!camera_position_from_view(view, camera) || !vec3_is_finite(camera)) return;
    const double now = memio::now_seconds();
    if (have_previous_camera) {
        const float dx = camera.x - previous_camera.x;
        const float dy = camera.y - previous_camera.y;
        const float dz = camera.z - previous_camera.z;
        const float jump = sqrtf(dx * dx + dy * dy + dz * dz);
        const double dt = now - previous_at;
        if (dt >= 0.0 && dt < 0.5 && std::isfinite(jump) && jump > 8.0F) {
            ++s_watch_summary_camera_jumps;
            if (watch_allow())
                ;
        }
    }
    const float ldx = local.x - camera.x;
    const float ldy = local.y - camera.y;
    const float ldz = local.z - camera.z;
    const float mismatch = sqrtf(ldx * ldx + ldy * ldy + ldz * ldz);
    if (std::isfinite(mismatch) && mismatch > 25.0F) {
        ++s_watch_summary_camera_mismatch;
        if (watch_allow())
            ;
    }
    previous_camera = camera;
    previous_at = now;
    have_previous_camera = true;
}

enum PositionReadFlags : unsigned short {
    PositionDirectTried = 1U << 0,
    PositionDirectReadOk = 1U << 1,
    PositionDirectValid = 1U << 2,
    PositionKccTried = 1U << 3,
    PositionKccOk = 1U << 4,
    PositionModelTried = 1U << 5,
    PositionModelReadOk = 1U << 6,
    PositionModelValid = 1U << 7,
    PositionCacheUsed = 1U << 8
};

struct EspSkipLog {
    uint64_t player = 0;
    uint64_t duplicate_of = 0;
    size_t index = 0;
    size_t population = 0;
    char reason = 0;
    char position_source = 0;
    unsigned short position_read_flags = 0;
    bool has_position = false;
    bool camera_basis_valid = false;
    bool camera_rotation_valid = false;
    bool camera_from_transform = false;
    bool has_bottom_world = false;
    bool has_top_world = false;
    bool has_box_bounds = false;
    Vec3 position{};
    Vec3 camera_position{};
    Vec3 camera_right{};
    Vec3 camera_up{};
    Vec3 camera_forward{};
    Vec4 camera_rotation{};
    Vec3 bottom_world{};
    Vec3 top_world{};
    float distance = -1.0F;
    float screen_width = 0.0F;
    float screen_height = 0.0F;
    float body_height = 0.0F;
    float box_cx = 0.0F;
    float box_cy = 0.0F;
    float box_half_width = 0.0F;
    float box_half_height = 0.0F;
    float screen_margin = 0.0F;
    unsigned long long read_fails_total = 0;
    // Биты 1/2 — нижняя/верхняя точка тела была подтянута к ближней плоскости
    // (см. body_clip.h). Без этого в логе не отличить «обе точки за плоскостью»
    // от «одну обрезали, бокс нарисован по обрезанной».
    int body_clipped = 0;
    W2SProjectionTrace bottom_projection{};
    W2SProjectionTrace top_projection{};
};

static void format_debug_vec3(const Vec3* value, char* out, size_t out_size) {
    if (!value) {
        std::snprintf(out, out_size, "n/a");
        return;
    }
    std::snprintf(out, out_size, "(%.1f %.1f %.1f)",
                  (double)value->x, (double)value->y, (double)value->z);
}

static void format_position_read_flags(unsigned short flags, char* out, size_t out_size) {
    const char* direct = !(flags & PositionDirectTried) ? "not-tried" :
        !(flags & PositionDirectReadOk) ? "read-fail" :
        (flags & PositionDirectValid) ? "valid" : "invalid-world";
    const char* kcc = !(flags & PositionKccTried) ? "not-tried" :
        (flags & PositionKccOk) ? "ok" : "read-fail";
    const char* model = !(flags & PositionModelTried) ? "not-tried" :
        !(flags & PositionModelReadOk) ? "read-fail" :
        (flags & PositionModelValid) ? "valid" : "invalid-world";
    std::snprintf(out, out_size, "direct=%s,kcc=%s,model=%s,cache=%s",
                  direct, kcc, model, (flags & PositionCacheUsed) ? "used" : "none");
}

static void format_projection_trace(const W2SProjectionTrace& trace, char* out, size_t out_size) {
    if (trace.state == W2SProjectionState::NotRun) {
        std::snprintf(out, out_size, "not-run");
        return;
    }
    if (trace.camera_space) {
        std::snprintf(out, out_size, "%s:camera=(%.3f %.3f %.3f) screen=(%.1f %.1f)",
                      w2s_projection_state_name(trace.state),
                      (double)trace.x, (double)trace.y, (double)trace.z,
                      (double)trace.screen_x, (double)trace.screen_y);
    } else {
        std::snprintf(out, out_size, "%s:clip=(%.3f %.3f %.3f %.3f) clip_w=%.3f screen=(%.1f %.1f)",
                      w2s_projection_state_name(trace.state),
                      (double)trace.x, (double)trace.y, (double)trace.z, (double)trace.w,
                      (double)trace.w, (double)trace.screen_x, (double)trace.screen_y);
    }
}


// tag: 0 = главный путь (боксы строились), иначе номер раннего выхода.
static void watch_boxes(const std::vector<EspBox>& result, size_t local_index,
                        const std::vector<char>* skip, int tag) {
    ++s_watch_frame;
    const double watch_now = memio::now_seconds();
    const unsigned long long fails = g_mem.read_fails_total();
    const unsigned long long fails_delta = fails - s_watch_fails;
    s_watch_fails = fails;
    s_watch_summary_fails += fails_delta;
    ++s_watch_summary_frames;
    if (tag > 0 && tag < 16) ++s_watch_summary_early[tag];

    int drawn_players = 0;
    for (const EspBox& box : result) if (!box.is_bot) ++drawn_players;
    if (drawn_players == 0) ++s_watch_summary_empty;
    s_watch_summary_min = std::min(s_watch_summary_min, drawn_players);
    s_watch_summary_max = std::max(s_watch_summary_max, drawn_players);

    if (g_frame_local_valid) {
        if (s_watch_prev_local_valid) {
            const float dx = g_frame_local_pos.x - s_watch_prev_local.x;
            const float dy = g_frame_local_pos.y - s_watch_prev_local.y;
            const float dz = g_frame_local_pos.z - s_watch_prev_local.z;
            const float jump = sqrtf(dx * dx + dy * dy + dz * dz);
            const double dt = watch_now - s_watch_prev_local_at;
            if (dt >= 0.0 && dt < 0.5 && std::isfinite(jump) && jump > 8.0F) {
                ++s_watch_summary_local_jumps;
                if (watch_allow())
                    ;
            }
        }
        s_watch_prev_local = g_frame_local_pos;
        s_watch_prev_local_valid = true;
        s_watch_prev_local_at = watch_now;
    }

    // Популяция: кто добавился/ушёл.
    {
        int added = 0, gone = 0;
        uint64_t added_first = 0, gone_first = 0;
        for (uint64_t p : g_frame_transforms) {
            bool had = false;
            for (uint64_t q : s_watch_pop) if (q == p) { had = true; break; }
            if (!had) { ++added; if (!added_first) added_first = p; }
        }
        for (uint64_t q : s_watch_pop) {
            bool has = false;
            for (uint64_t p : g_frame_transforms) if (p == q) { has = true; break; }
            if (!has) { ++gone; if (!gone_first) gone_first = q; }
        }
        s_watch_pop = g_frame_transforms;
        if ((added || gone) && watch_allow()) {
            auto uid_of = [](uint64_t addr) -> const char* {
                auto it = g_player_track.find(addr);
                return (it != g_player_track.end() && it->second.uid[0]) ? it->second.uid : "—";
            };
        }
    }

    // Локальный: смена адреса.
    bool local_flipped = false;
    {
        uint64_t addr = 0;
        if (local_index < g_frame_transforms.size()) addr = g_frame_transforms[local_index];
        if (addr != s_watch_local_addr) {
            if (s_watch_local_addr && addr) {
                local_flipped = true;
                if (watch_allow())
                    ;
            }
            s_watch_local_addr = addr;
        }
    }

    // Нарисованные в этом кадре (боты — не игроки, их пропускаем).
    std::vector<uint64_t> drawn_now;
    drawn_now.reserve(result.size());
    for (const EspBox& box : result) {
        if (box.is_bot) continue;
        drawn_now.push_back(box.id);
        const float cx = (box.x1 + box.x2) * 0.5F;
        const float cy = (box.y1 + box.y2) * 0.5F;
        auto pose = s_watch_box_pose.find(box.id);
        if (pose != s_watch_box_pose.end() && pose->second.frame + 1 == s_watch_frame) {
            const float screen_dx = cx - pose->second.cx;
            const float screen_dy = cy - pose->second.cy;
            const float screen_jump = sqrtf(screen_dx * screen_dx + screen_dy * screen_dy);
            const float distance_jump = fabsf(box.distance - pose->second.distance);
            const float distance_limit = std::max(10.0F, fabsf(pose->second.distance) * 0.25F);
            const float screen_limit = std::max(180.0F, std::min(g_frame_sw, g_frame_sh) * 0.18F);
            if ((std::isfinite(distance_jump) && distance_jump > distance_limit) ||
                (std::isfinite(screen_jump) && screen_jump > screen_limit)) {
                ++s_watch_summary_jumps;
                if (watch_allow())
                    ;
            }
        }
        s_watch_box_pose[box.id] = {cx, cy, box.distance, s_watch_frame};
        auto it = s_watch_gap.find(box.id);
        if (it != s_watch_gap.end() && it->second > 0) {
            ++s_watch_summary_returns;
            if (watch_allow())
                ;
        }
    }

    // Пропущенные в этом кадре (только главный путь; gap 0 = был в прошлом).
    if (skip && tag == 0) {
        for (size_t i = 0; i < skip->size() && i < g_frame_transforms.size(); ++i) {
            const char reason = (*skip)[i];
            if (!reason) continue;
            const uint64_t player = g_frame_transforms[i];
            auto it = s_watch_gap.find(player);
            if (it == s_watch_gap.end() || it->second != 0) continue;
            ++s_watch_summary_losses;
            if (!watch_allow()) continue;
            // Коды причин (почему бокс пропущен): local = сам стал локальным,
            // null = нулевой адрес, dup = задавлен дубликатом того же uid,
            // filter = отбраковал фильтр позиции, dist = дальше 300м,
            // bottom/top = точка не спроецировалась, h2px = высота < 2px.
            const char* why = "?";
            switch (reason) {
                case 'L': why = "local"; break;
                case 'Z': why = "null"; break;
                case 'S': why = "dup"; break;
                case 'P': why = "filter"; break;
                case 'D': why = "dist"; break;
                case 'B': why = "bottom"; break;
                case 'T': why = "top"; break;
                case 'H': why = "h2px"; break;
                case 'O': why = "offscreen"; break;
                case 'X': why = "dead"; break;
            }
        }
    }

    // Ранний выход: боксов нет вообще.
    if (tag != 0) {
        bool had = false;
        for (const auto& kv : s_watch_gap) if (kv.second == 0) { had = true; break; }
        if (had && watch_allow())
            ;
    }

    // Состарить счётчики, отметить нарисованные, прибрать протухшие.
    for (auto& kv : s_watch_gap) ++kv.second;
    for (uint64_t id : drawn_now) s_watch_gap[id] = 0;
    for (auto it = s_watch_gap.begin(); it != s_watch_gap.end();) {
        if (it->second > 600) it = s_watch_gap.erase(it); else ++it;
    }
    for (auto it = s_watch_box_pose.begin(); it != s_watch_box_pose.end();) {
        if (it->second.frame + 600 < s_watch_frame) it = s_watch_box_pose.erase(it);
        else ++it;
    }

    if (watch_now - s_watch_summary_at >= 1.0) {
        char exits[160]{};
        size_t used = 0;
        for (int i = 1; i < 16; ++i) {
            if (!s_watch_summary_early[i] || used + 16 >= sizeof(exits)) continue;
            used += (size_t)snprintf(exits + used, sizeof(exits) - used,
                                     "%s%d:%d", used ? "," : "", i, s_watch_summary_early[i]);
        }
        s_watch_summary_at = watch_now;
        s_watch_summary_frames = s_watch_summary_empty = 0;
        s_watch_summary_min = 1000000; s_watch_summary_max = 0;
        s_watch_summary_returns = s_watch_summary_losses = 0;
        s_watch_summary_jumps = s_watch_summary_local_jumps = 0;
        s_watch_summary_camera_jumps = s_watch_summary_camera_mismatch = 0;
        s_watch_summary_fails = 0;
        memset(s_watch_summary_early, 0, sizeof(s_watch_summary_early));
    }
}

// ---- Здоровье прямого пути позиции -----------------------------------------
//
// Профиль сборки отдаёт смещение поля позиции (PLAYER_POSITION), и пока игра
// не обновляется, этого достаточно. После обновления поле уезжает — и прямой
// путь начинает отдавать конечный мусор, который не проходит гейт мировых
// координат. В логе 11:52-11:54 так было у КАЖДОГО игрока каждый кадр
// (pos_reads={direct=invalid-world,...}), и ESP всё это время ехал на
// kcc-фолбэке: снаружи это не видно вообще, поэтому без своей строки
// устаревший профиль не отличить от здорового конвейера.
//
// Цепочка discover/recheck (find_direct_player_position_offset и компания)
// написана была ровно для этого, но не вызывалась ниоткуда: профильное
// смещение держалось вечно. Здесь — детект, лог и попытка переключить
// смещение среди уже известных.
static int    s_direct_dead_players = 0;   // игроков с прямой попыткой чтения
static int    s_direct_dead_count   = 0;   // из них — без мировой позиции
static int    s_direct_dead_frames  = 0;
static double s_direct_dead_since   = 0.0;
static double s_direct_dead_log_at  = -1e9;

// Диапазон развёртки вокруг профильного смещения. После обновления игры поле
// позиции уезжает внутри того же класса, далеко не уходит — но из всех восьми
// известных смещений оно при этом выпадает.
static const uint64_t k_position_scan_first = 0x1A0;
static const uint64_t k_position_scan_last  = 0x300;
static const uint64_t k_position_scan_step  = 0x4;

// Сколько игроков отдают на этом смещении ту же точку, что и их kcc. kcc —
// эталон, проверенный всей работой фолбэка: прямое поле либо совпадает с ней,
// либо позицией не является. Прежняя эвристика (сколько игроков дали
// правдоподобную позицию плюс разброс) таким доказательством не была: в логе
// 19:16 она не подтвердила ни одного смещения из восьми за 3.5 минуты, и ESP
// всё это время ехал на kcc, хотя поле просто уехало.
static int count_position_offset_matches(const std::vector<uint64_t>& players,
                                         const std::vector<Vec3>& reference,
                                         const std::vector<char>& have_reference,
                                         uint64_t offset) {
    int matches = 0;
    for (size_t i = 0; i < players.size(); ++i) {
        if (!have_reference[i] || !players[i]) continue;
        Vec3 position = rd_v3(players[i] + offset);
        if (!vec3_is_finite(position)) continue;
        const float dx = position.x - reference[i].x;
        const float dy = position.y - reference[i].y;
        const float dz = position.z - reference[i].z;
        if (dx * dx + dy * dy + dz * dz < 9.0F) ++matches;   // < 3 м: тик синхронизации
    }
    return matches;
}

static void revalidate_dead_direct_position(const std::vector<uint64_t>& players) {
    // Эталон — kcc-позиция тех же игроков этого же кадра.
    std::vector<Vec3> reference(players.size());
    std::vector<char> have_reference(players.size(), 0);
    size_t reference_count = 0;
    for (size_t i = 0; i < players.size(); ++i) {
        if (!players[i]) continue;
        auto aux = g_player_aux.find(players[i]);
        if (aux == g_player_aux.end() || !aux->second.kcc) continue;
        Vec3 kcc{};
        if (player_kcc_position(aux->second, kcc)) {
            reference[i] = kcc;
            have_reference[i] = 1;
            ++reference_count;
        }
    }

    uint64_t best_offset = 0;
    int best_matches = 0;
    if (reference_count > 0) {
        for (uint64_t offset : k_known_position_offsets) {
            const int matches = count_position_offset_matches(players, reference, have_reference, offset);
            if (matches > best_matches) { best_matches = matches; best_offset = offset; }
        }
        const int need = reference_count > 1 ? 2 : 1;
        if (best_matches < need) {
            for (uint64_t offset = k_position_scan_first; offset <= k_position_scan_last;
                 offset += k_position_scan_step) {
                bool known = false;
                for (uint64_t k : k_known_position_offsets) {
                    if (k == offset) { known = true; break; }
                }
                if (known) continue;
                const int matches = count_position_offset_matches(players, reference, have_reference, offset);
                if (matches > best_matches) { best_matches = matches; best_offset = offset; }
            }
        }
        if (best_matches < (reference_count > 1 ? 2 : 1)) best_offset = 0;
    }

    if (!best_offset && reference_count == 0) {
        // Эталона нет вовсе (kcc не отдался ни у одного игрока) — старая
        // эвристика по разбросу, лучше чем ничего.
        double best_score = 0.0;
        for (uint64_t offset : k_known_position_offsets) {
            double score = 0.0;
            if (!evaluate_player_position_offset(players, offset, score)) continue;
            const double count = floor(score / 1000000.0);
            const double best_count = floor(best_score / 1000000.0);
            // Только разостное подтверждение: одному игроку правдоподобную
            // мировую позицию отдаёт и чужое поле, и на пустом сервере это
            // вырождалось бы в миграцию боксов по мусорному смещению.
            if (count >= 2.0 && count > best_count) { best_offset = offset; best_score = score; }
        }
    }
    if (!best_offset) {
        // Попытка повторяется каждые 5 c, а ситуация пока не меняется: в логе
        // 19:16 это дало 45 одинаковых строк. Одна в минуту — достаточно.
        static double s_fail_log_at = -1e9;
        const double now = mono_seconds();
        if (now - s_fail_log_at >= 60.0) {
            s_fail_log_at = now;
        }
        return;
    }
    g_use_direct_player_position = true;
    g_player_position_offset = best_offset;
    g_player_position_validated = true;
    g_matrix_configuration_validated = false;
    g_body_caches_dirty = true;   // кеши поз/скелетов держат мировые координаты
    g_direct_position_fail_streak = 0;
    g_direct_position_fail_since = 0.0;
}

static void note_direct_position_health(const std::vector<uint64_t>& players,
                                        const std::vector<char>& sources) {
    if (!(g_use_direct_player_position && g_player_position_offset != 0 &&
          s_direct_dead_players > 0 && s_direct_dead_count == s_direct_dead_players)) {
        s_direct_dead_frames = 0;
        s_direct_dead_since = 0.0;
        return;
    }
    if (s_direct_dead_since == 0.0) s_direct_dead_since = mono_seconds();
    ++s_direct_dead_frames;
    const double now = mono_seconds();
    const double dead_for = now - s_direct_dead_since;
    if (dead_for >= 5.0 && now - s_direct_dead_log_at >= 30.0) {
        s_direct_dead_log_at = now;
        size_t kcc_n = 0, model_n = 0, cache_n = 0;
        for (char s : sources) {
            if (s == 'K') ++kcc_n;
            else if (s == 'M') ++model_n;
            else if (s == 'C') ++cache_n;
        }
    }
    // Раз в ~5 c мертвого пути пробуем известные смещения: событие редкое,
    // а поиск читает позиции всех игроков на каждое смещение.
    if (s_direct_dead_frames >= 300) {
        s_direct_dead_frames = 0;
        revalidate_dead_direct_position(players);
    }
}


// Пол высоты проекции тела, px (см. отсев 'H' ниже). Отрисовка и так растит
// бокс до своего минимума (esp_draw: kMinBoxSide), но конвейер обязан не терять
// игрока раньше неё.
constexpr float kMinProjectedHeight = 3.0F;

// ---- Кадр ESP: состояние публикации, перепроецирование, тик игры ------------
//
// Удержанная запись (битый кадр, см. esp_box_hold.h) не может повторять прошлые
// ЭКРАННЫЕ координаты: они посчитаны прошлой матрицей вида и, как только камера
// поехала, разъезжаются с миром — ровно жалоба «визуалы плавают по экрану, а не
// держатся на объектах». Поэтому бокс несёт мировой якорь (см. EspBox) и такой
// кадр считает экран заново ТЕКУЩЕЙ матрицей. Матрицы нет — записи нет: пустой
// кадр хуже одного, зато он честный.

bool esp_frame_projection_valid() { return g_frame_vp_valid; }

bool esp_frame_camera_position(float out[3]) {
    if (!out) return false;
    if (g_frame_cam_basis_valid) {
        out[0] = g_frame_cam_pos.x; out[1] = g_frame_cam_pos.y; out[2] = g_frame_cam_pos.z;
        return true;
    }
    if (g_frame_local_valid) {
        out[0] = g_frame_local_pos.x; out[1] = g_frame_local_pos.y; out[2] = g_frame_local_pos.z;
        return true;
    }
    return false;
}

// Кромка кадра — тот же запас, что у отсева 'O' в конвейере и у маркеров.
static float frame_edge_margin(float sw, float sh) {
    const float long_side = sw > sh ? sw : sh;
    return long_side * 0.25F;
}

bool esp_box_reproject(EspBox& box) {
    if (!box.anchor_valid) return false;
    if (!g_frame_vp_valid) return false;
    const float sw = g_frame_sw, sh = g_frame_sh;
    if (!(sw > 1.0F) || !(sh > 1.0F)) return false;
    if (!std::isfinite(box.anchor_bottom_y) || !std::isfinite(box.anchor_top_y)) return false;

    // Ноги и верх тела — та же геометрия, которой пользовался здоровый кадр
    // (низ тела уже с поправкой выбранного пути позиции), и тот же разбор
    // передней плоскости: точку за ней конвейер подтягивает к отрезку, иначе
    // игрок в упор не спроецировался бы вовсе. Подтянутую точку прижимаем к
    // кромке — она улетает на тысячи пикселей (screen ~ 1/near).
    Vec3 bottom{box.anchor_feet[0], box.anchor_bottom_y, box.anchor_feet[2]};
    Vec3 top{box.anchor_feet[0], box.anchor_top_y, box.anchor_feet[2]};
    const bodyclip::Segment seg = bodyclip::clip_segment(
        bottom, top, bodyclip::plane_from_vp(g_frame_vp), bodyclip::kNearDepth);
    if (!seg.visible) return false;
    Vec2 sf{}, st{};
    if (!w2s(g_frame_vp, bottom, sw, sh, sf, false)) return false;
    if (!w2s(g_frame_vp, top, sw, sh, st, false)) return false;
    if (!std::isfinite(sf.x) || !std::isfinite(sf.y) ||
        !std::isfinite(st.x) || !std::isfinite(st.y)) return false;
    const float screen_margin = frame_edge_margin(sw, sh);
    if (seg.moved_bottom) sf = bodyclip::clamp_to_margin(sf, sw, sh, screen_margin);
    if (seg.moved_top) st = bodyclip::clamp_to_margin(st, sw, sh, screen_margin);

    float height = fabsf(st.y - sf.y);
    if (!std::isfinite(height)) return false;
    if (height < kMinProjectedHeight) height = kMinProjectedHeight;
    const float cx = (sf.x + st.x) * 0.5F;
    const float cy = (sf.y + st.y) * 0.5F;
    const float half_h = height * 0.5F;
    const float half_w = height * PLAYER_BOX_WIDTH_RATIO * 0.5F;
    const float edge = frame_edge_margin(sw, sh);
    if (cx + half_w < -edge || cx - half_w > sw + edge ||
        cy + half_h < -edge || cy - half_h > sh + edge) return false;
    box.x1 = cx - half_w; box.y1 = cy - half_h;
    box.x2 = cx + half_w; box.y2 = cy + half_h;

    // 3D-короб (chams) — из того же якоря: полуширина и полуглубина тела
    // постоянны, меняются только ноги и границы по высоте.
    constexpr float box_half_width = 0.35F, box_half_depth = 0.35F;
    const Vec3 world_corners[8] = {
        {box.anchor_feet[0] - box_half_width, box.anchor_bottom_y, box.anchor_feet[2] - box_half_depth},
        {box.anchor_feet[0] + box_half_width, box.anchor_bottom_y, box.anchor_feet[2] - box_half_depth},
        {box.anchor_feet[0] + box_half_width, box.anchor_bottom_y, box.anchor_feet[2] + box_half_depth},
        {box.anchor_feet[0] - box_half_width, box.anchor_bottom_y, box.anchor_feet[2] + box_half_depth},
        {box.anchor_feet[0] - box_half_width, box.anchor_top_y, box.anchor_feet[2] - box_half_depth},
        {box.anchor_feet[0] + box_half_width, box.anchor_top_y, box.anchor_feet[2] - box_half_depth},
        {box.anchor_feet[0] + box_half_width, box.anchor_top_y, box.anchor_feet[2] + box_half_depth},
        {box.anchor_feet[0] - box_half_width, box.anchor_top_y, box.anchor_feet[2] + box_half_depth}
    };
    for (int corner = 0; corner < 8; ++corner) {
        Vec2 sc{};
        const bool projected = w2s(g_frame_vp, world_corners[corner], sw, sh, sc, false);
        box.corner_visible[corner] = projected &&
                                     sc.x >= 0.0F && sc.x <= sw && sc.y >= 0.0F && sc.y <= sh;
        box.corners[corner][0] = projected ? sc.x : -1.0F;
        box.corners[corner][1] = projected ? sc.y : -1.0F;
    }

    // Кости скелета — мировые точки из якоря (там уже только свежие).
    for (int bone = 0; bone < ESP_BONE_COUNT; ++bone) {
        const bool anchored = box.has_skeleton && (box.anchor_bone_ok & (1u << bone)) != 0;
        Vec2 sb{};
        const bool ok = anchored && w2s(g_frame_vp, Vec3{box.anchor_bone[bone][0],
                                                          box.anchor_bone[bone][1],
                                                          box.anchor_bone[bone][2]},
                                        sw, sh, sb, false);
        box.bone_valid[bone] = ok;
        if (ok) { box.bones[bone][0] = sb.x; box.bones[bone][1] = sb.y; }
    }

    // Точки аима НЕ перепроецируем. При удержании для ESP они устарели:
    // оставляем якорь для рисунка, но не разрешаем выстрел по старой кости.
    for (int slot = 0; slot < 3; ++slot) {
        box.aim_valid[slot] = false;
        box.aim_world_valid[slot] = false;
    }
    // Они привязаны к кадру съёма: там было упреждение
    // по цели, баллистика и подъём на дальность (set_aim_point), и повторить их
    // из мирового якоря нечем. Свежая, но чужая точка в прицеле хуже одного
    // кадра без неё: аим и так держит цель своим окном захвата.
    float cam[3];
    if (esp_frame_camera_position(cam)) {
        const float dx = box.anchor_feet[0] - cam[0];
        const float dy = box.anchor_feet[1] - cam[1];
        const float dz = box.anchor_feet[2] - cam[2];
        const float d = sqrtf(dx * dx + dy * dy + dz * dz);
        if (std::isfinite(d)) box.distance = d;
    }
    return true;
}

bool esp_marker_reproject(EspMarker& marker) {
    if (!marker.world_valid) return false;
    if (!g_frame_vp_valid) return false;
    const float sw = g_frame_sw, sh = g_frame_sh;
    if (!(sw > 1.0F) || !(sh > 1.0F)) return false;
    const Vec3 world{marker.world[0], marker.world[1], marker.world[2]};
    Vec2 screen{};
    if (!w2s(g_frame_vp, world, sw, sh, screen, false)) return false;
    if (!std::isfinite(screen.x) || !std::isfinite(screen.y)) return false;
    const float edge = frame_edge_margin(sw, sh);
    if (screen.x < -edge || screen.x > sw + edge ||
        screen.y < -edge || screen.y > sh + edge) return false;
    marker.x = screen.x;
    marker.y = screen.y;
    // Дистанция — до ТЕКУЩЕЙ камеры: удержанная метка обязана показывать
    // расстояние до объекта сейчас, а не то, что было на битом кадре.
    float cam[3];
    if (esp_frame_camera_position(cam)) {
        const float dx = world.x - cam[0], dy = world.y - cam[1], dz = world.z - cam[2];
        const float d = sqrtf(dx * dx + dy * dy + dz * dz);
        if (std::isfinite(d)) marker.distance = d;
    }
    return true;
}

// ---- Тик кадра игры --------------------------------------------------------
// Move.Tick (KCC.Move + 0x34) — счётчик кадров движения локального игрока, его
// пишет сама игра в конструкторе Move(position, yRotation, deltaTime, tick).
// Ровно он и нужен: оверлей, который пересчитывает камеру и позиции своим
// таймером, идёт с игрой вразнобой (двое часов с разной частотой дают биения), и
// камера, снятая в начале нашего кадра, каждый раз попадает в другую точку её
// кадра. Позиции от этого «дышат» на панораме, хотя каждая по отдельности
// верна. Привязка к тику игры убирает именно биения, оставляя постоянный
// (незаметный) сдвиг.
static uint32_t s_game_tick_last = 0;
static bool     s_game_tick_valid = false;
static double   s_game_tick_at = 0.0;
static float    s_game_frame_hz = 0.0F;
static int      s_game_tick_steady = 0;

static bool read_local_move_clock(uint32_t& tick, float& delta) {
    tick = 0;
    delta = 0.0F;
    if (!g_local_player) return false;
    const PlayerAux& aux = player_aux(g_local_player);
    if (!aux.kcc) return false;
    uint32_t value = 0;
    if (!rd_exact(aux.kcc + KCC_MOVE + 0x34, value)) return false;
    tick = value;
    float dt = 0.0F;
    if (rd_exact(aux.kcc + KCC_MOVE + 0x30, dt) && std::isfinite(dt) &&
        dt > 0.0005F && dt < 0.5F) delta = dt;
    return true;
}

// Тик прочитан: обновляем оценку частоты кадров игры по переходам тика.
// Медиану не держим — темп меняется (троттлинг, 90/120 Гц), и сглаживание по
// переходам идёт за ним без ступенек.
static void game_tick_observe(uint32_t tick, double now) {
    if (!s_game_tick_valid) {
        s_game_tick_valid = true;
        s_game_tick_last = tick;
        s_game_tick_at = now;
        s_game_tick_steady = 0;
        return;
    }
    if (tick == s_game_tick_last) return;
    const double period = now - s_game_tick_at;
    if (period > 0.0005 && period < 0.5) {
        const float hz = (float)(1.0 / period);
        s_game_frame_hz = (s_game_frame_hz > 1.0F) ? (s_game_frame_hz * 0.75F + hz * 0.25F) : hz;
        if (++s_game_tick_steady > 240) s_game_tick_steady = 240;
    } else {
        s_game_tick_steady = 0;
    }
    s_game_tick_last = tick;
    s_game_tick_at = now;
}

bool esp_game_tick_now(uint32_t& tick) {
    float delta = 0.0F;
    if (!read_local_move_clock(tick, delta)) {
        s_game_tick_valid = false;
        s_game_tick_steady = 0;
        return false;
    }
    game_tick_observe(tick, mono_seconds());
    return true;
}

int esp_game_frame_tick() {
    uint32_t tick = 0;
    float delta = 0.0F;
    if (!read_local_move_clock(tick, delta)) {
        s_game_tick_valid = false;
        s_game_tick_steady = 0;
        return ESP_GAME_FRAME_UNKNOWN;
    }
    if (!s_game_tick_valid) {
        game_tick_observe(tick, mono_seconds());
        return ESP_GAME_FRAME_ADVANCED;
    }
    if (tick == s_game_tick_last) return ESP_GAME_FRAME_SAME;
    game_tick_observe(tick, mono_seconds());
    return ESP_GAME_FRAME_ADVANCED;
}

float esp_game_frame_hz() {
    // Пока переходов тика мало, оценке верить нельзя (пауза в меню, загрузка
    // карты, смерть локального игрока): вызывающий берёт свой запасной темп.
    if (s_game_tick_steady < 8) return 0.0F;
    if (!(s_game_frame_hz >= 24.0F && s_game_frame_hz <= 240.0F)) return 0.0F;
    return s_game_frame_hz;
}

std::vector<EspBox> esp_get_boxes(int overlay_width, int overlay_height) {
    g_frame_bot_count = 0;
    g_box_frame_transient = true;
    g_aim_lead = {};
    g_aim_velocity = {};
    g_aim_inherited_velocity = {};
    g_aim_ballistic_params = {};
    g_aim_is_instant_ray = false;
    std::vector<EspBox> result;
    auto transient_return = [&](int tag) {
        watch_boxes(result, (size_t)-1, nullptr, tag);
        return result;
    };

    // Watchdog: every frame that fails to publish a camera + local position
    // counts up, and a few seconds of that means some cached offset or class
    // pointer did not survive the last world reload. Rebuilding everything is
    // cheap and rare, and it is what keeps a death / respawn from killing the
    // whole ESP until the app is restarted.
    if (++g_frame_publish_fail_streak > 240) {
        g_frame_publish_fail_streak = 0;
        // A camera publication gap does not prove that the player population
        // changed. The old recovery cleared every player/position cache here,
        // turning a temporary camera EIO burst into a long full ESP outage.
        // Retry only camera/matrix state and keep the last confirmed players.
        g_matrix_configuration_validated = false;
        g_camera_matrix_physical_match = false;
        s_fv_guard = {};
        // ESP recovery must never tear down an otherwise healthy attachment:
        // viewmodel/FOV/weapon controllers share it. Process replacement and
        // sustained memory-read failure are handled independently in process.cpp.
    }

    // Markers reuse this frame's camera; invalidate it until it is rebuilt so
    // an early return here can never leave them projecting through a stale one.
    g_frame_vp_valid = false;
    g_frame_local_valid = false;
    g_frame_cam_basis_valid = false;
    g_frame_player_count = 0;

    if (g_pid <= 0 || !g_il2cpp_base) {
            return result;
    }

    // Фрикам — до всех ранних выходов: сводки и покадровая запись нужны и в
    // пустой местности (раньше тик стоял за выходом «нет игроков» и там не
    // бежал вовсе — остаточные мерцания было нечем смотреть), а сами
    // early-out'ы тику не помеха (у него свои проверки живости трансформа).
    freecam_tick();

    uint64_t native_cam = 0;
    Mat4 projection{}, view{}, vp{};
    float sw = overlay_width >= 100 ? (float)overlay_width : 1080.0F;
    float sh = overlay_height >= 100 ? (float)overlay_height : 2400.0F;
    if (!std::isfinite(sw) || sw < 100.0F || sw > 10000.0F) sw = 1080.0F;
    if (!std::isfinite(sh) || sh < 100.0F || sh > 10000.0F) sh = 2400.0F;
    g_last_overlay_sw = sw;
    g_last_overlay_sh = sh;

    std::vector<uint64_t>& s_transforms = g_frame_transforms;
    bool player_list_ok = false;
    std::vector<uint64_t> refreshed = read_configured_player_transforms(&player_list_ok);
    // One incomplete registry sample must not remove a player. This matters at
    // low FPS because one bad frame stays visible much longer. Two consecutive
    // stable snapshots must agree on the absence; an unreadable list keeps the
    // previous snapshot without advancing the confirmation.
    if (player_list_ok) {
        for (uint64_t player : refreshed) g_frame_transforms_lost.erase(player);

        for (uint64_t previous : s_transforms) {
            bool present = false;
            for (uint64_t player : refreshed) {
                if (player == previous) { present = true; break; }
            }
            if (present) continue;

            int& missing_snapshots = g_frame_transforms_lost[previous];
            if (++missing_snapshots < 2) {
                refreshed.push_back(previous);
                continue;
            }

            g_player_track.erase(previous);
            g_player_aux.erase(previous);
            g_player_text.erase(previous);
            g_mount_latch.erase(previous);
            g_skeletons.erase(previous);
            g_frame_transforms_lost.erase(previous);
        }
        s_transforms = std::move(refreshed);
        g_frame_transforms_empty_streak = s_transforms.empty()
            ? g_frame_transforms_empty_streak + 1 : 0;
    }
    if (s_transforms.empty()) {
        // Alone on the server: no player boxes, but markers and the farm still
        // need this frame's camera. A validated empty list is healthy; a failed
        // resolver is not, even though this camera-only frame can publish.
        note_player_pipeline_health(player_list_ok);
        publish_camera_only_frame(sw, sh);
        if (g_aim_bones_requested && g_state.aim_touch) {
            const uint64_t local_player = resolve_local_player();
            const PlayerAux* local_aux = nullptr;
            bool local_crouched = false;
            if (valid_obj(local_player)) {
                PlayerAux& aux = player_aux(local_player);
                local_aux = &aux;
                local_crouched = player_is_crouched(aux);
            }
            read_local_aim_reference(local_player, local_aux, local_crouched);
            update_aim_ballistics(local_player);
        }
        append_bot_boxes(result); // боты — и без игроков
        g_box_frame_transient = !player_list_ok;
        return transient_return(1);
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

    // The selected build profile already contains the PlayerManager position
    // field. Use it directly; KCC/model below cover objects whose synced field
    // has not been initialized yet.
    if (!g_player_position_validated) {
        g_player_position_offset = PLAYER_POSITION;
        g_use_direct_player_position = true;
        g_player_position_validated = true;
    }

    bool transform_camera_mode = false; // light fix: always use native cam matrices, avoid dead-body-as-camera on death
    if (!transform_camera_mode) {
        // The uploaded log showed multi-second outages almost exclusively at
        // the managed-camera chain (early=3). Read through stable cached
        // anchors and fall back to the last confirmed native camera when one
        // pointer read fails; the matrix read still validates that fallback.
        // Растяг экрана (Aspect Ratio, меню «Визуалы → Ещё настройки»):
        // горизонтальный масштаб проекции — это cot(fov/2) / aspect, поэтому
        // aspect ДЕЛИМ на коэффициент: горизонтальный FOV растёт, края
        // уходят за кадр, а модели врагов становятся шире/жирнее (круги-
        // прицелы овальные — норма для фичи). Через умножение эффект был
        // обратный — картинка сплющивалась (наблюдение с устройства 04.10).
        // Пишем КАЖДЫЙ кадр тем же g_mem, что и аим: движок может возвращать
        // aspect на место при смене вида. Базу берём из геометрии экрана
        // (sw/sh), а не из памяти — там уже может лежать наша же запись. При
        // возврате слайдера в 1.00 аспект один раз восстанавливается в ровный
        // sw/sh.
        //
        // Значение передаём в чтение кадра: проекция ЭТОГО кадра обязана быть
        // посчитана ровно тем aspect, который мы кладём в камеру. Раньше запись
        // шла уже после сборки проекции, и на каждом шаге слайдера картинка игры
        // и визуалы оказывались в разных aspect — вся сцена прыгала по X.
        const float stretched = stretched_camera_aspect(sw, sh);
        if (!read_camera_frame(sw / sh, native_cam, projection, view, stretched))
            return transient_return(5);
        // freecam_tick() — в начале кадра (см. выше): ему нельзя за ранние выходы.
        g_matrix_configuration_validated = true;
        g_camera_matrix_physical_match = true;
        if (stretched > 0.0F && native_cam)
            wr_buf(native_cam + CAMERA_ASPECT, &stretched, sizeof(stretched));
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
    std::vector<char> position_source(s_transforms.size(), 0);
    std::vector<unsigned short> position_read_flags(s_transforms.size(), 0);
    std::vector<char> skip_reason(s_transforms.size(), 0); // наблюдатель мерцаний: код пропуска
    size_t live_player_positions = 0;
    s_direct_dead_players = 0;
    s_direct_dead_count = 0;

    Vec3 camera_position{};
    if (!camera_position_from_view(view, camera_position)) {
        append_bot_boxes(result);
        return transient_return(8);
    }
    local = camera_position;
    has_local_position = true;
    Vec3 projection_camera_right{}, projection_camera_up{}, projection_camera_forward{};
    const bool projection_camera_basis_valid = camera_basis_from_view(
        view, projection_camera_right, projection_camera_up, projection_camera_forward);

    for (size_t index = 0; index < s_transforms.size(); ++index) {
        const uint64_t player = s_transforms[index];
        if (!player) continue;

        Vec3 candidate{};
        char source = 0;
        unsigned short read_flags = PositionDirectTried;
        const bool direct_read_ok = read_entity_position(player, candidate);
        if (direct_read_ok) read_flags |= PositionDirectReadOk;
        // Пуловой мусор свежего PlayerManager отдаёт в поле позиции вектор
        // (x, 0.0, 0.0): world-space-гейт его пропускает, и бокс уходит под
        // карту. В логе 16:05 — pos=(10.9 0.0 0.0), reason=bottom, «ПРОПАЛ»,
        // возврат через 155 кадров (2.6 c). Сентинель position_near_world_origin
        // написан ровно под это, но включается ТОЛЬКО пока у трека нет ни одной
        // подтверждённой позиции: у карты, где игрок реально стоит у нуля
        // (спавн/стройка в центре), подтверждённая точка нулём быть не
        // перестаёт, и гейт по ней стёр бы живого игрока с ESP навсегда.
        auto tracked = g_player_track.find(player);
        const bool have_confirmed = tracked != g_player_track.end() && tracked->second.has_last;
        const bool origin_gate = position_near_world_origin(candidate) && !have_confirmed;
        bool read_ok = direct_read_ok && position_looks_like_world_space(candidate) && !origin_gate;
        if (read_ok) {
            read_flags |= PositionDirectValid;
            source = 'D';
        }
        if (g_use_direct_player_position && g_player_position_offset != 0) {
            ++s_direct_dead_players;
            if (direct_read_ok && !read_ok) ++s_direct_dead_count;
        }
        if (!read_ok) {
            const PlayerAux& aux = player_aux(player);
            read_flags |= PositionKccTried;
            read_ok = player_kcc_position(aux, candidate, !have_confirmed);
            if (read_ok) {
                read_flags |= PositionKccOk;
                source = 'K';
                if (!g_use_direct_player_position) candidate.y += 1.60F;
            } else {
                read_flags |= PositionModelTried;
                const bool model_read_ok = player_model_position(player, candidate);
                if (model_read_ok) {
                    read_flags |= PositionModelReadOk;
                    candidate.y += g_use_direct_player_position ? -0.90F : 0.90F;
                    read_ok = position_looks_like_world_space(candidate) &&
                              !(position_near_world_origin(candidate) && !have_confirmed);
                    if (read_ok) {
                        read_flags |= PositionModelValid;
                        source = 'M';
                    }
                }
            }
        }

        if (!read_ok) {
            // A read failure never removes a visible player. Keep the last
            // accepted world position until the confirmed list retires him.
            if (tracked == g_player_track.end() || !tracked->second.has_last) {
                position_read_flags[index] = read_flags;
                continue;
            }
            candidate = tracked->second.last;
            read_flags |= PositionCacheUsed;
            source = 'C';
        } else {
            ++live_player_positions;
            PlayerTrack& track = track_player(player, candidate);
            tracked = g_player_track.find(player);

            // lastSavedPosition у удалённых игроков обновляется сетевыми тиками,
            // а оверлей рисуется 60 Гц. Между тиками продвигаем только обычный
            // direct-сэмпл по измеренной скорости. KCC/model и транспорт уже
            // дают живой Transform, поэтому повторная экстраполяция им навредит.
            //
            // Потолок упреждения для РИСУЕМОЙ точки — kDrawnPredictionMaxDistance
            // (полшага), а не метр с лишним: полный запас нужен прицелу, который
            // сам его учитывает, а бокс от него уезжает вперёд игрока, и на
            // панораме это читается как «визуал не держится на объекте».
            if (source == 'D' && player != g_local_player &&
                !player_is_mounted(player) && !player_mount_engaged(player) &&
                track.have_vel_ref) {
                candidate = playermotion::predict(candidate, track.vel,
                                                  track.vel_ref_t, mono_seconds(),
                                                  playermotion::kDrawnPredictionMaxDistance);
            }
        }

        position_read_flags[index] = read_flags;
        positions[index] = candidate;
        position_ok[index] = 1;
        position_source[index] = source;
        if (tracked != g_player_track.end()) {
            tracked->second.filt.drawn = candidate;
            tracked->second.filt.drawn_t = mono_seconds();
            tracked->second.filt.has_drawn = true;
        }
    }

    note_direct_position_health(s_transforms, position_source);

    // Local identity only excludes our own PlayerManager. Distance always uses
    // the camera from this exact projection frame, never a stale player field.
    for (size_t index = 0; index < s_transforms.size(); ++index) {
        if (s_transforms[index] == g_local_player) {
            local_entity_index = index;
            break;
        }
    }
    if (local_entity_index == s_transforms.size()) {
        for (size_t index = 0; index < s_transforms.size(); ++index) {
            if (player_is_local(s_transforms[index]) == 1) {
                local_entity_index = index;
                g_local_player = s_transforms[index];
                break;
            }
        }
    }
    g_local_position_fail_streak = 0;

    // The camera from this projection frame is the sole distance origin.
    watch_camera_and_local(view, local);
    note_player_pipeline_health(player_list_ok && live_player_positions > 0);

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
            return transient_return(9);
        }
        local = transform_camera_position; has_local_position = true;
    }

    Vec3 diagnostic_camera_position = transform_camera_mode ? transform_camera_position : camera_position;
    Vec3 diagnostic_camera_right = projection_camera_right;
    Vec3 diagnostic_camera_up = projection_camera_up;
    Vec3 diagnostic_camera_forward = projection_camera_forward;
    bool diagnostic_camera_basis_valid = projection_camera_basis_valid;
    const bool diagnostic_camera_rotation_valid = transform_camera_mode &&
        std::isfinite(transform_camera_rotation.x) && std::isfinite(transform_camera_rotation.y) &&
        std::isfinite(transform_camera_rotation.z) && std::isfinite(transform_camera_rotation.w);
    if (transform_camera_mode) {
        diagnostic_camera_basis_valid = false;
        if (diagnostic_camera_rotation_valid) {
            Vec4 normalized_rotation = transform_camera_rotation;
            if (normalize_quaternion(normalized_rotation)) {
                diagnostic_camera_right = rotate_vector(normalized_rotation, {1.0F, 0.0F, 0.0F});
                diagnostic_camera_up = rotate_vector(normalized_rotation, {0.0F, 1.0F, 0.0F});
                diagnostic_camera_forward = rotate_vector(normalized_rotation, {0.0F, 0.0F, 1.0F});
                diagnostic_camera_basis_valid = vec3_is_finite(diagnostic_camera_right) &&
                    vec3_is_finite(diagnostic_camera_up) && vec3_is_finite(diagnostic_camera_forward);
            }
        }
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
        if (g_aim_bones_requested && g_state.aim_touch)
            update_aim_ballistics(local_player);
    }

    // Когда этот игрок в последний раз был виден в кадре: кромка гасит бокс с
    // гистерезисом (см. отсев 'O' ниже), а карта чистится по размеру, как
    // s_lying_since.
    static std::unordered_map<uint64_t, double> s_box_seen_at;

    // Draw every remote PlayerManager returned by the player registry.
    // No UID, name, mount-point or movement heuristic is allowed to hide one.
    for (size_t i = 0; i < s_transforms.size(); ++i) {
        EspSkipLog diagnostic{};
        diagnostic.player = s_transforms[i];
        diagnostic.index = i;
        diagnostic.population = s_transforms.size();
        diagnostic.position_source = position_source[i];
        diagnostic.position_read_flags = position_read_flags[i];
        diagnostic.has_position = position_ok[i] != 0;
        if (diagnostic.has_position) diagnostic.position = positions[i];
        diagnostic.camera_position = diagnostic_camera_position;
        diagnostic.camera_right = diagnostic_camera_right;
        diagnostic.camera_up = diagnostic_camera_up;
        diagnostic.camera_forward = diagnostic_camera_forward;
        diagnostic.camera_basis_valid = diagnostic_camera_basis_valid;
        diagnostic.camera_rotation_valid = diagnostic_camera_rotation_valid;
        diagnostic.camera_from_transform = transform_camera_mode;
        diagnostic.camera_rotation = transform_camera_rotation;
        diagnostic.screen_width = sw;
        diagnostic.screen_height = sh;
        auto mark_skip = [&](char reason) {
            skip_reason[i] = reason;
            diagnostic.reason = reason;
            diagnostic.read_fails_total = g_mem.read_fails_total();
        };
        if (i == local_entity_index) { skip_reason[i] = 'L'; continue; }
        if (!s_transforms[i]) { skip_reason[i] = 'Z'; continue; }
        if (!position_ok[i]) { mark_skip('P'); continue; }
        Vec3 feet = positions[i];
        // На транспорте поле позиции замирает в точке посадки, и бокс едет
        // «по памяти» либо отстаёт. Пока пассажир в седле, позиция берётся из
        // трансформа, привязанного к сиденью; состояние видно в логе по
        // префиксу «транспорт:».
        apply_mounted_position(s_transforms[i], feet);
        diagnostic.position = feet;

        float distance = -1.0F;
        if (has_local_position) {
            float dx = feet.x - local.x, dy = feet.y - local.y, dz = feet.z - local.z;
            distance = sqrtf(dx * dx + dy * dy + dz * dz);
            if (!std::isfinite(distance)) { diagnostic.distance = distance; mark_skip('D'); continue; }
        }
        diagnostic.distance = distance;

        // Призрачные копии из пула объектов: игра держит в реестре второй
        // объект того же игрока (копия после посадки в транспорт, остаток
        // респауна). У копии тот же uid — подавляем всё, кроме объекта,
        // который рисовали в прошлый раз. Иначе копия попадает в счётчик
        // игроков «фантомом»: бокс у неё на замороженной точке посадки или за
        // экраном, а число сверху мигает 1/0, когда копия приходит и уходит.
        {
            auto track_it = g_player_track.find(s_transforms[i]);
            if (track_it != g_player_track.end() && track_it->second.uid[0]) {
                uint64_t& pick = g_player_track_pick[track_it->second.uid];
                if (pick && pick != s_transforms[i]) {
                    bool pick_alive = false;
                    for (uint64_t p : s_transforms) if (p == pick) { pick_alive = true; break; }
                    // Труп в реестре uid не удерживает: игрок уже респавнился, и
                    // показывать надо свежий объект. Две копии трупа между собой
                    // по-прежнему не меняются — выбор не дёргается зря.
                    const bool pick_corpse = pick_alive && player_track_is_corpse(pick);
                    const bool candidate_corpse = player_track_is_corpse(s_transforms[i]);
                    if (pick_alive && !(pick_corpse && !candidate_corpse)) {
                        diagnostic.duplicate_of = pick;
                        mark_skip('S');
                        continue;
                    }
                }
                pick = s_transforms[i];
            }
        }

        // Counted before any screen-space checks: the pill counter must see
        // players behind us too (360 degrees), not only the ones on screen.
        ++g_frame_player_count;

        // Crouch-aware body height from the character controller.
        PlayerAux& aux = player_aux(s_transforms[i]);
        const bool crouched = player_is_crouched(aux);
        float body_height = crouched ? aux.crouch_height : aux.normal_height;
        if (!(body_height > 0.6F && body_height < 2.6F)) body_height = PLAYER_HEIGHT;
        diagnostic.body_height = body_height;

        Vec3 body_bottom = {feet.x, feet.y, feet.z};
        Vec3 body_top = {feet.x, feet.y + body_height, feet.z};
        if (transform_camera_mode || !g_use_direct_player_position) { body_bottom.y = feet.y - 1.60F; body_top.y = feet.y + 0.20F; }

        // Ближняя плоскость: в упор верхняя (а при взгляде снизу — нижняя)
        // точка уходит за неё, и без отсечения весь бокс выбрасывался целиком,
        // хотя вторая точка была в кадре. Подробнее — в body_clip.h.
        const Vec4 body_plane = transform_camera_mode
            ? bodyclip::plane_from_camera(transform_camera_position,
                                          rotate_vector(transform_camera_rotation, {0.0F, 0.0F, 1.0F}))
            : bodyclip::plane_from_vp(vp);
        const bodyclip::Segment body_seg =
            bodyclip::clip_segment(body_bottom, body_top, body_plane, bodyclip::kNearDepth);
        if (!body_seg.visible) { mark_skip('B'); continue; }
        diagnostic.body_clipped = (body_seg.moved_bottom ? 1 : 0) | (body_seg.moved_top ? 2 : 0);

        diagnostic.bottom_world = body_bottom;
        diagnostic.top_world = body_top;
        diagnostic.has_bottom_world = true;
        diagnostic.has_top_world = true;

        Vec2 sf{}, sh2{};
        bool bottom_visible = transform_camera_mode
            ? w2s_transform_camera(transform_camera_position, transform_camera_rotation, body_bottom, sw, sh, sf, false, &diagnostic.bottom_projection)
            : w2s_with_trace(vp, body_bottom, sw, sh, sf, false, &diagnostic.bottom_projection);
        if (!bottom_visible) { mark_skip('B'); continue; }
        bool top_visible = transform_camera_mode
            ? w2s_transform_camera(transform_camera_position, transform_camera_rotation, body_top, sw, sh, sh2, false, &diagnostic.top_projection)
            : w2s_with_trace(vp, body_top, sw, sh, sh2, false, &diagnostic.top_projection);
        if (!top_visible) { mark_skip('T'); continue; }

        const float screen_margin = std::max(sw, sh) * 0.25F;
        // Подтянутая к плоскости точка улетает на тысячи пикселей за экран
        // (screen = f(1/near)). Прижимаем только её и только когда её реально
        // двигали: честную проекцию трогать нельзя, иначе дальний игрок вместо
        // mark_skip('O') начал бы рисоваться прижатым к кромке.
        if (body_seg.moved_bottom) sf = bodyclip::clamp_to_margin(sf, sw, sh, screen_margin);
        if (body_seg.moved_top) sh2 = bodyclip::clamp_to_margin(sh2, sw, sh, screen_margin);

        float height = fabsf(sh2.y - sf.y);
        if (!std::isfinite(height)) { mark_skip('H'); continue; }
        // Ноль пикселей высоты — это не «игрока нет», а «обе точки легли в одну
        // строку»: строго вдоль линии взгляда и на большой дистанции высота
        // схлопывается, и бокс мигал вместо того, чтобы стать минимальным
        // прямоугольником. Отрисовка и так растит его до kMinBoxSide — тот же
        // пол ставим в конвейере, чтобы кадр не терял игрока целиком.
        if (height < kMinProjectedHeight) height = kMinProjectedHeight;
        float cx = (sf.x + sh2.x) * 0.5F;
        float cy = (sf.y + sh2.y) * 0.5F;
        float half_w = height * PLAYER_BOX_WIDTH_RATIO * 0.5F;
        float half_h = height * 0.5F;
        diagnostic.box_cx = cx;
        diagnostic.box_cy = cy;
        diagnostic.box_half_width = half_w;
        diagnostic.box_half_height = half_h;
        diagnostic.screen_margin = screen_margin;
        diagnostic.has_box_bounds = true;
        // Test the box edges, not only its centre. At 5-10 m a player can
        // cover much of the viewport while the centre is already past the old
        // margin, which made close ESP vanish despite still intersecting it.
        //
        // Кромка — с гистерезисом, ровно как у маркеров (game_markers.cpp):
        // пока бокс виден, гасим его только когда он ушёл ещё на четверть малой
        // стороны. Без этого объект на границе зажигался и гас на каждом кадре
        // панорамы: камера за кадр сдвигала проекцию на пару пикселей, а бокс
        // стоял точно на пороге.
        const float edge_enter = std::max(sw, sh) * 0.25F;
        const float edge_hold = edge_enter + std::min(sw, sh) * 0.25F;
        const double edge_now = mono_seconds();
        auto box_seen = s_box_seen_at.find(s_transforms[i]);
        const bool box_was_visible = box_seen != s_box_seen_at.end() &&
                                     (edge_now - box_seen->second) <= 0.35;
        const float edge = box_was_visible ? edge_hold : edge_enter;
        if (cx + half_w < -edge || cx - half_w > sw + edge ||
            cy + half_h < -edge || cy - half_h > sh + edge) {
            mark_skip('O');
            continue;
        }

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
        // The base lead follows this target's smoothed world-space velocity.
        {
            auto vt = g_player_track.find(s_transforms[i]);
            if (vt != g_player_track.end() && vt->second.have_vel_ref) {
                const Vec3 velocity = vt->second.vel;
                const double age = mono_seconds() - vt->second.vel_ref_t;
                if (age >= 0.0 && age <= 0.35 && aim_ballistics::finite(velocity)) {
                    g_aim_velocity = velocity;
                    g_aim_lead.x = velocity.x * g_aim_lead_seconds;
                    g_aim_lead.y = velocity.y * g_aim_lead_seconds;
                    g_aim_lead.z = velocity.z * g_aim_lead_seconds;
                } else {
                    g_aim_velocity = {};
                    g_aim_lead = {};
                }
            } else {
                g_aim_velocity = {};
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
            if (rd_exact(s_transforms[i] + PLAYER_MANAGER_RESPAWNING, flag))
                box.respawning = flag != 0;
        }
        box.health = -1.f;
        {
            const uint64_t vitals = rd_ptr(s_transforms[i] + go_active::PLAYER_VITALS);
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
                const uint64_t entity = rd_ptr(vitals + go_active::PMP_ENTITY);
                if (entity >= 0x10000) {
                    const uint64_t hv = rd_ptr(entity + go_active::PMK_HEALTH);
                    if (hv >= 0x10000) {
                        // Слот значения: дизассемблер сеттера показывает +0x20
                        // (старое значение при этом относит в +0x24), а по
                        // раскладке AsyncReactiveProperty<T> из дампа значение
                        // должно лежать в +0x18. Тип brz.Health в дампе не
                        // разрешён (там просто «?»), поэтому берём тот слот,
                        // где лечит правдоподобное здоровье, — и печатаем оба,
                        // чтобы следующая сборка уже знала точно.
                        const float hp20 = rd<float>(hv + go_active::HEALTH_VALUE);
                        const float hp18 = rd<float>(hv + go_active::ARP_LATEST_VALUE);
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
            const uint64_t ref = rd_ptr(s_transforms[i] + go_active::PLAYER_DEATH_HANDLER);
            if (ref >= 0x10000) {
                const uint64_t dh = rd_ptr(ref + go_active::DEATH_HANDLER_REF_TARGET);
                if (dh >= 0x10000) {
                    const uint32_t f = rd<uint8_t>(dh + go_active::DEATH_HANDLER_DEATH);
                    if (f == 1) { box.death_flag = 1; box.dead = true; }
                    else if (f == 0) box.death_flag = 0;
                }
            }
        }
        // Поза «лежит» — только как запасной признак и очень строго.
        // Порог высоты 0.85 ложно ловил ПРИСЕВШЕГО игрока: в логе 00:53
        // живой (здоровье 100, флаг смерти 0) сидел с высотой скелета
        // 0.77-0.86 м и бокс мерцал «мёртв/жив» на каждом пересечении
        // порога. Теперь: присевшего игра знает сама (поза/состояние KCC)
        // и поза его не трогает; труп по высоте — 0.3-0.5 м, порог 0.60;
        // горизонталь трупа — от метра, у сидящего 0.51-0.61 м; и гистерезис
        // 0.35 с, чтобы шум скелета на один-два кадра не гасил бокс.
        // Поза движения — собственный ответ игры: MoveState.DEAD (7) в
        // KCC.Move.State. Это состояние самой игры, а не догадка по углам
        // скелета, поэтому оно дешевле и надёжнее любого порога; лежачей позы
        // (prone) в этой сборке нет вовсе — Move.Pose знает только Stand и
        // Crouch (dump 11949, enum Pose TypeDefIndex 4285).
        {
            const PlayerAux& aux = player_aux(s_transforms[i]);
            if (aux.kcc) {
                const int32_t move_state = rd<int32_t>(aux.kcc + KCC_MOVE + 0x00);
                if (move_state == KCC_MOVE_STATE_DEAD) box.dead = true;
            }
        }
        if (!crouched && !box.dead && box.death_flag < 0 && box.skel_bones >= 11 &&
            box.skel_up >= 0.f && box.skel_flat >= 0.f) {
            const bool lying = box.skel_up < 0.60f ||
                               (box.skel_flat > 1.0f && box.skel_flat > box.skel_up * 1.5f);
            static std::unordered_map<uint64_t, double> s_lying_since;
            const double t_lie = memio::now_seconds();
            auto it = s_lying_since.find(s_transforms[i]);
            if (lying) {
                if (it == s_lying_since.end()) s_lying_since.emplace(s_transforms[i], t_lie);
                else if (t_lie - it->second >= 0.35) box.dead = true;
            } else if (it != s_lying_since.end()) {
                s_lying_since.erase(it);
            }
            if (s_lying_since.size() > 256) s_lying_since.clear();
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
            }
        }
        // A confirmed corpse must disappear immediately. The five-second list
        // grace protects living players from read gaps, not dead PlayerManagers.
        if (box.dead) {
            mark_skip('X');
            if (g_frame_player_count > 0) --g_frame_player_count;
            // Метка для подавления дубликатов выше: этот объект — труп, uid он
            // не удерживает, и свежий объект игрока получает бокс.
            auto corpse_track = g_player_track.find(s_transforms[i]);
            if (corpse_track != g_player_track.end()) {
                corpse_track->second.corpse = true;
                corpse_track->second.corpse_at = mono_seconds();
            }
            continue;
        }
        // ---- Мировой якорь -------------------------------------------------
        // Ровно та геометрия, что ушла в экран этого кадра. Битый кадр
        // перепроецирует её текущей матрицей вида (esp_box_reproject) вместо
        // того, чтобы заморозить экранные координаты: замороженный бокс
        // разъезжается с миром, как только камера поехала.
        box.anchor_valid = !transform_camera_mode;
        box.anchor_feet[0] = feet.x; box.anchor_feet[1] = feet.y; box.anchor_feet[2] = feet.z;
        box.anchor_bottom_y = body_bottom.y;
        box.anchor_top_y = body_top.y;
        if (box.anchor_valid && box.has_skeleton) {
            auto skel_it = g_skeletons.find(s_transforms[i]);
            if (skel_it != g_skeletons.end()) {
                for (int b = 0; b < ESP_BONE_COUNT; ++b) {
                    if (!box.bone_valid[b]) continue;
                    const Vec3 w = skel_it->second.bone_world[b];
                    if (!vec3_is_finite(w)) continue;
                    box.anchor_bone[b][0] = w.x;
                    box.anchor_bone[b][1] = w.y;
                    box.anchor_bone[b][2] = w.z;
                    box.anchor_bone_ok |= (1u << b);
                }
            }
        }
        result.push_back(box);
        s_box_seen_at[s_transforms[i]] = mono_seconds();
        if (s_box_seen_at.size() > 256) s_box_seen_at.clear();
        auto live_track = g_player_track.find(s_transforms[i]);
        if (live_track != g_player_track.end()) {
            live_track->second.corpse = false;
            live_track->second.corpse_at = 0.0;
        }
    }

    append_bot_boxes(result);
    g_box_frame_transient = false;
    watch_boxes(result, local_entity_index, &skip_reason, 0);

    return result;
}

int esp_nearby_player_count() { return g_frame_player_count; }
int esp_nearby_bot_count() {
    // Bot markers have their own throttled registry/cache and do not require
    // skeleton ESP or aimbot mode to be enabled.
    esp_bot_aim_maintain();
    return static_cast<int>(esp_bot_aim_list().size());
}

// ---- Диагностика конвейера костей из терминала (xvcen skel) ---------------
//
// Файлового лога на устройстве нет, поэтому «скелеты не работают на этом
// устройстве» разбирается только так: короткоживущий процесс
// привязывается к игре выбранным транспортом, один раз прогоняет весь
// конвейер (список игроков -> модель -> имя GameObject -> раскладка иерархии
// -> мировые позиции костей -> проекция) и печатает, на каком шаге он
// рассыпался. В обычной работе чит этого не делает.
int esp_skeleton_diag(char* out, size_t cap, int max_players) {
    if (!out || cap < 128) return -1;
    out[0] = '\0';
    size_t pos = 0;
    auto say = [&](const char* fmt, ...) {
        if (pos + 1 >= cap) return;
        va_list ap;
        va_start(ap, fmt);
        const int n = vsnprintf(out + pos, cap - pos, fmt, ap);
        va_end(ap);
        if (n > 0) pos += ((size_t)n < cap - pos) ? (size_t)n : cap - pos - 1;
    };

    // Кости нужны и для ESP, и для аима: включаем оба запроса, иначе конвейер
    // их просто не строит (want_bones в esp_get_boxes).
    esp_set_skeleton_enabled(true);
    esp_set_aim_bones_enabled(true);

    say("== скелеты: диагностика конвейера ==\n");
    say("транспорт          : %s\n", g_mem.backend_name());
    say("привязка           : %d (0 = OK)\n", (int)esp_attach_state());
    say("чтений / отказов   : %llu / %llu\n",
        g_mem.reads_total(), g_mem.read_fails_total());
    say("меток TBI / мусора : %llu / %llu    обрывов: %llu\n",
        g_mem.tagged_total(), g_mem.junk_total(), g_mem.cut_total());
    say("профиль оффсетов   : %s\n", VersionName());

    const TransformHierarchyLayout& gl = g_transform_hierarchy_layout;
    say("раскладка общая    : найдена=%d перебор_идёт=%d (данные +0x%llx индекс +0x%llx)\n",
        (int)g_transform_hierarchy_layout_valid, (int)layout_scan_in_progress(),
        (unsigned long long)gl.data_offset, (unsigned long long)gl.index_offset);
    say("раскладка скелета  : найдена=%d (данные +0x%llx индекс +0x%llx матрицы +0x%llx"
        " индексы +0x%llx косвенно %d/%d)\n",
        (int)g_skeleton_layout_valid,
        (unsigned long long)g_skeleton_layout.data_offset,
        (unsigned long long)g_skeleton_layout.index_offset,
        (unsigned long long)g_skeleton_layout.matrices_offset,
        (unsigned long long)g_skeleton_layout.indices_offset,
        (int)g_skeleton_layout.matrices_indirect, (int)g_skeleton_layout.indices_indirect);
    say("имя GameObject     : найдено=%d (смещение +0x%llx, %s)\n",
        (int)g_go_name_offset_valid, (unsigned long long)g_go_name_offset,
        g_go_name_plain_pointer ? "сырой указатель" : "core::string");
    say("слот модели        : +0x%llx (%s)\n",
        (unsigned long long)character_model_offset(0),
        g_character_model_probed
            ? (g_character_model_offset ? "найден перебором" : "перебор не дал модели")
            : "вшитый");

    const std::vector<uint64_t> players = read_configured_player_transforms(nullptr);
    say("игроков в списке   : %d\n", (int)players.size());
    if (players.empty()) {
        say("!! список игроков пуст — конвейер костей не запускался\n");
        return 1;
    }

    // Полный прогон кадра: он и строит скелеты, и заполняет g_skeletons.
    const std::vector<EspBox> boxes = esp_get_boxes(1920, 1080);
    int skeletons_ok = 0;
    const int limit = max_players > 0 ? std::min<int>(max_players, (int)players.size()) : (int)players.size();
    for (int i = 0; i < limit; ++i) {
        const uint64_t player = players[i];
        say("[%d] игрок 0x%llx\n", i, (unsigned long long)player);

        const uint64_t native = resolve_player_native_transform(player);
        Vec3 pos{};
        const bool pos_ok = native && read_transform_hierarchy_position(native, pos);
        say("    трансформ 0x%llx позиция %s (%.1f, %.1f, %.1f)\n",
            (unsigned long long)native, pos_ok ? "ок" : "НЕТ",
            (double)pos.x, (double)pos.y, (double)pos.z);

        const uint64_t root = skeleton_model_root(player);
        static std::vector<uint64_t> nodes;
        collect_transform_subtree(root, nodes, 1024);
        say("    модель: корень 0x%llx, узлов %d\n",
            (unsigned long long)root, (int)nodes.size());

        int named = 0, hips = 0;
        char sample[256] = {};
        size_t sample_used = 0;
        for (uint64_t node : nodes) {
            char name[48];
            if (!read_transform_name(node, name, sizeof(name))) continue;
            ++named;
            if (match_bone_name(name) == BONE_HIPS) ++hips;
            if (sample_used + 1 < sizeof(sample) && named <= 8) {
                const size_t room = sizeof(sample) - sample_used;
                const int w = snprintf(sample + sample_used, room, "%s%s",
                                       sample_used ? ", " : "", name);
                if (w > 0) sample_used += (size_t)w < room ? (size_t)w : room - 1;
            }
        }
        say("    имена: прочитано %d, hips-кандидатов %d\n", named, hips);
        say("    пример имён: %s\n", sample[0] ? sample : "(нет)");

        int world_bones = 0;
        auto it = g_skeletons.find(player);
        if (it != g_skeletons.end()) {
            say("    кэш скелета: валиден=%d собрано=%d тумблер=%d отказов=%d\n",
                (int)it->second.valid, it->second.bone_count,
                it->second.bone_transform[BONE_HIPS] != 0, it->second.fail_streak);
            for (int b = 0; b < ESP_BONE_COUNT; ++b)
                if (it->second.bone_world_age[b] >= 1 && it->second.bone_world_age[b] <= 9)
                    ++world_bones;
        } else {
            say("    кэш скелета: НЕТ (построение не дошло до фиксации)\n");
        }
        say("    мировых позиций костей: %d из %d\n", world_bones, (int)ESP_BONE_COUNT);

        const EspBox* box = nullptr;
        for (const EspBox& candidate : boxes)
            if (candidate.id == player) { box = &candidate; break; }
        if (!box) {
            say("    бокс: НЕТ (игрок не попал в кадр ESP)\n");
            continue;
        }
        int valid = 0;
        for (int b = 0; b < ESP_BONE_COUNT; ++b) valid += box->bone_valid[b] ? 1 : 0;
        say("    бокс: has_skeleton=%d костей=%d/%d экран головы (%.0f, %.0f)\n",
            (int)box->has_skeleton, valid, (int)ESP_BONE_COUNT,
            (double)box->bones[BONE_HEAD][0], (double)box->bones[BONE_HEAD][1]);
        if (box->has_skeleton) ++skeletons_ok;
    }
    say("игроков со скелетом: %d из %d\n", skeletons_ok, limit);
    return skeletons_ok > 0 ? 0 : 2;
}
