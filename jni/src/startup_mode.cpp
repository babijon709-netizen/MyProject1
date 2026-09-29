#include "startup_mode.h"

#include "imgui.h"
#include "str.h"
#include "theme.h"

#include <string>

namespace {

bool g_ready = false;
std::string g_error;

void draw_mode_card(const char* id, const char* title, const char* detail,
                    memory::AccessMode mode, bool wide = false) {
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

bool StartupModeSelect(memory::AccessMode mode) {
    g_error.clear();
    if (mode == memory::AccessMode::Kernel) {
        std::string error;
        if (!memory::KernelDriverManager::Instance().Start(&error)) {
            g_error = error.empty() ? memory::KernelDriverManager::Instance().LastError() : error;
            g_ready = false;
            return false;
        }
    } else {
        memory::KernelDriverManager::Instance().Stop();
    }
    memory::SetAccessMode(mode);
    g_ready = true;
    return true;
}

const char* StartupModeError() { return g_error.c_str(); }

void RenderStartupModeMenu() {
    auto& io = ImGui::GetIO();
    const float width = io.DisplaySize.x > 900.f ? 700.f : io.DisplaySize.x - 48.f;
    const float height = io.DisplaySize.y > 900.f ? 390.f : io.DisplaySize.y - 48.f;
    ImGui::SetNextWindowPos({(io.DisplaySize.x - width) * 0.5f,
                             (io.DisplaySize.y - height) * 0.5f}, ImGuiCond_Always);
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
                   XS("/proc/<pid>/mem · обычный режим"), memory::AccessMode::External);
    ImGui::EndGroup();
    ImGui::SameLine(0.f, 22.f);
    ImGui::BeginGroup();
    draw_mode_card("kernel", XS("Kernel"),
                   XS("FTDriver · доступ через ядро"), memory::AccessMode::Kernel);
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
    ImGui::TextColored(C::Dim(), "%s", XS("Внешний режим не запускает драйвер."));
    ImGui::PopStyleColor();
    ImGui::End();
}

void StartupModeShutdown() {
    memory::KernelDriverManager::Instance().Stop();
    g_ready = false;
}
