#include "theme.h"

bool  g_darkTheme = true;
float g_themeT    = 1.f;
float g_menuFadeIn = 0.f;
// Дефолтный акцент — «фосфор» терминала: как только пользователь не выбирал
// свой цвет, весь GUI зелёный монохром (см. SyncAccentToTheme: idx 0 — без
// оверрайда, палитры Light/Dark сами зелёные).
ImVec4 g_accent = {0.200f, 0.851f, 0.353f, 1};
bool   g_accentOn = false;

void ApplyTheme() {
    ImGui::GetIO().ConfigDebugHighlightIdConflicts = false;
    auto& s = ImGui::GetStyle();
    // Терминал: никаких скруглений и теней — рамки рисует псевдографика.
    s.WindowRounding = 0; s.ChildRounding  = 0; s.FrameRounding = 0;
    s.GrabRounding   = 0; s.PopupRounding  = 0; s.TabRounding   = 0;
    s.WindowBorderSize = 0; s.FrameBorderSize = 0; s.ScrollbarRounding = 0;
    s.ItemSpacing  = {10, 0}; s.FramePadding  = {14, 12}; s.WindowPadding = {0, 0};
    s.ScrollbarSize = 0; s.GrabMinSize = 20;
    auto* c = s.Colors;
    c[ImGuiCol_WindowBg]              = C::Bg();
    c[ImGuiCol_ChildBg]               = {0, 0, 0, 0};
    c[ImGuiCol_Border]                = C::Sep();
    c[ImGuiCol_BorderShadow]          = {0, 0, 0, 0};
    c[ImGuiCol_Text]                  = C::Txt();
    c[ImGuiCol_TextDisabled]          = C::Dim();
    c[ImGuiCol_ScrollbarBg]           = {0, 0, 0, 0};
    c[ImGuiCol_ScrollbarGrab]         = {0, 0, 0, 0};
    c[ImGuiCol_ScrollbarGrabHovered]  = {0, 0, 0, 0};
    c[ImGuiCol_ScrollbarGrabActive]   = {0, 0, 0, 0};
    c[ImGuiCol_SliderGrab]            = {0, 0, 0, 0};
    c[ImGuiCol_SliderGrabActive]      = {0, 0, 0, 0};
    c[ImGuiCol_FrameBg]               = {0, 0, 0, 0};
    c[ImGuiCol_FrameBgHovered]        = {0, 0, 0, 0};
    c[ImGuiCol_FrameBgActive]         = {0, 0, 0, 0};
    c[ImGuiCol_Button]                = {0, 0, 0, 0};
    c[ImGuiCol_ButtonHovered]         = {0, 0, 0, 0};
    c[ImGuiCol_ButtonActive]          = {0, 0, 0, 0};
    c[ImGuiCol_Header]                = {0, 0, 0, 0};
    c[ImGuiCol_HeaderHovered]         = {0, 0, 0, 0};
    c[ImGuiCol_HeaderActive]          = {0, 0, 0, 0};
    c[ImGuiCol_NavHighlight]          = {0, 0, 0, 0};
    c[ImGuiCol_NavWindowingHighlight] = {0, 0, 0, 0};
    c[ImGuiCol_Separator]             = C::Sep();
}
