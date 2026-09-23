// ============================================================================
//  CheatMenu.cpp
//  Стилизованное GUI-меню на Dear ImGui в духе референса (тёмный фон,
//  розово-маджентовые акценты, закруглённые углы, моноширинный "терминальный"
//  вайб).
//
//  ВАЖНО: здесь ТОЛЬКО визуал. Никакой игровой логики, чтения памяти,
//  хуков, инъекций и т.п. Чекбоксы/слайдеры хранят значения в обычных
//  локальных переменных и ни на что не влияют.
//
//  Предполагается, что у тебя уже поднят бэкенд Dear ImGui (DirectX11/Win32
//  или GLFW/OpenGL — не важно). Сюда просто нужно воткнуть:
//      ApplyCustomStyle();      // один раз после ImGui::CreateContext()
//      RenderCheatMenu(&open);  // каждый кадр, между NewFrame() и Render()
//
//  Зависимости: только сам Dear ImGui (https://github.com/ocornut/imgui)
// ============================================================================

#include "imgui.h"
#include <cstring>

// ----------------------------------------------------------------------------
//  ЦВЕТОВАЯ ПАЛИТРА (под референс: чёрный фон, magenta/pink неон, зелень как
//  дополнительный акцент для "онлайн"-индикаторов)
// ----------------------------------------------------------------------------
namespace Palette
{
    constexpr ImVec4 Bg          = ImVec4(0.05f, 0.03f, 0.06f, 0.96f);
    constexpr ImVec4 BgChild     = ImVec4(0.08f, 0.05f, 0.10f, 0.85f);
    constexpr ImVec4 Border      = ImVec4(0.85f, 0.25f, 0.65f, 0.35f);
    constexpr ImVec4 Accent      = ImVec4(0.92f, 0.28f, 0.66f, 1.00f); // magenta
    constexpr ImVec4 AccentHover = ImVec4(1.00f, 0.45f, 0.78f, 1.00f);
    constexpr ImVec4 AccentActv  = ImVec4(0.75f, 0.15f, 0.50f, 1.00f);
    constexpr ImVec4 TextMain    = ImVec4(0.92f, 0.88f, 0.92f, 1.00f);
    constexpr ImVec4 TextDim     = ImVec4(0.55f, 0.50f, 0.58f, 1.00f);
    constexpr ImVec4 Green       = ImVec4(0.45f, 0.90f, 0.55f, 1.00f); // "online"
    constexpr ImVec4 Yellow      = ImVec4(0.95f, 0.85f, 0.40f, 1.00f); // заголовки секций
}

