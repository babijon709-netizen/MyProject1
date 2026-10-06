#include "fake_lag.h"
#include "logfile.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace fake_lag {
namespace {

constexpr int kCycleMs = 1000;
constexpr int kSleepSliceMs = 25;

int uid_for_pid(pid_t pid) {
    if (pid <= 0) return -1;
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%d/status", (int)pid);
    FILE* file = std::fopen(path, "r");
    if (!file) return -1;
    char line[256];
    int uid = -1;
    while (std::fgets(line, sizeof(line), file)) {
        if (std::strncmp(line, "Uid:", 4) != 0) continue;
        unsigned parsed = 0;
        if (std::sscanf(line + 4, "%u", &parsed) == 1 && parsed > 0)
            uid = (int)parsed;
        break;
    }
    std::fclose(file);
    return uid;
}

bool run_root(const std::string& command) {
    if (command.empty()) return false;
    std::string wrapped;
    if (geteuid() == 0) {
        wrapped = command;
    } else {
        // Commands are built exclusively from fixed text and a parsed numeric UID.
        wrapped = "su -c \"" + command + "\"";
    }
    const int rc = std::system(wrapped.c_str());
    return rc != -1 && WIFEXITED(rc) && WEXITSTATUS(rc) == 0;
}

std::string rule_command(const char* tool, const char* action, int uid, bool quiet) {
    char command[512];
    std::snprintf(command, sizeof(command),
                  "%s -w 2 %s OUTPUT -p udp -m owner --uid-owner %d "
                  "-m length --length 80:65535 -j DROP%s",
                  tool, action, uid, quiet ? " >/dev/null 2>&1" : "");
    return command;
}

void remove_rules_for_uid(int uid, bool purge_duplicates = false) {
    if (uid <= 0) return;
    for (const char* tool : {"iptables", "ip6tables"}) {
        const int attempts = purge_duplicates ? 8 : 1;
        for (int i = 0; i < attempts; ++i) {
            if (!run_root(rule_command(tool, "-D", uid, true))) break;
        }
    }
}

bool add_rules_for_uid(int uid) {
    if (uid <= 0) return false;
    const bool ipv4 = run_root(rule_command("iptables", "-I", uid, true));
    const bool ipv6 = run_root(rule_command("ip6tables", "-I", uid, true));
    return ipv4 || ipv6;
}

class Controller {
public:
    ~Controller() { shutdown(); }

    void update(bool enabled, int percent, pid_t pid) {
        percent_.store(clamp_percent(percent), std::memory_order_relaxed);

        const pid_t previous_pid = pid_.load(std::memory_order_relaxed);
        int uid = uid_.load(std::memory_order_relaxed);
        if (pid != previous_pid || (pid > 0 && uid <= 0)) {
            uid = uid_for_pid(pid);
            pid_.store(pid, std::memory_order_relaxed);
            uid_.store(uid, std::memory_order_relaxed);
        }

        if (!enabled) {
            // Cleanup remains on the worker. Joining here could freeze the
            // overlay behind an iptables lock for up to several seconds.
            running_.store(false, std::memory_order_release);
            if (uid <= 0 || cleaned_session_uid_.load(std::memory_order_relaxed) == uid)
                return;
            std::lock_guard<std::mutex> lock(thread_mutex_);
            if (worker_alive_.load(std::memory_order_acquire)) return;
            if (thread_.joinable()) thread_.join();
            worker_alive_.store(true, std::memory_order_release);
            status_.store(Status::Waiting, std::memory_order_relaxed);
            thread_ = std::thread(&Controller::cleanup_once, this, uid);
            return;
        }

        if (uid <= 0) {
            running_.store(false, std::memory_order_release);
            status_.store(Status::Waiting, std::memory_order_relaxed);
            return;
        }

        if (running_.load(std::memory_order_acquire)) return;
        std::lock_guard<std::mutex> lock(thread_mutex_);
        if (running_.load(std::memory_order_relaxed)) return;
        if (thread_.joinable()) {
            // A disabled worker may still be removing its last rule. Reap and
            // restart only after it has completed; update() will retry next frame.
            if (worker_alive_.load(std::memory_order_acquire)) {
                status_.store(Status::Waiting, std::memory_order_relaxed);
                return;
            }
            thread_.join();
        }
        running_.store(true, std::memory_order_release);
        worker_alive_.store(true, std::memory_order_release);
        status_.store(Status::Waiting, std::memory_order_relaxed);
        thread_ = std::thread(&Controller::loop, this);
    }

