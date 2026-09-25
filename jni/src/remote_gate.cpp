#include "remote_gate.h"
#include "device_identity.h"
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
#ifndef BENZWARE_POLL_SECONDS
#define BENZWARE_POLL_SECONDS 60
#endif
constexpr int kIntervalSeconds = BENZWARE_POLL_SECONDS;
#ifndef BENZWARE_GRACE_SECONDS
#define BENZWARE_GRACE_SECONDS 180
#endif
#ifndef BENZWARE_RETRY_DELAY_MS
#define BENZWARE_RETRY_DELAY_MS 2000
#endif
constexpr auto kGrace = std::chrono::seconds(BENZWARE_GRACE_SECONDS);
constexpr auto kTimeout = std::chrono::seconds(10);
enum class GateResult { allowed, denied, unavailable };

std::string fresh_nonce() {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return {};
    unsigned char bytes[16];
    size_t got = 0;
    while (got < sizeof(bytes)) {
        ssize_t n = read(fd, bytes + got, sizeof(bytes) - got);
        if (n > 0) got += static_cast<size_t>(n);
        else if (n < 0 && errno == EINTR) continue;
        else break;
    }
    close(fd);
    if (got != sizeof(bytes)) return {};
    const char* hex = "0123456789abcdef";
    std::string nonce;
    for (unsigned char c : bytes) { nonce += hex[c >> 4]; nonce += hex[c & 15]; }
    return nonce;
}

GateResult check_status(const DeviceIdentity& identity, bool register_device) {
    const char* url = BENZWARE_STATUS_URL;
    if (std::strncmp(url, "https://", 8) != 0 || std::strlen(url) <= 8 ||
        std::strpbrk(url, " \t\r\n") != nullptr) return GateResult::denied;
    std::string auth_url(url);
    if (auth_url.size() <= 7 || auth_url.compare(auth_url.size() - 7, 7, "/status") != 0)
        return GateResult::denied;
    auth_url.replace(auth_url.size() - 7, 7, "/auth");
    std::string nonce = fresh_nonce();
    if (nonce.empty()) return GateResult::unavailable;
    std::string body = DeviceIdentityJson(identity, register_device, nonce);

    int pipefd[2];
    if (pipe(pipefd) != 0) return GateResult::unavailable;
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
    if (curl_path.empty()) { close(pipefd[0]); close(pipefd[1]); return GateResult::unavailable; }
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
        const_cast<char*>("--header"), const_cast<char*>("Content-Type: application/json"),
        const_cast<char*>("--data-raw"), const_cast<char*>(body.c_str()),
        const_cast<char*>(auth_url.c_str()), nullptr
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
    if (child < 0) { close(pipefd[0]); return GateResult::unavailable; }
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
            if (response.size() + static_cast<size_t>(n) > 64) overflow = true;
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
            if (response.size() + static_cast<size_t>(n) > 64) overflow = true;
            else response.append(buffer, static_cast<size_t>(n));
        }
    }
    close(pipefd[0]);
    if (!exited || overflow || !WIFEXITED(status)) return GateResult::unavailable;
    if (WEXITSTATUS(status) == 0 && response == "enabled:" + nonce + "\n200\n")
        return GateResult::allowed;
    if (WEXITSTATUS(status) == 0 && response == "disabled\n200\n")
        return GateResult::denied;
    // A valid HTTP 200 with an invalid payload is a protocol/configuration
    // error, not permission to continue. 4xx also denies. Timeouts, TLS
    // failures, 429 and 5xx are temporarily unavailable.
    if (response.size() >= 4) {
        const std::string code = response.substr(response.size() - 4, 3);
        if (code == "200" || code == "400" || code == "401" ||
            code == "403" || code == "404") return GateResult::denied;
    }
    return GateResult::unavailable;
}
} // namespace

bool RemoteGateStart() {
    DeviceIdentity identity;
    if (!GetDeviceIdentity(identity)) {
        std::fprintf(stderr, "benzware: device ID unavailable; authorization denied\n");
        return false;
    }
    // Startup still fails closed. Retry only transport/5xx failures, not a
    // global OFF switch, a banned device, or an incompatible Worker protocol.
    GateResult result = GateResult::unavailable;
    for (int i = 0; i < 3 && main_thread_flag.load(); ++i) {
        result = check_status(identity, true);
        if (result != GateResult::unavailable) break;
        if (i < 2) std::this_thread::sleep_for(std::chrono::milliseconds(BENZWARE_RETRY_DELAY_MS));
    }
    if (!main_thread_flag.load() || result != GateResult::allowed) {
        std::fprintf(stderr, result == GateResult::denied
            ? "benzware: server denied access (off, banned or incompatible build)\n"
            : "benzware: authorization unavailable (network, Worker or quota)\n");
        return false;
    }
    std::thread([identity] {
        auto last_allowed = std::chrono::steady_clock::now();
        bool warned = false;
        while (main_thread_flag.load()) {
            for (int i = 0; i < kIntervalSeconds && main_thread_flag.load(); ++i)
                std::this_thread::sleep_for(std::chrono::seconds(1));
            if (!main_thread_flag.load()) break;
            GateResult result = check_status(identity, false);
            if (result == GateResult::allowed) {
                last_allowed = std::chrono::steady_clock::now();
                warned = false;
                continue;
            }
            if (result == GateResult::denied) {
                std::fprintf(stderr, "benzware: server revoked access; stopping\n");
            } else if (std::chrono::steady_clock::now() - last_allowed < kGrace) {
                if (!warned) std::fprintf(stderr,
                    "benzware: temporary authorization outage; retrying\n");
                warned = true;
                continue;
            } else {
                std::fprintf(stderr, "benzware: authorization outage exceeded grace; stopping\n");
            }
            main_thread_flag.store(false);
            break;
        }
    }).detach();
    return true;
}
