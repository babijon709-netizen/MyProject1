#include "theme.h"

bool  g_darkTheme = true;
float g_themeT = 1.f;
float g_menuFadeIn = 0.f;
ImVec4 g_accent = {0.545f, 0.486f, 1.000f, 1.f};
bool   g_accentOn = false;

void ApplyTheme() {
    ImGui::GetIO().ConfigDebugHighlightIdConflicts = false;
    auto& s = ImGui::GetStyle();
    s.WindowRounding = R::Sheet;
    s.ChildRounding = R::Card;
    s.FrameRounding = R::Btn;
    s.GrabRounding = R::Pill;
    s.PopupRounding = R::Sheet;
    s.TabRounding = R::Btn;
    s.ScrollbarRounding = R::Pill;
    s.WindowBorderSize = 1.f;
    s.ChildBorderSize = 0.f;
    s.FrameBorderSize = 0.f;
    s.PopupBorderSize = 1.f;
    s.ItemSpacing = {10.f, 0.f};
    s.ItemInnerSpacing = {8.f, 6.f};
    s.FramePadding = {14.f, 10.f};
    s.WindowPadding = {0.f, 0.f};
    s.ScrollbarSize = 0.f;
    s.GrabMinSize = 18.f;

    auto* c = s.Colors;
    c[ImGuiCol_WindowBg]              = C::Bg();
    c[ImGuiCol_ChildBg]               = {0, 0, 0, 0};
    c[ImGuiCol_PopupBg]               = C::Card();
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
