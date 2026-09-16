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

void start_attach_thread() {
    g_attach_running.store(true);
    g_attach_thread = std::thread([]() {
        while (g_attach_running.load()) {
            if (!g_esp_attached) {
                pid_t pid = find_game_pid();
                const bool by_package = (pid > 0);
                if (pid <= 0) pid = find_unity_pid();
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
