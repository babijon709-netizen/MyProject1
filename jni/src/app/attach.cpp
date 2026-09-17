// app/attach.cpp — Поиск процесса игры и поток привязки.
//
// Модуль разрезан из прежнего монолита jni/src/main.cpp;
// что здесь лежит и кто это зовёт — в шапке app/attach.h и в docs/CODE_MAP.md.


#include "app/common.h"
#include "aim/update.h"
#include "farm/controller.h"
#include "ui/config.h"
#include "ui/esp_overlay.h"
#include "ui/watermark.h"
#include "ui/window.h"
#include "app/attach.h"
#include "app/diag_log.h"
#include "app/lifecycle.h"    // шаг кадра и его пульс: сторож «чит завис»
#include <signal.h>           // pthread_kill: разбудить поток кадра
#include "esp/esp_time.h"     // mono_seconds: время для пульса и подтверждения
#include "esp/frame.h"        // g_frame_publish_fail_streak: причина перепривязки
#include "esp/mem.h"          // esp_alive_check / esp_rebind_memory и счётчики чтений

// Пакеты клиентов (релиз/бета). Play-бета ставится ТЕМ ЖЕ applicationId, что и
// релиз, — тогда имя пакета версию не различает, и какую версию читать, решает
// выбор человека (файл .build), а не имя процесса. Суффикс ниже — запасной
// вариант для сборок, которые ставятся отдельным APK: если у твоей беты такое
// имя, допиши строку, больше это нигде не зашито. Порядок поиска задаёт
// приоритет, а не ограничение: сначала пакет выбранной версии, затем второй
// (см. поток привязки ниже).
static const char* const kPackageRelease = "com.catsbit.oxidesurvivalisland";

static const char* const kPackageBeta    = "com.catsbit.oxidesurvivalisland.beta";

static const char* TargetPackageA() {
    return (go::CurrentBuild() == go::Build::Beta) ? kPackageBeta : kPackageRelease;
}

static const char* TargetPackageB() {
    return (go::CurrentBuild() == go::Build::Beta) ? kPackageRelease : kPackageBeta;
}

pid_t g_target_pid = -1;

bool  g_esp_attached = false;

// Привязка найдена по пакету (true) или по отображённому libil2cpp.so (false):
// от этого зависит, как проверять, что процесс всё ещё тот.
static bool  g_target_by_package = true;

static std::thread g_attach_thread;

static std::atomic<bool> g_attach_running{false};

// Стартовый экран выбора версии игры (релиз или бета). Пока он открыт, меню не
// рисуется, а аим и автофарм не трогают камеру — иначе бот водил бы палец прямо
// под пальцем пользователя, пока тот выбирает. См. RenderMenu.
bool g_buildPrompt = true;

// ---- Поиск процесса игры ----------------------------------------------------
// Раньше брался первый процесс, у которого cmdline совпал с пакетом. Процессов
// с таким cmdline на устройстве бывает несколько (клон приложения, рабочий
// профиль, «второе пространство»), и не у каждого отображён libil2cpp.so: если
// попадался не тот, привязка оставалась ложной навсегда — меню рисовалось, а
// все функции молчали. Теперь кандидат проверяется по карте памяти, а перебор
// заканчивается на первом пригодном (лишних обращений к /proc не делаем).

// 2 — основной процесс пакета, 1 — его дочерний (pkg:...), -1 — не он.
static int cmdline_rank(pid_t pid, const char* const* pkgs, int pkg_count) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    char command[256] = {};
    const ssize_t n = read(fd, command, sizeof(command) - 1);
    close(fd);
    if (n <= 0) return -1;
    for (int i = 0; i < pkg_count; ++i) {
        if (!pkgs[i] || !pkgs[i][0]) continue;
        if (strcmp(command, pkgs[i]) == 0) return 2;
        const size_t len = strlen(pkgs[i]);
        if (strncmp(command, pkgs[i], len) == 0 && command[len] == ':') return 1;
    }
    return -1;
}

static bool pid_cmdline_matches(pid_t pid) {
    const char* pkgs[2] = {TargetPackageA(), TargetPackageB()};
    return cmdline_rank(pid, pkgs, 2) >= 1;
}

