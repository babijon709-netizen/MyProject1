#pragma once
#include <string>
#include "ImGui/imgui.h"
namespace ImGui {
bool InputText(const char* label, std::string* str, ImGuiInputTextFlags flags=0, ImGuiInputTextCallback callback=nullptr, void* user_data=nullptr);
bool InputTextMultiline(const char* label, std::string* str, const ImVec2& size=ImVec2(0,0), ImGuiInputTextFlags flags=0, ImGuiInputTextCallback callback=nullptr, void* user_data=nullptr);
}
