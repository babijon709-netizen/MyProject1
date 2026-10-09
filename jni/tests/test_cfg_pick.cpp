// Host tests for the startup config selector (cfg_pick.h).
//
// Симптом: у части юзеров аим «случайно» не работает. Причина — два разных
// выбора «самого свежего» конфига: восстановление брало newest по всему
// каталогу, а загрузка — только среди первых kMaxConfigs (12) имён из списка.
// При большем числе файлов они расходились. Тест фиксирует один селектор:
// он не зависит от числа файлов, учитывает наносекунды и детерминирован при ничьей.
#include "cfg_pick.h"

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

namespace {
int failures = 0;
#define CHECK(cond, name) do { \
    if (!(cond)) { ++failures; printf("FAIL: %s (line %d)\n", name, __LINE__); } \
} while (0)

std::string MakeDir() {
    char tmpl[] = "/tmp/cfgpick_XXXXXX";
    char* d = mkdtemp(tmpl);
    return std::string(d) + "/";
}

void Touch(const std::string& dir, const std::string& file, time_t sec, long nsec = 0) {
    const std::string p = dir + file;
    int fd = open(p.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd >= 0) close(fd);
    struct timespec ts[2] = {{sec, nsec}, {sec, nsec}};
    utimensat(AT_FDCWD, p.c_str(), ts, 0);
}
}  // namespace

int main() {
    {
        std::string dir = MakeDir();
        std::string name;
        CHECK(!CfgPickNewest(dir.c_str(), name), "empty dir has no config");
    }
    {
        // 30 файлов (больше kMaxConfigs): самый свежий должен находиться,
        // даже если по алфавиту он далеко за первыми 12.
        std::string dir = MakeDir();
        for (int i = 1; i <= 30; i++) {
            char n[32];
            snprintf(n, sizeof(n), "config%d.cfg", i);
            Touch(dir, n, 1000 + i);
        }
        Touch(dir, "config9.cfg", 9999);
        std::string name;
        CHECK(CfgPickNewest(dir.c_str(), name), "picks from many files");
        CHECK(name == "config9", "newest beyond the 12-name window wins");
    }
    {
        std::string dir = MakeDir();
        Touch(dir, "a.cfg", 500, 100);
        Touch(dir, "b.cfg", 500, 200);
        std::string name;
        CHECK(CfgPickNewest(dir.c_str(), name) && name == "b", "nanosecond tie-break");
    }
    {
        std::string dir = MakeDir();
        Touch(dir, "x.cfg", 700);
        Touch(dir, "y.cfg", 700);
        std::string name;
        CHECK(CfgPickNewest(dir.c_str(), name) && name == "y", "same mtime resolves by name");
    }
    {
        // .tmp от атомарной записи и прочий мусор не должны выбираться.
        std::string dir = MakeDir();
        Touch(dir, "config1.cfg", 100);
        Touch(dir, "config1.cfg.tmp", 9000);
        Touch(dir, "notes.txt", 9000);
        std::string name;
        CHECK(CfgPickNewest(dir.c_str(), name) && name == "config1", "ignores non-.cfg files");
    }
    if (failures == 0) printf("test_cfg_pick: OK\n");
    return failures ? 1 : 0;
}
