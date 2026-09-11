// ============================================================================
//  driver.cpp — доступ к памяти игры в двух режимах: NONKERNEL и KERNEL.
//
//  KERNEL-режим поднимает FT-драйвер (FTDriver.zip): скрипт подбирается под
//  uname() телефона, копируется в /data/local/tmp и запускается через su.
//  После патча ядра чтение/запись памяти игры идут через ядро: сначала
//  process_vm_readv/writev (ядровой путь), при отказе — /proc/<pid>/mem.
//
//  Подбирание скрипта: точное совпадение x.y.z > генерик под x.y (5.10.sh) >
//  другой патч того же x.y. Вендорские сборки (-ColorOS, oneplus, oppo)
//  предпочитаются на соответствующих прошивках. Варианты b/c — запасные.
// ============================================================================
#include "driver.h"

#include <sys/uio.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/system_properties.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <signal.h>
#include <errno.h>
#include <cctype>
#include <cstdarg>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <string>
#include <chrono>
#include "applog.h"
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <algorithm>

namespace driver {

// ---- Глобальное состояние ---------------------------------------------------
static std::atomic<int> g_mode{(int)Mode::NonKernel};

static char g_kernel_release[96]  = {};

static std::mutex g_mem_mtx;              // защищает кэш fd /proc/<pid>/mem
static int  g_mem_fd  = -1;
static pid_t g_mem_pid = -1;
static bool g_mem_rw   = false;           // fd открыт на чтение+запись
static std::atomic<bool> g_prefers_mem{false}; // process_vm не пошёл — сидим на mem

static Stats g_stats{};

// ---- Прямые syscalls (NONKERNEL путь, как было) ------------------------------
static inline ssize_t pv_readv(pid_t pid, const struct iovec* l, unsigned long lc,
                               const struct iovec* r, unsigned long rc, unsigned long fl) {
    return syscall(__NR_process_vm_readv, pid, l, lc, r, rc, fl);
}
static inline ssize_t pv_writev(pid_t pid, const struct iovec* l, unsigned long lc,
                                const struct iovec* r, unsigned long rc, unsigned long fl) {
    return syscall(__NR_process_vm_writev, pid, l, lc, r, rc, fl);
}

// ---- /proc/<pid>/mem — запасной (ядровой) путь --------------------------------
static int open_mem_locked(pid_t pid, bool for_write) {
    if (g_mem_pid == pid && g_mem_fd >= 0 && (!for_write || g_mem_rw))
        return g_mem_fd;
    if (g_mem_fd >= 0) { close(g_mem_fd); g_mem_fd = -1; g_mem_pid = -1; g_mem_rw = false; }
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/mem", pid);
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd >= 0) {
        g_mem_rw = true;
    } else if (!for_write) {
        fd = open(path, O_RDONLY | O_CLOEXEC);
        g_mem_rw = false;
    }
    if (fd < 0) return -1;
    g_mem_fd = fd;
    g_mem_pid = pid;
    return fd;
}

// Перенос содержимого между списками iovec через pread/pwrite.
// Возвращает число перенесённых байт (0 = ничего, -1 = ошибка).
static ssize_t mem_transfer(pid_t pid, bool is_write,
                            const struct iovec* local_iov, unsigned long liovcnt,
                            const struct iovec* remote_iov, unsigned long riovcnt) {
    std::lock_guard<std::mutex> lk(g_mem_mtx);
    int fd = open_mem_locked(pid, is_write);
    if (fd < 0) return -1;

    size_t total = 0;
    unsigned long li = 0;
    size_t loff = 0;
    bool any_ok = false;

    for (unsigned long ri = 0; ri < riovcnt; ri++) {
        size_t   rrem  = remote_iov[ri].iov_len;
        uintptr_t rbase = (uintptr_t)remote_iov[ri].iov_base;
        while (rrem > 0 && li < liovcnt) {
            if (loff >= local_iov[li].iov_len) { li++; loff = 0; continue; }
            size_t want = rrem;
            if (want > local_iov[li].iov_len - loff) want = local_iov[li].iov_len - loff;
            ssize_t n;
            if (is_write)
                n = pwrite(fd, (const char*)local_iov[li].iov_base + loff, want,
                           (off_t)(rbase + (remote_iov[ri].iov_len - rrem)));
            else
                n = pread(fd, (char*)local_iov[li].iov_base + loff, want,
                          (off_t)(rbase + (remote_iov[ri].iov_len - rrem)));
            if (n < 0) return any_ok ? (ssize_t)total : -1;
            if (n == 0) return (ssize_t)total;
            any_ok = true;
            total  += (size_t)n;
            rrem   -= (size_t)n;
            loff   += (size_t)n;
            if ((size_t)n < want) return (ssize_t)total;
        }
    }
    return (ssize_t)total;
}

