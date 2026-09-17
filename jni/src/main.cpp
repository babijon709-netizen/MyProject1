#include "main.h"
#include "game.h"
#include "Blur/Blur.h"
#include "Android_touch/TouchHelperA.h"
#include "audio.h"
#include "process.h"
#include "hud.h"
#include "esp_draw.h"
#include "aim.h"
#include "farm.h"
#include "widgets.h"
#include "menu.h"
#include "config.h"
#include "theme.h"
#include "app_state.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <sys/stat.h>
#include <thread>

// Хук инициализации (пустая заглушка — точка расширения).
namespace prot {
static void Init() {}
}

float g_sw = 1920.f;
float g_sh = 1080.f;
std::atomic<bool> main_thread_flag{true};
std::atomic<bool> g_frame_done{true};
InputState g_input;
AppState g_state;

int main(int argc, char* argv[]) {
    signal(SIGINT,  [](int) { main_thread_flag.store(false); });
    signal(SIGTERM, [](int) { main_thread_flag.store(false); });
    signal(SIGHUP,  [](int) { main_thread_flag.store(false); });

    prot::Init();
    screen_config();

    mkdir(CfgDir(), 0777);
    int abs_ScreenX = displayInfo.height > displayInfo.width ? displayInfo.height : displayInfo.width;
    int abs_ScreenY = displayInfo.height < displayInfo.width ? displayInfo.height : displayInfo.width;

    g_sw = static_cast<float>(abs_ScreenX);
    g_sh = static_cast<float>(abs_ScreenY);

    native_window_screen_x = abs_ScreenX;
    native_window_screen_y = abs_ScreenY;
    if (!initGUI_draw(native_window_screen_x, native_window_screen_x, true)) return -1;
    Blur::Init();
    CfgWatchInit();
    AudioInit();
    if (!Touch_Init(displayInfo.width, displayInfo.height, displayInfo.orientation, false))
        Touch_Init(displayInfo.width, displayInfo.height, displayInfo.orientation, true);
    // Если полноценный тач не поднялся с первого раза (гонка за /dev/uinput
    // или grab на старте — обычное дело сразу после запуска игры), чит раньше
    // навсегда оставался в read-only: автофарм «просто не идёт», пока не
    // перезапустишь. Теперь фоновый поток раз в 3 секунды пробует поднять
    // инъекцию заново, пока не получится.
    static std::atomic<bool> s_touchRetryRun{true};
    std::thread([]() {
        while (s_touchRetryRun.load() && main_thread_flag.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            if (Touch_CanInject()) continue;
            Touch_Close();
            if (!Touch_Init(displayInfo.width, displayInfo.height, displayInfo.orientation, false))
                Touch_Init(displayInfo.width, displayInfo.height, displayInfo.orientation, true);
        }
    }).detach();
    start_attach_thread();
    LoadTabIcons();
    ApplyTheme();
    RestoreLang();          // язык из прошлого запуска (конфиг ниже может его переписать)
    RestoreBuild();         // версия игры из прошлого запуска — до привязки к процессу
    CfgScanDir();
    ConfigLoadLast();
    ApplyTheme();
    CenterMenuOnDisplay();
    g_menuFadeIn = 0.f;

    while (main_thread_flag) {
        g_frame_done.store(false);
        drawBegin();

        esp_mem_frame_begin();   // кадр начался: кэш блоков памяти игры сброшен
        DrawEspOverlay();
        UpdateAim(ImGui::GetIO().DeltaTime);
        UpdateFarm(ImGui::GetIO().DeltaTime);
        RenderMenu();
        drawEnd();
        g_frame_done.store(true);
    }
    while (!g_frame_done.load()) {}
    stop_attach_thread();
    process_detach();
    Blur::Free();
    CfgWatchFree();
    AudioFree();
    shutdown(); Touch_Close(); return 0;
}
