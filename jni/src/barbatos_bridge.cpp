#include "barbatos_bridge.h"

#include "barbatos_gui.h"
#include "app_state.h"
#include "cfg.h"
#include "config.h"
#include "hud.h"
#include "lang.h"
#include "process.h"
#include "media/barbatos_assets.h"

#include <GLES3/gl3.h>

#include <algorithm>
#include <cstdint>
#include <cstring>

// Texture decoding stays in menu.cpp, which already owns the stb_image
// implementation used by the original interface assets.
extern GLuint LoadMenuTextureFromMemory(const unsigned char* data, int length);

namespace {

barbatos_gui::GuiState g_gui;
barbatos_gui::TextureRegistry g_textures;
bool g_textures_loaded = false;
bool g_items_initialized = false;

using barbatos_gui::AimPoint;
using barbatos_gui::AssetId;
using barbatos_gui::ItemEntry;

ImTextureID LoadAsset(const unsigned char* data, unsigned int length) {
    return static_cast<ImTextureID>(LoadMenuTextureFromMemory(data, static_cast<int>(length)));
}

AimPoint ToAimPoint(int bone) {
    switch (bone) {
        case 0: return AimPoint::Head;
        case 2: return AimPoint::Pelvis;
        default: return AimPoint::Chest;
    }
}

int FromAimPoint(AimPoint point) {
    switch (point) {
        case AimPoint::Head: return 0;
        case AimPoint::Pelvis: return 2;
        default: return 1;
    }
}

float ToPercent(float value, float low, float high) {
    if (high <= low) return 0.0f;
    return std::clamp((value - low) * 100.0f / (high - low), 0.0f, 100.0f);
}

float FromPercent(float value, float low, float high) {
    return low + std::clamp(value, 0.0f, 100.0f) * (high - low) / 100.0f;
}

void InitializeItems() {
    if (g_items_initialized) return;
    g_items_initialized = true;

    // These rows expose the project's marker categories through the recovered
    // Barbatos "custom classes" surface. Names remain editable in the GUI.
    auto& items = g_gui.items.custom_items;
    items = {
        {"Ore",      "Ore",      cfg::esp::ore_stone_col},
        {"Animals",  "Animals",  cfg::esp::animal_col},
        {"Crates",   "Crates",   cfg::esp::loot_col},
        {"Pickups",  "Pickups",  cfg::esp::pickup_col},
        {"Bots",     "Bots",     cfg::esp::bot_col},
        {"Vehicles", "Vehicles", cfg::esp::vehicle_col},
        {"Scrap",    "Scrap",    cfg::esp::scrap_col},
    };
}

void PullHostState() {
    InitializeItems();

    g_gui.visible = menu_open;

    auto& aim = g_gui.aim;
    aim.aiming = g_state.aim_touch;
    aim.aim_position = g_state.aim_pos;
    aim.aim_point = ToAimPoint(g_state.aim_bone);
    aim.circle_aim = g_state.aim_special;
    aim.follow_gun_speed = ToPercent(g_state.gun_str, 1.0f, 10.0f);
    aim.all_aim_max_distance = g_state.gun_fov;

    auto& draw = g_gui.draw;
    draw.rect_box = g_state.esp_box;
    draw.line = g_state.esp_tracer;
    draw.skeleton = g_state.esp_skeleton;
    draw.name_and_distance = g_state.esp_name;
    draw.health = g_state.esp_wall;
    draw.player_weapon = g_state.esp_weapon;
    draw.box_class = g_state.esp_loot;
    draw.vehicle_class = g_state.esp_vehicles;
    draw.airdrop_class = g_state.esp_ore;
    draw.metro_crate_class = g_state.esp_pickup;
    draw.radar = g_state.esp_chams;
    draw.player_size = g_state.esp_thick;
    draw.skeleton_max_distance = g_state.marker_dist;
    draw.player_color = cfg::esp::box_col;
    draw.vehicle_color = cfg::esp::vehicle_col;

    auto& items = g_gui.items;
    items.all_items_size = g_state.esp_thick;
    items.all_items_distance = g_state.marker_dist;
    items.all_items_color = cfg::esp::loot_col;
    const bool enabled[] = {
        g_state.esp_ore, g_state.esp_animal, g_state.esp_loot,
        g_state.esp_pickup, g_state.esp_bots, g_state.esp_vehicles,
        g_state.esp_scrap,
    };
    const ImVec4 colors[] = {
        cfg::esp::ore_stone_col, cfg::esp::animal_col, cfg::esp::loot_col,
        cfg::esp::pickup_col, cfg::esp::bot_col, cfg::esp::vehicle_col,
        cfg::esp::scrap_col,
    };
    for (std::size_t i = 0; i < items.custom_items.size() && i < 7; ++i) {
        items.custom_items[i].enabled = enabled[i];
        items.custom_items[i].color = colors[i];
        items.custom_items[i].size = g_state.esp_thick;
        items.custom_items[i].max_distance = g_state.marker_dist;
    }

    // The recovered settings page drives the actual UI accent.  The selected
    // color is persisted through the same .accent path as the previous GUI.
    g_gui.settings.theme_color = g_state.ui_accent_rgb;
    g_gui.settings.pid = static_cast<int>(g_target_pid);
}

void PushHostState() {
    // A page can only edit its own state.  Keeping page-specific writes here
    // prevents the inactive reconstructed pages from overwriting a setting
    // changed on the page currently being rendered.
    if (g_gui.page == barbatos_gui::Page::Aim) {
        const auto& aim = g_gui.aim;
        g_state.aim_touch = aim.aiming;
        g_state.aim_pos = aim.aim_position;
        g_state.aim_bone = FromAimPoint(aim.aim_point);
        g_state.aim_special = aim.circle_aim;
        g_state.gun_str = FromPercent(aim.follow_gun_speed, 1.0f, 10.0f);
        g_state.gun_fov = std::clamp(aim.all_aim_max_distance, 5.0f, 180.0f);
    }

    if (g_gui.page == barbatos_gui::Page::Draw) {
        const auto& draw = g_gui.draw;
        g_state.esp_box = draw.rect_box;
        g_state.esp_tracer = draw.line;
        g_state.esp_skeleton = draw.skeleton;
        g_state.esp_name = draw.name_and_distance;
        g_state.esp_wall = draw.health;
        g_state.esp_weapon = draw.player_weapon;
        g_state.esp_loot = draw.box_class;
        g_state.esp_vehicles = draw.vehicle_class;
        g_state.esp_ore = draw.airdrop_class;
        g_state.esp_pickup = draw.metro_crate_class;
        g_state.esp_chams = draw.radar;
        g_state.esp_thick = std::clamp(draw.player_size, 1.0f, 12.0f);
        g_state.marker_dist = std::clamp(draw.skeleton_max_distance, 25.0f, 300.0f);
        cfg::esp::box_col = draw.player_color;
        cfg::esp::vehicle_col = draw.vehicle_color;
    }

    if (g_gui.page == barbatos_gui::Page::Items) {
        const auto& items = g_gui.items;
        if (items.custom_items.size() >= 7) {
            g_state.esp_ore = items.custom_items[0].enabled;
            g_state.esp_animal = items.custom_items[1].enabled;
            g_state.esp_loot = items.custom_items[2].enabled;
            g_state.esp_pickup = items.custom_items[3].enabled;
            g_state.esp_bots = items.custom_items[4].enabled;
            g_state.esp_vehicles = items.custom_items[5].enabled;
            g_state.esp_scrap = items.custom_items[6].enabled;
            cfg::esp::ore_stone_col = items.custom_items[0].color;
            cfg::esp::animal_col = items.custom_items[1].color;
            cfg::esp::loot_col = items.custom_items[2].color;
            cfg::esp::pickup_col = items.custom_items[3].color;
            cfg::esp::bot_col = items.custom_items[4].color;
            cfg::esp::vehicle_col = items.custom_items[5].color;
            cfg::esp::scrap_col = items.custom_items[6].color;
        }
    }

    if (g_gui.page == barbatos_gui::Page::Settings) {
        const ImVec4 color = g_gui.settings.theme_color;
        if (color.x != g_state.ui_accent_rgb.x || color.y != g_state.ui_accent_rgb.y ||
            color.z != g_state.ui_accent_rgb.z) {
            g_state.ui_accent_rgb = ImVec4(color.x, color.y, color.z, 1.0f);
            g_state.ui_accent_idx = 8; // custom accent
            RememberAccent();
        }

        // Barbatos offers English and Simplified Chinese; map English to the
        // host's existing English switch and retain Russian for the other
        // choice.  Persist only when the user actually changes this option.
        const lang::Lang selected = g_gui.settings.language == barbatos_gui::Language::English
                                        ? lang::LANG_EN
                                        : lang::LANG_RU;
        if (lang::current() != selected) {
            lang::set(selected);
            RememberLang();
        }
    }

    if (!g_gui.visible) menu_open = false;

    if (g_gui.settings.request_exit_and_save) {
        g_gui.settings.request_exit_and_save = false;
        ConfigSave();
        main_thread_flag.store(false);
    }
    if (g_gui.settings.request_kernel_exit) {
        g_gui.settings.request_kernel_exit = false;
        main_thread_flag.store(false);
    }
}

} // namespace

