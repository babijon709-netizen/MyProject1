#include "barbatos_gui_reconstructed.hpp"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdio>


namespace barbatos_gui {
namespace {

const ImVec4 kAccent(0.38f, 0.24f, 0.96f, 1.00f);
const ImVec4 kAccentSoft(0.31f, 0.22f, 0.60f, 0.80f);
const ImVec4 kPanel(0.075f, 0.085f, 0.11f, 0.98f);
const ImVec4 kPanelAlt(0.105f, 0.115f, 0.15f, 0.98f);
const ImVec4 kMuted(0.58f, 0.62f, 0.70f, 1.00f);
const ImVec4 kDanger(0.95f, 0.25f, 0.30f, 1.00f);

ImU32 U32(const ImVec4& c) {
    return ImGui::ColorConvertFloat4ToU32(c);
}

void DrawAsset(ImDrawList* dl, ImTextureID texture, const ImVec2& min, const ImVec2& max,
               ImU32 tint = IM_COL32_WHITE) {
    if (texture != ImTextureID{}) {
        dl->AddImage(texture, min, max, ImVec2(0, 0), ImVec2(1, 1), tint);
    }
}

bool NavButton(const char* id, const char* fallback, ImTextureID texture, bool active) {
    ImGui::PushID(id);
    const ImVec2 size(72.0f, 62.0f);
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton("##nav", size);
    const ImVec2 max(min.x + size.x, min.y + size.y);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const bool hovered = ImGui::IsItemHovered();
    const ImVec4 fill = active ? kAccentSoft : hovered ? ImVec4(0.18f, 0.19f, 0.25f, 1.0f)
                                                    : ImVec4(0.10f, 0.11f, 0.14f, 0.0f);
    dl->AddRectFilled(min, max, U32(fill), 14.0f);
    if (active) {
        dl->AddRectFilled(ImVec2(min.x, min.y + 10.0f), ImVec2(min.x + 4.0f, max.y - 10.0f),
                          U32(kAccent), 4.0f);
    }

    if (texture != ImTextureID{}) {
        DrawAsset(dl, texture, ImVec2(min.x + 18.0f, min.y + 12.0f),
                  ImVec2(min.x + 50.0f, min.y + 44.0f));
    } else {
        const ImVec2 ts = ImGui::CalcTextSize(fallback);
        dl->AddText(ImVec2(min.x + (size.x - ts.x) * 0.5f, min.y + 21.0f),
                    IM_COL32(225, 228, 238, 255), fallback);
    }
    ImGui::PopID();
    return pressed;
}

bool Toggle(const char* label, bool* value) {
    const float h = 36.0f;
    const float switch_w = 42.0f;
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const bool pressed = ImGui::InvisibleButton(label, ImVec2(width, h));
    if (pressed) {
        *value = !*value;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool hovered = ImGui::IsItemHovered();
    dl->AddRectFilled(min, ImVec2(min.x + width, min.y + h),
                      hovered ? IM_COL32(41, 44, 56, 210) : IM_COL32(31, 34, 43, 170), 8.0f);
    dl->AddText(ImVec2(min.x + 12.0f, min.y + 9.0f), IM_COL32(225, 228, 238, 255), label);

    const ImVec2 sw_min(min.x + width - switch_w - 10.0f, min.y + 8.0f);
    const ImVec2 sw_max(sw_min.x + switch_w, sw_min.y + 20.0f);
    dl->AddRectFilled(sw_min, sw_max, *value ? U32(kAccent) : IM_COL32(74, 78, 91, 255), 10.0f);
    const float knob_x = *value ? sw_max.x - 10.0f : sw_min.x + 10.0f;
    dl->AddCircleFilled(ImVec2(knob_x, sw_min.y + 10.0f), 7.0f, IM_COL32_WHITE);
    return pressed;
}

bool ActionButton(const char* label, const ImVec2& size = ImVec2(0, 38)) {
    ImGui::PushStyleColor(ImGuiCol_Button, kAccentSoft);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccent);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.30f, 0.18f, 0.78f, 1.0f));
    const bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(3);
    return pressed;
}

void SectionTitle(const char* title, const char* subtitle = nullptr) {
    ImGui::Spacing();
    ImGui::TextColored(kAccent, "%s", title);
    if (subtitle != nullptr) {
        ImGui::SameLine();
        ImGui::TextColored(kMuted, "%s", subtitle);
    }
    ImGui::Separator();
}

