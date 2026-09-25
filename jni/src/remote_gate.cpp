#include "remote_gate.h"
#include "auth_http.h"
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
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <signal.h>

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

    // Keep the hard 10-second deadline even if DNS/TLS blocks. No device
    // binary or shell utility is needed: the child uses bundled Mbed TLS.
    int pipefd[2];
    if (pipe(pipefd) != 0) return GateResult::unavailable;
    pid_t child = fork();
    if (child == 0) {
        close(pipefd[0]);
        const AuthHttpResult auth = PostDeviceAuth(auth_url, body, nonce);
        const char result = auth == AuthHttpResult::allowed ? 'A'
                          : auth == AuthHttpResult::denied ? 'D' : 'U';
        ssize_t n;
        do { n = write(pipefd[1], &result, 1); } while (n < 0 && errno == EINTR);
        _exit(n == 1 ? 0 : 1);
    }
    close(pipefd[1]);
    if (child < 0) { close(pipefd[0]); return GateResult::unavailable; }
    int status = 0;
    bool exited = false;
    const auto deadline = std::chrono::steady_clock::now() + kTimeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (waitpid(child, &status, WNOHANG) == child) { exited = true; break; }
        pollfd pfd{pipefd[0], POLLIN, 0};
        poll(&pfd, 1, 100);
    }
    if (!exited) {
        kill(child, SIGKILL);
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    }
    char result = 0;
    const ssize_t n = read(pipefd[0], &result, 1);
    close(pipefd[0]);
    if (!exited || !WIFEXITED(status) || WEXITSTATUS(status) != 0 || n != 1)
        return GateResult::unavailable;
    if (result == 'A') return GateResult::allowed;
    return result == 'D' ? GateResult::denied : GateResult::unavailable;
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
