#include "memory_mode.h"

#include "kernel_driver.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <unistd.h>

namespace memio {
namespace {
Backend g_backend = Backend::ProcMem;

// Kernel mode is only meaningful when the module is actually installed: every
// read would otherwise fail at attach time and the overlay would come up with
// a live menu and no game data, which looks like a broken cheat rather than a
// missing driver. The check is done once, before the backend is locked in.
struct DriverStatus {
    bool usable = false;
    const char* reason = "";
};

DriverStatus driver_status() {
    static bool cached = false;
    static DriverStatus status;
    if (cached) return status;
    cached = true;

    const kernel_driver::Availability found = kernel_driver::probe();
    if (found.openable) {
        status.usable = true;
        status.reason = "";
        return status;
    }
    if (!found.present) {
        status.reason = "driver node not found in /dev (module not loaded)";
        return status;
    }
    status.reason = (found.error == EACCES || found.error == EPERM)
                        ? "driver node found but cannot be opened (run as root)"
                        : "driver node found but cannot be opened";
    return status;
}

// Returns false when kernel mode was asked for but cannot be honoured.
bool select_kernel() {
    const DriverStatus status = driver_status();
    if (status.usable) {
        g_backend = Backend::KernelDriver;
        return true;
    }
    std::fflush(stdout);
    std::fprintf(stderr,
                 "Kernel backend unavailable: %s.\n"
                 "Install/load the kernel module, or start with --proc to use "
                 "/proc/<pid>/mem.\n",
                 status.reason);
    std::fflush(stderr);
    return false;
}

bool equals_ignore_case(const char* lhs, const char* rhs) {
    return lhs && rhs && strcasecmp(lhs, rhs) == 0;
}

bool set_from_value(const char* value) {
    if (!value) return false;
    if (equals_ignore_case(value, "kernel") ||
        equals_ignore_case(value, "driver") ||
        equals_ignore_case(value, "k")) {
        return select_kernel();
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
        if (strcmp(arg, "--kernel") == 0 || strcmp(arg, "--kernel-driver") == 0)
            return select_kernel();
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

    // The driver is probed before the menu is drawn so an uninstalled module is
    // visible in the list instead of being discovered only after the choice.
    const DriverStatus status = driver_status();

    for (int attempt = 0; attempt < 3; ++attempt) {
        std::fputs("\nSelect memory backend before start:\n"
                   "  [1] External  - /proc/<pid>/mem\n", stdout);
        if (status.usable)
            std::fputs("  [2] Kernel    - installed Android kernel driver\n", stdout);
        else
            std::printf("  [2] Kernel    - UNAVAILABLE: %s\n", status.reason);
        std::fputs("  [q] Exit\n"
                   "Choice [1/2]: ", stdout);
        std::fflush(stdout);

        char choice[32] = {};
        if (!std::fgets(choice, sizeof(choice), stdin)) return false;
        if (choice[0] == '2') {
            if (!select_kernel()) continue;  // stay in the menu, do not fall back silently
            std::printf("Selected: kernel driver\n");
            return true;
        }
        if (choice[0] == '1') {
            g_backend = Backend::ProcMem;
            std::fputs("Selected: /proc/<pid>/mem\n", stdout);
            return true;
        }
        break;
    }
    std::fputs("No backend selected. Exiting.\n", stdout);
    return false;
}

}  // namespace memio