void SliderRow(const char* label, float* value, float min, float max, const char* fmt = "%.1f") {
    ImGui::PushID(label);
    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::SliderFloat("##value", value, min, max, fmt);
    ImGui::PopID();
}

void ColorRow(const char* label, ImVec4* color) {
    ImGui::PushID(label);
    ImGui::TextUnformatted(label);
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 44.0f);
    ImGui::ColorEdit4("##color", &color->x,
                      ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaPreviewHalf);
    ImGui::PopID();
}

void RenderHome(const TextureRegistry& textures) {
    if (textures.Get(AssetId::Character) != ImTextureID{}) {
        const float h = std::min(260.0f, ImGui::GetContentRegionAvail().y * 0.44f);
        const float w = h * (1960.0f / 2156.0f);
        ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - w) * 0.5f);
        ImGui::Image(textures.Get(AssetId::Character), ImVec2(w, h));
    }

    SectionTitle("Home", "reconstructed from sub_1E5FE8");
    static const char* kNotices[] = {
        u8"本系列产品无任何收费以及内部等",
        u8"切勿被骗，开发者也不会主动联系你",
        u8"本系列作品仅供学习研究",
        u8"使用者的任何行为与应用作者无关",
        u8"请在24小时内删除本应用 [下载代表同意本协议]",
        u8"若有侵权违法行为,请联系我删除内容",
    };
    for (const char* notice : kNotices) {
        ImGui::BulletText("%s", notice);
    }
}

void RenderAim(AimState& s) {
    SectionTitle("Aiming", "Aim Pos");
    Toggle("Only Draw", &s.only_draw);
    Toggle("Main Aim", &s.main_aim);
    Toggle("Other Aim Func", &s.other_aim_functions);

    const char* points[] = {"Head", "Chest", "Pelvis"};
    int point = static_cast<int>(s.aim_point);
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::Combo("My Aim Player Place", &point, points, 3)) {
        s.aim_point = static_cast<AimPoint>(point);
    }

    SectionTitle("Click Aim");
    Toggle("Continuous Click Aiming", &s.continuous_click_aiming);
    Toggle("Circle Aim", &s.circle_aim);
    Toggle("Finger Interference Aim", &s.finger_interference_aim);
    Toggle("Hit aim", &s.hit_aim);
    Toggle("Not aim lie down", &s.not_aim_lying_down);
    Toggle("Not Ai", &s.not_aim_ai);

    char recoil[96];
    std::snprintf(recoil, sizeof(recoil), "%d Vertical Recoil Reduction",
                  static_cast<int>(s.vertical_recoil_reduction));
    ImGui::TextColored(kMuted, "%s", recoil);
    SliderRow("Follow Gun Speed", &s.follow_gun_speed, 0.0f, 100.0f);
    SliderRow("Aim Steadiness", &s.aim_steadiness, 0.0f, 100.0f);
    SliderRow("All Vertical Recoil Reduction", &s.all_vertical_recoil_reduction, 0.0f, 100.0f);

    SectionTitle("Aim Numerical Value");
    SliderRow("Horizontal Range", &s.horizontal_range, 0.0f, 500.0f);
    SliderRow("Vertical Range", &s.vertical_range, 0.0f, 500.0f);
    SliderRow("Extra parameters", &s.extra_parameters, 0.0f, 100.0f);
    SliderRow("Hip-Fire Aim Max Distance", &s.hip_fire_max_distance, 0.0f, 1000.0f, "%.0f");
    SliderRow("All Aim Max Distance", &s.all_aim_max_distance, 0.0f, 1000.0f, "%.0f");
    SliderRow("Parameters that limit the distance", &s.limiting_distance, 0.0f, 1000.0f, "%.0f");

    if (ImGui::CollapsingHeader("##NewAimPage1", ImGuiTreeNodeFlags_DefaultOpen)) {
        Toggle("Pos[Move to Game Shoot Button]", &s.move_game_shoot_button);
        Toggle("Aim Range Have Player To Run[Not Done]", &s.run_when_target_in_range);
        SliderRow("Second Once", &s.seconds_per_click, 0.01f, 5.0f, "%.2f");
        SliderRow("Speed [Second]", &s.speed_seconds, 0.01f, 5.0f, "%.2f");
        ImGui::InputInt("Write To Weapon ID", &s.weapon_id);

        Toggle("Not Shoot Auto Aiming", &s.not_shoot_auto_aiming);
        Toggle("Pos[Move to Game Aiming Button]", &s.move_game_aim_button);
        Toggle("Shoot Bullet Count > 5 Run", &s.run_after_five_bullets);
        const char* speed[] = {"Fast", "Average speed", "Slow"};
        int speed_index = static_cast<int>(s.speed_type);
        if (ImGui::Combo("Type", &speed_index, speed, 3)) {
            s.speed_type = static_cast<AimSpeed>(speed_index);
        }
        Toggle("Sniper rifle", &s.sniper_rifle);
        Toggle("Pos(Move the shoot button)", &s.move_shoot_button);
        Toggle("nullptr", &s.null_target);
    }
}

