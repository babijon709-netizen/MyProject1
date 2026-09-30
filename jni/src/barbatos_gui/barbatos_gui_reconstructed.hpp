#pragma once

// Reconstructed GUI-only interface for barbatos.sh.
// Requires Dear ImGui. No process access, game hooks, networking, updater,
// authentication, or aim/ESP implementation is included.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "imgui.h"

namespace barbatos_gui {

enum class Page : int {
    Home = 0,
    Draw = 1,
    Aim = 2,
    Items = 3,
    Settings = 4,
};

enum class Language : int {
    SimplifiedChinese = 0,
    English = 1,
};

enum class AimPoint : int {
    Head = 0,
    Chest = 1,
    Pelvis = 2,
};

enum class AimSpeed : int {
    Fast = 0,
    Average = 1,
    Slow = 2,
};

enum class HealthType : int {
    Circular = 0,
    RectOne = 1,
    RectTwo = 2,
    Formatted = 3,
    Wide = 4,
};

// IDs match assets/manifest.csv and the order in the unpacked ELF.
enum class AssetId : std::size_t {
    Profile = 0,
    DrawEye = 1,
    AimTarget = 2,
    ItemsCode = 3,
    Settings = 4,
    Players = 5,
    Vehicles = 6,
    DirectionArrows = 7,
    HalfCircle = 8,
    AddPlayer = 9,
    Hidden = 10,
    Crosshair = 11,
    Skeleton = 12,
    Grid = 13,
    Folder = 14,
    BoxScope = 15,
    AimScope = 16,
    DotsGrid = 17,
    InfectionTarget = 18,
    Weapons = 19,
    Sniper = 20,
    CodeWindow = 21,
    Checklist = 22,
    Info = 23,
    Menu = 24,
    SystemSettings = 25,
    Language = 26,
    ColorPicker = 27,
    Power = 28,
    CollapseUp = 29,
    ColorWheel = 30,
    Linux = 31,
    Android = 32,
    Check = 33,
    Radar = 34,
    RandomSelector = 35,
    Sliders = 36,
    Character = 37,
    Mascot = 38,
    Avatar = 39,
    Count = 40,
};

struct TextureRegistry {
    std::array<ImTextureID, static_cast<std::size_t>(AssetId::Count)> images{};

    ImTextureID Get(AssetId id) const {
        return images[static_cast<std::size_t>(id)];
    }
};

struct UpdateDialogState {
    bool open = false;
    bool mandatory = false;
    bool downloading = false;
    std::string current_version = "0.00";
    std::string new_version = "0.00";
    std::string message;
    float download_size_mb = 0.0f;
    float progress = 0.0f;
};

struct AimState {
    bool only_draw = false;
    bool main_aim = false;
    bool other_aim_functions = false;
    AimPoint aim_point = AimPoint::Chest;

    bool continuous_click_aiming = false;
    bool circle_aim = false;
    bool finger_interference_aim = false;
    bool hit_aim = false;
    bool not_aim_lying_down = false;
    bool not_aim_ai = false;

    float vertical_recoil_reduction = 0.0f;
    float follow_gun_speed = 50.0f;
    float aim_steadiness = 50.0f;
    float all_vertical_recoil_reduction = 0.0f;
    float horizontal_range = 150.0f;
    float vertical_range = 150.0f;
    float extra_parameters = 0.0f;
    float hip_fire_max_distance = 100.0f;
    float all_aim_max_distance = 300.0f;
    float limiting_distance = 100.0f;

    bool move_game_shoot_button = false;
    bool run_when_target_in_range = false;
    float seconds_per_click = 0.25f;
    float speed_seconds = 0.25f;
    int weapon_id = 0;

    bool not_shoot_auto_aiming = false;
    bool move_game_aim_button = false;
    bool run_after_five_bullets = false;
    AimSpeed speed_type = AimSpeed::Average;
    bool sniper_rifle = false;
    bool move_shoot_button = false;
    bool null_target = false;
};

struct DrawState {
    bool rect_box = false;
    bool line = false;
    bool skeleton = false;
    bool name_and_distance = false;
    bool health = false;
    bool player_weapon = false;

    bool player_class_draw = false;
    bool non_player_class_draw = false;
    bool not_draw_ai = false;
    bool disable_color_dynamic = false;
    bool other_class_draw = false;

    float player_size = 18.0f;
    float skeleton_max_distance = 400.0f;
    float player_adjust = 0.0f;
    ImVec4 player_color = ImVec4(0.20f, 0.85f, 1.00f, 1.00f);

    bool draw_vehicle_state = false;
    bool draw_vehicle_state_bug = false;
    float vehicle_size = 18.0f;
    float vehicle_adjust = 0.0f;
    ImVec4 vehicle_color = ImVec4(1.00f, 0.80f, 0.15f, 1.00f);

    float radar_angle = 90.0f;
    float radar_adjust = 0.0f;
    bool radar_position_mode = false;
    bool radar_position_and_size = false;

    float aimed_at_tip_size = 18.0f;
    float aimed_at_tip_adjust = 0.0f;
    ImVec4 aimed_at_tip_color = ImVec4(1.00f, 0.20f, 0.25f, 1.00f);

    float health_transparency = 1.0f;
    HealthType health_type = HealthType::Circular;
    ImVec4 health_color = ImVec4(0.20f, 1.00f, 0.35f, 1.00f);
    bool same_team_same_color = false;
    bool follow_health_color = false;
};

struct ItemEntry {
    std::string class_name;
    std::string display_name;
    ImVec4 color = ImVec4(0.95f, 0.75f, 0.20f, 1.0f);
    bool enabled = true;
};

struct ItemsState {
    bool open_class_name = false;
    bool developer_mode = false;
    bool my_items_mate = false;
    float all_items_size = 18.0f;
    float all_items_distance = 100.0f;
    ImVec4 all_items_color = ImVec4(0.95f, 0.75f, 0.20f, 1.0f);
    std::vector<ItemEntry> custom_items;
};

struct SettingsState {
    bool leave_process_running = false;
    bool not_screen_recording = false;
    Language language = Language::English;
    int main_tab = 0; // Main settings / More.
    int more_tab = 0; // Main / Island / Photo / Text / Dynamic.

    bool look_data = false;
    bool game_data = false;
    bool request_kernel_exit = false;
    bool request_exit_and_save = false;

    bool ipad_dispersion_effect = false;
    bool macos_magic_effect = false;
    float island_size = 1.0f;
    bool change_photo = false;
    bool change_text = false;
    bool dynamic_island = false;
    bool island_dynamic_test = false;

    bool restore_original_text = false;
    bool load_from_cloud = false;
    bool use_file_directory = false;
    bool restore_original_picture = false;

    std::string selected_font_path;
    std::string selected_picture_path;
    float preview_progress = 0.0f;

    // Diagnostic values shown by the recovered settings page.
    int pid = 0;
    std::uintptr_t libue4 = 0;
    std::uintptr_t matrix = 0;
    std::uintptr_t self = 0;
    std::uintptr_t array_address = 0;
    int array_count = 0;
    std::uintptr_t self_address = 0;
};

struct GuiState {
    bool visible = true;
    Page page = Page::Home;
    AimState aim;
    DrawState draw;
    ItemsState items;
    SettingsState settings;
    UpdateDialogState update;
};

// Render the reconstructed GUI. The caller owns the frame lifecycle and textures.
void RenderBarbatosGui(GuiState& state, const TextureRegistry& textures);

} // namespace barbatos_gui