// ----------------------------------------------------------------------------
//  Применение стиля (закруглённые углы, тонкие бордеры, компактные паддинги)
// ----------------------------------------------------------------------------
void ApplyCustomStyle()
{
    ImGuiStyle& s = ImGui::GetStyle();

    s.WindowRounding    = 10.0f;
    s.ChildRounding      = 8.0f;
    s.FrameRounding      = 6.0f;
    s.PopupRounding      = 8.0f;
    s.ScrollbarRounding  = 8.0f;
    s.GrabRounding       = 6.0f;
    s.TabRounding        = 6.0f;

    s.WindowBorderSize  = 1.0f;
    s.FrameBorderSize   = 1.0f;
    s.ChildBorderSize   = 1.0f;

    s.WindowPadding     = ImVec2(14, 14);
    s.FramePadding      = ImVec2(8, 5);
    s.ItemSpacing       = ImVec2(8, 8);
    s.ItemInnerSpacing  = ImVec2(6, 6);
    s.IndentSpacing     = 18.0f;

    ImVec4* c = s.Colors;
    using namespace Palette;

    c[ImGuiCol_WindowBg]         = Bg;
    c[ImGuiCol_ChildBg]          = BgChild;
    c[ImGuiCol_PopupBg]          = Bg;
    c[ImGuiCol_Border]           = Border;
    c[ImGuiCol_Text]             = TextMain;
    c[ImGuiCol_TextDisabled]     = TextDim;

    c[ImGuiCol_FrameBg]          = ImVec4(0.12f, 0.08f, 0.14f, 1.00f);
    c[ImGuiCol_FrameBgHovered]   = ImVec4(0.18f, 0.10f, 0.20f, 1.00f);
    c[ImGuiCol_FrameBgActive]    = ImVec4(0.22f, 0.12f, 0.24f, 1.00f);

    c[ImGuiCol_TitleBg]          = Bg;
    c[ImGuiCol_TitleBgActive]    = Bg;

    c[ImGuiCol_CheckMark]        = Accent;
    c[ImGuiCol_SliderGrab]       = Accent;
    c[ImGuiCol_SliderGrabActive] = AccentActv;

    c[ImGuiCol_Button]           = ImVec4(0.16f, 0.09f, 0.18f, 1.00f);
    c[ImGuiCol_ButtonHovered]    = ImVec4(Accent.x, Accent.y, Accent.z, 0.35f);
    c[ImGuiCol_ButtonActive]     = ImVec4(Accent.x, Accent.y, Accent.z, 0.55f);

    c[ImGuiCol_Header]           = ImVec4(Accent.x, Accent.y, Accent.z, 0.22f);
    c[ImGuiCol_HeaderHovered]    = ImVec4(Accent.x, Accent.y, Accent.z, 0.35f);
    c[ImGuiCol_HeaderActive]     = ImVec4(Accent.x, Accent.y, Accent.z, 0.50f);

    c[ImGuiCol_Tab]              = ImVec4(0.10f, 0.06f, 0.12f, 1.00f);
    c[ImGuiCol_TabHovered]       = AccentHover;
    c[ImGuiCol_TabActive]        = AccentActv;
    c[ImGuiCol_TabUnfocused]     = ImVec4(0.10f, 0.06f, 0.12f, 1.00f);
    c[ImGuiCol_TabUnfocusedActive]= ImVec4(0.16f, 0.09f, 0.18f, 1.00f);

    c[ImGuiCol_ScrollbarBg]      = ImVec4(0.05f, 0.03f, 0.06f, 0.60f);
    c[ImGuiCol_ScrollbarGrab]    = ImVec4(0.35f, 0.15f, 0.30f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered] = Accent;
    c[ImGuiCol_ScrollbarGrabActive]  = AccentActv;

    c[ImGuiCol_Separator]        = Border;
    c[ImGuiCol_ResizeGrip]       = ImVec4(0,0,0,0);
    c[ImGuiCol_ResizeGripHovered]= ImVec4(Accent.x, Accent.y, Accent.z, 0.4f);
}

// ----------------------------------------------------------------------------
//  Маленький хелпер: заголовок секции жёлтым, как "== Hardware ==" в референсе
// ----------------------------------------------------------------------------
static void SectionLabel(const char* label)
{
    ImGui::TextColored(Palette::Yellow, "%s", label);
    ImGui::Separator();
}

// ----------------------------------------------------------------------------
//  Заглушки состояния — просто переменные, ни на что не влияющие
// ----------------------------------------------------------------------------
struct MenuState
{
    // Вкладка "Aim"
    bool  aim_enabled      = false;
    float aim_fov          = 90.0f;
    float aim_smooth       = 4.0f;
    int   aim_bone         = 0; // Head / Chest / Neck

    // Вкладка "Visuals"
    bool  esp_box           = true;
    bool  esp_name          = true;
    bool  esp_health        = false;
    bool  esp_skeleton      = false;
    float esp_distance      = 350.0f;

    // Вкладка "Misc"
    bool  bhop              = false;
    bool  fullbright        = false;
    bool  no_flash          = false;
    float fov_changer       = 90.0f;

    // Общее
    char  config_name[64]   = "default";
    int   selected_tab      = 0;
};

static MenuState g_state;

