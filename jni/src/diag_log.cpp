#include "diag_log.h"
#include "config.h"
#include <cstdio>
#include <ctime>
#include <string>
#include <sys/stat.h>

static constexpr long kDiagMaxBytes = 1024L * 1024L;

void DiagLog(const char* fmt, ...) {
    const std::string path = std::string(CfgDir()) + "benzware.log";
    struct stat st{};
    const char* mode = (stat(path.c_str(), &st) == 0 && st.st_size > kDiagMaxBytes) ? "w" : "a";
    FILE* f = fopen(path.c_str(), mode);
    if (!f) return;
    const double t = (double)clock() / CLOCKS_PER_SEC;
    fprintf(f, "[%10.3f] ", t);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}
