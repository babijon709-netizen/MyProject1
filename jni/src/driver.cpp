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
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <algorithm>

namespace driver {

// ---- Глобальное состояние ---------------------------------------------------
static std::atomic<int> g_mode{(int)Mode::NonKernel};
static std::atomic<int> g_state{(int)KernelState::Idle};
static std::atomic<bool> g_verified{false};
static std::atomic<bool> g_launching{false};

static char g_kernel_release[96]  = {};
static char g_script_name[160]    = {};   // имя выбранного скрипта
static char g_script_path[300]    = {};   // полный путь (после копирования в /data/local/tmp)
static char g_su_path[128]        = {};
static char g_error[192]          = {};

static std::mutex g_mem_mtx;              // защищает кэш fd /proc/<pid>/mem
static int  g_mem_fd  = -1;
static pid_t g_mem_pid = -1;
static bool g_mem_rw   = false;           // fd открыт на чтение+запись
static std::atomic<bool> g_prefers_mem{false}; // process_vm не пошёл — сидим на mem

static Stats g_stats{};

// ---- Утилиты ----------------------------------------------------------------
static void set_error(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_error, sizeof(g_error), fmt, ap);
    va_end(ap);
}

static bool read_first_map(pid_t pid, uint64_t& base) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    FILE* f = fopen(path, "r");
    if (!f) return false;
    char line[512];
    bool ok = false;
    if (fgets(line, sizeof(line), f)) {
        unsigned long long b = 0;
        if (sscanf(line, "%llx-", &b) == 1 && b != 0) {
            base = b;
            ok = true;
        }
    }
    fclose(f);
    return ok;
}

