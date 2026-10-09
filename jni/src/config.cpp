#include "config.h"
#include <cmath>
#include "str.h"
#include "app_state.h"
#include "cfg.h"
#include "theme.h"
#include "hud.h"
#include "audio.h"
#include "lang.h"
#include "game.h"
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <string>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <time.h>
#include <utime.h>
#include <unistd.h>
#include <vector>

static constexpr uint8_t kXorKey   = 0xA7;



static const char* kCfgDir_() noexcept {
    static constexpr auto _s = xp::_mk("/data/local/benzware/");
    return _s.d();
}
#define kCfgDir (kCfgDir_())

ConfigEntry g_configs[kMaxConfigs] = {};
int         g_configCount    = 0;
int         g_configToDelete = -1;
char        g_loadedConfigName[64] = {};
float       g_cfgLoadAnim[kMaxConfigs] = {};
int         g_cfgLoadedIdx = -1;

static std::string CfgPath(const char* name) {
    return std::string(kCfgDir) + name + XS(".cfg");
}

static void XorBuf(uint8_t* buf, size_t sz) {
    for (size_t i = 0; i < sz; i++) buf[i] ^= (kXorKey ^ (uint8_t)(i * 0x1D + 0x3B));
}

void CfgScanDir() {
    mkdir(kCfgDir, 0777);
    g_configCount = 0;
    DIR* d = opendir(kCfgDir);
    if (!d) return;
    struct dirent* e;
    std::vector<std::string> names;
    while ((e = readdir(d)) != nullptr) {
        std::string fn = e->d_name;
        if (fn.size() > 4 && fn.substr(fn.size() - 4) == XS(".cfg"))
            names.push_back(fn.substr(0, fn.size() - 4));
    }
    closedir(d);
    std::sort(names.begin(), names.end());
    for (auto& n : names) {
        if (g_configCount >= kMaxConfigs) break;
        snprintf(g_configs[g_configCount++].name, 64, "%s", n.c_str());
    }
}

static int g_inotifyFd  = -1;
static int g_inotifyWd  = -1;

void CfgWatchInit() {
    g_inotifyFd = inotify_init1(IN_NONBLOCK);
    if (g_inotifyFd < 0) return;
    mkdir(kCfgDir, 0777);
    g_inotifyWd = inotify_add_watch(g_inotifyFd, kCfgDir,
        IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_MODIFY);
}

void CfgWatchFree() {
    if (g_inotifyFd >= 0) {
        if (g_inotifyWd >= 0) inotify_rm_watch(g_inotifyFd, g_inotifyWd);
        close(g_inotifyFd);
        g_inotifyFd = -1;
        g_inotifyWd = -1;
    }
}

void CfgWatchTick() {
    if (g_inotifyFd < 0) return;
    char buf[512];
    ssize_t n = read(g_inotifyFd, buf, sizeof(buf));
    if (n > 0) CfgScanDir();
}

