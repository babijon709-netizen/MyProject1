// Диагностический лог. Пишется в Загрузки (Download/xvcen.log), запасной
// вариант — каталог конфигов. Копим строки в памяти, сброс в файл каждую
// секунду или когда буфер полон; файл открываем только на время сброса
// (fd не живёт дольше одного flush — KERNEL.md §5).
#include "logfile.h"
#include "str.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <fcntl.h>
#include <mutex>
#include <thread>
#include <time.h>
#include <unistd.h>

namespace {
std::mutex g_log_mtx;
char g_log_buf[12288];
size_t g_log_used = 0;
std::atomic<bool> g_log_open{false};
std::atomic<int> g_stage{kStageNone};
std::atomic<unsigned long> g_frame{0};
char g_log_path[160] = {};
double g_start_at = 0.0;
double g_last_flush_at = 0.0;

double log_monotonic_s() {
    return std::chrono::duration_cast<std::chrono::duration<double>>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

double log_uptime_s() { return log_monotonic_s() - g_start_at; }

const char* log_path_candidate(int which) {
    static constexpr auto _a = xp::_mk("/storage/emulated/0/Download/xvcen.log");
    static constexpr auto _b = xp::_mk("/storage/emulated/0/benzware/xvcen.log");
    return which == 0 ? _a.d() : _b.d();
}

bool log_writable(const char* path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) { (void)errno; return false; }
    close(fd);
    return true;
}

void log_stamp(char* out, size_t cap) {
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tmv{};
    localtime_r(&ts.tv_sec, &tmv);
    snprintf(out, cap, "%02d:%02d:%02d.%03ld up=%.1fs f=%lu s%d ",
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec, ts.tv_nsec / 1000000L,
             log_uptime_s(), g_frame.load(), g_stage.load());
}

// Точка сброса буфера в файл. Мьютекс уже взят вызывающим.
void log_flush_locked() {
    if (!g_log_open.load() || g_log_used == 0 || !g_log_path[0]) return;
    int fd = open(g_log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) {
        // Основной путь недоступен — попробуем запасной один раз.
        const char* alt = log_path_candidate(1);
        if (strcmp(alt, g_log_path) != 0 && log_writable(alt)) {
            snprintf(g_log_path, sizeof(g_log_path), "%s", alt);
            fd = open(g_log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        }
        if (fd < 0) return;
    }
    ssize_t done = write(fd, g_log_buf, g_log_used);
    if (done < 0) done = 0;
    close(fd);
    if ((size_t)done < g_log_used) {
        memmove(g_log_buf, g_log_buf + done, g_log_used - (size_t)done);
        g_log_used -= (size_t)done;
    } else {
        g_log_used = 0;
    }
    g_last_flush_at = log_monotonic_s();
}

void log_append_locked(const char* text, size_t len) {
    if (!g_log_open.load() || len == 0) return;
    if (len > sizeof(g_log_buf) - 1) len = sizeof(g_log_buf) - 1;  // обрезка «вечной» строки
    if (len + 1 > sizeof(g_log_buf) - g_log_used) {
        log_flush_locked();
        if (len + 1 > sizeof(g_log_buf) - g_log_used) g_log_used = 0;  // файл недоступен
    }
    memcpy(g_log_buf + g_log_used, text, len);
    g_log_used += len;
    g_log_buf[g_log_used++] = '\n';
}

void log_uptime_watcher() {
    for (;;) {
        usleep(300 * 1000);
        if (!g_log_open.load()) continue;
        std::lock_guard<std::mutex> lock(g_log_mtx);
        if (g_log_used >= sizeof(g_log_buf) / 2 ||
            log_monotonic_s() - g_last_flush_at >= 1.0) {
            log_flush_locked();
        }
    }
}

} // namespace

void log_open() {
    if (g_log_open.load()) return;
    g_start_at = log_monotonic_s();
    g_last_flush_at = g_start_at;
    snprintf(g_log_path, sizeof(g_log_path), "%s", log_path_candidate(0));
    if (!log_writable(g_log_path))
        snprintf(g_log_path, sizeof(g_log_path), "%s", log_path_candidate(1));
    {
        std::lock_guard<std::mutex> lock(g_log_mtx);
        g_log_open.store(true);
        const char* hello = "=== xvcen debug log (Downloads) ===";
        log_append_locked(hello, strlen(hello));
        log_flush_locked();
    }
    static std::thread watcher(log_uptime_watcher);
    watcher.detach();
}

void log_close() {
    if (!g_log_open.load()) return;
    std::lock_guard<std::mutex> lock(g_log_mtx);
    const char* bye = "=== log closed ===";
    log_append_locked(bye, strlen(bye));
    log_flush_locked();
    g_log_open.store(false);
}

void log_stage(int stage) { g_stage.store(stage); }
void log_frame_beat(unsigned long frame) { g_frame.store(frame); }

void log_watchdog_start() {
    // Пульс-строчка раз в 5 с: если игра повисла, в логе видно, на какой
    // стадии и кадре замирала.
    static std::thread t([] {
        for (;;) {
            usleep(5 * 1000 * 1000);
            if (!g_log_open.load()) continue;
            char stamp[80], line[160];
            log_stamp(stamp, sizeof(stamp));
            int n = snprintf(line, sizeof(line), "%sheart-beat", stamp);
            std::lock_guard<std::mutex> lock(g_log_mtx);
            log_append_locked(line, (size_t)n);
            log_flush_locked();
        }
    });
    t.detach();
}

void log_line(const char* fmt, ...) {
    if (!g_log_open.load() || !fmt) return;
    char stamp[80];
    char body[768];
    char line[880];
    log_stamp(stamp, sizeof(stamp));
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(body, sizeof(body), fmt, args);
    va_end(args);
    if (n <= 0) return;
    if (n >= (int)sizeof(body)) n = (int)sizeof(body) - 1;
    int m = snprintf(line, sizeof(line), "%s%s", stamp, body);
    if (m <= 0) return;
    if (m >= (int)sizeof(line)) m = (int)sizeof(line) - 1;
    std::lock_guard<std::mutex> lock(g_log_mtx);
    log_append_locked(line, (size_t)m);
    if (g_log_used >= sizeof(g_log_buf) / 2) log_flush_locked();
}
