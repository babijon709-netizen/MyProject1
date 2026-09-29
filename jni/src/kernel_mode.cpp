#include "kernel_mode.h"
#include "mem_io.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace kernel_mode {
namespace {

std::string g_driver;
std::string g_diagnostic = "ordinary external mode selected";

bool regular_file(const std::string& path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::string dirname_of(const char* path) {
    if (!path || !*path) return ".";
    std::string s(path);
    const size_t slash = s.rfind('/');
    return slash == std::string::npos ? "." : (slash ? s.substr(0, slash) : "/");
}

void add_dir(std::vector<std::string>& dirs, const std::string& dir) {
    if (dir.empty()) return;
    if (std::find(dirs.begin(), dirs.end(), dir) == dirs.end()) dirs.push_back(dir);
}

std::vector<std::string> loader_dirs(const char* argv0, const std::string& override_dir) {
    std::vector<std::string> dirs;
    add_dir(dirs, override_dir);
    if (const char* env = getenv("XVCEN_DRIVERS_DIR")) add_dir(dirs, env);
    const std::string exe = dirname_of(argv0);
    add_dir(dirs, exe + "/drivers");
    add_dir(dirs, exe + "/drivers_extracted");
    add_dir(dirs, "./drivers_extracted");
    add_dir(dirs, "./drivers");
    add_dir(dirs, "/data/local/tmp/drivers");
    add_dir(dirs, "/data/local/tmp/drivers_extracted");
    add_dir(dirs, "/data/local/tmp");
    return dirs;
}

std::vector<std::string> find_loaders(const std::vector<std::string>& dirs) {
    std::vector<std::string> out;
    for (const std::string& dir : dirs) {
        DIR* d = opendir(dir.c_str());
        if (!d) continue;
        while (dirent* ent = readdir(d)) {
            const std::string name(ent->d_name);
            if (name.size() < 4 || name.compare(name.size() - 3, 3, ".sh") != 0) continue;
            const std::string path = dir + "/" + name;
            if (regular_file(path)) out.push_back(path);
        }
        closedir(d);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::string basename_no_ext(const std::string& path) {
    size_t slash = path.rfind('/');
    std::string n = path.substr(slash == std::string::npos ? 0 : slash + 1);
    if (n.size() > 3 && n.compare(n.size() - 3, 3, ".sh") == 0) n.resize(n.size() - 3);
    return n;
}

int match_score(const std::string& stem, const std::string& release) {
    int score = -1;
    // Extract the version even from names such as oppo4.19.81. Generic
    // major.minor loaders intentionally match all patch revisions.
    size_t version_start = 0;
    while (version_start < stem.size() && (stem[version_start] < '0' || stem[version_start] > '9')) ++version_start;
    size_t version_end = version_start;
    while (version_end < stem.size() && ((stem[version_end] >= '0' && stem[version_end] <= '9') || stem[version_end] == '.')) ++version_end;
    const std::string version = stem.substr(version_start, version_end - version_start);
    size_t first = version.find('.');
    size_t second = first == std::string::npos ? first : version.find('.', first + 1);
    const std::string family = second == std::string::npos ? version : version.substr(0, second);
    if (!family.empty() && release.compare(0, family.size(), family) == 0) score = 100 + (int)family.size();
    if (!version.empty() && release.compare(0, version.size(), version) == 0) score = 300 + (int)version.size();

    const bool wants_color = release.find("ColorOS") != std::string::npos ||
                             release.find("coloros") != std::string::npos ||
                             release.find("oppo") != std::string::npos;
    const bool wants_oneplus = release.find("oneplus") != std::string::npos ||
                               release.find("OnePlus") != std::string::npos;
    const bool is_color = stem.find("ColorOS") != std::string::npos || stem.find("oppo") != std::string::npos;
    const bool is_oneplus = stem.find("oneplus") != std::string::npos;
    if ((is_color && !wants_color) || (is_oneplus && !wants_oneplus)) score -= 200;
    if ((is_color && wants_color) || (is_oneplus && wants_oneplus)) score += 100;
    // b/c variants cannot be inferred safely from uname; prefer the un-suffixed build.
    if (!stem.empty() && (stem.back() == 'b' || stem.back() == 'c')) score -= 20;
    return score;
}

std::string automatic_loader(const std::vector<std::string>& loaders, const std::string& release) {
    int best = -1;
    std::string path;
    for (const std::string& candidate : loaders) {
        const int score = match_score(basename_no_ext(candidate), release);
        if (score > best) { best = score; path = candidate; }
    }
    return best >= 100 ? path : std::string();
}

bool run_loader(const std::string& path, std::string& error) {
    if (geteuid() != 0) {
        error = "kernel loader requires root (effective uid is not 0)";
        return false;
    }
    pid_t child = fork();
    if (child < 0) { error = std::string("fork failed: ") + strerror(errno); return false; }
    if (child == 0) {
        execl("/system/bin/sh", "sh", path.c_str(), (char*)nullptr);
        execl("/bin/sh", "sh", path.c_str(), (char*)nullptr); // host-side diagnostics/tests
        _exit(127);
    }
    int status = 0;
    for (int i = 0; i < 300; ++i) { // loaders should finish quickly; cap at 30 seconds
        const pid_t done = waitpid(child, &status, WNOHANG);
        if (done == child) {
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return true;
            char text[96];
            if (WIFEXITED(status)) snprintf(text, sizeof(text), "loader exited with status %d", WEXITSTATUS(status));
            else if (WIFSIGNALED(status)) snprintf(text, sizeof(text), "loader killed by signal %d", WTERMSIG(status));
            else snprintf(text, sizeof(text), "loader did not complete normally");
            error = text;
            return false;
        }
        if (done < 0) { error = std::string("waitpid failed: ") + strerror(errno); return false; }
        usleep(100000);
    }
    kill(child, SIGTERM);
    usleep(200000);
    kill(child, SIGKILL);
    waitpid(child, &status, 0);
    error = "loader timed out after 30 seconds";
    return false;
}

void usage(const char* argv0) {
    fprintf(stderr, "Usage: %s [--mode=external|kernel] [--driver=auto|PATH] [--drivers-dir=DIR]\n", argv0);
}

} // namespace

bool configure_before_start(int argc, char* argv[]) {
    enum Choice { kUnset, kExternal, kKernel } choice = kUnset;
    std::string requested_driver = "auto";
    std::string override_dir;
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (!strcmp(arg, "--mode=external") || !strcmp(arg, "--external")) choice = kExternal;
        else if (!strcmp(arg, "--mode=kernel") || !strcmp(arg, "--kernel")) choice = kKernel;
        else if (!strncmp(arg, "--driver=", 9)) requested_driver = arg + 9;
        else if (!strncmp(arg, "--drivers-dir=", 14)) override_dir = arg + 14;
        else if (!strcmp(arg, "--help")) { usage(argv[0]); return false; }
    }

    struct utsname un{};
    const std::string release = uname(&un) == 0 ? un.release : "unknown";
    const std::vector<std::string> loaders = find_loaders(loader_dirs(argv[0], override_dir));
    const std::string suggested = automatic_loader(loaders, release);

    if (choice == kUnset && isatty(STDIN_FILENO)) {
        fprintf(stderr,
                "\n=== xvcen launch mode ===\n"
                "Kernel: %s\n"
                "  1) Ordinary external mode  (/proc/<pid>/mem)\n"
                "  2) Kernel mode, automatic supplied driver%s%s\n"
                "Select [1-2]: ", release.c_str(), suggested.empty() ? " (no match found)" : ": ",
                suggested.empty() ? "" : suggested.c_str());
        fflush(stderr);
        char line[32]{};
        if (fgets(line, sizeof(line), stdin) && line[0] == '2') choice = kKernel;
        else choice = kExternal;
    }
    if (choice == kUnset) {
        // Never unexpectedly install a privileged driver in a non-interactive launch.
        choice = kExternal;
        fprintf(stderr, "xvcen: no terminal; selecting ordinary mode (use --mode=kernel explicitly)\n");
    }

    if (choice == kExternal) {
        memio::set_backend_mode(memio::BackendMode::ProcMem);
        g_driver.clear();
        g_diagnostic = "ordinary external mode: /proc/<pid>/mem";
        return true;
    }

    std::string path;
    if (requested_driver != "auto") {
        path = requested_driver;
        if (!regular_file(path)) {
            g_diagnostic = "requested driver does not exist: " + path;
            fprintf(stderr, "xvcen: %s\n", g_diagnostic.c_str());
            return false;
        }
    } else {
        path = suggested;
        if (path.empty()) {
            g_diagnostic = "no supplied driver matches kernel " + release;
            fprintf(stderr, "xvcen: %s; use --driver=PATH to choose a variant explicitly\n", g_diagnostic.c_str());
            return false;
        }
    }

    fprintf(stderr, "xvcen: loading kernel driver %s\n", path.c_str());
    std::string error;
    if (!run_loader(path, error)) {
        g_diagnostic = "driver load failed: " + error;
        fprintf(stderr, "xvcen: %s\n", g_diagnostic.c_str());
        return false; // no silent fallback: user explicitly selected kernel mode
    }
    g_driver = path;
    memio::set_backend_mode(memio::BackendMode::KernelProcessVm);
    g_diagnostic = "kernel mode loaded: " + path;
    fprintf(stderr, "xvcen: kernel backend ready; access will be verified on process attach\n");
    return true;
}

const char* selected_driver() { return g_driver.c_str(); }
const char* diagnostic() { return g_diagnostic.c_str(); }

} // namespace kernel_mode