struct CfgBlob {
    uint32_t magic;
    uint32_t version;
    bool  aim_touch, aim_pos, aim_special;
    int   aim_bone;
    bool  aim_vis_check, aim_draw_fov;
    //   aim_mode — режим аима (0 тач / 1 память / 2 сайлент). Лежит в байте
    //                                 выравнивания, который в раскладке и так
    //                                 был между aim_draw_fov и float aim_fov:
    //                                 sizeof(CfgBlob) остаётся 260, версия
    //                                 остаётся 4. Старые конфиги держат тут 0,
    //                                 то есть прежний тач-аим.
    uint8_t aim_mode;
    //   aim_smoothness -> aim_lead   (smoothing has its own gun_str slot,
    //                                 and this one was only ever a mirror).
    //                                 Reused: farm search range in metres
    //                                 (0 in old configs = use the default).
    float aim_fov, aim_lead;
    // Every field that a feature outgrew is renamed in place rather than
    // appended, so the byte layout — and with it version 4 and every config
    // already saved on a device — stays valid. Current renames:
    //   esp_hp        -> esp_ore          esp_ping        -> esp_animal
    //   esp_weapon_icon -> esp_team
    //   esp_health_col  -> esp_ally_col   esp_money_col   -> esp_animal_col
    //   esp_weapon_icon_col -> esp_loot_col
    //   esp_ping_col    -> esp_extra (packed scalars, see below)
    //   aim_smoothness  -> aim_lead
    //   esp_stroke      -> aim_touch_x    esp_rounding -> aim_touch_y
    //                      (точка пальца аимбота в долях экрана: те же два
    //                       float'а на своих местах; в старых конфигах там
    //                       2.0 и 0.0 — вне диапазона, значит «не задана»)
    bool  esp_box, esp_name, esp_ore, esp_wall, esp_chams;
    bool  esp_weapon, esp_team, esp_tracer, esp_skeleton;
    bool  aim_scope_only, esp_animal, esp_vis_check, esp_fill;
    float esp_thick, aim_touch_x, aim_touch_y, esp_fill_pct;
    float gun_str, gun_fov, gun_trigger_delay;
    //   ui_fps        -> ui_lang_en       (счётчик FPS убран навсегда, а слот
    //                                      bool в раскладке заморожен версией;
    //                                      теперь тут язык: 0 рус / 1 англ)
    bool  ui_lang_en, ui_dark_mode, ui_show_sep;
    ImVec4 esp_box_col, esp_box_col_invis, esp_name_col, esp_ally_col, esp_distance_col;
    ImVec4 esp_weapon_col, esp_loot_col, esp_tracer_col, esp_skeleton_col, esp_animal_col;
    // Four scalars that had no slot of their own:
    //   x = bit 0 loot ESP, bit 1 pickup ESP, bit 2 bots ESP, bit 3 vehicles ESP,
    //       bit 4 screen freecam button HIDDEN (inverted: old configs hold 0
    //       here, i.e. the button stays visible, as before), bit 5 scrap ESP,
    //       bit 6 fast fire, bit 7 fast reload, bit 8 house boxes HIDDEN
    //       (inverted like bit 4: old configs hold 0, i.e. visible, as before).
    //       Bit 9 belongs to a retired option: it is never written and is
    //       deliberately ignored on load so older profiles keep their other
    //       packed settings.
    //   y = marker draw distance (m),
    //   z = aim priority + 1 (so an old config's 1.0 still means "default"),
    //   w = pickup colour packed as r*65536 + g*256 + b (exact in a float,
    //       since 0xFFFFFF < 2^24); <= 1 means "never written, use default".
    ImVec4 esp_extra;
    int   esp_box_type;
    float esp_box_rounding;
};
static_assert(sizeof(CfgBlob) == 260, "stable cfg blob");

// UI state that used to live in sidecar files (.lang/.accent/.layout/.mode)
// is appended after ConfigExtension as a trailer. CfgBlob and the extension
// keep their layouts; readers that stop at the extension ignore the trailer.
struct CfgUiTrailer {
    uint32_t magic;          // "CUI1"
    uint32_t version;        // 1
    uint32_t accent_idx;     // 0..8, see app_state.h
    float    accent_rgb[3];
    uint32_t mem_backend;    // EspMemBackend chosen on the mode screen
    float    layout[20];     // -1 = not set
};
static constexpr uint32_t kCfgUiMagic = 0x31495543u;

static void LayoutToArray(float* v) {
    v[0]  = g_state.farm_joy_x;     v[1]  = g_state.farm_joy_y;
    v[2]  = g_state.farm_fire_x;    v[3]  = g_state.farm_fire_y;
    v[4]  = g_state.freecam_joy_x;  v[5]  = g_state.freecam_joy_y;
    v[6]  = g_state.hud_cnt_x;      v[7]  = g_state.hud_cnt_y;
    v[8]  = g_state.freecam_btn_x;  v[9]  = g_state.freecam_btn_y;
    v[10] = g_state.aim_tx;         v[11] = g_state.aim_ty;
    v[12] = g_state.freecam_look_x; v[13] = g_state.freecam_look_y;
    v[14] = g_state.freecam_look_radius;
    v[15] = g_state.freecam_joy_size;
    v[16] = g_state.fakelag_btn_x;  v[17] = g_state.fakelag_btn_y;
    v[18] = g_state.xray_btn_x;     v[19] = g_state.xray_btn_y;
}

