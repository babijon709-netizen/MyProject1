// logfile.cpp — реализация файлового лога (см. logfile.h).
//
// Здесь всё просто: кольцевой буфер строк в памяти, сброс пачкой, ротация
// по размеру. Ни потоков, ни inotify, ни fsync — лог диагностический, и
// потеря последней пачки при жёстком убийстве процесса неважна, а вот
// лишний syscall на каждой строке в кадре — важна.

#include "logfile.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <mutex>
#include <string>

namespace {

constexpr size_t   kFlushBytes    = 24u << 10;   // сбросить, накопив столько
constexpr double   kFlushSeconds  = 1.0;         // или не чаще раза в секунду
constexpr uint64_t kRotateBytes   = 3u << 20;    // 3 МБ — предел одного файла
constexpr double   kRetrySeconds  = 30.0;        // пауза после отказа открытия

std::mutex    g_mutex;
std::string   g_buffer;          // накопленные строки
std::string   g_path;            // <dir>/benzware.log
std::string   g_path_prev;       // <dir>/benzware.log.1
FILE*         g_file     = nullptr;
uint64_t      g_written  = 0;    // сколько байт ушло в текущий файл
double        g_last_flush = 0.0;
double        g_failed_at  = -1e9;
int           g_started    = 0;  // 1 — LogOpen отработал
unsigned long g_frame      = 0;
int           g_stage      = kStageNone;

double mono() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

void stamp(char* out, size_t cap) {
    struct timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm{};
    localtime_r(&ts.tv_sec, &tm);
    snprintf(out, cap, "[%02d:%02d:%02d.%03d] ",
             tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(ts.tv_nsec / 1000000));
}

// Вызывается под мьютексом.
void open_locked() {
    if (g_path.empty()) return;
    g_file = fopen(g_path.c_str(), "a");
    if (!g_file) return;
    chmod(g_path.c_str(), 0666);   // чтобы лог читался не только владельцем
    g_written = 0;
}

void flush_locked(bool rotate) {
    if (!g_file) return;
    if (!g_buffer.empty()) {
        const size_t n = fwrite(g_buffer.data(), 1, g_buffer.size(), g_file);
        g_written += n;
        g_buffer.clear();
        fflush(g_file);
    }
    if (!rotate || g_written < kRotateBytes) return;
    fclose(g_file);
    g_file = nullptr;
    // Прошлая копия затирается: храним ровно один предыдущий срез, иначе
    // лог забивает /data/local и его перестают забирать.
    remove(g_path_prev.c_str());
    rename(g_path.c_str(), g_path_prev.c_str());
    open_locked();
    if (g_file) {
        char head[64];
        stamp(head, sizeof(head));
        std::string line = head;
        line += "лог: ротация, предыдущий срез — benzware.log.1\n";
        fwrite(line.data(), 1, line.size(), g_file);
        fflush(g_file);
        g_written += line.size();
    }
}

}  // namespace

void LogOpen(const char* dir) {
    if (!dir || !dir[0]) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) { flush_locked(false); fclose(g_file); g_file = nullptr; }
    g_path      = std::string(dir) + "/benzware.log";
    g_path_prev = std::string(dir) + "/benzware.log.1";
    g_buffer.clear();
    g_buffer.reserve(kFlushBytes * 2);
    mkdir(dir, 0777);
    open_locked();
    if (!g_file) {
        g_failed_at = mono();
        return;
    }
    g_started = 1;
    g_last_flush = mono();
    char head[64];
    stamp(head, sizeof(head));
    std::string line = head;
    line += "лог: открыт ";
    line += g_path;
    line += "\n";
    fwrite(line.data(), 1, line.size(), g_file);
    fflush(g_file);
    g_written += line.size();
}

void LogClose() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_file) { g_buffer.clear(); return; }
    flush_locked(false);
    fclose(g_file);
    g_file = nullptr;
    g_started = 0;
}

void LogFlush() {
    std::lock_guard<std::mutex> lock(g_mutex);
    flush_locked(true);
    g_last_flush = mono();
}

void LogLine(const char* fmt, ...) {
    char text[1024];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    if (n <= 0) return;

    char head[64];
    stamp(head, sizeof(head));

    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_file) {
        // Файла нет (не открылся или ещё не открывали): копим только если
        // открытие вообще имело смысл, иначе буфер рос бы вечно.
        if (!g_started) return;
        const double now = mono();
        if (now - g_failed_at < kRetrySeconds) return;
        g_failed_at = now;
        open_locked();
        if (!g_file) return;
    }
    g_buffer += head;
    g_buffer.append(text, (size_t)n < sizeof(text) ? (size_t)n : sizeof(text) - 1);
    g_buffer += '\n';

    const double now = mono();
    if (g_buffer.size() >= kFlushBytes || now - g_last_flush >= kFlushSeconds) {
        flush_locked(true);
        g_last_flush = now;
    }
}

void LogStage(int stage) { g_stage = stage; }

void LogFrameBeat(unsigned long frame) {
    g_frame = frame;
    std::lock_guard<std::mutex> lock(g_mutex);
    const double now = mono();
    if (g_buffer.empty() || now - g_last_flush < kFlushSeconds) return;
    flush_locked(true);
    g_last_flush = now;
}

void LogWatchdogStart() { LogLine("лог: сторож запущен"); }
