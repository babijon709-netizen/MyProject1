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
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

// Единственный поддерживаемый клиент: пакет текущего релиза игры.
static const char* const kTargetPackage = "com.catsbit.oxidesurvivalisland";

pid_t g_target_pid = -1;
std::atomic<bool> g_esp_attached{false};
std::atomic<bool> g_attach_scanned{false};
// Official package or a renamed/cloned package candidate. Both still pass
// esp_init's release TypeInfo validation before attachment is accepted.
static bool g_target_by_package = true;
// Number of consecutive failed liveness probes. Kept for diagnostics/reset on
// a real process exit; a failed probe alone must not detach a still-running
// game, because selected kernel transports can recover their fd/socket later.
static int g_alive_check_failures = 0;
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
    if (best > 0) g_target_by_package = true;
    return best;
}

// A release APK may be installed through an app clone/work profile with a
// renamed applicationId. Find its Unity process as a candidate; esp_init then
// accepts it only when the fixed release TypeInfo offsets resolve the expected
// game classes, so this does not introduce another supported build.
static int find_renamed_release_candidates(pid_t* out, int capacity) {
    if (!out || capacity <= 0) return 0;
    DIR* dir = opendir("/proc");
    if (!dir) return 0;
    const pid_t self = getpid();
    int count = 0;
    struct dirent* entry;
    while (count < capacity && (entry = readdir(dir))) {
        const pid_t pid = (pid_t)atoi(entry->d_name);
        if (pid <= 0 || pid == self) continue;
        char path[32];
        snprintf(path, sizeof(path), "/proc/%d", pid);
        struct stat st{};
        if (stat(path, &st) != 0 || st.st_uid < 10000) continue;
        if (proc_libil2cpp(pid) == 1) out[count++] = pid;
    }
    closedir(dir);
    return count;
}

// Процесс всё ещё тот самый? Одно чтение cmdline вместо полного перебора /proc:
// перебор каждые 1.5 с виден в ядре, а привязку проверять нужно.
static bool pid_still_game(pid_t pid) {
    char path[32];
    snprintf(path, sizeof(path), "/proc/%d", pid);
    if (access(path, F_OK) != 0) return false;
    if (g_target_by_package && !pid_cmdline_matches(pid)) return false;
    return proc_libil2cpp(pid) == 1;
}

void start_attach_thread() {
    g_attach_running.store(true);
    g_attach_thread = std::thread([]() {
        while (g_attach_running.load()) {
            if (!g_esp_attached.load(std::memory_order_acquire)) {
                pid_t attached_pid = -1;
                const pid_t official = find_game_pid();
                g_attach_scanned.store(true);
                if (official > 0 && esp_init(official, true)) {
                    attached_pid = official;
                    g_target_by_package = true;
                } else if (official <= 0) {
                    // Try every renamed Unity application, not just the first:
                    // unrelated Unity apps are rejected by strict release
                    // TypeInfo validation in esp_init.
                    pid_t candidates[16] = {};
                    const int count = find_renamed_release_candidates(candidates, 16);
                    for (int i = 0; i < count; ++i) {
                        if (!esp_init(candidates[i])) continue;
                        attached_pid = candidates[i];
                        g_target_by_package = false;
                        break;
                    }
                }
                if (attached_pid > 0) {
                    g_target_pid = attached_pid;
                    g_esp_attached.store(true, std::memory_order_release);
                    g_alive_check_failures = 0;
                }
            } else {
                const bool same_process = pid_still_game(g_target_pid);
                if (!same_process) {
                    // Process death is final: all cached managed addresses now
                    // belong to another lifetime and must be discarded.
                    esp_reset();
                    g_esp_attached.store(false, std::memory_order_release);
                    g_target_pid = -1;
                    g_alive_check_failures = 0;
                } else if (esp_alive_check()) {
                    g_alive_check_failures = 0;
                } else {
                    // Do not detach from a live process merely because its
                    // memory transport missed several probes. Reader::verify()
                    // reopens the selected transport on every later probe; once
                    // a temporarily lost driver/fd returns, ESP resumes without
                    // the multi-minute process/class reattach cycle.
                    ++g_alive_check_failures;
                }
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
    g_esp_attached.store(false, std::memory_order_release);
    g_target_pid = -1;
    g_alive_check_failures = 0;
}

// CLI-команда dump: тонкие обёртки над теми же поисками, что у потока
// привязки (process.h), — без состояния потока и без чтения памяти.
pid_t process_find_game_pid() { return find_game_pid(); }
int process_find_unity_candidates(pid_t* out, int capacity) {
    return find_renamed_release_candidates(out, capacity);
}
