#include "memory_mode.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <unistd.h>

namespace memio {
namespace {
Backend g_backend = Backend::ProcMem;

bool equals_ignore_case(const char* lhs, const char* rhs) {
    return lhs && rhs && strcasecmp(lhs, rhs) == 0;
}

bool set_from_value(const char* value) {
    if (!value) return false;
    if (equals_ignore_case(value, "kernel") ||
        equals_ignore_case(value, "driver") ||
        equals_ignore_case(value, "k")) {
        g_backend = Backend::KernelDriver;
        return true;
    }
    if (equals_ignore_case(value, "proc") ||
        equals_ignore_case(value, "proc-mem") ||
        equals_ignore_case(value, "external") ||
        equals_ignore_case(value, "p")) {
        g_backend = Backend::ProcMem;
        return true;
    }
    return false;
}
}  // namespace

Backend backend() { return g_backend; }

const char* backend_name() {
    return g_backend == Backend::KernelDriver
               ? "kernel driver"
               : "/proc/<pid>/mem";
}

bool choose_backend(int argc, char* argv[]) {
    // Command-line options make the launcher usable without a terminal and are
    // also convenient for scripts. The menu is still the normal interactive
    // path requested by the application.
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (!arg) continue;
        if (strcmp(arg, "--kernel") == 0 || strcmp(arg, "--kernel-driver") == 0) {
            g_backend = Backend::KernelDriver;
            return true;
        }
        if (strcmp(arg, "--proc") == 0 || strcmp(arg, "--proc-mem") == 0 ||
            strcmp(arg, "--external") == 0) {
            g_backend = Backend::ProcMem;
            return true;
        }
        constexpr const char* kPrefix = "--memory-backend=";
        if (strncmp(arg, kPrefix, strlen(kPrefix)) == 0)
            return set_from_value(arg + strlen(kPrefix));
    }

    const char* configured = std::getenv("MEMORY_BACKEND");
    if (configured && configured[0]) return set_from_value(configured);

    if (!isatty(STDIN_FILENO)) {
        std::fputs("Memory backend: /proc/<pid>/mem (non-interactive default)\n", stdout);
        return true;
    }

    std::fputs("\nSelect memory backend before start:\n"
               "  [1] External  - /proc/<pid>/mem\n"
               "  [2] Kernel    - installed Android kernel driver\n"
               "  [q] Exit\n"
               "Choice [1/2]: ", stdout);
    std::fflush(stdout);

    char choice[32] = {};
    if (!std::fgets(choice, sizeof(choice), stdin)) return false;
    if (choice[0] == '2') {
        g_backend = Backend::KernelDriver;
        std::fputs("Selected: kernel driver\n", stdout);
        return true;
    }
    if (choice[0] == '1') {
        g_backend = Backend::ProcMem;
        std::fputs("Selected: /proc/<pid>/mem\n", stdout);
        return true;
    }
    std::fputs("No backend selected. Exiting.\n", stdout);
    return false;
}

}  // namespace memio