void RenderDraw(DrawState& s) {
    SectionTitle("Draw");
    Toggle("Rect Box", &s.rect_box);
    Toggle("Line", &s.line);
    Toggle("Skeleton", &s.skeleton);
    Toggle("Name And Distance", &s.name_and_distance);
    Toggle("Health", &s.health);
    Toggle("Player Weapon", &s.player_weapon);

    SectionTitle("Player classes");
    Toggle("Player Class Draw [Long Down Adjust]", &s.player_class_draw);
    Toggle("Not Player Class Draw [Long Down Adjust]", &s.non_player_class_draw);
    Toggle("Not Draw Ai", &s.not_draw_ai);
    Toggle("Off Color Dynamic", &s.disable_color_dynamic);
    Toggle("Else Class Draw [Long Down Adjust]", &s.other_class_draw);

    SectionTitle("Player draw");
    SliderRow("Size", &s.player_size, 4.0f, 80.0f, "%.0f");
    SliderRow("Skeleton Draw Distance Max", &s.skeleton_max_distance, 0.0f, 1000.0f, "%.0f");
    SliderRow("Adjust", &s.player_adjust, -100.0f, 100.0f);
    ColorRow("Draw Color", &s.player_color);

    SectionTitle("Vehicle");
    Toggle("Draw Vehicle State", &s.draw_vehicle_state);
    Toggle("Draw Vehicle State [bug]", &s.draw_vehicle_state_bug);
    SliderRow("Size##vehicle", &s.vehicle_size, 4.0f, 80.0f, "%.0f");
    SliderRow("Adjust##vehicle", &s.vehicle_adjust, -100.0f, 100.0f);
    ColorRow("Draw Color##vehicle", &s.vehicle_color);

    SectionTitle("Radar");
    SliderRow("Angle", &s.radar_angle, 0.0f, 360.0f, "%.0f");
    SliderRow("Adjust##radar", &s.radar_adjust, -100.0f, 100.0f);
    Toggle("Radar Pos", &s.radar_position_mode);
    Toggle("Pos | Size", &s.radar_position_and_size);

    SectionTitle("Player Aim You Tips Size");
    SliderRow("Size##aimed_tip", &s.aimed_at_tip_size, 4.0f, 80.0f, "%.0f");
    SliderRow("Adjust##aimed_tip", &s.aimed_at_tip_adjust, -100.0f, 100.0f);
    ColorRow("Draw Color##aimed_tip", &s.aimed_at_tip_color);

    SectionTitle("Health");
    SliderRow("Health Transparency", &s.health_transparency, 0.0f, 1.0f, "%.2f");
    const char* health_types[] = {
        "Circular Health", "Rect Health One", "Rect Health Two", u8"格式血量", "Wide Blood Volume"
    };
    int health_type = static_cast<int>(s.health_type);
    if (ImGui::Combo("Health Type", &health_type, health_types, 5)) {
        s.health_type = static_cast<HealthType>(health_type);
    }
    ColorRow("Health Color", &s.health_color);
    Toggle("Same team Same color", &s.same_team_same_color);
    Toggle("Follow blood volume", &s.follow_health_color);
}

