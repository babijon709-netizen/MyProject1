#pragma once

#include <cstdint>

namespace gui_input {

// Read-only touchscreen adapter used by the standalone GUI target. It never
// grabs a device and never creates uinput events; it only forwards the first
// real contact to ImGui on the render thread.
bool Start(int width, int height, uint32_t orientation);
void Stop();
void UpdateScreen(int width, int height, uint32_t orientation);
void ApplyToImGui();

}  // namespace gui_input