static void LayoutFromArray(const float* v) {
    auto cl = [](float x) { return (x > 0.f && x < 1.f) ? x : -1.f; };
    g_state.farm_joy_x = cl(v[0]);   g_state.farm_joy_y = cl(v[1]);
    g_state.farm_fire_x = cl(v[2]);  g_state.farm_fire_y = cl(v[3]);
    g_state.freecam_joy_x = cl(v[4]); g_state.freecam_joy_y = cl(v[5]);
    g_state.hud_cnt_x = cl(v[6]);    g_state.hud_cnt_y = cl(v[7]);
    g_state.freecam_btn_x = cl(v[8]); g_state.freecam_btn_y = cl(v[9]);
    g_state.aim_tx = cl(v[10]);      g_state.aim_ty = cl(v[11]);
    g_state.freecam_look_x = cl(v[12]);
    g_state.freecam_look_y = cl(v[13]);
    g_state.freecam_look_radius = (v[14] >= 0.10f && v[14] <= 0.48f) ? v[14] : 0.28f;
    g_state.freecam_joy_size = (v[15] >= 60.f && v[15] <= 200.f) ? v[15] : 100.f;
    g_state.fakelag_btn_x = cl(v[16]); g_state.fakelag_btn_y = cl(v[17]);
    g_state.xray_btn_x = cl(v[18]);    g_state.xray_btn_y = cl(v[19]);
}

static void WriteUiTrailer(std::ostream& f) {
    CfgUiTrailer t{};
    t.magic = kCfgUiMagic;
    t.version = 1;
    const int idx = g_state.ui_accent_idx;
    t.accent_idx = (uint32_t)((idx >= 0 && idx <= 8) ? idx : 0);
    t.accent_rgb[0] = g_state.ui_accent_rgb.x;
    t.accent_rgb[1] = g_state.ui_accent_rgb.y;
    t.accent_rgb[2] = g_state.ui_accent_rgb.z;
    t.mem_backend = (uint32_t)esp_get_mem_backend();
    LayoutToArray(t.layout);
    uint8_t buf[sizeof(t)];
    memcpy(buf, &t, sizeof(t));
    XorBuf(buf, sizeof(buf));
    f.write((char*)buf, sizeof(buf));
}

static bool ReadUiTrailer(std::istream& f, CfgUiTrailer& t) {
    memset(&t, 0, sizeof(t));
    for (int i = 0; i < 20; ++i) t.layout[i] = -1.f;
    uint8_t buf[sizeof(CfgUiTrailer)];
    f.read((char*)buf, sizeof(buf));
    size_t got = (size_t)f.gcount();
    constexpr size_t kLegacySize = sizeof(CfgUiTrailer) - 2 * sizeof(float);
    if (got != sizeof(buf) && got != kLegacySize) return false;
    XorBuf(buf, got);
    memcpy(&t, buf, got);
    return t.magic == kCfgUiMagic && t.version == 1;
}

// Newest .cfg by mtime, same rule as the ConfigLoadLast() fallback.
static std::string NewestCfgPath() {
    std::vector<std::string> names;
    if (DIR* d = opendir(kCfgDir)) {
        while (struct dirent* e = readdir(d)) {
            std::string fn = e->d_name;
            if (fn.size() > 4 && fn.substr(fn.size() - 4) == XS(".cfg"))
                names.push_back(fn);
        }
        closedir(d);
    }
    std::sort(names.begin(), names.end());
    std::string best;
    time_t best_mtime = 0;
    for (auto& fn : names) {
        const std::string p = std::string(kCfgDir) + fn;
        struct stat st{};
        if (stat(p.c_str(), &st) != 0) continue;
        if (best.empty() || st.st_mtime >= best_mtime) {
            best_mtime = st.st_mtime;
            best = p;
        }
    }
    return best;
}

static bool ReadNewestBlob(CfgBlob& s) {
    const std::string p = NewestCfgPath();
    if (p.empty()) return false;
    std::ifstream f(p, std::ios::binary);
    uint8_t buf[sizeof(s)];
    f.read((char*)buf, sizeof(buf));
    if ((size_t)f.gcount() != sizeof(buf)) return false;
    XorBuf(buf, sizeof(buf));
    memcpy(&s, buf, sizeof(s));
    return s.magic == 0x58564345U && s.version == 4;
}

static bool ReadNewestUi(CfgUiTrailer& t) {
    const std::string p = NewestCfgPath();
    if (p.empty()) return false;
    std::ifstream f(p, std::ios::binary);
    f.seekg(sizeof(CfgBlob) + sizeof(viewmodel::ConfigExtension));
    return f && ReadUiTrailer(f, t);
}

