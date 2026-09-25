#include "remote_gate.h"
#include "benzware_remote_config.h"
#include <atomic>
extern std::atomic<bool> main_thread_flag;

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <cstdlib>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <signal.h>

extern char **environ;

namespace {
constexpr int kIntervalSeconds = 30;
constexpr auto kTimeout = std::chrono::seconds(10);

bool check_status() {
    const char* url = BENZWARE_STATUS_URL;
    if (std::strncmp(url, "https://", 8) != 0 || std::strlen(url) <= 8 ||
        std::strpbrk(url, " \t\r\n") != nullptr) return false;

    int pipefd[2];
    if (pipe(pipefd) != 0) return false;
    // Resolve before fork: the child only uses async-signal-safe syscalls.
    std::string curl_path;
    const char* path = std::getenv("PATH");
    if (path) {
        std::string paths(path);
        size_t start = 0;
        while (start <= paths.size()) {
            size_t end = paths.find(':', start);
            if (end == std::string::npos) end = paths.size();
            // Ignore empty and relative PATH entries.
            if (end > start && paths[start] == '/') {
                std::string candidate = paths.substr(start, end - start) + "/curl";
                if (access(candidate.c_str(), X_OK) == 0) {
                    curl_path = candidate;
                    break;
                }
            }
            start = end + 1;
        }
    }
    if (curl_path.empty()) { close(pipefd[0]); close(pipefd[1]); return false; }
    // No shell, no redirects, no HTTP downgrade, no user-controlled proxy.
    // curl verifies HTTPS using the device/Termux trust store by default.
    char* const args[] = {
        const_cast<char*>("curl"), const_cast<char*>("-q"),
        const_cast<char*>("--silent"),
        const_cast<char*>("--fail"), const_cast<char*>("--write-out"),
        const_cast<char*>("%{http_code}\\n"), const_cast<char*>("--max-time"),
        const_cast<char*>("8"), const_cast<char*>("--connect-timeout"),
        const_cast<char*>("4"), const_cast<char*>("--proto"),
        const_cast<char*>("=https"), const_cast<char*>("--noproxy"),
        const_cast<char*>("*"), const_cast<char*>("--header"),
        const_cast<char*>("Cache-Control: no-store"),
        const_cast<char*>(url), nullptr
    };
    const char* binary_path = curl_path.c_str();
    pid_t child = fork();
    if (child == 0) {
        close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0) _exit(127);
        close(pipefd[1]);
        int nullfd = open("/dev/null", O_WRONLY);
        if (nullfd >= 0) { dup2(nullfd, STDERR_FILENO); close(nullfd); }
        execve(binary_path, args, environ);
        _exit(127);
    }
    close(pipefd[1]);
    if (child < 0) { close(pipefd[0]); return false; }
    fcntl(pipefd[0], F_SETFL, fcntl(pipefd[0], F_GETFL) | O_NONBLOCK);

    std::string response;
    bool overflow = false;
    bool exited = false;
    int status = 0;
    const auto deadline = std::chrono::steady_clock::now() + kTimeout;
    while (std::chrono::steady_clock::now() < deadline) {
        char buffer[64];
        ssize_t n;
        while ((n = read(pipefd[0], buffer, sizeof(buffer))) > 0) {
            if (response.size() + static_cast<size_t>(n) > 32) overflow = true;
            else response.append(buffer, static_cast<size_t>(n));
        }
        if (n < 0 && errno != EAGAIN && errno != EINTR) overflow = true;
        if (waitpid(child, &status, WNOHANG) == child) exited = true;
        if (exited) break;
        pollfd pfd{pipefd[0], POLLIN, 0};
        poll(&pfd, 1, 100);
    }
    if (!exited) {
        kill(child, SIGKILL);
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    }
    if (exited) {
        char buffer[64];
        ssize_t n;
        while ((n = read(pipefd[0], buffer, sizeof(buffer))) > 0) {
            if (response.size() + static_cast<size_t>(n) > 32) overflow = true;
            else response.append(buffer, static_cast<size_t>(n));
        }
    }
    close(pipefd[0]);
    return exited && !overflow && WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
           response == "enabled\n200\n";
}
} // namespace

bool RemoteGateStart() {
    if (!check_status() || !main_thread_flag.load()) {
        std::fprintf(stderr, "benzware: remote authorization unavailable or disabled\n");
        return false;
    }
    std::thread([] {
        while (main_thread_flag.load()) {
            // Short sleeps permit fast shutdown; no cached offline authorization.
            for (int i = 0; i < kIntervalSeconds && main_thread_flag.load(); ++i)
                std::this_thread::sleep_for(std::chrono::seconds(1));
            if (!main_thread_flag.load()) break;
            if (!check_status()) {
                std::fprintf(stderr, "benzware: remote authorization lost; stopping\n");
                main_thread_flag.store(false);
                break;
            }
        }
    }).detach();
    return true;
}
