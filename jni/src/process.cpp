#include "process.h"
#include "game.h"

#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <thread>
#include <unistd.h>

// Единственный поддерживаемый клиент: пакет текущего релиза игры.
static const char* const kTargetPackage = "com.catsbit.oxidesurvivalisland";

pid_t g_target_pid = -1;
bool  g_esp_attached = false;
static std::thread g_attach_thread;
static std::atomic<bool> g_attach_running{false};

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
    const char* pkgs[1] = {kTargetPackage};
    return cmdline_rank(pid, pkgs, 1) >= 1;
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
    const char* pkgs[1] = {kTargetPackage};
    const pid_t self = getpid();
    pid_t best = -1;
    int best_rank = -1;
    struct dirent* entry;
    while ((entry = readdir(dir))) {
        const pid_t pid = (pid_t)atoi(entry->d_name);
        if (pid <= 0 || pid == self) continue;
        const int rank = cmdline_rank(pid, pkgs, 1);
        if (rank < 0) continue;
        if (proc_libil2cpp(pid) == 1) { best = pid; break; }   // основной процесс игры — дальше не ищем
        if (best_rank < 0 || rank > best_rank) { best = pid; best_rank = rank; }
    }
    closedir(dir);
    return best;
}

// Процесс всё ещё тот самый? Одно чтение cmdline вместо полного перебора /proc:
// перебор каждые 1.5 с виден в ядре, а привязку проверять нужно.
static bool pid_still_game(pid_t pid) {
    char path[32];
    snprintf(path, sizeof(path), "/proc/%d", pid);
    if (access(path, F_OK) != 0) return false;
    return pid_cmdline_matches(pid) && proc_libil2cpp(pid) == 1;
}

void start_attach_thread() {
    g_attach_running.store(true);
    g_attach_thread = std::thread([]() {
        while (g_attach_running.load()) {
            if (!g_esp_attached) {
                pid_t pid = find_game_pid();
                if (pid <= 0) {
                    // Процесса игры нет: состояния не меняем, повторим попытку на
                    // следующем круге. Игроку об этом не сообщаем — тост «игра не
                    // запущена» только мешал (пока игра грузится, он вылезал всегда).
                } else if (esp_init(pid)) {
                    g_target_pid = pid;
                    g_esp_attached = true;
                } else {
                    // Нашли процесс, но привязаться не вышло: причина — из game.cpp
                    // (нет libil2cpp.so или память не читается). Состояние видно в
                    // esp_attach_state() для диагностики, тоста нет.
                }
            } else if (!pid_still_game(g_target_pid) || !esp_alive_check() ||
                       esp_wants_reattach()) {
                // Процесс сменился/умер или память перестала читаться (отобрали
                // доступ, перезапуск с тем же pid): привязываемся заново.
                esp_reset();
                g_esp_attached = false;
                g_target_pid = -1;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        }
    });
}

void stop_attach_thread() {
    g_attach_running.store(false);
    if (g_attach_thread.joinable()) g_attach_thread.join();
}
void process_detach() {
    esp_reset();
    g_esp_attached = false;
    g_target_pid = -1;
}
