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
#include <sys/syscall.h>   // __NR_gettid: номер потока кадра для сторожа

#include "app/attach.h"      // start_attach_thread/stop_attach_thread
#include "app/lifecycle.h"   // main_thread_flag, g_frame_done, prot::Init
#include "app/diag_log.h"   // журнал здоровья: его открывает этот же поток
#include "app/media.h"       // LoadAnimeImage, LoadTabIcons
#include "app/screen.h"      // g_sw, g_sh, CenterMenuOnDisplay
#include "ui/config.h"       // kCfgDir_, Cfg* — конфиги и язык
#include "ui/esp_overlay.h"  // DrawEspOverlay
#include "ui/settings.h"     // ui::bar
#include "ui/theme.h"        // ApplyTheme, g_menuFadeIn
#include "ui/window.h"       // RenderMenu
#include "aim/update.h"      // UpdateAim
#include "farm/controller.h" // UpdateFarm

// Какой сигнал попросил нас закончить (0 = обычный выход по своей воле).
// Записывается из обработчика как число и читается один раз в конце: так в
// журнале видно «нас попросили закрыться» отдельно от «мы завершились сами», а
// сам обработчик остаётся простым.
static std::atomic<int> g_exit_signal{0};

// Свой обработчик без SA_RESTART. Разница принципиальная: с SA_RESTART ядро
// ПЕРЕЗАПУСКАЕТ прерванный системный вызов, и поток, вставший в ожидание
// (обмен с системой, снятие кадра, запись на хранилище), сигнал просто не
// заметит — то есть разбудить зависший кадр будет нечем. Без SA_RESTART вызов
// возвращается с ошибкой, кадр доходит до конца цикла, и там уже видно, что
// пора уходить.
static void InstallExitSignal(int sig) {
    struct sigaction sa{};
    sa.sa_handler = [](int s) { g_exit_signal.store(s); main_thread_flag.store(false); };
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;              // без SA_RESTART — см. выше
    sigaction(sig, &sa, nullptr);
}

// Сторож кадра будит главный поток этим сигналом, когда тот не может закончить
// кадр (см. поток привязки в app/attach.cpp). Обработчик обязан быть: без него
// действие SIGUSR1 по умолчанию — завершить процесс, то есть «лечение» убило бы
// чит. Ничего, кроме отметки в атомарном флаге, здесь делать нельзя.
static std::atomic<bool> g_frame_wakeup{false};