// ---- Публичный iovec-интерфейс -----------------------------------------------
// KERNEL: ТОЛЬКО прямые syscalls process_vm_*v — копирование между адресными
// пространствами выполняет само ядро. Без файлов /proc/<pid>/mem и без
// запасных путей: если syscall отказал — это ошибка (пишем в лог не чаще
// раза в 15 с), тихого перехода на «обычное чтение» нет.
// NONKERNEL: process_vm, при отказе — /proc/<pid>/mem (как было раньше).
static std::atomic<bool>      g_kpath_logged{false};
static std::atomic<long long> g_kfail_ms{0};

static long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

ssize_t readv(pid_t pid, const struct iovec* local_iov, unsigned long liovcnt,
              const struct iovec* remote_iov, unsigned long riovcnt, unsigned long flags) {
    if (pid <= 0 || !local_iov || !remote_iov) { errno = EINVAL; return -1; }

    if ((Mode)g_mode.load() == Mode::Kernel) {
        ssize_t n = pv_readv(pid, local_iov, liovcnt, remote_iov, riovcnt, flags);
        if (n > 0) {
            g_stats.pv_reads++;
            if (!g_kpath_logged.exchange(true))
                applog::write("KERNEL: память читается напрямую через ядро "
                              "(process_vm syscall, uid=%d)", (int)getuid());
            return n;
        }
        if (n == 0) return 0;
        g_stats.failed_reads++;
        int e = errno;
        long long now = now_ms(), last = g_kfail_ms.load();
        if (now - last > 15000 && g_kfail_ms.compare_exchange_strong(last, now))
            applog::write("KERNEL: process_vm отказал (errno %d) — чтение через ядро недоступно", e);
        return n;
    }

    ssize_t n = pv_readv(pid, local_iov, liovcnt, remote_iov, riovcnt, flags);
    if (n > 0) { g_stats.pv_reads++; return n; }
    if (n == 0) return 0;
    n = mem_transfer(pid, false, local_iov, liovcnt, remote_iov, riovcnt);
    if (n > 0) {
        g_stats.mem_reads++;
        g_prefers_mem.store(true);
    } else {
        g_stats.failed_reads++;
    }
    return n;
}

ssize_t writev(pid_t pid, const struct iovec* local_iov, unsigned long liovcnt,
               const struct iovec* remote_iov, unsigned long riovcnt, unsigned long flags) {
    if (pid <= 0 || !local_iov || !remote_iov) { errno = EINVAL; return -1; }

    if ((Mode)g_mode.load() == Mode::Kernel) {
        ssize_t n = pv_writev(pid, local_iov, liovcnt, remote_iov, riovcnt, flags);
        if (n > 0) {
            g_stats.pv_writes++;
            if (!g_kpath_logged.exchange(true))
                applog::write("KERNEL: память пишется напрямую через ядро "
                              "(process_vm syscall, uid=%d)", (int)getuid());
            return n;
        }
        if (n == 0) return 0;
        g_stats.failed_writes++;
        int e = errno;
        long long now = now_ms(), last = g_kfail_ms.load();
        if (now - last > 15000 && g_kfail_ms.compare_exchange_strong(last, now))
            applog::write("KERNEL: process_vm_write отказал (errno %d)", e);
        return n;
    }

    ssize_t n = pv_writev(pid, local_iov, liovcnt, remote_iov, riovcnt, flags);
    if (n > 0) { g_stats.pv_writes++; return n; }
    if (n == 0) return 0;
    n = mem_transfer(pid, true, local_iov, liovcnt, remote_iov, riovcnt);
    if (n > 0) {
        g_stats.mem_writes++;
        g_prefers_mem.store(true);
    } else {
        g_stats.failed_writes++;
    }
    return n;
}

