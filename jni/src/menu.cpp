#include "menu.h"
#include "main.h"
#include "Android_touch/TouchHelperA.h"
#include "game.h"
#include "game_offsets_active.h"
#include "app_state.h"
#include "cfg.h"
#include "theme.h"
#include "ui_util.h"
#include "widgets.h"
#include "config.h"
#include "hud.h"
#include "audio.h"
#include "str.h"
#include "esp_draw.h"
#include "aim.h"
#include "logfile.h"
#include "VidAvatar.h"
#include "imgui_whitecloud.h"
#if __has_include("media/icons.h")
#  include "media/icons.h"
#  define ICONS_AVAILABLE
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include "stb_image/stb_image.h"
#include <cfloat>
#include <cmath>
#include <vector>
#include <string>

static GLuint g_tabIcons[kTabCount] = {};
static GLuint g_nikeLogoTex = 0;

static GLuint LoadTexFromMemory(const unsigned char* data, int len) {
    int w, h, ch;
    unsigned char* px = stbi_load_from_memory(data, len, &w, &h, &ch, 4);
    if (!px) return 0;
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    stbi_image_free(px);
    return tex;
}

void LoadTabIcons() {
#ifdef ICONS_AVAILABLE
    g_tabIcons[0] = LoadTexFromMemory(main_tab_png, (int)main_tab_png_len);
    g_tabIcons[1] = LoadTexFromMemory(aimbot_png,   (int)aimbot_png_len);
    g_tabIcons[2] = LoadTexFromMemory(visuals_png,  (int)visuals_png_len);
    g_tabIcons[3] = 0;
    g_tabIcons[4] = LoadTexFromMemory(misc_png,     (int)misc_png_len);
    g_tabIcons[5] = LoadTexFromMemory(settings_png, (int)settings_png_len);
    g_nikeLogoTex = g_tabIcons[0];
#endif
}

void CenterMenuOnDisplay() {
    float vw = 0.f, vh = 0.f;
    VisibleScreen(vw, vh);
    if (vw > 100.f && vh > 100.f) {
        g_win.pos = ImVec2((vw - 670.0f) * 0.5f, (vh - 470.0f) * 0.5f);
        g_win.tpos = g_win.pos;
        g_win.w = 670.0f;
        g_win.h = 470.0f;
    }
}

// Calibration mode state
int g_calibMode = 0;

static float s_fc_retry = 0.f;
static bool  s_fc_toasted = false;
static int   s_fc_in_frames = 0;
static float s_fc_in_maxdt = 0.f;
static int   s_fc_joy_id = -1, s_fc_look_id = -1, s_fc_up_id = -1, s_fc_down_id = -1;

void Freecam_ResetGrabs() {
    s_fc_joy_id = -1;
    s_fc_look_id = -1;
    s_fc_up_id = -1;
    s_fc_down_id = -1;
}

void UpdateFreecam(float dt) {
    if (g_state.freecam_on != esp_freecam_active()) {
        s_fc_retry -= dt;
        if (s_fc_retry <= 0.f) {
            s_fc_retry = 0.5f;
            if (esp_freecam_set(g_state.freecam_on)) {
                s_fc_toasted = false;
                if (g_state.freecam_on) ShowToast(XS("Фрикам включён"));
            } else if (g_state.freecam_on && !s_fc_toasted) {
                s_fc_toasted = true;
                ShowToast(XS("Фрикам: камера не найдена"));
            }
        }
    } else {
        s_fc_retry = 0.f;
        s_fc_toasted = false;
    }
    float vw = 0.f, vh = 0.f;
    VisibleScreen(vw, vh);
    if (vw < 100.f || vh < 100.f) return;

    if (esp_freecam_active()) {
        ++s_fc_in_frames;
        if (dt > s_fc_in_maxdt) s_fc_in_maxdt = dt;
    }
}

