#include "benz_log.h"
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <sys/time.h>
#ifdef __ANDROID__
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace {

const char* kLogPath = "/storage/emulated/0/Download/benzware.log";
const long long kRotateBytes = 1LL << 20;   // 1 МБ — пересоздаём файл
const long long kOpenRetryMs = 5000;        // хранилище ещё не готово

std::mutex g_mutex;
FILE*      g_file = nullptr;
long long  g_size = 0;            // приблизительный размер с открытия/ротации
long long  g_nextOpenTryMs = 0;   // когда пробовать открыть снова
bool       g_headerLogged = false;

long long nowMs() {
    struct timeval tv; gettimeofday(&tv, nullptr);
    return tv.tv_sec * 1000LL + tv.tv_usec / 1000;
}

long long tid() {
#ifdef __ANDROID__
    return (long long)syscall(SYS_gettid);
#else
    return 0;
#endif
}

void writeHeader(FILE* f) {
    fprintf(f, "лог открыт\n");
    fprintf(f, "файл: %s\n", kLogPath);
}

// Вызывать с захваченным g_mutex. Возвращает готовый файл или nullptr.
FILE* ensureOpen() {
    if (g_file) return g_file;
    if (nowMs() < g_nextOpenTryMs) return nullptr;
    g_nextOpenTryMs = nowMs() + kOpenRetryMs;
    FILE* f = fopen(kLogPath, "a");
    if (!f) return nullptr;
    g_file = f;
    setvbuf(f, nullptr, _IOLBF, 0);        // построчно: kill не съест хвост
    g_size = ftell(f);
    if (g_size >= kRotateBytes) {          // файл от прошлой сессии огромный
        fclose(f);
        f = fopen(kLogPath, "w");
        if (!f) { g_file = nullptr; return nullptr; }
        g_file = f;
        setvbuf(f, nullptr, _IOLBF, 0);
        g_size = 0;
    }
    if (!g_headerLogged || g_size == 0) {
        writeHeader(f);
        g_headerLogged = true;
    }
    return g_file;
}

void rotate() {
    if (g_file) fclose(g_file);
    FILE* f = fopen(kLogPath, "w");
    if (!f) { g_file = nullptr; g_size = 0; g_nextOpenTryMs = nowMs() + kOpenRetryMs; return; }
    g_file = f;
    setvbuf(f, nullptr, _IOLBF, 0);
    g_size = 0;
    writeHeader(f);
}

} // namespace

const char* benz_log_path() { return kLogPath; }

void benz_log(const char* fmt, ...) {
    char msg[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    std::lock_guard<std::mutex> lk(g_mutex);
    FILE* f = ensureOpen();
    if (!f) return;
    if (g_size >= kRotateBytes) { rotate(); f = g_file; if (!f) return; }

    char ts[24];
    struct timeval tv; gettimeofday(&tv, nullptr);
    struct tm tmv; localtime_r(&tv.tv_sec, &tmv);
    snprintf(ts, sizeof(ts), "%02d:%02d:%02d.%03d",
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec, (int)(tv.tv_usec / 1000));

    const int n = fprintf(f, "%s [t%lld] %s\n", ts, tid(), msg);
    if (n > 0) g_size += n;
}
