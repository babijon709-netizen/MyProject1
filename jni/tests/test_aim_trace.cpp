#include "aim_trace.h"

#include <cassert>
#include <cstring>
#include <filesystem>
#include <string>

static int countCommas(const char* s) {
    int n = 0;
    for (; *s && *s != '\n'; ++s) if (*s == ',') ++n;
    return n;
}

static std::string readAll(const std::string& p) {
    std::string out;
    FILE* f = std::fopen(p.c_str(), "r");
    if (!f) return out;
    char buf[1024];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}

int main() {
    using namespace aim_trace;

    // row and header have the same number of columns
    {
        Record r;
        r.active = true; r.mode = "touch"; r.wall = 12.5; r.dt = 1.f / 60.f;
        r.haveCam = 1; r.camYaw = 1.5f; r.camPitch = -2.f; r.targetId = 0x1234ull; r.worldDist = 42.f;
        char line[512];
        const int n = format_row(line, sizeof line, r);
        assert(n > 0);
        assert(countCommas(line) == countCommas(header()));
        assert(std::strstr(line, "touch") != nullptr);
    }

    // Writer: idle does not create a file, active does, idle closes it after the timeout
    {
        const std::string dir = "/tmp/aim_trace_test";
        std::filesystem::remove_all(dir);
        Writer w;
        w.setDirs({dir});

        Record idle; idle.active = false; idle.wall = 1.0;
        assert(w.record(idle) == Result::Idle);
        assert(w.path().empty());

        Record act; act.active = true; act.mode = "memory"; act.wall = 2.0; act.dt = 0.016f;
        assert(w.record(act) == Result::Written);
        act.wall = 2.1;
        assert(w.record(act) == Result::Written);
        const std::string p = w.path();
        assert(!p.empty());
        assert(p.find("/aim_memory_") != std::string::npos);

        // short gap keeps the file open
        Record gap; gap.active = false; gap.wall = 2.5;
        w.record(gap);
        assert(!w.path().empty());

        // long gap closes it
        Record late; late.active = false; late.wall = 4.0;
        w.record(late);
        w.close();

        const std::string text = readAll(p);
        assert(text.rfind(header(), 0) == 0);
        int lines = 0;
        for (char c : text) if (c == '\n') ++lines;
        assert(lines == 1 + 2);   // header + two rows
    }

    // Writer: a mode switch without an idle tick opens a new file
    {
        const std::string dir = "/tmp/aim_trace_test_switch";
        std::filesystem::remove_all(dir);
        Writer w;
        w.setDirs({dir});
        Record t; t.active = true; t.mode = "touch"; t.wall = 1.0;
        Record m; m.active = true; m.mode = "memory"; m.wall = 1.1;
        w.record(t);
        const std::string touchPath = w.path();
        w.record(m);
        assert(w.path() != touchPath);
        assert(w.path().find("/aim_memory_") != std::string::npos);
        w.close();
        assert(readAll(touchPath).find("memory") == std::string::npos);
    }

    // Writer: no writable directory reports OpenFailed once per session
    {
        Writer w;
        w.setDirs({"/nonexistent_root_dir_xyz/aim"});
        Record act; act.active = true; act.mode = "touch"; act.wall = 1.0;
        assert(w.record(act) == Result::OpenFailed);
        assert(w.record(act) == Result::Idle);
        Record idle; idle.active = false; idle.wall = 1.5;
        w.record(idle);
        assert(w.record(act) == Result::OpenFailed);   // new session retries and reports again
    }

    return 0;
}