// Main Menu Renderer (Exact NIKE GUI from jni.zip)
void RenderMenu() {
    float vw = 0.f, vh = 0.f;
    VisibleScreen(vw, vh);
    if (vw < 100.f || vh < 100.f) {
        vw = ImGui::GetIO().DisplaySize.x;
        vh = ImGui::GetIO().DisplaySize.y;
    }

    // Initialize snowflakes if not already done
    if (g_nike_snow.empty() && vw > 100.f && vh > 100.f) {
        Snowflake::CreateSnowFlakes(g_nike_snow, 60, 4.0f, 18.0f, 0, 0, (int)vw, (int)vh,
                                   Snowflake::vec3(0.0f, 0.005f), IM_COL32(99, 124, 146, 50));
    }

    // Initialize lyric system
    static bool lyricInited = false;
    if (!lyricInited) {
        InitLyricSystem();
        lyricInited = true;
    }
    UpdateLyricSystem();

    window_set();
    zhu_set();

    const float menuW = 670.0f;
    const float menuH = 470.0f;
    const float menuX = (vw - menuW) * 0.5f;
    const float menuY = (vh - menuH) * 0.5f;

    ImGui::SetNextWindowPos(ImVec2(menuX, menuY), ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2(menuW, menuH), ImGuiCond_Always);

    ImGui::Begin("##NIKE_MainWindow", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar);

    ImVec2 winPos = ImGui::GetWindowPos();
    ImVec2 winSize = ImGui::GetWindowSize();

    // Snowflake particle background update
    Snowflake::Update(g_nike_snow, Snowflake::vec3(ImGui::GetIO().MousePos.x, ImGui::GetIO().MousePos.y),
                      Snowflake::vec3(winPos.x, winPos.y));

    const float leftPanelW = 170.0f;
    const float rightPanelW = winSize.x - leftPanelW - 30.0f;

    // ==========================================
    // LEFT SIDEBAR
    // ==========================================

    // 1. Top Logo Card (190x160)
    ImGui::SetCursorPos(ImVec2(10.0f, 4.0f));
    ImGui::BeginChild("##LeftTopLogo", ImVec2(leftPanelW, 115.0f), true, ImGuiWindowFlags_NoScrollbar);
    DIAN(0.0f, 0.0f, 17.05f, 6.0f);
    ImGui::SetCursorPos(ImVec2(10.0f, 26.0f));
    ImGui::SetWindowFontScale(1.35f);
    ImGui::Text("BYSun");
    ImGui::SetWindowFontScale(0.85f);
    ImGui::SetCursorPos(ImVec2(10.0f, 52.0f));
    ImGui::TextColored(ImColor(155, 155, 155, 225), "NIKE Studio\nMyProject1\nCheat Edition");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::EndChild();

    // 2. Render Status Card (170x60)
    ImGui::SetCursorPos(ImVec2(10.0f, 125.0f));
    ImGui::BeginChild("##RenderCard", ImVec2(leftPanelW, 55.0f), true, ImGuiWindowFlags_NoScrollbar);
    ImGui::SetCursorPos(ImVec2(12.0f, 6.0f));
    ImGui::Text("Draw Fps: %.1f", ImGui::GetIO().Framerate);
    ImGui::SetCursorPos(ImVec2(12.0f, 28.0f));
    ImGui::Text("OpenGL 渲染");
    ImGui::SetCursorPos(ImVec2(120.0f, 28.0f));
    DIAN1(0.0f, 0.0f, 6.0f);
    ImGui::EndChild();

    // 3. Tab Buttons List (170 x H-195)
    ImGui::SetCursorPos(ImVec2(10.0f, 186.0f));
    ImGui::BeginChild("##LeftTabs", ImVec2(leftPanelW, winSize.y - 194.0f), true, ImGuiWindowFlags_NoScrollbar);
    ImGui::Separator();
    if (ImGui::Tabs(g_state.cur_tab == 0, "", "主页", ImVec2(leftPanelW - 12.0f, 44.0f))) {
        g_state.cur_tab = 0;
    }
    ImGui::Separator();
    if (ImGui::Tabs(g_state.cur_tab == 1, "", "绘制", ImVec2(leftPanelW - 12.0f, 44.0f))) {
        g_state.cur_tab = 1;
    }
    ImGui::Separator();
    if (ImGui::Tabs(g_state.cur_tab == 2, "", "辅助", ImVec2(leftPanelW - 12.0f, 44.0f))) {
        g_state.cur_tab = 2;
    }
    ImGui::Separator();
    if (ImGui::Tabs(g_state.cur_tab == 3, "", "设置", ImVec2(leftPanelW - 12.0f, 44.0f))) {
        g_state.cur_tab = 3;
    }
    ImGui::Separator();
    ImGui::EndChild();

    // ==========================================
    // RIGHT PANEL (TAB CONTENT)
    // ==========================================
    const float contentX = leftPanelW + 20.0f;
    const float contentY = 10.0f;

    if (g_state.cur_tab == 0) {
        // TAB 0: 主页 (Home / Notice / Themes / Lyrics)
        ImGui::SetCursorPos(ImVec2(contentX, contentY));
        ImGui::BeginChild("##Tab0_Home", ImVec2(rightPanelW, winSize.y - 20.0f), true);

        // Notice child
        ImGui::BeginChild("##NoticeBox", ImVec2(ImGui::GetContentRegionAvail().x, 100.0f), true, ImGuiWindowFlags_NoScrollbar);
        ImGui::SetWindowFontScale(0.85f);
        ImGui::Text("91工作室旗下所有产品仅供学习与技术参考使用，严禁用于任何商业用途及违法违规场景。\n用户下载、安装、使用本产品的任何操作，均应遵守相关法律法规及行业规范。\n开发者不对用户的任何违规操作承担责任，法律后果由用户自行承担。");
        ImGui::SetWindowFontScale(1.0f);
        ImGui::EndChild();

        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        ImGui::Text("当前版本V1.0");
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 140.0f);
        ImGui::Text("最新版本V1.0");

        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        static std::vector<std::string> theme_names = {"浅色主题", "深色主题"};
        选择条2(theme_names, g_darkTheme, 0, 110.0f, 38.0f);
        SwitchTheme(!g_darkTheme);

        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        ImGui::BeginChild("##LyricsBox", ImVec2(ImGui::GetContentRegionAvail().x, 140.0f), true, ImGuiWindowFlags_NoScrollbar);
        DrawLyrics();
        ImGui::EndChild();

        ImGui::EndChild();

    } else if (g_state.cur_tab == 1) {
        // TAB 1: 绘制 (Visuals / ESP)
        ImGui::SetCursorPos(ImVec2(contentX, contentY));
        ImGui::BeginChild("##Tab1_Visuals", ImVec2(rightPanelW, winSize.y - 20.0f), true);

        // Style bar
        static std::vector<std::string> draw_styles = {"长条", "联赛", "方条"};
        static float cur_draw_style = 0.0f;
        选择条(draw_styles, cur_draw_style, 0, 100.0f, 34.0f);
        ImGui::Dummy(ImVec2(0.0f, 6.0f));

        const float col2X = 240.0f;

        // Row 1
        M_SmallCheckbox("玩家信息", &g_state.esp_name);
        ImGui::SameLine(col2X);
        M_SmallCheckbox("玩家方框", &g_state.esp_box);

        // Row 2
        M_SmallCheckbox("玩家骨骼", &g_state.esp_skeleton);
        ImGui::SameLine(col2X);
        M_SmallCheckbox("玩家手持", &g_state.esp_weapon);

        // Row 3
        M_SmallCheckbox("玩家射线", &g_state.esp_tracer);
        ImGui::SameLine(col2X);
        M_SmallCheckbox("玩家距离", &g_state.esp_wall);

        // Row 4
        M_SmallCheckbox("玩家血量", &g_state.esp_team);
        ImGui::SameLine(col2X);
        M_SmallCheckbox("绘制被瞄", &g_state.esp_chams);

        // Row 5
        M_SmallCheckbox("绘投掷物", &g_state.esp_ore);
        ImGui::SameLine(col2X);
        M_SmallCheckbox("绘制载具", &g_state.esp_vehicles);

        // Row 6
        M_SmallCheckbox("绘制箭头", &g_state.esp_pickup);
        ImGui::SameLine(col2X);
        M_SmallCheckbox("绘制盒子", &g_state.esp_loot);

        // Row 7
        M_SmallCheckbox("被瞄预警", &g_state.esp_house_boxes);
        ImGui::SameLine(col2X);
        M_SmallCheckbox("忽略人鸡", &g_state.esp_bots);

        // Row 8
        M_SmallCheckbox("绘制雷达", &g_state.esp_scrap);
        ImGui::SameLine(col2X);
        M_SmallCheckbox("拖动雷达", &g_state.esp_turrets);

        // Row 9
        M_SmallCheckbox("剩下人数", &g_state.esp_backpacks);
        ImGui::SameLine(col2X);
        M_SmallCheckbox("显示帧率", &g_state.esp_sleepers);

        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        SliderFloat("线条粗细", &g_state.esp_thick, 1.0f, 6.0f, "%.1f");

        // Sync to backend cfg
        cfg::esp::box              = g_state.esp_box;
        cfg::esp::name_esp         = g_state.esp_name;
        cfg::esp::distance         = g_state.esp_wall;
        cfg::esp::weapon           = g_state.esp_weapon;
        cfg::esp::tracer           = g_state.esp_tracer;
        cfg::esp::skeleton         = g_state.esp_skeleton;
        cfg::esp::team             = g_state.esp_team;
        cfg::esp::ore              = g_state.esp_ore;
        cfg::esp::vehicle          = g_state.esp_vehicles;
        cfg::esp::pickup           = g_state.esp_pickup;
        cfg::esp::loot             = g_state.esp_loot;
        cfg::esp::bot              = g_state.esp_bots;
        cfg::esp::scrap            = g_state.esp_scrap;
        cfg::esp::tracer_thickness = g_state.esp_thick;

        ImGui::EndChild();

    } else if (g_state.cur_tab == 2) {
        // TAB 2: 辅助 (Aim / Combat)
        const float sub1W = rightPanelW * 0.54f;
        const float sub2W = rightPanelW - sub1W - 10.0f;
        const float subH1 = (winSize.y - 20.0f) * 0.52f;
        const float subH2 = (winSize.y - 20.0f) * 0.44f;

        // Sub 1: Top-Left Toggles
        ImGui::SetCursorPos(ImVec2(contentX, contentY));
        ImGui::BeginChild("##AimSub1", ImVec2(sub1W, subH1), true);
        M_SmallCheckbox("子弹追踪", &g_state.aim_touch);
        M_SmallCheckbox("漏打自瞄", &g_state.aim_pos);
        M_SmallCheckbox("忽略人机", &g_state.esp_bots);
        M_SmallCheckbox("忽略倒地", &g_state.esp_sleepers);
        M_SmallCheckbox("只开镜瞄", &g_state.aim_scope_only);
        M_SmallCheckbox("显示范围", &g_state.aim_special);
        M_SmallCheckbox("头部判定", &g_state.viewmodel.headshot_body);
        ImGui::EndChild();

        // Sub 2: Right Sliders
        ImGui::SetCursorPos(ImVec2(contentX + sub1W + 10.0f, contentY));
        ImGui::BeginChild("##AimSub2", ImVec2(sub2W, winSize.y - 20.0f), true);
        SliderFloat("追锁范围", &g_state.gun_fov, 10.0f, 1000.0f, "%.0f");
        SliderFloat("平滑力度", &g_state.gun_str, 1.0f, 50.0f, "%.1f");
        SliderFloat("预判力度", &g_state.aim_lead_ms, 0.0f, 100.0f, "%.1f");
        SliderFloat("追踪距离", &g_state.marker_dist, 0.0f, 300.0f, "%.0f");
        ImGui::EndChild();

        // Sub 3: Bottom-Left Targeting Setup
        ImGui::SetCursorPos(ImVec2(contentX, contentY + subH1 + 8.0f));
        ImGui::BeginChild("##AimSub3", ImVec2(sub1W, subH2), true, ImGuiWindowFlags_NoScrollbar);
        title("锁定设置");
        static std::vector<std::string> priority_names = {"准先", "距先"};
        whitecloud(priority_names, g_state.aim_priority, 0, 100.0f, 30.0f);
        ImGui::Dummy(ImVec2(0.0f, 2.0f));
        static std::vector<std::string> aim_mode_names = {"开火", "开镜", "火|镜", "任何"};
        whitecloud(aim_mode_names, g_state.aim_mode, 0, 52.0f, 30.0f);
        ImGui::Dummy(ImVec2(0.0f, 2.0f));
        static std::vector<std::string> bone_names = {"随机", "头部", "胸部", "坤部"};
        whitecloud(bone_names, g_state.aim_bone, 0, 52.0f, 30.0f);
        ImGui::EndChild();

        // Sync to backend cfg
        cfg::aim::enabled    = g_state.aim_touch || g_state.aim_pos;
        cfg::aim::vis_check  = g_state.aim_pos;
        cfg::aim::draw_fov   = g_state.aim_special;
        cfg::aim::scope_only = g_state.aim_scope_only;
        cfg::aim::fov        = g_state.gun_fov;
        cfg::aim::smoothness = g_state.gun_str;
        cfg::aim::bone       = g_state.aim_bone;

    } else if (g_state.cur_tab == 3) {
        // TAB 3: 设置 (Settings / Memory / Configs / Palette)
        ImGui::SetCursorPos(ImVec2(contentX, contentY));
        ImGui::BeginChild("##Tab3_Settings", ImVec2(rightPanelW, winSize.y - 20.0f), true);

        const float col2X = 240.0f;

        bool fps144 = (g_state.viewmodel.fps_limit == 144);
        if (M_SmallCheckbox("强制144帧", &fps144)) {
            g_state.viewmodel.fps_limit = fps144 ? 144 : 0;
        }
        ImGui::SameLine(col2X);
        M_SmallCheckbox("动态广角", &g_state.viewmodel.fov_enabled);

        M_SmallCheckbox("聚点", &g_state.viewmodel.no_spread);
        ImGui::SameLine(col2X);
        M_SmallCheckbox("秒趴", &g_state.viewmodel.fast_loot);

        M_SmallCheckbox("除雾", &g_state.always_day);
        ImGui::SameLine(col2X);
        M_SmallCheckbox("无散弹", &g_state.viewmodel.no_recoil);

        M_SmallCheckbox("快速开火", &g_state.fast_fire);
        ImGui::SameLine(col2X);
        M_SmallCheckbox("快速换弹", &g_state.fast_reload);

        M_SmallCheckbox("透视X射线", &g_state.xray_on);
        ImGui::SameLine(col2X);
        M_SmallCheckbox("自由视角", &g_state.freecam_on);

        M_SmallCheckbox("自动农场", &g_state.farm_on);

        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        title("参数调节");
        SliderFloat("动态广角大小", &g_state.viewmodel.fov, 80.0f, 200.0f, "%.0f");
        SliderFloat("农场搜寻范围", &g_state.farm_range, 10.0f, 300.0f, "%.0f");
        SliderFloat("自由视角速度", &g_state.freecam_speed, 1.0f, 50.0f, "%.1f");

        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        title("配置文件");
        if (ImGui::Button("保存配置 (Save)", ImVec2(140, 34))) {
            ConfigSave();
            ShowToast(XS("Конфиг сохранён"));
        }
        ImGui::SameLine();
        if (ImGui::Button("加载配置 (Load)", ImVec2(140, 34))) {
            ConfigLoad(0);
            ShowToast(XS("Конфиг загружен"));
        }

        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        title("白云调色板");
        白云调色板();

        ImGui::EndChild();
    }

    ImGui::End();
    二代线性插值();
}
