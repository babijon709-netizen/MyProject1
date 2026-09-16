// main.cpp — точка входа: инициализация, кадр меню, остановка потоков.
//
// Здесь остался только main(): сам софт разложен по модулям, и каждый из них
// подключает свои заголовки сам. Разрез монолита (6117 строк) — в коммитах
// после ef913b1; карта модулей — docs/CODE_MAP.md.
//
// Кадр (порядок важен, менять осознанно):
//   drawBegin()            — окно оверлея и ImGui готовы к кадру;
//   esp_mem_frame_begin()  — кэш чтений памяти игры сброшен на новый кадр;
//   DrawEspOverlay()       — рамки/скелеты/маркеры на экране;
//   UpdateAim(dt)          — аимбот ведёт камеру (палец);
//   UpdateFarm(dt)         — автофарм рубит ближайший узел;
//   RenderMenu()           — само меню;
//   drawEnd()              — отдать кадр дальше.
#include "app/common.h"

#include "app/attach.h"      // start_attach_thread/stop_attach_thread
#include "app/lifecycle.h"   // main_thread_flag, g_frame_done, prot::Init
#include "app/media.h"       // LoadAnimeImage, LoadTabIcons
#include "app/screen.h"      // g_sw, g_sh, CenterMenuOnDisplay
#include "ui/config.h"       // kCfgDir_, Cfg* — конфиги и язык
#include "ui/esp_overlay.h"  // DrawEspOverlay
#include "ui/settings.h"     // ui::bar
#include "ui/theme.h"        // ApplyTheme, g_menuFadeIn
#include "ui/window.h"       // RenderMenu
#include "aim/update.h"      // UpdateAim
#include "farm/controller.h" // UpdateFarm

int main(int argc, char* argv[]) {
    signal(SIGINT,  [](int) { main_thread_flag.store(false); });
    signal(SIGTERM, [](int) { main_thread_flag.store(false); });
    signal(SIGHUP,  [](int) { main_thread_flag.store(false); });

    prot::Init();
    screen_config();

    mkdir(kCfgDir_(), 0777);   // путь к каталогу конфигов — в ui/config
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
    LoadAnimeImage();
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

        ui::bar::set_game_alpha(0.f);
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
    if (g_esp_attached) {
        esp_reset();
        g_esp_attached = false;
    }
    Blur::Free();
    CfgWatchFree();
    AudioFree();
    shutdown(); Touch_Close(); return 0;
}
