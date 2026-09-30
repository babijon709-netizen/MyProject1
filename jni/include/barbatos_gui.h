#pragma once

// GUI-only reconstruction of the stripped ARM64 payload embedded in barbatos.sh.
// The interface mirrors recovered pages, controls, dialogs and picker branches.
// No process access, game hooks, networking, updater, authentication, aim or ESP
// implementation is included. The host application owns all side effects.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "ImGui/imgui.h"

namespace barbatos_gui {

enum class Page : int { Home = 0, Draw = 1, Aim = 2, Items = 3, Settings = 4 };
enum class Language : int { SimplifiedChinese = 0, English = 1 };
enum class AimPoint : int { Head = 0, Chest = 1, Pelvis = 2 };
enum class AimSpeed : int { Fast = 0, Average = 1, Slow = 2 };
enum class AimFunctionPanel : int { Main = 2, StopScopeFlash = 11, ContinuousClick = 22, Sniper = 33 };
enum class HealthType : int { Circular = 0, RectOne = 1, RectTwo = 2, Formatted = 3, Wide = 4 };
enum class GameDataMode : int { OfficialBaseChain = 0, SelfStructure = 1, Manual = 2 };
enum class DynamicStyle : int { MacOSMagic = 0, IpadOSDispersion = 1 };

// IDs follow assets/manifest.csv and the loader order recovered at 0x1789ac.
enum class AssetId : std::size_t {
    Profile = 0, DrawEye, AimTarget, ItemsCode, Settings,
    Players, Vehicles, DirectionArrows, HalfCircle, AddPlayer,
    Hidden, Crosshair, Skeleton, Grid, Folder,
    BoxScope, AimScope, DotsGrid, InfectionTarget, Weapons,
    Sniper, CodeWindow, Checklist, Info, Menu,
    SystemSettings, Language, ColorPicker, Power, CollapseUp,
    ColorWheel, Linux, Android, Check, Radar,
    RandomSelector, Sliders, Character, Mascot, Avatar,
    Count
};

struct TextureRegistry {
    std::array<ImTextureID, static_cast<std::size_t>(AssetId::Count)> images{};
    ImTextureID Get(AssetId id) const { return images[static_cast<std::size_t>(id)]; }
};

struct UpdateDialogState {
    bool open = false;
    bool mandatory = false;
    bool downloading = false;
    bool request_download = false;
    bool request_confirm = false;
    bool request_use_update = false;
    bool completed = false;
    bool starting = false;
    bool fetching_size = false;
    std::string current_version = "0.00";
    std::string new_version = "0.00";
    std::string message;
    float download_size_mb = 0.0f;
    float progress = 0.0f;
};

struct AimState {
    bool only_draw = false;
    bool aiming = false;
    bool aim_position = false;
    AimPoint aim_point = AimPoint::Chest;
    AimFunctionPanel panel = AimFunctionPanel::Main;

    // Main Aim overlay, recovered case 2.
    bool circle_aim = false;
    bool finger_interference_aim = false;
    bool hit_aim = false;
    bool not_aim_lying_down = false;
    bool not_aim_ai = false;
    bool auxiliary_only_draw = false;

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

    // Continuous Click Aiming, recovered case 22.
    bool continuous_click_aiming = false;
    bool move_game_shoot_button = false;
    bool run_when_target_in_range = false;
    float seconds_per_click = 0.25f;
    float speed_seconds = 0.25f;
    int click_weapon_id = 0;
    bool request_write_click_weapon_id = false;

    // Stop-firing scope-flash, recovered case 11.
    bool not_shoot_auto_aiming = false;
    bool move_game_aim_button = false;
    bool run_after_five_bullets = false;
    AimSpeed speed_type = AimSpeed::Average;

    // Sniper Aim, recovered case 33.
    bool sniper_rifle = false;
    bool move_shoot_button = false;
    bool auto_fire_after_aim_bug = false;
    int sniper_weapon_id = 0;
    bool request_write_sniper_weapon_id = false;
};

struct DrawState {
    // Player Class Draw [Long Down Adjust].
    bool rect_box = false;
    bool line = false;
    bool skeleton = false;
    bool name_and_distance = false;
    bool health = false;
    bool player_weapon = false;

    // Not Player Class Draw [Long Down Adjust].
    bool box_class = false;
    bool vehicle_class = false;
    bool airdrop_class = false;
    bool metro_crate_class = false;

    // Else Class Draw [Long Down Adjust].
    bool radar = false;
    bool aimed_warning = false;
    bool back_enemy_warning = false;
    bool not_draw_ai = false;
    bool disable_color_dynamic = false;

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
    bool show_more_colors = false;
};

struct ItemEntry {
    std::string class_name;
    std::string display_name;
    ImVec4 color = ImVec4(0.95f, 0.75f, 0.20f, 1.0f);
    bool enabled = true;
    float size = 18.0f;
    float max_distance = 100.0f;
    bool editor_open = false;
};

struct ItemsState {
    bool open_class_name = false;
    bool developer_mode = false;
    bool my_items_mate = false;
    float all_items_size = 18.0f;
    float all_items_distance = 100.0f;
    ImVec4 all_items_color = ImVec4(0.95f, 0.75f, 0.20f, 1.0f);
    bool show_more_colors = false;
    std::vector<ItemEntry> custom_items;
};

// GUI-only model of the recovered /storage/emulated/0 picker. The host may fill
// entries and react to selection; this library intentionally performs no I/O.
struct FilePickerState {
    bool open = false;
    std::string current_directory = "/storage/emulated/0";
    std::vector<std::string> directories;
    std::vector<std::string> files;
    int selected_index = -1;
};

struct SettingsState {
    bool leave_process_running = false;
    bool not_screen_recording = false;
    Language language = Language::English;

    int selected_panel = 0; // 0 main, 1 theme, 2 island, 3 photo, 4 text, 5 dynamic, 6 GameData.
    bool request_kernel_exit = false;
    bool request_exit_and_save = false;

    ImVec4 theme_color = ImVec4(0.38f, 0.24f, 0.96f, 1.00f);
    float island_size = 1.0f;
    bool change_photo = false;
    bool change_text = false;
    bool dynamic_island = false;
    bool island_dynamic_test = false;
    DynamicStyle dynamic_style = DynamicStyle::MacOSMagic;
    std::array<bool, 6> dynamic_tasks{{false, false, false, false, false, false}};
    std::string dynamic_long_text = u8"岛二[超长文本啊啊啊啊啊]";

    bool restore_original_text = false;
    bool load_from_cloud = false;
    bool use_font_directory = false;
    bool restore_original_picture = false;
    bool use_picture_directory = false;
    bool request_set_picture_top_left = false;

    std::string selected_font_path;
    std::string selected_picture_path;
    std::vector<std::string> cloud_fonts;
    FilePickerState font_picker;
    FilePickerState picture_picker;
    float preview_progress = 0.0f;

    // GameData overlay recovered under selector value 0x309.
    int pid = 0;
    std::uintptr_t libue4 = 0;
    std::uintptr_t matrix = 0;
    std::uintptr_t self = 0;
    std::uintptr_t array_address = 0;
    int array_count = 0;
    std::uintptr_t self_address = 0;
    int automatic_index = 0;
    GameDataMode game_data_mode = GameDataMode::OfficialBaseChain;
    bool request_open_pointer_table = false;
    std::vector<std::uintptr_t> pointer_table;
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

// Render the reconstructed interface. Caller owns ImGui frame lifecycle,
// textures and all actions requested through state flags.
void RenderBarbatosGui(GuiState& state, const TextureRegistry& textures);

} // namespace barbatos_gui