// 1 — libil2cpp.so отображён, 0 — нет, -1 — карта не читается.
static int proc_libil2cpp(pid_t pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    std::string text;
    char buf[8192];
    for (;;) {
        const ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            text.append(buf, (size_t)n);
            if (text.size() > (8u << 20)) break;   // столько карта не занимает
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        break;
    }
    close(fd);
    return text.find("libil2cpp.so") != std::string::npos ? 1 : 0;
}

static pid_t find_game_pid() {
    DIR* dir = opendir("/proc");
    if (!dir) return -1;
    const char* pkgs[2] = {TargetPackageA(), TargetPackageB()};
    const pid_t self = getpid();
    pid_t best = -1;
    int best_rank = -1;
    struct dirent* entry;
    while ((entry = readdir(dir))) {
        const pid_t pid = (pid_t)atoi(entry->d_name);
        if (pid <= 0 || pid == self) continue;
        const int rank = cmdline_rank(pid, pkgs, 2);
        if (rank < 0) continue;
        if (proc_libil2cpp(pid) == 1) { best = pid; break; }   // основной процесс игры — дальше не ищем
        if (best_rank < 0 || rank > best_rank) { best = pid; best_rank = rank; }
    }
    closedir(dir);
    if (best > 0) g_target_by_package = true;
    return best;
}

// Пакет мог быть переименован (клон приложения, рабочий профиль, сборка с другим
// applicationId) — тогда ищем просто процесс с отображённым libil2cpp.so. Чужие
// приложения не цепляем: только процессы-приложения (uid >= 10000, то есть не
// система и не root).
static pid_t find_unity_pid() {
    DIR* dir = opendir("/proc");
    if (!dir) return -1;
    const pid_t self = getpid();
    pid_t found = -1;
    struct dirent* entry;
    while ((entry = readdir(dir))) {
        const pid_t pid = (pid_t)atoi(entry->d_name);
        if (pid <= 0 || pid == self) continue;
        char path[32];
        snprintf(path, sizeof(path), "/proc/%d", pid);
        struct stat st{};
        if (stat(path, &st) != 0 || st.st_uid < 10000) continue;
        if (proc_libil2cpp(pid) == 1) { found = pid; g_target_by_package = false; break; }
    }
    closedir(dir);
    return found;
}

// Процесс всё ещё тот самый? Одно чтение cmdline вместо полного перебора /proc:
// перебор каждые 1.5 с виден в ядре, а привязку проверять нужно.
static bool pid_still_game(pid_t pid) {
    char path[32];
    snprintf(path, sizeof(path), "/proc/%d", pid);
    if (access(path, F_OK) != 0) return false;
    if (g_target_by_package) return pid_cmdline_matches(pid);
    return proc_libil2cpp(pid) == 1;
}

// Имя состояния привязки — для журнала здоровья.
static const char* AttachStateName(EspAttachState state) {
    switch (state) {
        case ESP_ATTACH_OK:        return "ок";
        case ESP_ATTACH_NO_PID:    return "нет процесса игры";
        case ESP_ATTACH_NO_LIB:    return "нет libil2cpp.so";
        case ESP_ATTACH_NO_ACCESS: return "память не читается";
    }
    return "?";
}

