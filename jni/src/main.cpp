#include "main.h"
#include "game.h"
#include "Blur/Blur.h"
#include "Android_touch/TouchHelperA.h"
#include "audio.h"
#include "process.h"
#include "hud.h"
#include "esp_draw.h"
#include "aim.h"
#include "silent_aim.h"
#include "farm.h"
#include "widgets.h"
#include "menu.h"
#include "config.h"
#include "logfile.h"
#include "theme.h"
#include "app_state.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <ctime>
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
    RestoreAccent();        // акцент интерфейса из прошлого запуска
    RestoreLayout();        // кнопки и тач-зоны из прошлого запуска (точку аима конфиг ниже может переписать)
    CfgScanDir();
    ConfigLoadLast();
    ApplyTheme();
    CenterMenuOnDisplay();
    g_menuFadeIn = 0.f;

    LogOpen();
    LogWatchdogStart();
    unsigned long frame = 0;
    while (main_thread_flag) {
        ++frame;
        g_frame_done.store(false);
        LogStage(kStageFrameBegin);
        // Вскрытие тормозов оверлея (лог 07:37: кадр встаёт на 200–400 мс
        // пачками, писатель при этом ровный — стоит именно main). Если кадр
        // дольше 100 мс — пишем, в какой стадии он стоял и сколько из этого
        // наш cpu: мало cpu = поток не работал, а ждал чужого (своп/GPU или
        // чужую блокировку). Гладкие 60 fps (17 мс) молчат. Замеры — vdso,
        // на кадр стоят сотни наносекунд.
        const auto slowWall0 = std::chrono::steady_clock::now();
        struct timespec slowCpu0{};
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &slowCpu0);
        double slowT[7]; int slowI = 0;
        auto slowMark = [&]() {
            slowT[slowI++] = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - slowWall0).count();
        };
        slowMark();   // 0: начало кадра
        drawBegin();

        esp_mem_frame_begin();   // кадр начался: кэш блоков памяти игры сброшен
        esp_viewmodel_tick(g_state.viewmodel); // independent of ESP/aim/farm
        LogStage(kStageEsp);
        DrawEspOverlay();
        slowMark();   // 1: после ESP
        UpdateAim(ImGui::GetIO().DeltaTime);
        slowMark();   // 2: после аима
        LogStage(kStageFarm);
        UpdateFarm(ImGui::GetIO().DeltaTime);
        slowMark();   // 3: после фарма
        LogStage(kStageMenu);
        RenderMenu();
        slowMark();   // 4: после меню
        LogStage(kStageFreecam);
        UpdateFreecam(ImGui::GetIO().DeltaTime);   // фрикам: полёт камеры
        AimEndFrame(); // закрыть lease, если меню/фрикам выключили сайлент позднее
        slowMark();   // 5: после фрикама
        LogStage(kStageFrameEnd);
        drawEnd();
        slowMark();   // 6: после свопа
        LogFrameBeat(frame);
        if (slowT[6] - slowT[0] > 100.0) {
            struct timespec slowCpu1{};
            clock_gettime(CLOCK_THREAD_CPUTIME_ID, &slowCpu1);
            const double slowCpu = (slowCpu1.tv_sec - slowCpu0.tv_sec) * 1000.0 +
                                   (slowCpu1.tv_nsec - slowCpu0.tv_nsec) * 1e-6;
            LogLine("тормоз: кадр %lu %.0fмс (cpu %.0f, фрикам %d): esp %.0f аим %.0f фарм %.0f меню %.0f фри %.0f своп %.0f",
                    frame, slowT[6] - slowT[0], slowCpu, (int)g_state.freecam_on,
                    slowT[1] - slowT[0], slowT[2] - slowT[1], slowT[3] - slowT[2],
                    slowT[4] - slowT[3], slowT[5] - slowT[4], slowT[6] - slowT[5]);
        }
        // Гистограмма кадров раз в секунду: ловит МИКРОтормоза (<100мс), ниже
        // порога «тормоза». Чистое наблюдение: на кадр не влияет.
        {
            static unsigned hist[5] = {};
            static double worst = 0.0, worstSplit[7] = {};
            static unsigned long worstFrame = 0;
            static double lastHist = -1e9;
            const double total = slowT[6] - slowT[0];
            hist[total < 20.0 ? 0 : total < 34.0 ? 1 : total < 50.0 ? 2 : total < 100.0 ? 3 : 4]++;
            if (total > worst) {
                worst = total; worstFrame = frame;
                for (int k = 0; k < 7; ++k) worstSplit[k] = slowT[k];
            }
            const double nowSec = std::chrono::duration<double>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            if (nowSec - lastHist >= 1.0) {
                lastHist = nowSec;
                // Сколько чтений памяти пришлось на кадр за секунду и сколько из
                // них отказало: отличает «аим раздул объём чтений» от «каждое
                // чтение ждёт в ядре» (см. esp_mem_reads_total).
                static unsigned long long lastReads = 0, lastFails = 0;
                const unsigned long long readsNow = esp_mem_reads_total();
                const unsigned long long failsNow = esp_mem_read_fails_total();
                const unsigned frames = hist[0] + hist[1] + hist[2] + hist[3] + hist[4];
                const double readsPerFrame = frames ? (double)(readsNow - lastReads) / (double)frames : 0.0;
                const unsigned long long failsDelta = failsNow - lastFails;
                lastReads = readsNow; lastFails = failsNow;
                LogLine("кадргист: %u/%u/%u/%u/%u (<20/<34/<50/<100/>=100мс), худший %lu %.0fмс (esp %.0f аим %.0f фарм %.0f меню %.0f фри %.0f своп %.0f) чтений/кадр %.0f отказов %llu",
                        hist[0], hist[1], hist[2], hist[3], hist[4], worstFrame, worst,
                        worstSplit[1] - worstSplit[0], worstSplit[2] - worstSplit[1],
                        worstSplit[3] - worstSplit[2], worstSplit[4] - worstSplit[3],
                        worstSplit[5] - worstSplit[4], worstSplit[6] - worstSplit[5],
                        readsPerFrame, failsDelta);
                hist[0] = hist[1] = hist[2] = hist[3] = hist[4] = 0;
                worst = 0.0; worstFrame = 0;
            }
        }
        g_frame_done.store(true);
    }
    LogClose();
    while (!g_frame_done.load()) {}
    // Сначала останавливаем фонового writer: во время восстановления камеры и
    // object-data он уже не должен менять ось выстрела.
    silent_aim::shutdown();
    // Камеру возвращаем на место ДО отвязки: во фрикаме она улетела от тела,
    // а после выхода чита возвращать её уже некому.
    esp_freecam_set(false);
    stop_attach_thread();
    esp_viewmodel_reset(true);
    process_detach();
    Blur::Free();
    CfgWatchFree();
    AudioFree();
    shutdown(); Touch_Close(); return 0;
}