// Поиск процесса игры по /proc/*/cmdline (как find_pid в main.cpp, но свой:
// модуль драйвера самодостаточен и не зависит от UI-кода).
static pid_t find_process(const char* package) {
    DIR* d = opendir("/proc");
    if (!d) return -1;
    struct dirent* e;
    char path[256], cmd[256];
    pid_t fallback = -1;
    while ((e = readdir(d))) {
        pid_t pid = (pid_t)atoi(e->d_name);
        if (pid <= 0) continue;
        snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
        FILE* f = fopen(path, "r");
        if (!f) continue;
        memset(cmd, 0, sizeof(cmd));
        size_t n = fread(cmd, 1, sizeof(cmd) - 1, f);
        fclose(f);
        if (!n) continue;
        if (strcmp(cmd, package) == 0) { fallback = -1; closedir(d); return pid; }
        size_t pl = strlen(package);
        if (fallback <= 0 && strncmp(cmd, package, pl) == 0 && cmd[pl] == ':')
            fallback = pid;
    }
    closedir(d);
    return fallback;
}

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
ssize_t readv(pid_t pid, const struct iovec* local_iov, unsigned long liovcnt,
              const struct iovec* remote_iov, unsigned long riovcnt, unsigned long flags) {
    if (pid <= 0 || !local_iov || !remote_iov) { errno = EINVAL; return -1; }

    bool kernel = ((Mode)g_mode.load() == Mode::Kernel);
    if (!kernel || !g_prefers_mem.load()) {
        ssize_t n = pv_readv(pid, local_iov, liovcnt, remote_iov, riovcnt, flags);
        if (n > 0) { g_stats.pv_reads++; return n; }
        if (n == 0) return 0;
        // отказ (EPERM/ESRCH/...) — пробуем /proc/<pid>/mem
    }

    ssize_t n = mem_transfer(pid, false, local_iov, liovcnt, remote_iov, riovcnt);
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

    bool kernel = ((Mode)g_mode.load() == Mode::Kernel);
    if (!kernel || !g_prefers_mem.load()) {
        ssize_t n = pv_writev(pid, local_iov, liovcnt, remote_iov, riovcnt, flags);
        if (n > 0) { g_stats.pv_writes++; return n; }
        if (n == 0) return 0;
    }

    ssize_t n = mem_transfer(pid, true, local_iov, liovcnt, remote_iov, riovcnt);
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

// ---- Версия ядра --------------------------------------------------------------
static bool parse_kernel(const char* release, int& x, int& y, int& z) {
    x = y = z = 0;
    int n = 0;
    if (sscanf(release, "%d.%d.%d", &x, &y, &z) == 3) n = 3;
    else if (sscanf(release, "%d.%d", &x, &y) == 2) { n = 2; z = 0; }
    else return false;
    return n >= 2 && x > 0 && y >= 0;
}

// ---- Разбор имён скриптов FTDriver --------------------------------------------
struct Script {
    std::string path;
    std::string base;     // имя без .sh
    int  x = 0, y = 0, z = 0;
    bool generic = false; // нет патча: "5.10.sh"
    int  variant = 0;     // 0 обычный, 1 "b", 2 "c"
    std::string flavor;   // "", "coloros", "oneplus", "oppo"
    std::string device;   // например "a92s" (oppoa92s4.14.186)
};

// Выделить токен версии x.y[.z] начиная с позиции i. Возвращает конец токена
// или -1, если в i версии нет.
static int version_token_at(const std::string& s, size_t i, int& x, int& y, int& z, bool& has_z) {
    size_t p = i;
    auto digits = [&](int& out) -> bool {
        size_t st = p;
        while (p < s.size() && s[p] >= '0' && s[p] <= '9') p++;
        if (p == st) return false;
        out = atoi(s.substr(st, p - st).c_str());
        return true;
    };
    x = y = z = 0; has_z = false;
    if (!digits(x)) return -1;
    if (p >= s.size() || s[p] != '.') return -1;
    p++;
    if (!digits(y)) return -1;
    if (p < s.size() && s[p] == '.') {
        size_t save = p;
        p++;
        if (!digits(z)) { p = save; return (int)p; }
        has_z = true;
    }
    return (int)p;
}

static bool parse_script(const std::string& fname, const std::string& fullpath, Script& out) {
    out = Script{}; // полное состояние по умолчанию (out переиспользуется)
    if (fname.size() < 4) return false;
    std::string base = fname.substr(0, fname.size() - 3);
    if (fname.compare(fname.size() - 3, 3, ".sh") != 0) return false;

    // Ищем ПОСЛЕДНИЙ токен вида x.y[.z] (у "oppoa92s4.14.186" версия в конце).
    int best = -1;
    int bx = 0, by = 0, bz = 0;
    bool bz_has = false;
    for (size_t i = 0; i < base.size(); i++) {
        if (!isdigit((unsigned char)base[i])) continue;
        if (i > 0 && isdigit((unsigned char)base[i - 1])) continue; // начало числа
        int x, y, z; bool hz;
        int end = version_token_at(base, i, x, y, z, hz);
        if (end < 0) continue;
        // токен должен быть «чистым»: следующий символ — не цифра и не точка
        if ((size_t)end < base.size() &&
            (isdigit((unsigned char)base[end]) || base[end] == '.')) continue;
        best  = (int)i;
        bx = x; by = y; bz = z; bz_has = hz;
        i   = (size_t)end - 1;
    }
    if (best < 0) return false;

    out.path = fullpath;
    out.base = base;
    out.x = bx; out.y = by; out.z = bz_has ? bz : 0;
    out.generic = !bz_has;

    std::string prefix = base.substr(0, (size_t)best);
    std::string suffix = base.substr((size_t)best);
    // выкинем саму версию из suffix
    {
        int x, y, z; bool hz;
        int end = version_token_at(suffix, 0, x, y, z, hz);
        if (end < 0) return false; // быть не должно, но на всякий случай
        suffix = suffix.substr((size_t)end);
    }
    std::transform(prefix.begin(), prefix.end(), prefix.begin(), ::tolower);
    std::transform(suffix.begin(), suffix.end(), suffix.begin(), ::tolower);

    // Префикс-вендор: "oppo", "oppoa92s"
    if (!prefix.empty()) {
        if (prefix.rfind("oppo", 0) == 0) {
            out.flavor = "oppo";
            out.device = prefix.substr(4);
        } else {
            out.flavor = prefix;
        }
    }

    // Суффикс: "", "b", "c", "-coloros", "b-coloros", "oneplus"...
    if (!suffix.empty()) {
        size_t p = 0;
        if (suffix[p] == '-') p++;
        if (p < suffix.size() && (suffix[p] == 'b' || suffix[p] == 'c') &&
            (p + 1 == suffix.size() || suffix[p + 1] == '-')) {
            out.variant = (suffix[p] == 'b') ? 1 : 2;
            p++;
            if (p < suffix.size() && suffix[p] == '-') p++;
        }
        std::string fl = suffix.substr(p);
        if (!fl.empty()) out.flavor = fl; // oneplus / coloros перекрывают префикс
    }
    return true;
}

static void collect_scripts(const std::string& dir, std::vector<Script>& out) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d))) {
        std::string n = e->d_name;
        if (n.size() < 4 || n.compare(n.size() - 3, 3, ".sh") != 0) continue;
        Script s;
        std::string full = dir + (dir.back() == '/' ? "" : "/") + n;
        if (parse_script(n, full, s)) out.push_back(s);
    }
    closedir(d);
}