void LoadBarbatosTextures() {
    if (g_textures_loaded) return;
    g_textures_loaded = true;

    g_textures.images[static_cast<std::size_t>(AssetId::Profile)] =
        LoadAsset(barbatos_profile_png, barbatos_profile_png_len);
    g_textures.images[static_cast<std::size_t>(AssetId::DrawEye)] =
        LoadAsset(barbatos_draw_png, barbatos_draw_png_len);
    g_textures.images[static_cast<std::size_t>(AssetId::AimTarget)] =
        LoadAsset(barbatos_aim_png, barbatos_aim_png_len);
    g_textures.images[static_cast<std::size_t>(AssetId::ItemsCode)] =
        LoadAsset(barbatos_items_png, barbatos_items_png_len);
    g_textures.images[static_cast<std::size_t>(AssetId::Settings)] =
        LoadAsset(barbatos_settings_png, barbatos_settings_png_len);
    g_textures.images[static_cast<std::size_t>(AssetId::Character)] =
        LoadAsset(barbatos_character_png, barbatos_character_png_len);
    g_textures.images[static_cast<std::size_t>(AssetId::Avatar)] =
        LoadAsset(barbatos_avatar_png, barbatos_avatar_png_len);
}

void RenderBarbatosMenu() {
    PullHostState();
    barbatos_gui::RenderBarbatosGui(g_gui, g_textures);
    PushHostState();
}
