#include "startup_mode.h"

#include "imgui.h"
#include "hud.h"
#include "str.h"
#include "theme.h"

#include <string>

namespace {

bool g_ready = false;
std::string g_error;

void draw_mode_card(const char* id, const char* title, const char* detail,
                    memaccess::AccessMode mode, bool wide = false) {
    const ImVec2 size = wide ? ImVec2(-1.f, 76.f) : ImVec2(0.f, 112.f);
    ImGui::PushID(id);
    if (ImGui::Button(title, size)) StartupModeSelect(mode);
    ImGui::SameLine(0.f, 12.f);
    ImGui::BeginGroup();
    ImGui::TextUnformatted(detail);
    ImGui::EndGroup();
    ImGui::PopID();
}

} // namespace

bool StartupModeReady() { return g_ready; }

bool StartupModeSelect(memaccess::AccessMode mode) {
    g_error.clear();
    if (mode == memaccess::AccessMode::Kernel) {
        std::string error;
        if (!memaccess::KernelDriverManager::Instance().Start(&error)) {
            g_error = error.empty() ? memaccess::KernelDriverManager::Instance().LastError() : error;
            g_ready = false;
            return false;
        }
    } else {
        memaccess::KernelDriverManager::Instance().Stop();
    }
    memaccess::SetAccessMode(mode);
    g_ready = true;
    return true;
}

const char* StartupModeError() { return g_error.c_str(); }

void RenderStartupModeMenu() {
    // ImGui uses the square overlay surface, while the visible scene can be
    // portrait or landscape. Center against the same visible rectangle as the
    // main GUI; using io.DisplaySize here places the dialog in the unused part
    // of the square surface on portrait devices.
    float screen_w = 0.f, screen_h = 0.f;
    VisibleScreen(screen_w, screen_h);
    if (screen_w < 100.f || screen_h < 100.f) return;

    const float margin = 16.f;
    const float width = ImMin(700.f, ImMax(240.f, screen_w - margin * 2.f));
    const float height = ImMin(390.f, ImMax(220.f, screen_h - margin * 2.f));
    const float pos_x = ImMax(margin, (screen_w - width) * 0.5f);
    const float pos_y = ImMax(margin, (screen_h - height) * 0.5f);
    ImGui::SetNextWindowPos({pos_x, pos_y}, ImGuiCond_Always);
    ImGui::SetNextWindowSize({width, height}, ImGuiCond_Always);
    ImGui::Begin("##startup_mode", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings);

    ImGui::PushStyleColor(ImGuiCol_Text, C::Txt());
    ImGui::SetCursorPos({32.f, 28.f});
    ImGui::TextUnformatted(XS("Выбери режим доступа"));
    ImGui::SetCursorPos({32.f, 62.f});
    ImGui::TextColored(C::Dim(), "%s", XS("Режим запускается до поиска процесса игры"));

    ImGui::SetCursorPos({32.f, 112.f});
    ImGui::BeginGroup();
    draw_mode_card("external", XS("External"),
                   XS("/proc/<pid>/mem · обычный режим"), memaccess::AccessMode::External);
    ImGui::EndGroup();
    ImGui::SameLine(0.f, 22.f);
    ImGui::BeginGroup();
    draw_mode_card("kernel", XS("Kernel"),
                   XS("FTDriver · запусти вручную для своего ядра"), memaccess::AccessMode::Kernel);
    ImGui::EndGroup();

    ImGui::SetCursorPos({32.f, 256.f});
    ImGui::TextColored(C::Dim(), "%s", XS("Kernel mode не переключается на External автоматически."));
    if (!g_error.empty()) {
        ImGui::SetCursorPos({32.f, 286.f});
        ImGui::PushTextWrapPos(width - 32.f);
        ImGui::TextColored({1.f, 0.38f, 0.38f, 1.f}, "%s", g_error.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::SetCursorPos({32.f, height - 48.f});
    ImGui::TextColored(C::Dim(), "%s", XS("FTDriver запускается вручную до выбора Kernel mode."));
    ImGui::PopStyleColor();
    ImGui::End();
}

void StartupModeShutdown() {
    memaccess::KernelDriverManager::Instance().Stop();
    g_ready = false;
}
