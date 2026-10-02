#include "main.h"
#include "menu.h"
#include "theme.h"
#include "hud.h"
#include "config.h"
#include "audio.h"
#include "app_state.h"
#include "overlay_geometry.h"
#include "Blur/Blur.h"
#include "gui_only_input.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <thread>

float g_sw = 1920.f;
float g_sh = 1080.f;
std::atomic<bool> main_thread_flag{true};
std::atomic<bool> g_frame_done{true};
InputState g_input;
AppState g_state;

static void StopGui(int) {
    main_thread_flag.store(false);
}

int main() {
    signal(SIGINT, StopGui);
    signal(SIGTERM, StopGui);
    signal(SIGHUP, StopGui);

    screen_config();
    const OverlayGeometry overlay =
        MakeOverlayGeometry(displayInfo.width, displayInfo.height);
    g_sw = static_cast<float>(overlay.scene_width);
    g_sh = static_cast<float>(overlay.scene_height);
    native_window_screen_x = overlay.surface_side;
    native_window_screen_y = overlay.surface_side;

    if (!initGUI_draw(overlay.surface_side, overlay.surface_side, true))
        return 1;

    Blur::Init();
    CfgWatchInit();
    AudioInit();
    Touch_Init(displayInfo.width, displayInfo.height, displayInfo.orientation, true);
    LoadTabIcons();
    ApplyTheme();
    RestoreLang();
    RestoreAccent();
    RestoreLayout();
    CfgScanDir();
    ConfigLoadLast();
    ApplyTheme();
    CenterMenuOnDisplay();
    g_menuFadeIn = 0.f;

    while (main_thread_flag.load()) {
        g_frame_done.store(false);
        gui_input::ApplyToImGui();
        drawBegin();
        RenderMenu();
        DrawToast(ImGui::GetIO().DeltaTime);
        DrawWatermark(ImGui::GetIO().DeltaTime);
        drawEnd();
        g_frame_done.store(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    ConfigSave();
    CfgWatchFree();
    AudioFree();
    Blur::Free();
    Touch_Close();
    shutdown();
    return 0;
}