int main(int argc, char* argv[]) {
    g_main_thread = pthread_self();      // кого будить сторожу кадра
    g_main_tid.store((int)syscall(__NR_gettid));   // и как он называется в ядре
    InstallExitSignal(SIGINT);
    InstallExitSignal(SIGTERM);
    InstallExitSignal(SIGHUP);
    {
        struct sigaction sa{};
        sa.sa_handler = [](int) { g_frame_wakeup.store(true); };
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction(SIGUSR1, &sa, nullptr);
    }

    prot::Init();
    screen_config();

    mkdir(kCfgDir_(), 0777);   // путь к каталогу конфигов — в ui/config
    diag_init(kCfgDir_());     // журнал здоровья рядом с конфигами (app/diag_log.h)
    diag_install_crash_handler();  // падение процесса тоже попадает в журнал

    // Попросить систему не убивать наш процесс первой. Оверлей живёт рядом с
    // игрой и на планшетах с небольшим объёмом памяти вполне может попасть под
    // системный менеджер памяти (LMK) — для человека это выглядит как «чит сам
    // выключился через некоторое время». Понижение приоритета на убийство
    // требует прав root (у нас они есть для чтения памяти игры); если не вышло,
    // просто отмечаем это в журнале — дальше видно по статистике процесса.
    {
        int fd = open("/proc/self/oom_score_adj", O_WRONLY | O_CLOEXEC);
        if (fd >= 0) {
            ssize_t w = write(fd, "-300", 4);
            close(fd);
            diag_log("app", "приоритет выживания (oom_score_adj): %s",
                     (w == 4) ? "понижен убийца памяти реже нас выбирает" : "не принят системой");
        } else {
            diag_log("app", "приоритет выживания (oom_score_adj): нет доступа к /proc/self");
        }
    }

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
    diag_log("app", "старт: экран %dx%d, ориентация %u, темп оверлея %.0f Гц, пик панели %.0f Гц, "
                    "сборка %s, тач: %s",
             abs_ScreenX, abs_ScreenY, displayInfo.orientation, overlay_pace_hz(), overlay_peak_hz(),
             go::CurrentBuild() == go::Build::Beta ? "бета" : "релиз",
             Touch_CanInject() ? "инъекция есть" : "только чтение");

    // Если полноценный тач не поднялся с первого раза (гонка за /dev/uinput
    // или grab на старте — обычное дело сразу после запуска игры), чит раньше
    // навсегда оставался в read-only: автофарм «просто не идёт», пока не
    // перезапустишь. Теперь фоновый поток пробует поднять инъекцию заново.
    //
    // Чего нельзя было делать — и что тут переделано. Прежняя версия на каждой
    // попытке звала Touch_Close() и полный Touch_Init(): он перечисляет
    // /dev/input, НА ВРЕМЯ ЗАБИРАЕТ у игры тачскрин (EVIOCGRAB) и сбрасывает
    // состояние всех пальцев. На устройстве, где /dev/uinput закрыт (а это как
    // раз «некоторые устройства»), проба проваливалась всегда, и весь этот
    // цикл повторялся каждые 3 секунды — с рывками ввода у игры, сбросом
    // удерживаемого пальца аима и пересозданием читателей тача (через них же
    // меню получает тапы). Со стороны: «чит сам выключился» / «перестал
    // реагировать». Теперь сначала дешёвая проба uinput без захвата устройств,
    // и только если она прошла — настоящее пересоздание тача; интервал растёт
    // (3 с → 10 с → 30 с), чтобы не молотить этим вечно.
    static std::atomic<bool> s_touchRetryRun{true};
    std::thread([]() {
        int delay_seconds = 3;
        int attempts = 0;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(delay_seconds));
            if (!s_touchRetryRun.load() || !main_thread_flag.load()) return;
            if (Touch_CanInject()) { delay_seconds = 3; attempts = 0; continue; }
            if (!Touch_ProbeInject()) {
                // Инъекция недоступна в принципе (SELinux/нет root): трогать тач
                // нельзя — иначе игра потеряет ввод, а аим останется без пальца.
                if (++attempts == 1 || attempts % 20 == 0) {
                    diag_log("touch", "инъекция недоступна, проб не удалась (попытка %d, интервал %d с) — "
                                      "работаем в режиме чтения", attempts, delay_seconds);
                }
                if (delay_seconds < 10)      delay_seconds = 10;
                else if (delay_seconds < 30) delay_seconds = 30;
                continue;
            }
            Touch_Close();
            const bool ok = Touch_Init(displayInfo.width, displayInfo.height, displayInfo.orientation, false)
                         || Touch_Init(displayInfo.width, displayInfo.height, displayInfo.orientation, true);
            diag_log("touch", "переподключение тача: %s (попытка %d)",
                     Touch_CanInject() ? "инъекция поднялась" : (ok ? "только чтение" : "не вышло"), attempts + 1);
            delay_seconds = 3;
            attempts = 0;
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

    unsigned long long frames_drawn = 0;
    while (main_thread_flag) {
        g_frame_done.store(false);
        // Каждый шаг отмечен по имени: если кадр встанет, в журнале будет видно,
        // на чём именно (см. сторож кадра в app/attach.cpp).
        FrameStage("drawBegin");
        drawBegin();

        ui::bar::set_game_alpha(0.f);
        FrameStage("кадр памяти игры");
        esp_mem_frame_begin();   // кадр начался: кэш блоков памяти игры сброшен
        FrameStage("ESP");
        DrawEspOverlay();
        FrameStage("аим");
        UpdateAim(ImGui::GetIO().DeltaTime);
        FrameStage("автофарм");
        UpdateFarm(ImGui::GetIO().DeltaTime);
        FrameStage("меню");
        RenderMenu();
        FrameStage("снятие кадра");
        drawEnd();
        ++frames_drawn;
        FrameDone();
        // Сторож будил нас сигналом, пока кадр стоял. Отмечаем, что кадр после
        // этого доехал: по журналу сразу видно, помогла ли побудка.
        if (g_frame_wakeup.exchange(false))
            diag_log("app", "кадр, стоявший в ожидании, завершился после сигнала сторожа");
        g_frame_done.store(true);
    }
    // Почему чит закончил работу: свой выход, сигнал извне или падение (для
    // падения строка ниже не появится — вместо неё будет «=== ПАДЕНИЕ» из
    // обработчика). Это первое, что нужно смотреть в журнале, когда «чит
    // выключился сам».
    diag_log("app", "выход: %s, кадров нарисовано %llu",
             g_exit_signal.load() ? "получен сигнал завершения" : "по своей воле",
             frames_drawn);
    if (g_exit_signal.load())
        diag_log("app", "сигнал завершения: %d", g_exit_signal.load());
    diag_flush(300);   // хвост журнала: запись асинхронная, даём ей доехать
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
