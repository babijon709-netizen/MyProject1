#include "applog.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

namespace applog {

namespace {

std::mutex s_mtx;
FILE*      s_file      = nullptr;
char       s_path[160] = {};
long long  s_ftdrv_off = 0;   // сколько байт ftdrv.log уже зеркалировано

const char* kCandidates[] = {
    "/sdcard/Download/benzware.log",
    "/storage/emulated/0/Download/benzware.log",
    "/data/local/tmp/benzware.log",
};

// Не даём логу бесконечно расти: слишком большой — переименовываем в .1.
void maybe_rotate(const char* p) {
    struct stat st;
    if (stat(p, &st) != 0 || st.st_size < 1200 * 1024) return;
    char old[200];
    snprintf(old, sizeof(old), "%s.1", p);
    rename(p, old);
}

} // namespace

void init() {
    std::lock_guard<std::mutex> lk(s_mtx);
    if (s_file) return;
    for (const char* p : kCandidates) {
        maybe_rotate(p);
        FILE* f = fopen(p, "ab");
        if (!f) continue;
        s_file = f;
        snprintf(s_path, sizeof(s_path), "%s", p);
        break;
    }
}

void write(const char* fmt, ...) {
    std::lock_guard<std::mutex> lk(s_mtx);
    if (!s_file) return;

    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    time_t t = time(nullptr);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char stamp[16];
    strftime(stamp, sizeof(stamp), "%H:%M:%S", &tmv);

    fprintf(s_file, "[%s] %s\n", stamp, msg);
    fflush(s_file);
}

void mirror_ftdrv() {
    std::lock_guard<std::mutex> lk(s_mtx);
    if (!s_file) return;

    FILE* src = fopen("/data/local/tmp/ftdrv.log", "rb");
    if (!src) return;
    if (fseek(src, 0, SEEK_END) != 0) { fclose(src); return; }
    long sz = ftell(src);
    if (sz < 0) { fclose(src); return; }
    if (s_ftdrv_off > sz) s_ftdrv_off = 0;            // файл перезаписали заново
    if (sz == s_ftdrv_off) { fclose(src); return; }   // новых строк нет

    fseek(src, s_ftdrv_off, SEEK_SET);
    char buf[4096];
    size_t n;
    bool any = false;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
        if (!any) { fputs("---- ftdrv.log ----\n", s_file); any = true; }
        fwrite(buf, 1, n, s_file);
    }
    if (any) fflush(s_file);
    s_ftdrv_off = sz;
    fclose(src);
}

const char* path() {
    return s_path;
}

} // namespace applog