// Startup restore: state comes from the newest config, nothing else on disk.
void RestoreLang() {
    CfgBlob s;
    if (ReadNewestBlob(s)) lang::set(s.ui_lang_en ? lang::LANG_EN : lang::LANG_RU);
}

void RestoreAccent() {
    CfgUiTrailer t;
    if (!ReadNewestUi(t)) return;
    int idx = (int)t.accent_idx;
    if (idx < 0 || idx > 8) idx = 0;
    auto cl = [](float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); };
    g_state.ui_accent_idx = idx;
    g_state.ui_accent_rgb = {cl(t.accent_rgb[0]), cl(t.accent_rgb[1]), cl(t.accent_rgb[2]), 1.0f};
}

void RestoreLayout() {
    CfgUiTrailer t;
    if (ReadNewestUi(t)) LayoutFromArray(t.layout);
}

// Transport chosen on the mode screen in the session that wrote the newest config.
int CfgSavedMemBackend() {
    CfgUiTrailer t;
    if (!ReadNewestUi(t) || t.mem_backend > 3) return -1;
    return (int)t.mem_backend;
}

static void ConfigSaveToPath(const std::string& path) {
    CfgBlob s;
    s.magic   = 0x58564345U;
    s.version = 4;
    s.aim_touch   = g_state.aim_touch;   s.aim_pos     = g_state.aim_pos;
    s.aim_special = g_state.aim_special;
    s.aim_bone    = g_state.aim_bone;
    s.aim_vis_check  = cfg::aim::vis_check;
    s.aim_draw_fov   = cfg::aim::draw_fov;
    s.aim_mode       = (uint8_t)((g_state.aim_mode >= 0 && g_state.aim_mode <= 2) ? g_state.aim_mode : 0);
    s.aim_fov        = cfg::aim::fov;
    // Слот aim_lead давно свободен (см. CfgBlob) — теперь в нём живёт
    // дальность автофарма. Старые конфиги держат тут 0 и дадут дефолт.
    s.aim_lead       = g_state.farm_range;
    s.esp_box     = g_state.esp_box;     s.esp_name    = g_state.esp_name;
    s.esp_ore     = g_state.esp_ore;     s.esp_wall    = g_state.esp_wall;
    s.esp_chams   = g_state.esp_chams;
    s.esp_weapon      = g_state.esp_weapon;
    s.esp_team        = g_state.esp_team;
    s.esp_tracer      = g_state.esp_tracer;
    s.esp_skeleton    = g_state.esp_skeleton;
    s.aim_scope_only  = g_state.aim_scope_only;
    s.esp_animal      = g_state.esp_animal;
    s.esp_vis_check   = cfg::esp::vis_check;
    s.esp_fill        = cfg::esp::fill;
    s.esp_thick       = g_state.esp_thick;
    // Слоты esp_stroke/esp_rounding теперь несут точку пальца аима; сюда идёт
    // как есть (-1 = не задана, тогда работает дефолт 74%/50%).
    s.aim_touch_x     = g_state.aim_tx;
    s.aim_touch_y     = g_state.aim_ty;
    s.esp_fill_pct    = cfg::esp::fill_pct;
    s.gun_str     = g_state.gun_str;
    s.gun_fov     = g_state.gun_fov;
    s.gun_trigger_delay     = g_state.gun_trigger_delay;
    s.ui_lang_en  = lang::english();     s.ui_dark_mode= g_state.ui_dark_mode;
    s.ui_show_sep = g_state.ui_show_sep;
    s.esp_box_col          = cfg::esp::box_col;
    s.esp_box_col_invis    = cfg::esp::box_col_invis;
    s.esp_name_col         = cfg::esp::name_col;
    s.esp_ally_col         = cfg::esp::ally_col;
    s.esp_distance_col     = cfg::esp::distance_col;
    s.esp_weapon_col       = cfg::esp::weapon_col;
    s.esp_loot_col         = cfg::esp::loot_col;
    s.esp_tracer_col       = cfg::esp::tracer_col;
    s.esp_skeleton_col     = cfg::esp::skeleton_col;
    s.esp_animal_col       = cfg::esp::animal_col;
    {
        // Цвета ботов/машин не сохраняются (в esp_extra нет места) — сессионные.
        const int flags = (g_state.esp_loot ? 1 : 0) | (g_state.esp_pickup ? 2 : 0) |
                          (g_state.esp_bots ? 4 : 0) | (g_state.esp_vehicles ? 8 : 0) |
                          (g_state.freecam_btn_show ? 0 : 16) | (g_state.esp_scrap ? 32 : 0) |
                          (g_state.fast_fire ? 64 : 0) | (g_state.fast_reload ? 128 : 0) |
                          // Инвертировано, как бит 4: старые конфиги держат 0,
                          // то есть ящики в домах видны — как до разделения.
                          (g_state.esp_house_boxes ? 0 : 256) |
                          (g_state.fakelag_btn_show ? 0 : 1024) |
                          (g_state.xray_btn_show ? 0 : 2048);
        auto ch = [](float v) { int i = (int)(v * 255.f + 0.5f); return i < 0 ? 0 : (i > 255 ? 255 : i); };
        const float packed = (float)(ch(cfg::esp::pickup_col.x) * 65536
                                   + ch(cfg::esp::pickup_col.y) * 256
                                   + ch(cfg::esp::pickup_col.z));
        s.esp_extra = {(float)flags, g_state.marker_dist,
                       (float)(g_state.aim_priority + 1), packed};
    }
    s.esp_box_type         = cfg::esp::box_type;
    s.esp_box_rounding     = cfg::esp::box_rounding;
    uint8_t buf[sizeof(s)];
    memcpy(buf, &s, sizeof(s));
    XorBuf(buf, sizeof(s));
    std::ofstream f(path, std::ios::binary);
    if (f) {
        f.write((char*)buf, sizeof(buf));
        auto extension = viewmodel::encode(g_state.viewmodel);
        extension.flags |= (g_state.esp_turrets ? 128u : 0u) |
                           (g_state.esp_backpacks ? 256u : 0u) |
                           (g_state.esp_sleepers ? 512u : 0u) |
                           (g_state.esp_cupboards ? 1024u : 0u);
        // v4+: упреждение аима в миллисекундах (0 = выкл).
        extension.aim_lead_ms = g_state.aim_lead_ms;
        uint8_t extra[sizeof(extension)];
        memcpy(extra, &extension, sizeof(extra));
        XorBuf(extra, sizeof(extra));
        f.write((char*)extra, sizeof(extra));
        WriteUiTrailer(f);
        f.close();
    }
}

