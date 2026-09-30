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
#include "overlay_geometry.h"
#include "mode_select.h"
#include "mem_io.h"
#include "kernel_io.h"
#include <sys/socket.h>
#include <sys/utsname.h>
#include <cerrno>
#include <cstring>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <ctime>
#include <sys/stat.h>
#include <thread>

// Диагностика кернел-режима из терминала (Termux/adb), без оверлея и игры:
//   su -c /data/local/tmp/xvcen --check-driver   — базовая проверка
//   su -c /data/local/tmp/xvcen --scan-driver    — + полный скан кодов команд
// Печатает: root/не root, ядро, семейство сокета FT-драйвера и класс маячка,
// живые пробы команд по собственной памяти (оба известных протокола FT),
// QX-драйвер (/dev-узел) и RT-драйвер (хук syscall ioctl), выбранный
// протокол и коды чтения/записи, а в режиме скана — карту всех кодов
// 0..0x7FF, которые двигают данные. Классы независимы: достаточно любого
// подтверждённого. Коды выхода: 0 — какой-то кернел-транспорт готов,
// 1 — не найдено НИЧЕГО, 2 — сокет FT есть, но коды не подтверждены
// (режим всё равно включается и проверяется привязкой к игре).
namespace driver_check {

static int Run(bool full_scan) {
    struct utsname un{};
    if (uname(&un) != 0) un.release[0] = 0;
    printf("== xvcen: проверка кернел режима%s ==\n",
           full_scan ? " (полный скан кодов команд)" : "");
    printf("ядро устройства : %s\n", un.release);
    printf("uid / euid      : %d / %d%s\n", getuid(), geteuid(),
           geteuid() == 0 ? " (root)" : "  <-- НЕ ROOT");
    if (geteuid() != 0)
        printf("!! кернел режим требует root: сокет драйвера создаётся только\n"
               "   привилегированным процессом, без root маячок не виден.\n"
               "   Запуск: su -c <путь>/xvcen\n");

    // 1) Поиск семейства сокета FT-драйвера по маячку на пробе socket(fam, 6, 0):
    //    1338 — bing-класс, 126/ENOKEY — gongchuang-класс. Раннего выхода
    //    здесь НЕТ: QX и RT не имеют сокета, их блоки ниже проверяются
    //    независимо от этого результата.
    int beacon = 0;
    const int fam = memio::driver_find_family(&beacon);
    const bool ft_present = (fam >= 0);
    if (!ft_present) {
        printf("FT-драйвер      : не найден (маячки errno %d/%d на семействах %d..%d)\n",
               memio::kDriverProbeErrno, memio::kGongProbeErrno,
               memio::kDriverFamilyFirst, memio::kDriverFamilyLast);
    } else {
        printf("FT-драйвер      : найден, семейство сокета %d, маячок errno %d (%s)\n",
               fam, beacon,
               beacon == memio::kDriverProbeErrno ? "bing-класс" : "gongchuang-класс");
    }

    // 2) Сокеты обоих классов (какие поднимутся) + живые пробы команд по
    //    СОБСТВЕННОЙ памяти: /proc не участвует, данные сойдутся, только если
    //    ядро реально сходило по страницам нашего процесса.
    memio::KernelTransport& kt = memio::kernel_transport();
    int bfd = -1, gfd = -1;
    if (ft_present) {
        errno = 0;
        bfd = kt.connect();
        printf("сокет bing      : %s%s\n",
               bfd >= 0 ? "поднят (тип 4)" : "не поднялся",
               bfd >= 0 ? "" : (errno ? " — не bing-класс" : ""));
        errno = 0;
        gfd = kt.gong_connect();
        printf("сокет gong      : %s%s\n",
               gfd >= 0 ? "поднят (SOCK_RAW/0x090419)" : "не поднялся",
               gfd >= 0 ? "" : " — не gongchuang-класс");

        printf("пробы известных команд (своя память, игра не трогается):\n");
        const int cand[4] = { memio::kDriverCmdRead, memio::kDriverCmdWrite, 0x300, 0x303 };
        for (int i = 0; i < 4; ++i) {
            const memio::CmdProbe p = kt.classify_cmd(cand[i]);
            if (p.kind == memio::CMD_READ)
                printf("  код 0x%03X: ЧТЕНИЕ — память -> наш буфер, OK\n", cand[i]);
            else if (p.kind == memio::CMD_WRITE)
                printf("  код 0x%03X: ЗАПИСЬ — наш буфер -> память, OK\n", cand[i]);
            else if (p.err != 0)
                printf("  код 0x%03X: отказ, errno %d\n", cand[i], p.err);
            else
                printf("  код 0x%03X: принята, но данные не двигались\n", cand[i]);
        }
        if (gfd >= 0) {
            const memio::CmdProbe vm = kt.classify_gong_vm(gfd, memio::kGongCmdAccessVm);
            printf("  gong op4      : %s\n", vm.kind == memio::CMD_READ
                   ? "ACCESS_PROCESS_VM работает (копия from -> to)" : "нет");
            if (kt.gong_attach((int)getpid())) {
                const memio::CmdProbe pr = kt.classify_pte(gfd, memio::kGongCmdPteRead);
                const memio::CmdProbe pw = kt.classify_pte(gfd, memio::kGongCmdPteWrite);
                printf("  gong op5/op6  : чтение %s, запись %s\n",
                       pr.kind == memio::CMD_READ ? "OK" : "нет",
                       pw.kind == memio::CMD_WRITE ? "OK" : "нет");
            }
        }
    }

    // 2б) QX-драйвер (класс wanbai): /dev-узел со случайным именем, команды
    //     ioctl 0x801/0x802. Пробы по собственной памяти, как у bing.
    bool qx_ok = false;
    {
        memio::QxTransport& qt = memio::qx_transport();
        const int qfd = qt.connect();
        if (qfd < 0) {
            printf("QX-драйвер     : узел не найден (известные имена + эвристика /dev)\n");
        } else {
            printf("QX-драйвер     : узел %s, fd %d\n", qt.device_path(), qfd);
            const memio::CmdProbe qp = qt.classify();
            if (qp.kind == memio::CMD_READ) {
                printf("  чтение 0x801 / запись 0x802: OK (своя память)\n");
                qx_ok = true;
            } else if (qp.err != 0)
                printf("  проба: отказ, errno %d (узел открыт, но это не QX или драйвер не отвечает)\n", qp.err);
            else
                printf("  проба: данные не двигались\n");
        }
    }

    // 2в) RT-драйвер (класс hookdd, RT-hook.7z): не имеет ни сокета, ни
    //     /dev-узла — подменяет sys_call_table[__NR_ioctl=29]. Проба —
    //     ioctl(-1, 0x259, ...) по собственной памяти: без драйвера ядро
    //     отвечает EBADF, с драйвером возвращается 0.
    bool rt_ok = false;
    {
        memio::RtTransport& rtt = memio::rt_transport();
        const memio::CmdProbe rp = rtt.classify();
        if (rp.kind == memio::CMD_READ) {
            printf("RT-драйвер     : хук syscall ioctl стоит\n");
            printf("  чтение 0x259 / запись 0x25a: OK (своя память)\n");
            rt_ok = true;
        } else if (rp.err == EBADF) {
            printf("RT-драйвер     : хука нет (ioctl уходит в ядро без перехвата)\n");
        } else if (rp.err == EPERM) {
            printf("RT-драйвер     : хук стоит, но проба без root (гейт uid/euid/capable)\n");
        } else if (rp.err != 0) {
            printf("RT-драйвер     : проба отказ, errno %d\n", rp.err);
        } else {
            printf("RT-драйвер     : ioctl принят, но данные не двигались\n");
        }
    }

    // 3) Автоопределение протокола FT — ровно то, что делает чит при выборе
    //    кернела (короткий список, затем полный скан при неудаче).
    if (ft_present) {
        kt.detect_protocol();
        printf("авторизация 0x300: %s\n",
               kt.auth_sent() ? "пароль отправлен (флаг в драйвере)" : "не потребовалась");
        printf("протокол        : %s%s\n", kt.proto_name(),
               kt.map_known() ? " (подтверждён пробами)" : " (НЕ подтверждён — коды дефолтные)");
        printf("транспорт команд: %s\n",
               kt.cmd_via_ioctl() ? "ioctl(fd, код, &req) — сборка 5.10-класса"
                                  : "getsockopt(fd, pid, код, &req, &len)");
    }

    // 4) Полный скан 0..0x7FF: карта ВСЕХ кодов, которые двигают данные.
    //    Нужен, если и автоопределение ничего не нашло: тогда по этой карте
    //    видно, какими кодами говорит эта сборка драйвера.
    if (full_scan && ft_present) {
        printf("полный скан кодов 0x000..0x7FF, bing-структура {pid,addr,buf,size,magic}:\n");
        int moved = 0, silent = 0, rd = 0, wr = 0;
        long errs = 0;
        for (int c = 0; c <= 0x7FF; ++c) {
            const memio::CmdProbe p = kt.classify_cmd(c);
            if (p.kind == memio::CMD_READ) {
                printf("  код 0x%03X: ЧТЕНИЕ\n", c);
                ++moved; if (!rd) rd = c;
            } else if (p.kind == memio::CMD_WRITE) {
                printf("  код 0x%03X: ЗАПИСЬ\n", c);
                ++moved; if (!wr) wr = c;
            } else if (p.err == 0) {
                printf("  код 0x%03X: принята, данные не двигались\n", c);
                ++silent;
            } else {
                ++errs;
            }
        }
        printf("итог скана      : команд с данными %d, «молчащих» %d, отказов %ld\n",
               moved, silent, errs);
        if (gfd >= 0 && kt.gong_attach((int)getpid())) {
            printf("скан pte-структурой {vaddr,buffer,size} на gong-сокете, коды 0..15:\n");
            for (int c = 0; c <= 15; ++c) {
                const memio::CmdProbe p = kt.classify_pte(gfd, c);
                if (p.kind == memio::CMD_READ)       printf("  код 0x%02X: ЧТЕНИЕ\n", c);
                else if (p.kind == memio::CMD_WRITE) printf("  код 0x%02X: ЗАПИСЬ\n", c);
                else if (p.err == 0)                 printf("  код 0x%02X: молчит\n", c);
                else                                 printf("  код 0x%02X: errno %d\n", c, p.err);
            }
        }
    }

    // 5) Итог: чем чит будет ходить в память игры. Классы независимы —
    //    достаточно любого подтверждённого транспорта.
    if (kt.map_known()) {
        printf("итог            : FT готов — протокол %s, чтение кодом 0x%03X,\n"
               "                  запись кодом 0x%03X; /proc не участвует\n",
               kt.proto_name(), kt.read_cmd(), kt.write_cmd());
        return 0;
    }
    if (qx_ok) {
        printf("итог            : QX готов — чтение 0x801 / запись 0x802; /proc не участвует\n");
        return 0;
    }
    if (rt_ok) {
        printf("итог            : RT готов — хук ioctl, чтение 0x259 / запись 0x25a;\n"
               "                  ни сокета, ни fd — самый скрытный класс\n");
        return 0;
    }
    if (ft_present) {
        printf("итог            : коды команд НЕ подтверждены. Включай кернел в меню —\n"
               "                  привязка к игре проверит транспорт по-настоящему;\n"
               "                  если откажет (тост с errno) — пришли вывод:\n"
               "                  su -c <путь>/xvcen --scan-driver\n");
        return 2;
    }
    printf("итог            : НИ ОДИН класс драйвера не найден (FT/QX/RT).\n"
           "   Прошей драйвер под СВОЮ версию ядра (uname -r) скриптом .sh\n"
           "   (drivers.7z — FT, kernal-based-Esp.zip — QX, RT-hook.7z — RT)\n"
           "   и повтори проверку. Драйверы не переживают перезагрузку —\n"
           "   после ребута прошивай заново.\n");
    const int fd = ::open("/proc/1/mem", O_RDWR | O_CLOEXEC);
    printf("экстернал       : /proc/<pid>/mem %s\n",
           fd >= 0 ? "доступен (root)" : "недоступен");
    if (fd >= 0) close(fd);
    return 1;
}

}  // namespace driver_check

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
    // Проверка драйвера из терминала (Termux/adb): su -c ./xvcen --check-driver
    // (базовая) или --scan-driver (полный скан кодов команд) — без GUI,
    // до запуска оверлея и игры.
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--check-driver") == 0) return driver_check::Run(false);
        if (strcmp(argv[i], "--scan-driver") == 0) return driver_check::Run(true);
    }

    signal(SIGINT,  [](int) { main_thread_flag.store(false); });
    signal(SIGTERM, [](int) { main_thread_flag.store(false); });
    signal(SIGHUP,  [](int) { main_thread_flag.store(false); });

    prot::Init();
    screen_config();

    mkdir(CfgDir(), 0777);
    const OverlayGeometry overlay =
        MakeOverlayGeometry(displayInfo.width, displayInfo.height);

    g_sw = static_cast<float>(overlay.scene_width);
    g_sh = static_cast<float>(overlay.scene_height);

    native_window_screen_x = overlay.scene_width;
    native_window_screen_y = overlay.scene_height;
    // Surface оставляем квадратным, как в old_source. Android композитор сам
    // поворачивает/обрезает overlay при смене ориентации; прямоугольный
    // landscape Surface (max x min) в portrait показывал только его верхний
    // фрагмент. Квадрат max x max инвариантен к 0/90/180/270 градусам, а
    // видимая геометрия меню всё равно ограничивается реальными width/height.
    if (!initGUI_draw(overlay.surface_side, overlay.surface_side, true)) return -1;
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
    // Экран выбора режима (экстернал / кернел-драйвер): собственный цикл
    // отрисовки, поток привязки стартует ТОЛЬКО после выбора — транспорт
    // памяти задаётся один раз, до первого bind.
    if (!modesel::RunModeSelect()) {
        // Приложение закрыли, не выбрав режим: выходим тем же порядком,
        // что и обычный выход (привязки и чит-конвейеров ещё нет — их
        // остановка здесь пустая, но последовательность та же).
        silent_aim::shutdown();
        esp_freecam_set(false);
        stop_attach_thread();
        esp_viewmodel_reset(true);
        process_detach();
        Blur::Free();
        CfgWatchFree();
        AudioFree();
        shutdown(); Touch_Close(); return 0;
    }
    start_attach_thread();
    LoadTabIcons();
    ApplyTheme();
    RestoreLang();          // язык из прошлого запуска (конфиг ниже может его переписать)
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
        // Индикация привязки (тосты «игра найдена / не найдена») убрана:
        // состояние видно по счётчику игроков в пилюле.
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