static std::string exe_dir() {
    char buf[512] = {};
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return "/data/local/tmp";
    buf[n] = 0;
    std::string s(buf);
    size_t p = s.rfind('/');
    return p == std::string::npos ? "/data/local/tmp" : s.substr(0, p);
}

// ---- Вендор телефона -----------------------------------------------------------
struct Vendor {
    bool oneplus = false;
    bool oppo    = false;
    bool coloros = false; // OPPO/realme/OnePlus с прошивкой ColorOS
};

static Vendor detect_vendor() {
    Vendor v;
    char brand[PROP_VALUE_MAX] = {}, manu[PROP_VALUE_MAX] = {}, dev[PROP_VALUE_MAX] = {};
    char oplus[PROP_VALUE_MAX] = {};
    __system_property_get("ro.product.brand", brand);
    __system_property_get("ro.product.manufacturer", manu);
    __system_property_get("ro.product.device", dev);
    __system_property_get("ro.build.version.oplusrom.ui", oplus);
    // case-insensitive contains (strcasestr есть не во всех bionic)
    auto has = [](const char* hay, const char* needle) -> bool {
        if (!hay || !hay[0]) return false;
        size_t nl = strlen(needle);
        for (const char* p = hay; *p; p++) {
            size_t i = 0;
            while (i < nl && p[i] &&
                   tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) i++;
            if (i == nl) return true;
        }
        return false;
    };
    v.oneplus = has(brand, "oneplus") || has(manu, "oneplus") || has(dev, "oneplus");
    v.oppo    = has(brand, "oppo")    || has(manu, "oppo")    || has(dev, "oppo");
    v.coloros = v.oneplus || v.oppo ||
                has(brand, "realme") || has(manu, "realme") || oplus[0];
    return v;
}

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

// ---- Подбор скрипта ---------------------------------------------------------------
// Чистая функция выбора: лучший скрипт из списка под конкретную версию ядра.
static const Script* choose_script(const char* release, const std::vector<Script>& scripts,
                                   const Vendor& v) {
    int kx, ky, kz;
    if (!parse_kernel(release, kx, ky, kz)) return nullptr;

    const Script* winner = nullptr;
    long bestScore = -1;

    for (const Script& s : scripts) {
        // Версия: только то же x.y (эксплойт под чужую ветку не запускать).
        if (s.x != kx || s.y != ky) continue;
        long score;
        if (!s.generic && s.z == kz)      score = 400;  // точное совпадение x.y.z
        else if (s.generic)               score = 300;  // генерик 5.10.sh
        else if (s.z <= kz)               score = 170;  // соседний патч ниже
        else                              score = 150;  // патч выше — хуже
        // Вендорская сборка.
        if (s.flavor == "coloros")        score += v.coloros ? 60 : 5;
        else if (s.flavor == "oneplus")   score += v.oneplus ? 65 : 5;
        else if (s.flavor == "oppo")      score += v.oppo ? 55 : 5;
        else                              score += 30;  // обычная сборка
        // Варианты b/c — запасные (неизвестно, чем отличаются).
        if (s.variant == 1) score -= 8;
        if (s.variant == 2) score -= 16;
        if (score > bestScore) { bestScore = score; winner = &s; }
    }
    return winner;
}