void ConfigSave() {
    mkdir(kCfgDir, 0777);
    int idx = 1;
    char baseName[64];
    std::string cfgpath;
    do {
        snprintf(baseName, sizeof(baseName), XS("config%d"), idx++);
        cfgpath = CfgPath(baseName);
    } while (access(cfgpath.c_str(), F_OK) == 0);
    ConfigSaveToPath(cfgpath);
    CfgScanDir();
    ShowToast(XS("Конфиг создан"));
    PlaySound(SND_SUCCESS);
}

void ConfigUpdate(int idx) {
    if (idx < 0 || idx >= g_configCount) return;
    std::string path = CfgPath(g_configs[idx].name);
    ConfigSaveToPath(path);
    ShowToast(XS("Конфиг сохранён"));
    PlaySound(SND_SUCCESS);
}

void ConfigLoad(int idx, bool announce) {
    if (idx < 0 || idx >= g_configCount) return;
    std::string path = CfgPath(g_configs[idx].name);
    std::ifstream f(path, std::ios::binary);
    if (!f) { if (announce) ShowToast(XS("Файл не найден")); return; }

    CfgBlob s;

    uint8_t buf[sizeof(s)];
    f.read((char*)buf, sizeof(buf));
    size_t got = (size_t)f.gcount();
    uint8_t extra[sizeof(viewmodel::ConfigExtension)]{};
    f.read((char*)extra, sizeof(extra));
    size_t extraGot = (size_t)f.gcount();
    CfgUiTrailer ui{};
    const bool hasUi = ReadUiTrailer(f, ui);
    f.close();
    if (got != sizeof(buf)) { if (announce) ShowToast(XS("Несовместимый конфиг")); return; }
    XorBuf(buf, sizeof(s));
    memcpy(&s, buf, sizeof(s));
    if (s.magic != 0x58564345U || s.version != 4) { if (announce) ShowToast(XS("Старый конфиг — пересохрани")); return; }
    // Загруженный конфиг становится самым свежим: startup восстанавливает по mtime.
    utime(path.c_str(), nullptr);

    XorBuf(extra, extraGot);
    g_state.viewmodel = viewmodel::decode(extra, extraGot);
    g_state.esp_turrets = g_state.esp_backpacks = g_state.esp_sleepers = false;
    g_state.esp_cupboards = false;
    // Упреждения в старых прицепах (v1–v3) нет — сброс в 0; v4–v6 могут
    // вернуть своё значение. Сам прицеп 36 байт с v4; прицеп v3 был 32 —
    // обрабатываем оба размера, иначе старые конфиги потеряли бы турели,
    // рюкзаки и спящих.
    g_state.aim_lead_ms = 0.f;
    if (extraGot == 32 || extraGot == sizeof(viewmodel::ConfigExtension)) {
        viewmodel::ConfigExtension extension;
        memcpy(&extension, extra, extraGot);   // v3: 32 байта, хвост — дефолты
        if (extension.magic == 0x31574d56u &&
            (extension.version == 3 || extension.version == 4 || extension.version == 5 || extension.version == 6) &&
            extension.size == extraGot) {
            g_state.esp_turrets = (extension.flags & 128u) != 0;
            g_state.esp_backpacks = (extension.flags & 256u) != 0;
            g_state.esp_sleepers = (extension.flags & 512u) != 0;
            g_state.esp_cupboards = (extension.flags & 1024u) != 0;
            if (extension.version >= 4)
                g_state.aim_lead_ms = viewmodel::decode_aim_lead_ms(extra, extraGot);
            g_state.aim_predict = std::round(g_state.aim_lead_ms / kAimPredictStepMs);
        }
    }
    g_state.sl_custom_fov = {}; g_state.sl_hands_x = {};
    g_state.sl_hands_y = {}; g_state.sl_hands_z = {};
    g_state.aim_touch   = s.aim_touch;
    // «Только видимых» убран из меню — значение из конфига игнорируется.
    g_state.aim_pos     = false;
    g_state.aim_special = s.aim_special;
    g_state.aim_scope_only = s.aim_scope_only;
    cfg::aim::scope_only   = s.aim_scope_only;
    g_state.aim_bone    = s.aim_bone;
    // Режим аима: старые конфиги (и любой левый байт) дают 0 — прежний тач.
    g_state.aim_mode    = (s.aim_mode <= 2) ? (int)s.aim_mode : 0;
    cfg::aim::vis_check  = false;
    cfg::aim::draw_fov   = s.aim_draw_fov;
    cfg::aim::fov        = s.aim_fov;
    // Дальность автофарма переехала в бывший слот aim_lead; в старых
    // конфигах там 0 — тогда остаётся дефолт 100 м.
    g_state.farm_range   = (s.aim_lead >= 10.f && s.aim_lead <= 300.f) ? s.aim_lead : 100.f;
    g_state.esp_box     = s.esp_box;     g_state.esp_name    = s.esp_name;
    g_state.esp_wall    = s.esp_wall;
    g_state.esp_chams   = s.esp_chams;
    g_state.esp_weapon      = s.esp_weapon;
    g_state.esp_tracer      = s.esp_tracer;
    g_state.esp_skeleton    = s.esp_skeleton;
    g_state.esp_ore         = s.esp_ore;
    g_state.esp_animal      = s.esp_animal;
    g_state.esp_team        = s.esp_team;
    // Packed scalars. A config written before these existed holds the old ping
    // colour here, so each value is range-checked and falls back to its default.
    {
        const int flags = (s.esp_extra.x >= 0.f && s.esp_extra.x < 4096.f) ? (int)(s.esp_extra.x + 0.5f) : 0;
        g_state.esp_loot   = (flags & 1) != 0;
        g_state.esp_pickup = (flags & 2) != 0;
        g_state.esp_bots     = (flags & 4) != 0;
        g_state.esp_vehicles = (flags & 8) != 0;
        g_state.freecam_btn_show = (flags & 16) == 0;
        g_state.esp_scrap = (flags & 32) != 0;
        g_state.fast_fire = (flags & 64) != 0;
        g_state.fast_reload = (flags & 128) != 0;
        g_state.esp_house_boxes = (flags & 256) == 0;
        g_state.fakelag_btn_show = (flags & 1024) == 0;
        g_state.xray_btn_show = (flags & 2048) == 0;
        g_state.marker_dist = (s.esp_extra.y >= 25.f && s.esp_extra.y <= 300.f) ? s.esp_extra.y : 150.f;
        int pr = (int)(s.esp_extra.z + 0.5f) - 1;
        g_state.aim_priority = (pr >= 0 && pr <= 2) ? pr : 0;
        // Packed pickup colour. Configs written before it existed hold the old
        // ping alpha (exactly 1.0) here, which is why the check is "> 1".
        if (s.esp_extra.w > 1.f && s.esp_extra.w <= 16777215.f) {
            int rgb = (int)(s.esp_extra.w + 0.5f);
            cfg::esp::pickup_col = ImVec4(((rgb >> 16) & 0xFF) / 255.f,
                                          ((rgb >> 8) & 0xFF) / 255.f,
                                          (rgb & 0xFF) / 255.f, 1.f);
        }
    }
    g_state.esp_thick   = s.esp_thick;
    g_state.gun_str     = s.gun_str;
    g_state.gun_fov     = s.gun_fov;
    if (!(g_state.gun_fov >= 5.f)) g_state.gun_fov = 5.f;
    if (g_state.gun_fov > 180.f) g_state.gun_fov = 180.f;
    g_state.gun_trigger_delay     = s.gun_trigger_delay;
    g_state.ui_dark_mode= s.ui_dark_mode;
    // Язык интерфейса живёт в бывшем слоте ui_fps (счётчик FPS убран навсегда).
    // Старые конфиги держат там false — это и есть русский по умолчанию.
    lang::set(s.ui_lang_en ? lang::LANG_EN : lang::LANG_RU);
    // ui_show_sep из конфига игнорируется: рамки карточек всегда включены.
    g_state.ui_fps      = false;
    g_state.ui_show_sep = true;
    // Акцент и раскладка из трейлера (старые конфиги без него не трогают текущие).
    // Идёт до точки аима: её значение из блоба побеждает.
    if (hasUi) {
        int idx = (int)ui.accent_idx;
        if (idx < 0 || idx > 8) idx = 0;
        auto cl = [](float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); };
        g_state.ui_accent_idx = idx;
        g_state.ui_accent_rgb = {cl(ui.accent_rgb[0]), cl(ui.accent_rgb[1]), cl(ui.accent_rgb[2]), 1.0f};
        LayoutFromArray(ui.layout);
    }
    // Точка пальца аима. В конфигах, где этого поля ещё не было, лежат значения
    // прежних настроек ESP (2.0 и 0.0) — они вне 0..1 и читаются как «не задана».
    g_state.aim_tx = (s.aim_touch_x > 0.f && s.aim_touch_x < 1.f) ? s.aim_touch_x : -1.f;
    g_state.aim_ty = (s.aim_touch_y > 0.f && s.aim_touch_y < 1.f) ? s.aim_touch_y : -1.f;
    cfg::esp::box_col          = s.esp_box_col;
    cfg::esp::box_col_invis    = s.esp_box_col_invis;
    cfg::esp::name_col         = s.esp_name_col;
    cfg::esp::distance_col     = s.esp_distance_col;
    cfg::esp::weapon_col       = s.esp_weapon_col;
    cfg::esp::tracer_col       = s.esp_tracer_col;
    cfg::esp::skeleton_col     = s.esp_skeleton_col;
    cfg::esp::animal_col       = s.esp_animal_col;
    cfg::esp::loot_col         = s.esp_loot_col;
    // The ally colour reuses a slot that older configs left pure black.
    if (s.esp_ally_col.x + s.esp_ally_col.y + s.esp_ally_col.z > 0.05f)
        cfg::esp::ally_col     = s.esp_ally_col;
    cfg::esp::box_type         = s.esp_box_type;
    cfg::esp::box_rounding     = s.esp_box_rounding;
    // ---- Санитизация: конфиг кочует между билдами с одним version=4 ------
    // Слоты годами переименовывались «на месте», поэтому в принятом поле
    // может лежать байт чужого смысла: NaN, значение старого диапазона,
    // alpha 0 у цвета. Раньше проверялась только часть полей — отсюда
    // жалобы «у одного аим мёртв, у другого скелеты не видно». Тут всё,
    // что дальше читают фичи, приводится к диапазону UI-слайдера;
    // NaN/вне-диапазона → дефолт слайдера. Мёртвые слоты (box_type,
    // box_rounding, fill_pct, stroke/rounding в cfg::esp) ни одна фича не
    // читает — их не трогаем.
    {
        auto num = [](float v, float lo, float hi, float deflt) {
            return (std::isfinite(v) && v >= lo && v <= hi) ? v : deflt;
        };
        g_state.esp_thick = num(g_state.esp_thick, 0.5f, 5.f, 2.f);
        g_state.gun_str = num(g_state.gun_str, 1.f, 10.f, 5.f);
        cfg::aim::fov = num(cfg::aim::fov, 5.f, 180.f, 80.f);
        g_state.gun_trigger_delay = num(g_state.gun_trigger_delay, 0.f, 1.f, 0.f);
        if (g_state.aim_bone < 0 || g_state.aim_bone > 2) g_state.aim_bone = 0;
        cfg::aim::bone = g_state.aim_bone;
        // Цвет: каналы 0..1; alpha 0 из старого слота делал фичу «включена,
        // но невидима» (классика у скелетов) — поднимаем до непрозрачной.
        auto col = [&num](ImVec4& c) {
            c.x = num(c.x, 0.f, 1.f, 1.f);
            c.y = num(c.y, 0.f, 1.f, 1.f);
            c.z = num(c.z, 0.f, 1.f, 1.f);
            if (!std::isfinite(c.w) || c.w <= 0.02f || c.w > 1.f) c.w = 1.f;
        };
        col(cfg::esp::box_col);       col(cfg::esp::box_col_invis);
        col(cfg::esp::name_col);      col(cfg::esp::distance_col);
        col(cfg::esp::weapon_col);    col(cfg::esp::tracer_col);
        col(cfg::esp::skeleton_col);  col(cfg::esp::animal_col);
        col(cfg::esp::loot_col);      col(cfg::esp::ally_col);
        col(cfg::esp::pickup_col);
        // bool-байты из блоба могут нести не 0/1 (в старых версиях там были
        // иные поля): чтение bool с таким представлением формально UB.
        // Приводим к 0/1 через байт, не доверяя g_state значению как bool.
        auto b01 = [](bool& b) { b = *reinterpret_cast<const unsigned char*>(&b) != 0; };
        b01(g_state.aim_touch); b01(g_state.aim_special); b01(g_state.aim_scope_only);
        b01(cfg::aim::draw_fov);
        b01(g_state.esp_box); b01(g_state.esp_name); b01(g_state.esp_wall);
        b01(g_state.esp_chams); b01(g_state.esp_weapon); b01(g_state.esp_tracer);
        b01(g_state.esp_skeleton); b01(g_state.esp_ore); b01(g_state.esp_animal);
        b01(g_state.esp_team); b01(g_state.ui_dark_mode);
    }
    g_darkTheme = g_state.ui_dark_mode;
    snprintf(g_loadedConfigName, sizeof(g_loadedConfigName), "%s", g_configs[idx].name);
    g_cfgLoadedIdx = idx;
    for (int i = 0; i < kMaxConfigs; i++) g_cfgLoadAnim[i] = 0.f;
    if (announce) {
        ShowToast(XS("Конфиг загружен"));
        PlaySound(SND_SUCCESS);
    }
}

void ConfigLoadLast() {
    time_t best_mtime = 0;
    int best = -1;
    for (int i = 0; i < g_configCount; i++) {
        struct stat st{};
        if (stat(CfgPath(g_configs[i].name).c_str(), &st) != 0) continue;
        if (best < 0 || st.st_mtime >= best_mtime) {
            best_mtime = st.st_mtime;
            best = i;
        }
    }
    if (best >= 0) ConfigLoad(best, false);
}

void ConfigDelete(int idx) {
    if (idx < 0 || idx >= g_configCount) return;
    std::string path = CfgPath(g_configs[idx].name);
    if (strcmp(g_configs[idx].name, g_loadedConfigName) == 0) {
        g_loadedConfigName[0] = '\0';
        g_cfgLoadedIdx = -1;
        for (int i = 0; i < kMaxConfigs; i++) g_cfgLoadAnim[i] = 0.f;
    }
    remove(path.c_str());
    CfgScanDir();
    ShowToast(XS("Конфиг удалён"));
    PlaySound(SND_CLICK);
}

const char* CfgDir() { return kCfgDir; }
