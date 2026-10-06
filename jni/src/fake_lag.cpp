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

constexpr int kCycleMs = 500;
constexpr int kSleepSliceMs = 20;
constexpr int kCleanupAttempts = 6;
constexpr int kPurgeLimit = 32;

struct ActiveRules {
    bool ipv4 = false;
    bool ipv6 = false;
    bool any() const { return ipv4 || ipv6; }
};

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
        // Commands contain only fixed text and a parsed numeric UID.
        wrapped = "su -c \"" + command + "\"";
    }
    const int rc = std::system(wrapped.c_str());
    return rc != -1 && WIFEXITED(rc) && WEXITSTATUS(rc) == 0;
}

std::string rule_command(const char* tool, const char* action, int uid) {
    char command[512];
    std::snprintf(command, sizeof(command),
                  "%s -w 2 %s OUTPUT -p udp -m owner --uid-owner %d "
                  "-m length --length 80:65535 -j DROP >/dev/null 2>&1",
                  tool, action, uid);
    return command;
}

bool rule_exists(const char* tool, int uid) {
    return run_root(rule_command(tool, "-C", uid));
}

bool purge_tool(const char* tool, int uid) {
    for (int removed = 0; removed < kPurgeLimit; ++removed) {
        if (!rule_exists(tool, uid)) return true;
        bool deleted = false;
        for (int retry = 0; retry < kCleanupAttempts && !deleted; ++retry) {
            deleted = run_root(rule_command(tool, "-D", uid));
            if (!deleted) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (!deleted) return !rule_exists(tool, uid);
    }
    return !rule_exists(tool, uid);
}

bool purge_rules_for_uid(int uid) {
    if (uid <= 0) return true;
    const bool ipv4 = purge_tool("iptables", uid);
    const bool ipv6 = purge_tool("ip6tables", uid);
    return ipv4 && ipv6;
}

ActiveRules add_rules_for_uid(int uid) {
    ActiveRules rules;
    if (uid <= 0) return rules;
    rules.ipv4 = run_root(rule_command("iptables", "-I", uid));
    rules.ipv6 = run_root(rule_command("ip6tables", "-I", uid));
    return rules;
}

bool remove_active_rules(int uid, ActiveRules& rules) {
    if (uid <= 0) return !rules.any();
    if (rules.ipv4 && run_root(rule_command("iptables", "-D", uid)))
        rules.ipv4 = false;
    if (rules.ipv6 && run_root(rule_command("ip6tables", "-D", uid)))
        rules.ipv6 = false;
    return !rules.any();
}

bool cleanup_rules_with_retries(int uid, ActiveRules* active = nullptr) {
    for (int attempt = 0; attempt < kCleanupAttempts; ++attempt) {
        bool active_clear = !active || remove_active_rules(uid, *active);
        const bool duplicates_clear = purge_rules_for_uid(uid);
        if (active) {
            if (active->ipv4 && !rule_exists("iptables", uid)) active->ipv4 = false;
            if (active->ipv6 && !rule_exists("ip6tables", uid)) active->ipv6 = false;
            active_clear = !active->any();
        }
        if (active_clear && duplicates_clear) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(75));
    }
    return false;
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
            // The worker owns cleanup. Joining here could freeze the overlay
            // behind an iptables lock; shutdown() still performs a final join.
            running_.store(false, std::memory_order_release);
            if (uid <= 0 || cleaned_session_uid_.load(std::memory_order_relaxed) == uid)
                return;
            start_cleanup_worker(uid);
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
        const int uid = uid_.load(std::memory_order_relaxed);
        if (uid > 0 && !purge_rules_for_uid(uid))
            LogLine("FakeLag: финальная очистка uid %d не подтверждена", uid);
    }

    Status status() const { return status_.load(std::memory_order_relaxed); }