bool read_process_memory(pid_t pid, uint64_t addr, void* out, size_t len) {
    if (!addr || !out || !len) return false;
    struct iovec l = { out, len };
    struct iovec r = { (void*)addr, len };
    return readv(pid, &l, 1, &r, 1, 0) == (ssize_t)len;
}

bool write_process_memory(pid_t pid, uint64_t addr, const void* in, size_t len) {
    if (!addr || !in || !len) return false;
    struct iovec l = { (void*)in, len };
    struct iovec r = { (void*)addr, len };
    return writev(pid, &l, 1, &r, 1, 0) == (ssize_t)len;
}

Stats stats() { return g_stats; }

// ---- Проверка, что FT-драйвер прошит в ядро --------------------------------
// FT-драйвер патчит в ядре проверки доступа (ptrace_may_access и родственные):
// после прошивки ЧУЖУЮ память может читать даже непривилегированный процесс.
// Именно это и проверяем: ребёнок роняет свои права до обычного пользователя
// (uid 2000, shell) и пробует прочитать 8 байт кода нашего root-процесса
// прямым syscall process_vm_readv.
//   ядро без драйвера -> EPERM (читать чужое нельзя);
//   ядро с драйвером  -> чтение проходит.
// Так KERNEL-режим зависит именно от драйвера, а не от прав root.
bool driver_active() {
    int fds[2];
    if (pipe(fds) != 0) return false;
    uint64_t addr = (uint64_t)(uintptr_t)&driver_active;   // валидный адрес в нас

    pid_t child = fork();
    if (child < 0) { close(fds[0]); close(fds[1]); return false; }
    if (child == 0) {
        // ребёнок: непривилегированный пользователь
        close(fds[0]);
        if (setgid(2000) != 0 || setuid(2000) != 0) _exit(50);
        unsigned char buf[8];
        struct iovec l = { buf, sizeof(buf) };
        struct iovec r = { (void*)(uintptr_t)addr, sizeof(buf) };
        ssize_t n = pv_readv(getppid(), &l, 1, &r, 1, 0);
        if (n > 0) _exit(42);                    // прочиталось — драйвер в ядре
        unsigned char e = (unsigned char)(errno ? errno : 99);
        if (write(fds[1], &e, 1) < 0) {}         // отдадим errno родителю
        _exit(43);
    }
    close(fds[1]);

    int st = 0;
    bool done = false;
    for (int i = 0; i < 30 && !done; i++) {       // до 3 секунд
        if (waitpid(child, &st, WNOHANG) == child) { done = true; break; }
        usleep(100 * 1000);
    }
    if (!done) { kill(child, SIGKILL); waitpid(child, &st, 0); close(fds[0]);
                 applog::write("KERNEL: probe драйвера завис — считаю, что драйвера нет");
                 return false; }

    unsigned char e = 0;
    ssize_t got = read(fds[0], &e, 1);
    close(fds[0]);
    if (WIFEXITED(st) && WEXITSTATUS(st) == 42) {
        applog::write("KERNEL: FT-драйвер в ядре активен "
                      "(непривилегированное чтение прошло)");
        return true;
    }
    if (WIFEXITED(st) && WEXITSTATUS(st) == 50) {
        applog::write("KERNEL: не смог сбросить права для probe — считаю, что драйвера нет");
        return false;
    }
    applog::write("KERNEL: FT-драйвер не обнаружен в ядре (непривилегированное "
                  "чтение: errno %d)%s", (int)(got == 1 ? e : 0),
                  (got == 1 && e == 1) ? " — доступ запрещён, драйвер не прошит" : "");
    return false;
}

