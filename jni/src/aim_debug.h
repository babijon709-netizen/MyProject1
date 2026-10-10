#pragma once
// Отладочный лог тач-аима. Включается файлом-флагом в каталоге конфигов:
//   touch /data/local/benzware/aim_debug      — включить
//   rm    /data/local/benzware/aim_debug      — выключить
// Пишется в /data/local/benzware/aim_debug.log (append). При размере больше
// kMaxBytes текущий лог переезжает в aim_debug.log.1. Флаг проверяется раз в
// секунду, чтобы не дёргать stat на каждом такте.
#include "config.h"              // CfgDir()
#include <cstdarg>
#include <cstdio>
#include <string>
#include <sys/stat.h>
#include <time.h>

namespace aimdbg {

constexpr long kMaxBytes = 4L * 1024 * 1024;

inline double NowSec() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

inline bool Enabled() {
    static double s_checkedAt = -1e9;
    static bool   s_on = false;
    const double now = NowSec();
    if (now - s_checkedAt >= 1.0) {
        s_checkedAt = now;
        struct stat st{};
        s_on = stat((std::string(CfgDir()) + "aim_debug").c_str(), &st) == 0;
    }
    return s_on;
}

inline void Line(const char* fmt, ...) {
    if (!Enabled()) return;
    const std::string base = std::string(CfgDir()) + "aim_debug.log";
    struct stat st{};
    if (stat(base.c_str(), &st) == 0 && st.st_size > kMaxBytes)
        rename(base.c_str(), (base + ".1").c_str());
    FILE* f = fopen(base.c_str(), "a");
    if (!f) return;
    fprintf(f, "%.3f ", NowSec());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

} // namespace aimdbg