// Доступ к памяти потерян по-настоящему?
//
// Одно чтение ELF-заголовка отвечает на вопрос «читается ли процесс», и одиночный
// его отказ на устройстве под нагрузкой бывает случайным: игра грузит мир, память
// под давлением, страница ушла в zram, дескриптор стал негодным. Раньше ЛЮБОЙ
// отказ сразу вёл к esp_reset() — обнулялись классы, смещения, кадр, — и чит
// оставался выключенным до следующей удачной привязки. Со стороны это и есть
// «чит сам выключился через некоторое время», а на слабом устройстве (планшет,
// который и греется, и тормозит) — ещё и надолго.
//
// Теперь потеря доступа подтверждается замером: сначала пробуем самое дешёвое —
// переоткрыть /proc/<pid>/mem и перечитать пробу; если не помогло, ждём 150 мс и
// проверяем ещё раз. Только после этого привязку рвём. Настоящий уход процесса
// ловится отдельно, по /proc (pid_still_game), и он по-прежнему мгновенный.
static bool access_confirmed_lost(pid_t pid, const char** why, int& err_out) {
    if (esp_alive_check()) return false;
    if (esp_rebind_memory()) {
        diag_log("attach", "чтение мигнуло — переоткрыли /proc/%d/mem, доступ вернулся", (int)pid);
        return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    if (esp_alive_check()) return false;
    err_out = g_mem.last_error();
    if (g_mem.last_error_was_open()) *why = "открытие /proc/<pid>/mem";
    else if (g_mem.fail_phase() && g_mem.fail_phase()[0]) *why = g_mem.fail_phase();
    else *why = "чтение памяти";
    return true;
}

// ---- Сторож кадра ----------------------------------------------------------
//
// Зачем. Жалоба «чит в один момент полностью завис» раньше не оставляла в журнале
// ничего: последний пульс — и тишина, ни «выхода», ни «ПАДЕНИЯ». Причина в том,
// что зависает ГЛАВНЫЙ поток: он куда-то встал (обмен с системой, снятие кадра,
// внешнее хранилище) и кадры перестали идти, а писать о поломке было некому —
// поток привязки жив, но о кадре он ничего не знал.
//
// Теперь знает: главный поток каждый кадр отмечает шаг и время, а этот сторож
// (он же поток привязки) раз в полторы секунды смотрит, давно ли кадр
// заканчивался. Встал — пишем, на каком шаге и в каком состоянии поток (S/D/R по
// /proc), а после 20 с пробуем его разбудить сигналом: обработчик SIGUSR1 в
// main.cpp поставлен БЕЗ SA_RESTART, поэтому прерванный системный вызов вернётся
// с ошибкой и кадр доедет до конца цикла. Дальше видно по журналу, помогло ли.
static void FrameWatchTick() {
    static double last_report = 0.0;
    static int    wakeups = 0;

    const double beat = g_frame_heartbeat.load();
    if (beat <= 0.0) return;               // кадров ещё не было — сторожить нечего
    const double now = mono_seconds();
    const double stalled = now - beat;
    if (stalled < 6.0) {
        // Кадры снова идут. Если до этого мы будили поток — отмечаем, что помогло.
        if (wakeups > 0) {
            diag_log("app", "поток кадра снова рисует (пробуждений сигналом было: %d)", wakeups);
            wakeups = 0;
        }
        return;
    }

    if (now - last_report >= 10.0) {
        last_report = now;
        // Состояние главного потока: D — ждёт в системном вызове, R — работает,
        // S — спит. Вместе с шагом кадра этого хватает, чтобы понять, где встал.
        char state[96] = "нет данных";
        const int tid = g_main_tid.load();
        if (tid > 0) {
            char path[64];
            snprintf(path, sizeof(path), "/proc/self/task/%d/stat", tid);
            int fd = open(path, O_RDONLY | O_CLOEXEC);
            if (fd >= 0) {
                char buf[512];
                ssize_t n = read(fd, buf, sizeof(buf) - 1);
                close(fd);
                if (n > 0) {
                    buf[n] = 0;
                    char* close_paren = strrchr(buf, ')');
                    if (close_paren && close_paren[1] == ' ')
                        snprintf(state, sizeof(state), "%c", close_paren[2]);
                }
            }
        }
        diag_log("app", "КАДР НЕ ЗАВЕРШАЕТСЯ %.1f с: шаг «%s», поток кадра в состоянии %s "
                        "(кадров всего %llu, потоков живо — см. пульс)",
                 stalled, g_frame_stage.load(), state, g_frame_count.load());
    }

    if (stalled > 20.0 && g_main_thread) {
        // Будим: сигнал прервёт ожидание. Если поток в этот момент считает, а не
        // ждёт, вреда нет — обработчик только ставит флаг.
        pthread_kill(g_main_thread, SIGUSR1);
        if (++wakeups <= 3)
            diag_log("app", "пробую разбудить поток кадра сигналом (%.1f с без кадра)", stalled);
    }
}

void start_attach_thread() {
    g_attach_running.store(true);
    g_attach_thread = std::thread([]() {
        int  failed_init_attempts = 0;   // подряд неудачных попыток привязки
        int  attach_count = 0;           // сколько раз привязывались за сеанс
        double pulse_at = 0.0;           // когда писать «пульс» в журнал
        unsigned long long pulse_reads = 0, pulse_fails = 0;
        bool  logged_no_process = false;
        while (g_attach_running.load()) {
            const double now = mono_seconds();
            if (!g_esp_attached) {
                pid_t pid = find_game_pid();
                if (pid <= 0) pid = find_unity_pid();
                if (pid <= 0) {
                    // Процесса игры нет: состояния не меняем, повторим попытку на
                    // следующем круге. Игроку об этом не сообщаем — тост «игра не
                    // запущена» только мешал (пока игра грузится, он вылезал всегда).
                    if (!logged_no_process) {
                        logged_no_process = true;
                        diag_log("attach", "процесса игры нет — ждём (сборка %s)",
                                 go::CurrentBuild() == go::Build::Beta ? "бета" : "релиз");
                    }
                } else if (esp_init(pid)) {
                    g_target_pid = pid;
                    g_esp_attached = true;
                    logged_no_process = false;
                    failed_init_attempts = 0;
                    ++attach_count;
                    diag_log("attach", "привязались: pid=%d база=0x%llx сборка=%s (привязка №%d)",
                             (int)pid, (unsigned long long)g_il2cpp_base,
                             go::CurrentBuild() == go::Build::Beta ? "бета" : "релиз", attach_count);
                } else {
                    // Нашли процесс, но привязаться не вышло: нет libil2cpp.so или
                    // память не читается. В журнал — раз в несколько попыток, чтобы
                    // не залить файл, когда игра ещё грузится.
                    if (++failed_init_attempts == 1 || failed_init_attempts % 20 == 0) {
                        diag_log("attach", "привязка не вышла: pid=%d состояние=%s errno=%d (попыток %d)",
                                 (int)pid, AttachStateName(esp_attach_state()),
                                 g_mem.last_error(), failed_init_attempts);
                    }
                }
            } else if (!pid_still_game(g_target_pid)) {
                // Процесс сменился или умер: это потеря по-настоящему, привязываемся
                // заново (кэши мира обнулять обязательно — адреса чужие).
                diag_log("attach", "процесс ушёл: pid=%d — перепривязка", (int)g_target_pid);
                esp_reset();
                g_esp_attached = false;
                g_target_pid = -1;
            } else if (esp_wants_reattach()) {
                // Сторож ESP трижды сбросил кэши мира и не получил кадр: дело не в
                // кэшах, а в самой привязке (база, права, перезапуск игры).
                diag_log("attach", "перепривязка по сторожу ESP (кадров без публикации: %d)",
                         g_frame_publish_fail_streak);
                esp_reset();
                g_esp_attached = false;
                g_target_pid = -1;
            } else {
                const char* why = "?";
                int err = 0;
                if (access_confirmed_lost(g_target_pid, &why, err)) {
                    diag_log("attach",
                             "доступ к памяти потерян (%s, errno=%d): чтений=%llu отказов=%llu "
                             "мусорных=%llu обрывов=%llu переоткрытий=%llu, fps=%.0f — перепривязка",
                             why, err, g_mem.reads_total(), g_mem.read_fails_total(),
                             g_mem.junk_total(), g_mem.cut_total(), g_mem.reopens_total(), overlay_fps());
                    esp_reset();
                    g_esp_attached = false;
                    g_target_pid = -1;
                }
            }

            // Пульс: раз в 30 с одна строка о том, как шли дела. По ней видно, что
            // было ДО поломки (частота кадров, отказы чтений), а не только факт
            // «выключилось».
            if (now - pulse_at >= 30.0) {
                pulse_at = now;
                const unsigned long long reads = g_mem.reads_total();
                const unsigned long long fails = g_mem.read_fails_total();
                char self[192];
                diag_self_stats(self, sizeof(self));
                diag_log("health", "пульс: привязано=%s fps=%.0f чтений=%llu(+%llu) отказов=%llu(+%llu) "
                                   "инъекция=%s, %s",
                         g_esp_attached ? "да" : "нет", overlay_fps(), reads, reads - pulse_reads,
                         fails, fails - pulse_fails, Touch_CanInject() ? "есть" : "нет", self);
                pulse_reads = reads;
                pulse_fails = fails;
            }
            FrameWatchTick();
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        }
    });
}

void stop_attach_thread() {
    g_attach_running.store(false);
    if (g_attach_thread.joinable()) g_attach_thread.join();
}