// ---- Режим -------------------------------------------------------------------
void set_mode(Mode m) {
    g_mode.store((int)m);
    // Смена pid-контекста: сбросить закэшированный mem-fd и предпочтение.
    g_prefers_mem.store(false);
    {
        std::lock_guard<std::mutex> lk(g_mem_mtx);
        if (g_mem_fd >= 0) { close(g_mem_fd); g_mem_fd = -1; g_mem_pid = -1; g_mem_rw = false; }
    }
}
Mode mode() { return (Mode)g_mode.load(); }
const char* mode_name() { return (Mode)g_mode.load() == Mode::Kernel ? "KERNEL" : "NONKERNEL"; }

// ---- su -------------------------------------------------------------------------
static bool find_su(char* out, size_t n) {
    if (getuid() == 0) { snprintf(out, n, "(root)"); return true; }
    static const char* kSuPaths[] = {
        "/system/bin/su", "/system/xbin/su", "/sbin/su", "/system/sd/xbin/su",
        "/su/bin/su", "/su/xbin/su", "/debug_ramdisk/su", "/data/local/tmp/su",
    };
    for (const char* p : kSuPaths) {
        if (access(p, X_OK) == 0) { snprintf(out, n, "%s", p); return true; }
    }
    return false;
}

// ---- Автоматическое получение root --------------------------------------------
// Софт рассчитан на запуск от root (как в логах: чтение памяти игры идёт
// через process_vm_readv, а без root ядро его просто запрещает). Если бинаррь
// запущен обычным пользователем и есть su — перезапускаем себя через su.
// Флаг --rooted в argv защищает от цикла перезапуска.
static bool su_probe_works(const char* su, bool use_dash_c) {
    int fds[2];
    if (pipe(fds) != 0) return false;
    pid_t child = fork();
    if (child < 0) { close(fds[0]); close(fds[1]); return false; }
    if (child == 0) {
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        close(fds[0]); close(fds[1]);
        if (use_dash_c) execl(su, su, "-c", "id", (char*)nullptr);
        else            execl(su, su, "0", "id", (char*)nullptr);
        _exit(127);
    }
    close(fds[1]);
    char buf[256] = {};
    ssize_t total = 0;
    // читаем с таймаутом 6 секунд (ждём ответа su / диалог Magisk)
    struct pollfd pfd = { fds[0], POLLIN, 0 };
    for (int waited = 0; waited < 60; waited++) {
        int r = poll(&pfd, 1, 100);
        if (r > 0) {
            ssize_t n = read(fds[0], buf + total, sizeof(buf) - 1 - total);
            if (n <= 0) break;
            total += n;
            if (strstr(buf, "uid=")) break;
        }
        int st = 0;
        if (waitpid(child, &st, WNOHANG) == child) break;
        usleep(100 * 1000);
    }
    close(fds[0]);
    int st = 0;
    kill(child, SIGKILL);
    waitpid(child, &st, 0);
    return total > 0 && strstr(buf, "uid=0");
}

// Проверяет, поднялась ли уже root-копия этого бинарника (su -> sh -> exe).
static bool scan_root_copy(const char* exe) {
    DIR* d = opendir("/proc");
    if (!d) return false;
    struct dirent* de;
    while ((de = readdir(d))) {
        if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
        char link[96], target[512];
        snprintf(link, sizeof(link), "/proc/%s/exe", de->d_name);
        ssize_t n = readlink(link, target, sizeof(target) - 1);
        if (n <= 0) continue;
        target[n] = 0;
        if (strcmp(target, exe) != 0) continue;
        char sp[96];
        snprintf(sp, sizeof(sp), "/proc/%s/status", de->d_name);
        FILE* f = fopen(sp, "r");
        if (!f) continue;
        char line[256];
        bool isroot = false;
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "Uid:", 4) == 0) {
                unsigned u = 1;
                if (sscanf(line + 4, "%u", &u) == 1) isroot = (u == 0);
                break;
            }
        }
        fclose(f);
        if (isroot) { closedir(d); return true; }
    }
    closedir(d);
    return false;
}

