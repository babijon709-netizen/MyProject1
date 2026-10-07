#include "fake_lag.h"
#include "logfile.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <sys/wait.h>
#include <thread>

namespace fake_lag {
namespace {

constexpr const char* kChain = "XVCEN_FL";
constexpr int kCleanupLimit = 512;
constexpr int kCleanupAttempts = 3;
constexpr int kRetryMs = 1000;

int execute_root(const std::string& command) {
    FILE* pipe = popen(("su -c '" + command + "'").c_str(), "r");
    if (!pipe) return -1;
    const int result = pclose(pipe);
    if (result == -1 || !WIFEXITED(result)) return -1;
    return WEXITSTATUS(result);
}

int iptables(const std::string& arguments) {
    return execute_root("iptables -w 2 " + arguments + " >/dev/null 2>&1");
}

bool remove_chain() {
    int jump = iptables(std::string("-C INPUT -j ") + kChain);
    for (int i = 0; i < kCleanupLimit && jump == 0; ++i) {
        if (iptables(std::string("-D INPUT -j ") + kChain) != 0) return false;
        jump = iptables(std::string("-C INPUT -j ") + kChain);
    }
    if (jump != 1) return false;

    const std::string chain = std::string("-S ") + kChain;
    const int exists = iptables(chain);
    if (exists == 0) {
        if (iptables(std::string("-F ") + kChain) != 0 ||
            iptables(std::string("-X ") + kChain) != 0)
            return false;
    } else if (exists != 1) {
        return false;
    }
    return iptables(std::string("-C INPUT -j ") + kChain) == 1 &&
           iptables(chain) == 1;
}

bool remove_legacy_rules() {
    constexpr const char* check =
        "-C INPUT ! -i lo -p udp -m length --length 80:65535 -j DROP";
    constexpr const char* remove =
        "-D INPUT ! -i lo -p udp -m length --length 80:65535 -j DROP";
    for (int i = 0; i < kCleanupLimit; ++i) {
        const int found = iptables(check);
        if (found == 1) return true;
        if (found != 0 || iptables(remove) != 0) return false;
    }
    return iptables(check) == 1;
}

bool cleanup_all() {
    const bool own_rules = remove_chain();
    const bool old_rules = remove_legacy_rules();
    return own_rules && old_rules;
}

bool cleanup_with_retries() {
    for (int attempt = 0; attempt < kCleanupAttempts; ++attempt) {
        if (cleanup_all()) return true;
        if (attempt + 1 < kCleanupAttempts)
            std::this_thread::sleep_for(std::chrono::milliseconds(75));
    }
    return false;
}

bool install_rule() {
    if (!remove_chain() || iptables(std::string("-N ") + kChain) != 0)
        return false;

    const std::string filter = std::string("-A ") + kChain +
        " ! -i lo -p udp -m length --length 80:65535 -j DROP";
    if (iptables(filter) != 0 ||
        iptables(std::string("-I INPUT 1 -j ") + kChain) != 0) {
        remove_chain();
        return false;
    }
    return true;
}

class Controller {
public:
    ~Controller() { shutdown(); }

    void update(bool enabled, int percent) {
        percent = clamp_percent(percent);
        bool disabling = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            disabling = enabled_ && !enabled;
            if (enabled_ != enabled || percent_ != percent) {
                enabled_ = enabled;
                percent_ = percent;
                ++generation_;
            }
        }
        cv_.notify_all();

        if (!initialized_) {
            initialized_ = true;
            last_cleanup_retry_ = std::chrono::steady_clock::now();
            cleanup_pending_.store(!cleanup_all(), std::memory_order_release);
            if (cleanup_pending_.load(std::memory_order_acquire)) {
                set_status(Status::Failed);
                LogLine("FakeLag: стартовая очистка правил не подтверждена");
            }
        }

        if (enabled) start_worker();
        else stop_worker(disabling);
    }

    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            enabled_ = false;
            ++generation_;
        }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();

        const bool clean = cleanup_with_retries();
        cleanup_pending_.store(!clean, std::memory_order_release);
        set_status(clean ? Status::Off : Status::Failed);
        if (!clean) LogLine("FakeLag: очистка правил при завершении не подтверждена");
    }

    Status status() const { return status_.load(std::memory_order_relaxed); }