// ----------------------------------------------------------------------------
//  Основная отрисовка меню
// ----------------------------------------------------------------------------
void RenderCheatMenu(bool* p_open)
{
    if (!p_open || !*p_open) return;

    ImGui::SetNextWindowSize(ImVec2(560, 420), ImGuiCond_FirstUseEver);
    ImGui::PushFont(nullptr); // подставь сюда свой моноширинный шрифт (JetBrains Mono / Consolas)

    if (!ImGui::Begin("legit.gui", p_open,
                       ImGuiWindowFlags_NoCollapse))
    {
        ImGui::End();
        ImGui::PopFont();
        return;
    }

    // ---- Шапка в стиле "Hey, Vanil" ----------------------------------------
    ImGui::TextColored(Palette::Accent, "Привет,");
    ImGui::SameLine();
    ImGui::Text("user");
    ImGui::SameLine(ImGui::GetWindowWidth() - 90);
    ImGui::TextColored(Palette::Green, "* online");

    ImGui::Dummy(ImVec2(0, 6));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 6));

    // ---- Вкладки -------------------------------------------------------------
    if (ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_None))
    {
        if (ImGui::BeginTabItem("Aim"))
        {
            SectionLabel("Aimbot (визуал, без логики)");
            ImGui::Checkbox("Включить", &g_state.aim_enabled);
            ImGui::SliderFloat("FOV", &g_state.aim_fov, 10.0f, 180.0f, "%.0f");
            ImGui::SliderFloat("Плавность", &g_state.aim_smooth, 0.0f, 10.0f, "%.1f");
            const char* bones[] = { "Голова", "Грудь", "Шея" };
            ImGui::Combo("Точка прицела", &g_state.aim_bone, bones, IM_ARRAYSIZE(bones));
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Visuals"))
        {
            SectionLabel("ESP (визуал, без логики)");
            ImGui::Checkbox("Boxes", &g_state.esp_box);
            ImGui::Checkbox("Nicknames", &g_state.esp_name);
            ImGui::Checkbox("Health bar", &g_state.esp_health);
            ImGui::Checkbox("Skeleton", &g_state.esp_skeleton);
            ImGui::SliderFloat("Дистанция прорисовки", &g_state.esp_distance, 0.0f, 1000.0f, "%.0f");
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Misc"))
        {
            SectionLabel("Разное (визуал, без логики)");
            ImGui::Checkbox("Bunny hop", &g_state.bhop);
            ImGui::Checkbox("Fullbright", &g_state.fullbright);
            ImGui::Checkbox("No flash", &g_state.no_flash);
            ImGui::SliderFloat("FOV changer", &g_state.fov_changer, 60.0f, 120.0f, "%.0f");
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Config"))
        {
            SectionLabel("Конфиги");
            ImGui::InputText("Имя конфига", g_state.config_name, IM_ARRAYSIZE(g_state.config_name));
            ImGui::Dummy(ImVec2(0, 4));
            ImGui::Button("Сохранить", ImVec2(120, 0));
            ImGui::SameLine();
            ImGui::Button("Загрузить", ImVec2(120, 0));
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    // ---- Нижний "статус-бар" в духе цветных точек из референса --------------
    ImGui::Dummy(ImVec2(0, 10));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 4));

    const ImVec4 dots[] = {
        ImVec4(0.6f,0.6f,0.6f,1), ImVec4(0.9f,0.2f,0.2f,1), ImVec4(0.9f,0.6f,0.2f,1),
        ImVec4(0.9f,0.9f,0.3f,1), ImVec4(0.3f,0.8f,0.3f,1), ImVec4(0.3f,0.6f,0.9f,1),
        ImVec4(0.6f,0.3f,0.9f,1), ImVec4(0.9f,0.3f,0.7f,1)
    };
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    for (int i = 0; i < IM_ARRAYSIZE(dots); ++i)
    {
        ImU32 col = ImGui::ColorConvertFloat4ToU32(dots[i]);
        draw->AddCircleFilled(ImVec2(p.x + 8 + i * 20.0f, p.y + 8), 6.0f, col);
    }
    ImGui::Dummy(ImVec2(0, 20));

    ImGui::End();
    ImGui::PopFont();
}

// ----------------------------------------------------------------------------
//  Пример использования (псевдо-main-loop):
//
//  ImGui::CreateContext();
//  ApplyCustomStyle();
//  bool menu_open = true;
//  while (running)
//  {
//      ImGui_ImplX_NewFrame();
//      ImGui::NewFrame();
//
//      RenderCheatMenu(&menu_open);
//
//      ImGui::Render();
//      ImGui_ImplX_RenderDrawData(ImGui::GetDrawData());
//  }
// ----------------------------------------------------------------------------
