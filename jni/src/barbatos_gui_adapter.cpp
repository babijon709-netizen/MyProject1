#include "barbatos_gui/barbatos_gui_reconstructed.hpp"
#include "app_state.h"

// Compatibility adapter: the replacement owns presentation, while AppState remains
// the single source of truth consumed by aim/ESP/farm/game code.
namespace {
barbatos_gui::GuiState ui;
void Pull() {
    ui.page = static_cast<barbatos_gui::Page>(g_state.cur_tab == 1 ? 2 : g_state.cur_tab == 2 ? 1 : g_state.cur_tab == 3 ? 3 : 4);
    auto &a=ui.aim; a.main_aim=g_state.aim_touch; a.only_draw=g_state.aim_scope_only; a.other_aim_functions=g_state.aim_special;
    a.aim_point=static_cast<barbatos_gui::AimPoint>(g_state.aim_bone);
    auto &d=ui.draw; d.rect_box=g_state.esp_box; d.name_and_distance=g_state.esp_name; d.skeleton=g_state.esp_skeleton; d.player_weapon=g_state.esp_weapon; d.line=g_state.esp_tracer;
    auto &s=ui.settings; s.look_data=g_state.always_day; s.request_kernel_exit=false;
    auto &it=ui.items; it.open_class_name=g_state.esp_loot; it.my_items_mate=g_state.esp_pickup;
}
void Push() {
    g_state.aim_touch=ui.aim.main_aim; g_state.aim_scope_only=ui.aim.only_draw; g_state.aim_special=ui.aim.other_aim_functions; g_state.aim_bone=(int)ui.aim.aim_point;
    g_state.esp_box=ui.draw.rect_box; g_state.esp_name=ui.draw.name_and_distance; g_state.esp_skeleton=ui.draw.skeleton; g_state.esp_weapon=ui.draw.player_weapon; g_state.esp_tracer=ui.draw.line;
    g_state.always_day=ui.settings.look_data; g_state.esp_loot=ui.items.open_class_name; g_state.esp_pickup=ui.items.my_items_mate;
    g_state.cur_tab = (int)ui.page;
}
}
void RenderMenu() { Pull(); barbatos_gui::RenderBarbatosGui(ui, {}); Push(); }