    void shutdown() {
        running_.store(false, std::memory_order_release);
        std::lock_guard<std::mutex> lock(thread_mutex_);
        if (thread_.joinable()) thread_.join();
    }

    Status status() const { return status_.load(std::memory_order_relaxed); }

private:
    void cleanup_once(int uid) {
        // This also clears a rule left by SIGKILL or a previous crash, even
        // when FakeLag is disabled in the new session.
        remove_rules_for_uid(uid, true);
        cleaned_session_uid_.store(uid, std::memory_order_relaxed);
        status_.store(Status::Off, std::memory_order_relaxed);
        worker_alive_.store(false, std::memory_order_release);
    }

    void sleep_interruptible(int milliseconds) {
        while (milliseconds > 0 && running_.load(std::memory_order_acquire)) {
            const int step = milliseconds > kSleepSliceMs ? kSleepSliceMs : milliseconds;
            std::this_thread::sleep_for(std::chrono::milliseconds(step));
            milliseconds -= step;
        }
    }

    void loop() {
        bool blocked = false;
        int blocked_uid = -1;
        int cleaned_uid = -1;
        int logged_uid = -1;
        while (running_.load(std::memory_order_acquire)) {
            const int uid = uid_.load(std::memory_order_relaxed);
            if (uid <= 0) {
                status_.store(Status::Waiting, std::memory_order_relaxed);
                sleep_interruptible(100);
                continue;
            }
            if (blocked && blocked_uid != uid) {
                remove_rules_for_uid(blocked_uid, true);
                blocked = false;
                blocked_uid = -1;
            }
            if (cleaned_uid != uid) {
                // Clean rules left by an interrupted previous session once,
                // not on every one-second phase.
                remove_rules_for_uid(uid, true);
                cleaned_uid = uid;
            }
            if (!blocked) {
                if (!add_rules_for_uid(uid)) {
                    status_.store(Status::Failed, std::memory_order_relaxed);
                    sleep_interruptible(1000);
                    continue;
                }
                blocked = true;
                blocked_uid = uid;
                status_.store(Status::Active, std::memory_order_relaxed);
                if (logged_uid != uid) {
                    logged_uid = uid;
                    LogLine("FakeLag: активен для uid %d", uid);
                }
                const int block_ms = kCycleMs * percent_.load(std::memory_order_relaxed) / 100;
                sleep_interruptible(block_ms);
            }
            if (!running_.load(std::memory_order_acquire)) break;
            if (blocked) {
                remove_rules_for_uid(blocked_uid);
                blocked = false;
                blocked_uid = -1;
            }
            const int unblock_ms = kCycleMs -
                kCycleMs * percent_.load(std::memory_order_relaxed) / 100;
            sleep_interruptible(unblock_ms);
        }
        if (blocked) remove_rules_for_uid(blocked_uid);
        if (cleaned_uid > 0)
            cleaned_session_uid_.store(cleaned_uid, std::memory_order_relaxed);
        status_.store(Status::Off, std::memory_order_relaxed);
        worker_alive_.store(false, std::memory_order_release);
        LogLine("FakeLag: выключен, правила удалены");
    }

    std::atomic<bool> running_{false};
    std::atomic<bool> worker_alive_{false};
    std::atomic<int> percent_{45};
    std::atomic<int> uid_{-1};
    std::atomic<int> cleaned_session_uid_{-1};
    std::atomic<pid_t> pid_{-1};
    std::atomic<Status> status_{Status::Off};
    std::mutex thread_mutex_;
    std::thread thread_;
};

Controller& controller() {
    static Controller value;
    return value;
}

} // namespace

void update(bool enabled, int percent, pid_t game_pid) {
    controller().update(enabled, percent, game_pid);
}

void shutdown() {
    controller().shutdown();
}

Status status() {
    return controller().status();
}

} // namespace fake_lag
