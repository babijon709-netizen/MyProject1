// ui/config.cpp — Конфиги: файлы, XOR, слежение за каталогом.
//
// Модуль разрезан из прежнего монолита jni/src/main.cpp;
// что здесь лежит и кто это зовёт — в шапке ui/config.h и в docs/CODE_MAP.md.


#include "app/common.h"
#include "app/attach.h"
#include "ui/layout.h"
#include "ui/popover.h"
#include "ui/settings.h"
#include "ui/tabs.h"
#include "ui/theme.h"
#include "ui/toast.h"
#include "ui/window.h"
#include "ui/config.h"

static constexpr uint8_t kXorKey   = 0xA7;

const char* kCfgDir_() noexcept {
    static constexpr auto _s = xp::_mk("/storage/emulated/0/benzhack/");
    return _s.d();
}

#define kCfgDir (kCfgDir_())

ConfigEntry g_configs[kMaxConfigs] = {};

int         g_configCount    = 0;

int         g_configToDelete = -1;

static char        g_loadedConfigName[64] = {};

float       g_cfgLoadAnim[kMaxConfigs] = {};

int         g_cfgLoadedIdx = -1;

static std::string CfgPath(const char* name) {
    return std::string(kCfgDir) + name + XS(".cfg");
}

static std::string CfgLastPath() {
    return std::string(kCfgDir) + XS(".last");
}

static void RememberLastConfigName(const char* name) {
    if (!name || !name[0]) return;
    mkdir(kCfgDir, 0777);
    std::ofstream f(CfgLastPath(), std::ios::trunc);
    if (f) f << name;
}

static void ForgetLastConfigName() {
    remove(CfgLastPath().c_str());
}

// Язык интерфейса лежит рядом с конфигами отдельным файлом (.lang): выбор
// нужен и без конфига, иначе он терялся бы при каждом запуске. Конфиг тоже
// несёт язык — при загрузке конфиг побеждает и переписывает этот файл.
static std::string CfgLangPath() {
    return std::string(kCfgDir) + ".lang";
}

void RememberLang() {
    mkdir(kCfgDir, 0777);
    std::ofstream f(CfgLangPath(), std::ios::trunc);
    if (f) f << (lang::english() ? "en" : "ru");
}

void RestoreLang() {
    std::ifstream f(CfgLangPath());
    if (!f) return;
    std::string v;
    std::getline(f, v);
    if (!v.empty() && v[0] == 'e') lang::set(lang::LANG_EN);
}

// Оффсеты релиза и беты разные (jni/src/game_offsets_beta.h), и версия
// выбирается в меню. Выбор хранится рядом с конфигами отдельным файлом, как
// язык: он нужен ещё до загрузки любого конфига — от него зависит, по какой
// раскладке читать память игры и к какому пакету подключаться.
//
static std::string CfgBuildPath() {
    return std::string(kCfgDir) + ".build";
}

static void RememberBuild() {
    mkdir(kCfgDir, 0777);
    std::ofstream f(CfgBuildPath(), std::ios::trunc);
    if (f) f << (go::CurrentBuild() == go::Build::Beta ? "beta" : "release");
}

void RestoreBuild() {
    std::ifstream f(CfgBuildPath());
    if (!f) return;
    std::string v;
    std::getline(f, v);
    if (!v.empty() && v[0] == 'b') go::SelectBuild(go::Build::Beta);
}

// Применить выбор версии: подставить оффсеты выбранного клиента и, если версия
// реально сменилась, переподключиться. Всё, что было вычитано по прежней
// раскладке (классы, инстансы, кеши игроков), после смены недействительно —
// поэтому esp_reset(), а поток привязки подключится заново сам.
void ApplyBuildChoice(go::Build b) {
    if (b == go::Build::Beta && !go::BetaAvailable()) b = go::Build::Release;
    const bool changed = (go::CurrentBuild() != b);
    go::SelectBuild(b);
    RememberBuild();
    if (changed) {
        esp_reset();
        g_esp_attached = false;
        g_target_pid = -1;
        char _b[160];
        snprintf(_b, sizeof(_b), "%s|%s", XS("Версия игры"),
                 b == go::Build::Beta ? XS("Бета") : XS("Релиз"));
        ShowToast(_b);
    }
    PlaySound(SND_CLICK);
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
    //   x = bit 0 loot ESP, bit 1 pickup ESP,
    //   y = marker draw distance (m),
    //   z = aim priority + 1 (so an old config's 1.0 still means "default"),
    //       plus 8 when the aim mode is "Мемори" — see below,
    //   w = pickup colour packed as r*65536 + g*256 + b (exact in a float,
    //       since 0xFFFFFF < 2^24); <= 1 means "never written, use default".
    ImVec4 esp_extra;
    int   esp_box_type;
    float esp_box_rounding;
};