private:
    struct Settings {
        bool enabled;
        int percent;
        unsigned generation;
    };

    Settings settings() {
        std::lock_guard<std::mutex> lock(mutex_);
        return {enabled_, percent_, generation_};
    }

    void start_worker() {
        if (worker_.joinable() && !worker_alive_.load(std::memory_order_acquire))
            worker_.join();
        if (worker_.joinable()) return;
        worker_alive_.store(true, std::memory_order_release);
        worker_ = std::thread(&Controller::loop, this);
    }

    void stop_worker(bool retry_cleanup) {
        const bool had_worker = worker_.joinable();
        if (had_worker) worker_.join();
        const auto now = std::chrono::steady_clock::now();
        const bool retry_due = now - last_cleanup_retry_ >= std::chrono::seconds(1);
        if (cleanup_pending_.load(std::memory_order_acquire) &&
            (retry_cleanup || had_worker || retry_due)) {
            last_cleanup_retry_ = now;
            const bool clean = cleanup_all();
            cleanup_pending_.store(!clean, std::memory_order_release);
            set_status(clean ? Status::Off : Status::Failed);
        } else if (!cleanup_pending_.load(std::memory_order_acquire)) {
            set_status(Status::Off);
        }
    }

    void set_status(Status value) {
        const Status old = status_.exchange(value, std::memory_order_relaxed);
        if (old != value) LogLine("FakeLag: статус %d", static_cast<int>(value));
    }

    bool wait_for(int milliseconds, unsigned generation) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(milliseconds), [&] {
            return !enabled_ || generation_ != generation;
        });
        return enabled_ && generation_ == generation;
    }

    void loop() {
        bool blocked = false;
        set_status(Status::Waiting);

        while (true) {
            const Settings current = settings();
            if (!current.enabled) break;

            if (cleanup_pending_.load(std::memory_order_acquire)) {
                const bool clean = cleanup_all();
                cleanup_pending_.store(!clean, std::memory_order_release);
                if (!clean) {
                    set_status(Status::Failed);
                    if (!wait_for(kRetryMs, current.generation)) {
                        if (!settings().enabled) break;
                    }
                    continue;
                }
            }

            if (!blocked) {
                if (!install_rule()) {
                    cleanup_pending_.store(!remove_chain(), std::memory_order_release);
                    set_status(Status::Failed);
                    wait_for(kRetryMs, current.generation);
                    continue;
                }
                blocked = true;
                set_status(Status::Active);
            }

            if (current.percent >= 90) {
                if (!wait_for(1000, current.generation)) {
                    const Settings changed = settings();
                    if (!changed.enabled) break;
                    if (changed.percent < 90) {
                        const bool clean = remove_chain();
                        cleanup_pending_.store(!clean, std::memory_order_release);
                        blocked = false;
                        set_status(clean ? Status::Waiting : Status::Failed);
                    }
                }
                continue;
            }

            if (!wait_for(blocked_ms(current.percent), current.generation)) {
                if (!settings().enabled) break;
                cleanup_pending_.store(!remove_chain(), std::memory_order_release);
                blocked = false;
                continue;
            }

            const bool clean = remove_chain();
            cleanup_pending_.store(!clean, std::memory_order_release);
            blocked = false;
            if (!clean) {
                set_status(Status::Failed);
                wait_for(kRetryMs, current.generation);
                continue;
            }
            set_status(Status::Waiting);
            wait_for(unblocked_ms(current.percent), current.generation);
        }

        const bool clean = remove_chain();
        cleanup_pending_.store(!clean, std::memory_order_release);
        worker_alive_.store(false, std::memory_order_release);
        set_status(clean ? Status::Off : Status::Failed);
        if (!clean) LogLine("FakeLag: очистка после остановки не подтверждена");
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    bool enabled_ = false;
    int percent_ = 45;
    unsigned generation_ = 0;
    std::thread worker_;
    std::atomic<bool> worker_alive_{false};
    std::atomic<bool> cleanup_pending_{false};
    std::atomic<Status> status_{Status::Off};
    std::chrono::steady_clock::time_point last_cleanup_retry_{};
    bool initialized_ = false;
};

Controller& controller() {
    static Controller instance;
    return instance;
}

} // namespace

void update(bool enabled, int percent) {
    controller().update(enabled, percent);
}

void shutdown() {
    controller().shutdown();
}

Status status() {
    return controller().status();
}

} // namespace fake_lag