void RenderItems(ItemsState& s) {
    SectionTitle("Items");
    Toggle("Open Class Name", &s.open_class_name);
    Toggle("Dev Mode", &s.developer_mode);
    Toggle("My Items-Mate", &s.my_items_mate);

    if (s.developer_mode) {
        ImGui::BeginChild("##centreTwo", ImVec2(0, 130), true);
        ImGui::TextWrapped(u8"自定义物资教程");
        ImGui::BulletText(u8"打开开发者模式");
        ImGui::BulletText(u8"长按屏幕内你需要绘制的类名");
        ImGui::BulletText(u8"如多个类名重叠可长按之后滑动选择");
        ImGui::BulletText(u8"更改名字在启动本辅助的文件内更改");
        ImGui::EndChild();
    }

    SliderRow("All Items Size", &s.all_items_size, 4.0f, 80.0f, "%.0f");
    SliderRow("All Items Distance[100 to Not bound]", &s.all_items_distance, 0.0f, 1000.0f, "%.0f");
    ColorRow("Draw Color", &s.all_items_color);

    static const char* categories[] = {u8"盒子", u8"载具", u8"空投", u8"地铁宝箱"};
    SectionTitle("##ItemsColorAdjustPage");
    for (const char* category : categories) {
        ImGui::BulletText("%s", category);
    }

    if (!s.custom_items.empty()) {
        SectionTitle("Custom classes");
        for (std::size_t i = 0; i < s.custom_items.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            Toggle(s.custom_items[i].display_name.c_str(), &s.custom_items[i].enabled);
            ImGui::PopID();
        }
    }
}

void DiagnosticText(const SettingsState& s) {
    ImGui::Text("pid: %d  libUE4: 0x%" PRIxPTR, s.pid, s.libue4);
    ImGui::Text("Matrix: 0x%" PRIxPTR "  Self: 0x%" PRIxPTR, s.matrix, s.self);
    ImGui::Text("Arrayaddr: 0x%" PRIxPTR "  Count: %d", s.array_address, s.array_count);
    ImGui::Text("SelfAddr: 0x%" PRIxPTR, s.self_address);
}