static bool pick_script(Script& best) {
    struct utsname u;
    if (uname(&u) != 0) { set_error("uname() failed"); return false; }
    snprintf(g_kernel_release, sizeof(g_kernel_release), "%s", u.release);

    std::string dir = exe_dir();
    std::vector<std::string> dirs = {
        dir + "/drivers/ft", dir + "/drivers", dir + "/ft", dir + "/FTDriver", dir,
        "/data/local/tmp/drivers/ft", "/data/local/tmp/drivers", "/data/local/tmp/ft",
        "/data/local/tmp/FTDriver", "/data/local/tmp",
        "/sdcard/Download/drivers/ft", "/sdcard/Download/ft", "/sdcard/Download",
    };
    std::vector<Script> scripts;
    for (auto& d : dirs) collect_scripts(d, scripts);
    if (scripts.empty()) {
        set_error("скрипты FTDriver не найдены (положи папку drivers/ft рядом с бинарем)");
        return false;
    }

    Vendor v = detect_vendor();
    const Script* winner = choose_script(u.release, scripts, v);
    if (!winner) {
        int kx, ky, kz;
        parse_kernel(u.release, kx, ky, kz);
        set_error("нет драйвера под ядро %d.%d.%d (в FTDriver %zu скриптов)",
                  kx, ky, kz, scripts.size());
        return false;
    }
    best = *winner;
    return true;
}

// ---- Запуск скрипта ----------------------------------------------------------------
static pid_t launch_script(const char* su, const char* script, const char* log) {
    pid_t child = fork();
    if (child < 0) return -1;
    if (child == 0) {
        setsid();
        int fd = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
        signal(SIGINT, SIG_IGN); signal(SIGHUP, SIG_IGN);
        if (geteuid() == 0) {
            execl("/system/bin/sh", "sh", script, (char*)nullptr);
        } else {
            char cmd[512];
            snprintf(cmd, sizeof(cmd), "sh '%s'", script);
            execl(su, su, "-c", cmd, (char*)nullptr);
        }
        _exit(127);
    }
    return child;
}

// Копия скрипта в /data/local/tmp (su-шелл может не видеть /sdcard).
static bool ensure_local_copy(const Script& s) {
    snprintf(g_script_name, sizeof(g_script_name), "%s.sh", s.base.c_str());
    if (s.path.rfind("/data/local/tmp/", 0) == 0) {
        snprintf(g_script_path, sizeof(g_script_path), "%s", s.path.c_str());
        return true;
    }
    snprintf(g_script_path, sizeof(g_script_path), "/data/local/tmp/ftdrv_%s.sh", s.base.c_str());
    FILE* in = fopen(s.path.c_str(), "rb");
    if (!in) { set_error("не открыть %s", s.path.c_str()); return false; }
    FILE* out = fopen(g_script_path, "wb");
    if (!out) { fclose(in); set_error("не создать %s", g_script_path); return false; }
    char buf[16384];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) fwrite(buf, 1, n, out);
    bool ok = ferror(in) == 0 && ferror(out) == 0;
    fclose(in); fclose(out);
    chmod(g_script_path, 0700);
    return ok;
}

