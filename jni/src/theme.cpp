#include "theme.h"

bool  g_darkTheme = true;
float g_themeT = 1.f;
float g_menuFadeIn = 0.f;
ImVec4 g_accent = {0.380f, 0.240f, 0.960f, 1.f};
bool   g_accentOn = false;

void ApplyTheme() {
    ImGui::GetIO().ConfigDebugHighlightIdConflicts = false;
    auto& s = ImGui::GetStyle();

    // Recovered Barbatos proportions.
    s.WindowRounding = 18.f;
    s.ChildRounding = 14.f;
    s.FrameRounding = 9.f;
    s.GrabRounding = 9.f;
    s.PopupRounding = 12.f;
    s.TabRounding = 9.f;
    s.ScrollbarRounding = 9.f;
    s.WindowBorderSize = 0.f;
    s.FrameBorderSize = 0.f;
    s.ItemSpacing = {8.f, 8.f};
    s.FramePadding = {12.f, 9.f};
    s.WindowPadding = {0.f, 0.f};
    s.ScrollbarSize = 8.f;
    s.GrabMinSize = 16.f;

    auto* c = s.Colors;
    c[ImGuiCol_WindowBg] = C::Bg();
    c[ImGuiCol_ChildBg] = C::LeftBg();
    c[ImGuiCol_Border] = C::UA(C::Sep(), 0.60f);
    c[ImGuiCol_BorderShadow] = {0, 0, 0, 0};
    c[ImGuiCol_Text] = C::Txt();
    c[ImGuiCol_TextDisabled] = C::Dim();
    c[ImGuiCol_FrameBg] = g_darkTheme ? ImVec4{0.13f, 0.14f, 0.18f, 1.f} : C::Card();
    c[ImGuiCol_FrameBgHovered] = g_darkTheme ? ImVec4{0.19f, 0.18f, 0.28f, 1.f} : C::TrkOff();
    c[ImGuiCol_FrameBgActive] = C::UA(C::Acc(), 0.80f);
    c[ImGuiCol_CheckMark] = C::Acc();
    c[ImGuiCol_SliderGrab] = C::Acc();
    c[ImGuiCol_SliderGrabActive] = C::AccLight();
    c[ImGuiCol_Button] = C::UA(C::AccDk(), 0.80f);
    c[ImGuiCol_ButtonHovered] = C::Acc();
    c[ImGuiCol_ButtonActive] = C::AccDk();
    c[ImGuiCol_Header] = C::UA(C::AccDk(), 0.80f);
    c[ImGuiCol_HeaderHovered] = C::Acc();
    c[ImGuiCol_HeaderActive] = C::AccDk();
    c[ImGuiCol_Tab] = g_darkTheme ? ImVec4{0.12f, 0.13f, 0.17f, 1.f} : C::Card();
    c[ImGuiCol_TabActive] = C::UA(C::AccDk(), 0.80f);
    c[ImGuiCol_TabHovered] = C::Acc();
    c[ImGuiCol_Separator] = C::Sep();
    c[ImGuiCol_ScrollbarBg] = {0, 0, 0, 0};
    c[ImGuiCol_ScrollbarGrab] = C::UA(C::Dim(), 0.55f);
    c[ImGuiCol_ScrollbarGrabHovered] = C::UA(C::Dim(), 0.75f);
    c[ImGuiCol_ScrollbarGrabActive] = C::Acc();
    c[ImGuiCol_NavHighlight] = {0, 0, 0, 0};
    c[ImGuiCol_NavWindowingHighlight] = {0, 0, 0, 0};
}
