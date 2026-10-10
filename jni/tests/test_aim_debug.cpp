#include "aim_debug.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <chrono>
#include <sys/stat.h>
#include <unistd.h>

static const char* kDir = "/tmp/xvcen_aimdbg_test/";
const char* CfgDir() { return kDir; }

static bool exists(const std::string& p) { struct stat st{}; return stat(p.c_str(), &st) == 0; }
static long size(const std::string& p) { struct stat st{}; return stat(p.c_str(), &st) == 0 ? (long)st.st_size : -1; }

int main() {
    std::string d = kDir;
    std::system(("rm -rf " + d + " && mkdir -p " + d).c_str());

    // флага нет: лог не пишется
    aimdbg::Line("should not appear");
    assert(!exists(d + "aim_debug.log"));

    // флаг включён (проверка раз в секунду)
    FILE* f = fopen((d + "aim_debug").c_str(), "w"); assert(f); fclose(f);
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    aimdbg::Line("pick id=%d", 42);
    assert(exists(d + "aim_debug.log"));
    assert(size(d + "aim_debug.log") > 0);

    // большой лог переезжает в .1 и создаётся заново
    std::system(("truncate -s 5M " + d + "aim_debug.log").c_str());
    aimdbg::Line("after rotate");
    assert(exists(d + "aim_debug.log.1"));
    assert(size(d + "aim_debug.log") < 1024);

    // флаг снят: после окна проверки запись останавливается
    unlink((d + "aim_debug").c_str());
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    const long before = size(d + "aim_debug.log");
    aimdbg::Line("off");
    assert(size(d + "aim_debug.log") == before);

    std::printf("ALL AIM-DEBUG TESTS PASS\n");
    return 0;
}