// ---- Фоновое ожидание готовности -----------------------------------------------------
static void wait_kernel_thread(std::string package, pid_t child) {
    const auto started  = std::chrono::steady_clock::now();
    const auto deadline = started + std::chrono::seconds(40);
    const auto gameWait = started + std::chrono::seconds(12);

    // Даём su секунду: мгновенный выход с кодом 127 — su/sh не нашлись.
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    {
        int st = 0;
        if (waitpid(child, &st, WNOHANG) == child && WEXITSTATUS(st) == 127) {
            set_error("не удалось выполнить скрипт (su/sh отсутствуют?)");
            g_state.store((int)KernelState::Failed);
            g_launching.store(false);
            return;
        }
    }

    while (std::chrono::steady_clock::now() < deadline) {
        pid_t pid = find_process(package.c_str());
        if (pid > 0) {
            uint64_t base = 0;
            if (read_first_map(pid, base)) {
                unsigned char probe[16] = {};
                if (read_process_memory(pid, base, probe, sizeof(probe))) {
                    g_verified.store(true);
                    g_state.store((int)KernelState::Ready);
                    return;
                }
            }
        } else if (std::chrono::steady_clock::now() > gameWait) {
            // Игры нет (софт запущен раньше игры) — драйвер считается
            // поднятым без проверки; attach-поток проверит при подключении.
            g_verified.store(false);
            g_state.store((int)KernelState::Ready);
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
    }

    // Драйвер не поднялся: если процесс su уже умер — это ошибка запуска,
    // иначе память игры так и не стала доступной.
    int st = 0;
    pid_t r = waitpid(child, &st, WNOHANG);
    if (r == child)
        set_error("драйвер завершился (код %d) — память игры недоступна", WEXITSTATUS(st));
    else
        set_error("таймаут: память игры так и не читается (драйвер не поднялся?)");
    g_state.store((int)KernelState::Failed);
    g_launching.store(false);
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
        fprintf(stderr, "[xvcen] su не найден: запусти от root, иначе память игры не прочитается\n");
        return false;
    }
    bool dash_c = su_probe_works(su, true);
    if (!dash_c && !su_probe_works(su, false)) {
        fprintf(stderr, "[xvcen] root через %s не выдан (проверь диалог Magisk/KSU)\n", su);
        return false;
    }

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

// ---- Хвост лога драйвера (для экрана загрузки) --------------------------------
static char g_drvlog_tail[640] = {};
const char* driver_log_tail() {
    FILE* f = fopen("/data/local/tmp/ftdrv.log", "rb");
    if (!f) { g_drvlog_tail[0] = 0; return g_drvlog_tail; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    long off = sz > (long)(sizeof(g_drvlog_tail) - 1) ? sz - (long)(sizeof(g_drvlog_tail) - 1) : 0;
    fseek(f, off, SEEK_SET);
    size_t n = fread(g_drvlog_tail, 1, sizeof(g_drvlog_tail) - 1, f);
    g_drvlog_tail[n] = 0;
    fclose(f);
    // выкидываем \r и непечатаемые
    for (size_t i = 0; i < n; i++) {
        char c = g_drvlog_tail[i];
        if (c != '\n' && (c < 0x20 || c == 0x7f)) g_drvlog_tail[i] = ' ';
    }
    return g_drvlog_tail;
}

// ---- Публичное API ядрового драйвера ---------------------------------------------------
bool start_kernel_driver(const char* game_package) {
    if (g_launching.exchange(true)) return false; // уже запускается
    g_state.store((int)KernelState::Detecting);
    g_error[0] = 0;
    g_verified.store(false);

    Script s;
    if (!pick_script(s)) {
        g_state.store((int)KernelState::NoScript);
        g_launching.store(false);
        return false;
    }
    if (!ensure_local_copy(s)) {
        g_state.store((int)KernelState::NoScript);
        g_launching.store(false);
        return false;
    }

    char su[128] = {};
    snprintf(su, sizeof(su), "%s", su_path());
    if (!su[0]) {
        set_error("su не найден: для KERNEL-режима нужен root");
        g_state.store((int)KernelState::NoRoot);
        g_launching.store(false);
        return false;
    }

    g_state.store((int)KernelState::Launching);
    pid_t child = launch_script(su, g_script_path, "/data/local/tmp/ftdrv.log");
    if (child < 0) {
        set_error("fork()/execl не удался: %s", strerror(errno));
        g_state.store((int)KernelState::Failed);
        g_launching.store(false);
        return false;
    }

    // Дальше — фоновый поток: ждём, пока память игры станет читаться.
    g_state.store((int)KernelState::Waiting);
    std::thread(wait_kernel_thread, std::string(game_package), child).detach();
    return true;
}

KernelState kernel_state() { return (KernelState)g_state.load(); }
bool        kernel_verified() { return g_verified.load(); }
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
const char* driver_script()  { return g_script_name; }
const char* su_path()        { return g_su_path; }
const char* last_error()     { return g_error; }

const char* kernel_state_text() {
    switch ((KernelState)g_state.load()) {
        case KernelState::Idle:      return "не запускался";
        case KernelState::Detecting: return "определяю ядро...";
        case KernelState::NoScript:  return "нет скрипта под ядро";
        case KernelState::NoRoot:    return "нет root (su)";
        case KernelState::Launching: return "запускаю драйвер...";
        case KernelState::Waiting:   return "жду ядро...";
        case KernelState::Ready:     return kernel_verified() ? "ядро активно" : "запущен (без проверки)";
        case KernelState::Failed:    return "ошибка";
    }
    return "?";
}

} // namespace driver