void RenderSettings(SettingsState& s, const TextureRegistry& textures) {
    SectionTitle("Settings");
    Toggle("Leave Procs Run [KSU Self-contained]", &s.leave_process_running);
    Toggle("Not Screen Recording", &s.not_screen_recording);

    if (ImGui::BeginTabBar("##Settingcentre")) {
        if (ImGui::BeginTabItem("Main Settings")) {
            Toggle("Look Data", &s.look_data);
            Toggle("GameData", &s.game_data);
            if (s.look_data || s.game_data) {
                ImGui::BeginChild("##gameData", ImVec2(0, 118), true);
                DiagnosticText(s);
                ImGui::EndChild();
            }
            if (ActionButton("Kernel Exit", ImVec2(-1, 38))) {
                s.request_kernel_exit = true;
            }
            if (ActionButton("Exit & Save Configuration", ImVec2(-1, 38))) {
                s.request_exit_and_save = true;
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Language Mode")) {
            int language = static_cast<int>(s.language);
            ImGui::RadioButton(u8"简体中文", &language, 0);
            ImGui::SameLine();
            ImGui::RadioButton("English", &language, 1);
            s.language = static_cast<Language>(language);
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("More")) {
            ImGui::TextColored(kMuted, "Main | Island | Photo | Text | Dynamic");
            Toggle("MacOS \"Magic effect\"", &s.macos_magic_effect);
            Toggle("IpadOS \"Dispersion effect\"", &s.ipad_dispersion_effect);
            SliderRow("Island Size", &s.island_size, 0.25f, 3.0f, "%.1f");
            Toggle("Change Photo", &s.change_photo);
            Toggle("Change Text [Use TTF Format", &s.change_text);
            Toggle("Dynamic", &s.dynamic_island);
            Toggle("Island Dynamic Test", &s.island_dynamic_test);

            if (s.change_text) {
                ImGui::TextUnformatted(s.selected_font_path.c_str());
            }
            if (s.change_photo) {
                ImGui::TextUnformatted(s.selected_picture_path.c_str());
            }

            Toggle("Restore the original Text", &s.restore_original_text);
            Toggle("From the cloud", &s.load_from_cloud);
            Toggle("Use File Dir Switch", &s.use_file_directory);
            Toggle("Restore the original picture", &s.restore_original_picture);

            ImGui::ProgressBar(std::clamp(s.preview_progress, 0.0f, 1.0f), ImVec2(-1, 0), "PhotoLook");
            if (textures.Get(AssetId::Avatar) != ImTextureID{}) {
                ImGui::Image(textures.Get(AssetId::Avatar), ImVec2(72, 72));
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

void RenderUpdateDialog(UpdateDialogState& u) {
    if (!u.open) {
        return;
    }
    ImGui::OpenPopup("Find the new version");
    const ImGuiWindowFlags flags = ImGuiWindowFlags_None;
    if (ImGui::BeginPopupModal("Find the new version", &u.open, flags)) {
        ImGui::TextColored(u.mandatory ? kDanger : kMuted,
                           "%s", u.mandatory ? "[Mandatory update]" : "[Not mandatory update]");
        ImGui::Text("Now version: %s", u.current_version.c_str());
        ImGui::Text("New version: %s", u.new_version.c_str());
        ImGui::Text("Size: %.2f MB", u.download_size_mb);
        if (!u.message.empty()) {
            ImGui::TextWrapped("%s", u.message.c_str());
        }
        ImGui::ProgressBar(std::clamp(u.progress, 0.0f, 1.0f), ImVec2(-1, 0), "Load progress");
        if (!u.downloading) {
            if (ActionButton("Download", ImVec2(140, 38))) {
                u.downloading = true;
            }
            ImGui::SameLine();
            if (ActionButton("Confirm Update", ImVec2(160, 38))) {
                u.downloading = true;
            }
        } else {
            ActionButton("Use Update", ImVec2(140, 38));
        }
        if (!u.mandatory) {
            ImGui::SameLine();
            if (ImGui::Button("Cancel Update", ImVec2(140, 38))) {
                u.open = false;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
}

void ApplyStyle() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 18.0f;
    style.ChildRounding = 14.0f;
    style.FrameRounding = 9.0f;
    style.PopupRounding = 12.0f;
    style.ScrollbarRounding = 9.0f;
    style.WindowPadding = ImVec2(12, 12);
    style.ItemSpacing = ImVec2(8, 8);

    style.Colors[ImGuiCol_WindowBg] = kPanel;
    style.Colors[ImGuiCol_ChildBg] = kPanelAlt;
    style.Colors[ImGuiCol_Border] = ImVec4(0.25f, 0.26f, 0.34f, 0.60f);
    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.13f, 0.14f, 0.18f, 1.0f);
    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.19f, 0.18f, 0.28f, 1.0f);
    style.Colors[ImGuiCol_FrameBgActive] = kAccentSoft;
    style.Colors[ImGuiCol_CheckMark] = kAccent;
    style.Colors[ImGuiCol_SliderGrab] = kAccent;
    style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.55f, 0.40f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_Header] = kAccentSoft;
    style.Colors[ImGuiCol_HeaderHovered] = kAccent;
    style.Colors[ImGuiCol_Tab] = ImVec4(0.12f, 0.13f, 0.17f, 1.0f);
    style.Colors[ImGuiCol_TabActive] = kAccentSoft;
    style.Colors[ImGuiCol_TabHovered] = kAccent;
}

} // namespace

void RenderBarbatosGui(GuiState& state, const TextureRegistry& textures) {
    if (!state.visible) {
        return;
    }

    ApplyStyle();
    ImGui::SetNextWindowSize(ImVec2(1040, 680), ImGuiCond_FirstUseEver);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
                                   ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoScrollbar |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus;

    if (ImGui::Begin("Hello ImGui", &state.visible, flags)) {
        ImGui::BeginChild("##left", ImVec2(104, 0), true, ImGuiWindowFlags_NoScrollbar);
        ImGui::TextColored(kAccent, "Dome");
        ImGui::TextColored(kMuted, "DitPro");
        ImGui::TextUnformatted("SDK");
        ImGui::Spacing();

        struct NavItem { Page page; AssetId asset; const char* fallback; };
        static constexpr NavItem nav[] = {
            {Page::Home, AssetId::Profile, "H"},
            {Page::Draw, AssetId::DrawEye, "D"},
            {Page::Aim, AssetId::AimTarget, "A"},
            {Page::Items, AssetId::ItemsCode, "I"},
            {Page::Settings, AssetId::Settings, "S"},
        };
        for (const NavItem& item : nav) {
            if (NavButton(item.fallback, item.fallback, textures.Get(item.asset), state.page == item.page)) {
                state.page = item.page;
            }
            ImGui::Spacing();
        }
        ImGui::EndChild();

        ImGui::SameLine();
        ImGui::BeginChild("##content", ImVec2(0, 0), false);
        switch (state.page) {
            case Page::Home: RenderHome(textures); break;
            case Page::Draw: RenderDraw(state.draw); break;
            case Page::Aim: RenderAim(state.aim); break;
            case Page::Items: RenderItems(state.items); break;
            case Page::Settings: RenderSettings(state.settings, textures); break;
        }
        ImGui::EndChild();
    }
    ImGui::End();

    RenderUpdateDialog(state.update);
}

} // namespace barbatos_gui