private:
    void start_cleanup_worker(int uid) {
        std::lock_guard<std::mutex> lock(thread_mutex_);
        if (worker_alive_.load(std::memory_order_acquire)) return;
        if (thread_.joinable()) thread_.join();
        worker_alive_.store(true, std::memory_order_release);
        status_.store(Status::Waiting, std::memory_order_relaxed);
        thread_ = std::thread(&Controller::cleanup_once, this, uid);
    }

    void cleanup_once(int uid) {
        const bool clean = cleanup_rules_with_retries(uid);
        if (clean) {
            cleaned_session_uid_.store(uid, std::memory_order_relaxed);
            status_.store(Status::Off, std::memory_order_relaxed);
            LogLine("FakeLag: правила uid %d полностью удалены", uid);
        } else {
            cleaned_session_uid_.store(-1, std::memory_order_relaxed);
            status_.store(Status::Failed, std::memory_order_relaxed);
            LogLine("FakeLag: ОШИБКА очистки uid %d, повторю", uid);
        }
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
        ActiveRules active;
        int active_uid = -1;
        int cleaned_uid = -1;
        int logged_uid = -1;
        bool cleanup_ok = true;

        while (running_.load(std::memory_order_acquire)) {
            const int uid = uid_.load(std::memory_order_relaxed);
            if (uid <= 0) {
                status_.store(Status::Waiting, std::memory_order_relaxed);
                sleep_interruptible(100);
                continue;
            }

            if (active.any() && active_uid != uid) {
                cleanup_ok = cleanup_rules_with_retries(active_uid, &active);
                if (!cleanup_ok) {
                    status_.store(Status::Failed, std::memory_order_relaxed);
                    break;
                }
                active_uid = -1;
            }

            if (cleaned_uid != uid) {
                cleanup_ok = cleanup_rules_with_retries(uid);
                if (!cleanup_ok) {
                    status_.store(Status::Failed, std::memory_order_relaxed);
                    sleep_interruptible(500);
                    continue;
                }
                cleaned_uid = uid;
                cleaned_session_uid_.store(uid, std::memory_order_relaxed);
            }

            active = add_rules_for_uid(uid);
            if (!active.any()) {
                status_.store(Status::Failed, std::memory_order_relaxed);
                sleep_interruptible(500);
                continue;
            }
            active_uid = uid;
            cleaned_session_uid_.store(-1, std::memory_order_relaxed);
            status_.store(Status::Active, std::memory_order_relaxed);
            if (logged_uid != uid) {
                logged_uid = uid;
                LogLine("FakeLag: активен для uid %d, цикл %dмс, интенсивность %d%%",
                        uid, kCycleMs, percent_.load(std::memory_order_relaxed));
            }

            const int block_ms = kCycleMs * percent_.load(std::memory_order_relaxed) / 100;
            sleep_interruptible(block_ms);

            // Never insert a new rule until every rule inserted by this cycle
            // has been removed successfully.
            for (int retry = 0; active.any() && retry < kCleanupAttempts; ++retry) {
                if (remove_active_rules(active_uid, active)) break;
                status_.store(Status::Failed, std::memory_order_relaxed);
                LogLine("FakeLag: удаление правила uid %d не прошло, повтор %d/%d",
                        active_uid, retry + 1, kCleanupAttempts);
                std::this_thread::sleep_for(std::chrono::milliseconds(75));
            }
            if (active.any()) {
                cleanup_ok = false;
                break;
            }
            cleanup_ok = purge_rules_for_uid(active_uid);
            if (!cleanup_ok) {
                status_.store(Status::Failed, std::memory_order_relaxed);
                break;
            }
            cleaned_session_uid_.store(active_uid, std::memory_order_relaxed);
            active_uid = -1;

            if (!running_.load(std::memory_order_acquire)) break;
            const int unblock_ms = kCycleMs - block_ms;
            sleep_interruptible(unblock_ms);
        }

        const int cleanup_uid = active_uid > 0 ? active_uid : uid_.load(std::memory_order_relaxed);
        cleanup_ok = cleanup_rules_with_retries(cleanup_uid, &active);
        if (cleanup_ok) {
            cleaned_session_uid_.store(cleanup_uid, std::memory_order_relaxed);
            status_.store(Status::Off, std::memory_order_relaxed);
            LogLine("FakeLag: выключен, все правила uid %d удалены", cleanup_uid);
        } else {
            cleaned_session_uid_.store(-1, std::memory_order_relaxed);
            status_.store(Status::Failed, std::memory_order_relaxed);
            LogLine("FakeLag: ОШИБКА удаления правил uid %d после выключения", cleanup_uid);
        }
        worker_alive_.store(false, std::memory_order_release);
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