// Запускает root-копию процесса; родитель ждёт подтверждения и завершается.
// Возвращает false, если root получить не удалось (продолжаем как есть).
bool try_escalate_root(int argc, char* argv[]) {
    if (getuid() == 0) return true;               // уже root
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--rooted") == 0) return false;  // уже пробуем

    char su[128] = {};
    if (!find_su(su, sizeof(su))) {
        applog::write("root: su не найден (Magisk/KSU не установлен?)");
        fprintf(stderr, "[xvcen] su не найден: запусти от root, иначе память игры не прочитается\n");
        return false;
    }
    bool dash_c = su_probe_works(su, true);
    if (!dash_c && !su_probe_works(su, false)) {
        applog::write("root: %s не даёт root (диалог Magisk/KSU не подтверждён)", su);
        fprintf(stderr, "[xvcen] root через %s не выдан (проверь диалог Magisk/KSU)\n", su);
        return false;
    }
    applog::write("root: доступ есть (%s, режим su %s)", su, dash_c ? "-c" : "0");

    char exe[512] = {};
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return false;
    exe[n] = 0;

    // команда: "<exe>" --rooted [исходные аргументы]
    char cmd[1024] = {};
    int w = snprintf(cmd, sizeof(cmd), "\"%s\" --rooted", exe);
    for (int i = 1; i < argc && w > 0 && w < (int)sizeof(cmd) - 2; i++)
        w += snprintf(cmd + w, sizeof(cmd) - w, " \"%s\"", argv[i]);

    fprintf(stderr, "[xvcen] перезапускаюсь от root (%s)...\n", su);
    pid_t child = fork();
    if (child < 0) return false;
    if (child == 0) {
        setsid();
        if (dash_c) execl(su, su, "-c", cmd, (char*)nullptr);
        else        execl(su, su, "0", "sh", "-c", cmd, (char*)nullptr);
        _exit(127);
    }
    // Ждём до 15 секунд: если root-копия поднялась — выходим сразу, как
    // только увидим её в /proc; если ребёнок умер — root не дали.
    for (int waited = 0; waited < 150; waited++) {
        int st = 0;
        if (waitpid(child, &st, WNOHANG) == child) {
            if (WIFEXITED(st) && WEXITSTATUS(st) == 127) {
                fprintf(stderr, "[xvcen] su не смог запустить команду, продолжаю без root\n");
                return false;
            }
            fprintf(stderr, "[xvcen] root-копия умерла (код %d), продолжаю без root\n",
                    WIFEXITED(st) ? WEXITSTATUS(st) : -1);
            return false;
        }
        if (scan_root_copy(exe)) {
            fprintf(stderr, "[xvcen] root-копия работает\n");
            _exit(0);
        }
        usleep(100 * 1000);
    }
    _exit(0);  // таймаут: считаем, что root-копия поднялась (медленный /proc)
}

// Ленивое чтение uname(): первый вызов заполняет и кэширует версию ядра,
// дальше только отдаёт (стартовое окно показывает её сразу).
const char* kernel_version() {
    static std::atomic<bool> checked{false};
    if (!checked.exchange(true)) {
        if (!g_kernel_release[0]) {
            struct utsname u;
            if (uname(&u) == 0)
                snprintf(g_kernel_release, sizeof(g_kernel_release), "%s", u.release);
        }
    }
    return g_kernel_release;
}
} // namespace driver