static void ConfigSaveToPath(const std::string& path) {
    CfgBlob s;
    s.magic   = 0x58564345U;
    s.version = 4;
    s.aim_touch   = g_state.aim_touch;   s.aim_pos     = g_state.aim_pos;
    s.aim_special = g_state.aim_special;
    s.aim_bone    = g_state.aim_bone;
    s.aim_vis_check  = cfg::aim::vis_check;
    s.aim_draw_fov   = cfg::aim::draw_fov;
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
        const int flags = (g_state.esp_loot ? 1 : 0) | (g_state.esp_pickup ? 2 : 0);
        auto ch = [](float v) { int i = (int)(v * 255.f + 0.5f); return i < 0 ? 0 : (i > 255 ? 255 : i); };
        const float packed = (float)(ch(cfg::esp::pickup_col.x) * 65536
                                   + ch(cfg::esp::pickup_col.y) * 256
                                   + ch(cfg::esp::pickup_col.z));
        // z: приоритет цели плюс режим аима отдельным разрядом (8 = «Мемори»).
        // Разряд свободен по построению: в старых конфигах здесь лежала альфа
        // прежнего пинга (0..1), а с тех пор как сюда упаковали приоритет, тут
        // бывает только 1..3. Значит «>= 8» не может приехать ни из одного
        // конфига, записанного до появления режимов, и включение «Мемори» — это
        // всегда выбор пользователя, а не унаследованное значение.
        const int packedZ = g_state.aim_priority + 1 + (g_state.aim_mode == 1 ? 8 : 0);
        s.esp_extra = {(float)flags, g_state.marker_dist, (float)packedZ, packed};
    }
    s.esp_box_type         = cfg::esp::box_type;
    s.esp_box_rounding     = cfg::esp::box_rounding;
    uint8_t buf[sizeof(s)];
    memcpy(buf, &s, sizeof(s));
    XorBuf(buf, sizeof(s));
    std::ofstream f(path, std::ios::binary);
    if (f) { f.write((char*)buf, sizeof(buf)); f.close(); }
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
    RememberLastConfigName(baseName);
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
    f.close();
    if (got != sizeof(buf)) { if (announce) ShowToast(XS("Несовместимый конфиг")); return; }
    XorBuf(buf, sizeof(s));
    memcpy(&s, buf, sizeof(s));
    if (s.magic != 0x58564345U || s.version != 4) { if (announce) ShowToast(XS("Старый конфиг — пересохрани")); return; }

    g_state.aim_touch   = s.aim_touch;
    // «Только видимых» убран из меню — значение из конфига игнорируется.
    g_state.aim_pos     = false;
    g_state.aim_special = s.aim_special;
    g_state.aim_scope_only = s.aim_scope_only;
    cfg::aim::scope_only   = s.aim_scope_only;
    g_state.aim_bone    = s.aim_bone;
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
        const int flags = (s.esp_extra.x >= 0.f && s.esp_extra.x < 4.f) ? (int)(s.esp_extra.x + 0.5f) : 0;
        g_state.esp_loot   = (flags & 1) != 0;
        g_state.esp_pickup = (flags & 2) != 0;
        g_state.marker_dist = (s.esp_extra.y >= 25.f && s.esp_extra.y <= 300.f) ? s.esp_extra.y : 150.f;
        int pr = (int)(s.esp_extra.z + 0.5f) - 1;
        // Разряд 8 — режим аима (см. ConfigSaveToPath). Всё, что не 8..10, —
        // старое значение: приоритет по умолчанию, режим «Тач».
        g_state.aim_mode = (pr >= 8 && pr <= 10) ? 1 : 0;
        if (g_state.aim_mode) pr -= 8;
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
    RememberLang();
    // ui_show_sep из конфига игнорируется: рамки карточек всегда включены.
    g_state.ui_fps      = false;
    g_state.ui_show_sep = true;
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
    g_darkTheme = g_state.ui_dark_mode;
    snprintf(g_loadedConfigName, sizeof(g_loadedConfigName), "%s", g_configs[idx].name);
    g_cfgLoadedIdx = idx;
    for (int i = 0; i < kMaxConfigs; i++) g_cfgLoadAnim[i] = 0.f;
    RememberLastConfigName(g_configs[idx].name);
    if (announce) {
        ShowToast(XS("Конфиг загружен"));
        PlaySound(SND_SUCCESS);
    }
}

void ConfigLoadLast() {
    std::ifstream f(CfgLastPath());
    std::string name;
    if (f) {
        std::getline(f, name);
        while (!name.empty() && (name.back() == '\n' || name.back() == '\r' || name.back() == ' '))
            name.pop_back();
    }
    if (!name.empty()) {
        for (int i = 0; i < g_configCount; i++) {
            if (strcmp(g_configs[i].name, name.c_str()) == 0) {
                ConfigLoad(i, false);
                return;
            }
        }
    }
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
