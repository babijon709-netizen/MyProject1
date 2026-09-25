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
#include "remote_gate.h"
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

    if (LogOpen()) LogLine("журнал: %s", LogPath());
    if (!RemoteGateStart()) {
        LogLine("запуск остановлен: RemoteGateStart отказал");
        LogClose();
        return 1;
    }

    prot::Init();
    screen_config();

    mkdir(CfgDir(), 0777);
    int abs_ScreenX = displayInfo.height > displayInfo.width ? displayInfo.height : displayInfo.width;
    int abs_ScreenY = displayInfo.height < displayInfo.width ? displayInfo.height : displayInfo.width;

    g_sw = static_cast<float>(abs_ScreenX);
    g_sh = static_cast<float>(abs_ScreenY);

    native_window_screen_x = abs_ScreenX;
    native_window_screen_y = abs_ScreenY;
    if (!initGUI_draw(native_window_screen_x, native_window_screen_x, true)) {
        LogLine("запуск остановлен: initGUI_draw отказал");
        LogClose();
        return -1;
    }
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
    LoadTabIcons();
    ApplyTheme();
    RestoreLang();          // язык из прошлого запуска (конфиг ниже может его переписать)
    RestoreBuild();         // версия игры из прошлого запуска — до привязки к процессу
    RestoreAccent();        // акцент интерфейса из прошлого запуска
    RestoreLayout();        // кнопки и тач-зоны из прошлого запуска (точку аима конфиг ниже может переписать)
    start_attach_thread();
    CfgScanDir();
    ConfigLoadLast();
    ApplyTheme();
    CenterMenuOnDisplay();
    g_menuFadeIn = 0.f;

    // Low-overhead frame probe. Five-second windows record both stage maxima
    // and the breakdown of the slowest full frame; logging every frame would
    // itself create the pauses this probe is meant to diagnose.
    using ProbeClock = std::chrono::steady_clock;
    struct FrameProbeWindow {
        unsigned long long frames = 0, slow_frames = 0;
        unsigned long long reads_sum = 0, aim_reads_sum = 0;
        unsigned long long fails_sum = 0, resolves_sum = 0;
        unsigned long long peak_reads = 0, peak_aim_reads = 0;
        unsigned long long peak_fails = 0, peak_resolves = 0;
        double total_sum_ms = 0.0, peak_total_ms = 0.0;
        double peak_begin_ms = 0.0, peak_viewmodel_ms = 0.0, peak_esp_ms = 0.0;
        double peak_aim_ms = 0.0, peak_farm_ms = 0.0, peak_menu_ms = 0.0;
        double peak_freecam_ms = 0.0, peak_draw_end_ms = 0.0;
        double max_esp_ms = 0.0, max_aim_ms = 0.0, max_menu_ms = 0.0;
        double max_draw_end_ms = 0.0;
    } probe;
    auto probe_window_start = ProbeClock::now();
    auto elapsed_ms = [](ProbeClock::time_point a, ProbeClock::time_point b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };

    while (main_thread_flag) {
        const auto frame_start = ProbeClock::now();
        const unsigned long long reads_start = esp_mem_reads_total();
        const unsigned long long fails_start = esp_mem_read_fails_total();
        const unsigned long long resolves_start = esp_mem_aim_resolve_attempts_total();

        g_frame_done.store(false);
        drawBegin();
        esp_mem_frame_begin();   // кэш чтений памяти игры сбрасывается каждый кадр
        const auto begin_done = ProbeClock::now();
        esp_viewmodel_tick(g_state.viewmodel);
        const auto viewmodel_done = ProbeClock::now();
        DrawEspOverlay();
        const auto esp_done = ProbeClock::now();
        const unsigned long long reads_after_esp = esp_mem_reads_total();
        UpdateAim(ImGui::GetIO().DeltaTime);
        const auto aim_done = ProbeClock::now();
        const unsigned long long reads_after_aim = esp_mem_reads_total();
        const unsigned long long aim_reads_frame = reads_after_aim - reads_after_esp;
        UpdateFarm(ImGui::GetIO().DeltaTime);
        const auto farm_done = ProbeClock::now();
        RenderMenu();
        AimEndFrame();
        const auto menu_done = ProbeClock::now();
        UpdateFreecam(ImGui::GetIO().DeltaTime);
        const auto freecam_done = ProbeClock::now();
        drawEnd();
        const auto frame_done = ProbeClock::now();
        g_frame_done.store(true);

        const double begin_ms = elapsed_ms(frame_start, begin_done);
        const double viewmodel_ms = elapsed_ms(begin_done, viewmodel_done);
        const double esp_ms = elapsed_ms(viewmodel_done, esp_done);
        const double aim_ms = elapsed_ms(esp_done, aim_done);
        const double farm_ms = elapsed_ms(aim_done, farm_done);
        const double menu_ms = elapsed_ms(farm_done, menu_done);
        const double freecam_ms = elapsed_ms(menu_done, freecam_done);
        const double draw_end_ms = elapsed_ms(freecam_done, frame_done);
        const double total_ms = elapsed_ms(frame_start, frame_done);
        const unsigned long long reads_frame = esp_mem_reads_total() - reads_start;
        const unsigned long long fails_frame = esp_mem_read_fails_total() - fails_start;
        const unsigned long long resolves_frame =
            esp_mem_aim_resolve_attempts_total() - resolves_start;

        ++probe.frames;
        probe.total_sum_ms += total_ms;
        probe.reads_sum += reads_frame;
        probe.aim_reads_sum += aim_reads_frame;
        probe.fails_sum += fails_frame;
        probe.resolves_sum += resolves_frame;
        if (total_ms >= 33.3) ++probe.slow_frames;
        if (reads_frame > probe.peak_reads) probe.peak_reads = reads_frame;
        if (aim_reads_frame > probe.peak_aim_reads) probe.peak_aim_reads = aim_reads_frame;
        if (fails_frame > probe.peak_fails) probe.peak_fails = fails_frame;
        if (resolves_frame > probe.peak_resolves) probe.peak_resolves = resolves_frame;
        if (esp_ms > probe.max_esp_ms) probe.max_esp_ms = esp_ms;
        if (aim_ms > probe.max_aim_ms) probe.max_aim_ms = aim_ms;
        if (menu_ms > probe.max_menu_ms) probe.max_menu_ms = menu_ms;
        if (draw_end_ms > probe.max_draw_end_ms) probe.max_draw_end_ms = draw_end_ms;
        if (total_ms > probe.peak_total_ms) {
            probe.peak_total_ms = total_ms;
            probe.peak_begin_ms = begin_ms;
            probe.peak_viewmodel_ms = viewmodel_ms;
            probe.peak_esp_ms = esp_ms;
            probe.peak_aim_ms = aim_ms;
            probe.peak_farm_ms = farm_ms;
            probe.peak_menu_ms = menu_ms;
            probe.peak_freecam_ms = freecam_ms;
            probe.peak_draw_end_ms = draw_end_ms;
        }

        const auto now = ProbeClock::now();
        if (elapsed_ms(probe_window_start, now) >= 5000.0) {
            const double avg_ms = probe.frames ? probe.total_sum_ms / probe.frames : 0.0;
            const double avg_reads = probe.frames
                ? static_cast<double>(probe.reads_sum) / probe.frames : 0.0;
            const double avg_aim_reads = probe.frames
                ? static_cast<double>(probe.aim_reads_sum) / probe.frames : 0.0;
            const double avg_fails = probe.frames
                ? static_cast<double>(probe.fails_sum) / probe.frames : 0.0;
            const double avg_resolves = probe.frames
                ? static_cast<double>(probe.resolves_sum) / probe.frames : 0.0;
            LogLine("кадр-профиль: aim_mode=%d n=%llu avg=%.1fms peak=%.1fms "
                    "peakStages[begin=%.1f viewmodel=%.1f ESP=%.1f aim=%.1f farm=%.1f UI=%.1f freecam=%.1f drawEnd=%.1f] "
                    "stageMax[ESP=%.1f aim=%.1f UI=%.1f drawEnd=%.1f] slow>33ms=%llu "
                    "memReads=%.0f/frame peak=%llu aimReads=%.0f/frame peak=%llu "
                    "readFails=%.1f/frame peak=%llu MouseLookResolve=%.2f/frame peak=%llu",
                    g_state.aim_mode, probe.frames, avg_ms, probe.peak_total_ms,
                    probe.peak_begin_ms, probe.peak_viewmodel_ms, probe.peak_esp_ms,
                    probe.peak_aim_ms, probe.peak_farm_ms, probe.peak_menu_ms,
                    probe.peak_freecam_ms, probe.peak_draw_end_ms,
                    probe.max_esp_ms, probe.max_aim_ms, probe.max_menu_ms,
                    probe.max_draw_end_ms, probe.slow_frames, avg_reads,
                    probe.peak_reads, avg_aim_reads, probe.peak_aim_reads,
                    avg_fails, probe.peak_fails, avg_resolves, probe.peak_resolves);
            probe = FrameProbeWindow{};
            probe_window_start = now;
        }
    }
    silent_aim::shutdown();
    while (!g_frame_done.load()) {}
    // Камеру возвращаем на место ДО отвязки: во фрикаме она улетела от тела,
    // а после выхода чита возвращать её уже некому.
    esp_freecam_set(false);
    stop_attach_thread();
    esp_viewmodel_reset(true);
    process_detach();
    Blur::Free();
    CfgWatchFree();
    AudioFree();
    shutdown(); Touch_Close();
    LogLine("штатное завершение");
    LogClose();
    return 0;
}
