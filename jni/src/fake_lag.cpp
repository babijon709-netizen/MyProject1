#include "fake_lag.h"
#include "logfile.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <sys/wait.h>
#include <thread>

namespace fake_lag {
namespace {

constexpr const char* kBlockCommand =
    "iptables -w 2 -I INPUT ! -i lo -p udp -m length --length 80:65535 -j DROP >/dev/null 2>&1";
constexpr const char* kCheckCommand =
    "iptables -w 2 -C INPUT ! -i lo -p udp -m length --length 80:65535 -j DROP >/dev/null 2>&1";
constexpr const char* kDeleteCommand =
    "iptables -w 2 -D INPUT ! -i lo -p udp -m length --length 80:65535 -j DROP >/dev/null 2>&1";
constexpr int kCleanupRetries = 8;
constexpr int kPurgeLimit = 64;

int execute_root_status(const char* command) {
    char buffer[512];
    std::snprintf(buffer, sizeof(buffer), "su -c '%s'", command);
    FILE* pipe = popen(buffer, "r");
    if (!pipe) return -1;
    const int result = pclose(pipe);
    if (result == -1 || !WIFEXITED(result)) return -1;
    return WEXITSTATUS(result);
}

bool purge_all_rules() {
    for (int retry = 0; retry < kCleanupRetries; ++retry) {
        for (int removed = 0; removed < kPurgeLimit; ++removed) {
            const int check = execute_root_status(kCheckCommand);
            if (check == 1) return true;
            if (check != 0) break;
            if (execute_root_status(kDeleteCommand) != 0) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(75));
    }
    return execute_root_status(kCheckCommand) == 1;
}

class Controller {
public:
    ~Controller() { shutdown(); }

    void update(bool enabled, int percent, pid_t) {
        percent_.store(clamp_percent(percent), std::memory_order_relaxed);
        if (enabled) start();
        else stop_async();
    }

    void shutdown() {
        enabled_.store(false, std::memory_order_relaxed);
        running_.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(thread_mutex_);
            if (thread_.joinable()) thread_.join();
        }
        const bool clean = purge_all_rules();
        blocked_ = false;
        cleanup_confirmed_.store(clean, std::memory_order_relaxed);
        status_.store(clean ? Status::Off : Status::Failed, std::memory_order_relaxed);
        if (!clean) LogLine("FakeLag: финальная очистка всех INPUT-правил не подтверждена");
    }

    Status status() const { return status_.load(std::memory_order_relaxed); }

private:
    void start() {
        enabled_.store(true, std::memory_order_relaxed);
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

        cleanup_confirmed_.store(false, std::memory_order_relaxed);
        blocked_ = false;
        running_.store(true, std::memory_order_release);
        worker_alive_.store(true, std::memory_order_release);
        status_.store(Status::Waiting, std::memory_order_relaxed);
        thread_ = std::thread(&Controller::loop, this);
    }

    void stop_async() {
        enabled_.store(false, std::memory_order_relaxed);
        running_.store(false, std::memory_order_release);
        if (cleanup_confirmed_.load(std::memory_order_acquire)) {
            status_.store(Status::Off, std::memory_order_relaxed);
            return;
        }

        std::lock_guard<std::mutex> lock(thread_mutex_);
        if (worker_alive_.load(std::memory_order_acquire)) return;
        if (thread_.joinable()) thread_.join();
        worker_alive_.store(true, std::memory_order_release);
        status_.store(Status::Waiting, std::memory_order_relaxed);
        thread_ = std::thread(&Controller::cleanup_only, this);
    }

    void cleanup_only() {
        finish_cleanup("выключение");
    }

    void finish_cleanup(const char* reason) {
        const bool clean = purge_all_rules();
        blocked_ = false;
        cleanup_confirmed_.store(clean, std::memory_order_release);
        status_.store(clean ? Status::Off : Status::Failed, std::memory_order_relaxed);
        if (!clean)
            LogLine("FakeLag: %s — не удалось удалить все INPUT-правила", reason);
        worker_alive_.store(false, std::memory_order_release);
    }

    void loop() {
        // Remove leftovers from an earlier crash and every duplicate created by
        // older builds before inserting a new rule.
        if (!purge_all_rules()) {
            running_.store(false, std::memory_order_release);
            finish_cleanup("запуск");
            return;
        }
        cleanup_confirmed_.store(true, std::memory_order_relaxed);
        LogLine("FakeLag: запущен, INPUT UDP 80:65535, цикл 1000мс");

        while (running_.load(std::memory_order_acquire) &&
               enabled_.load(std::memory_order_relaxed)) {
            const int percent = percent_.load(std::memory_order_relaxed);
            if (percent >= 90) {
                if (!blocked_) {
                    if (execute_root_status(kBlockCommand) == 0) {
                        blocked_ = true;
                        cleanup_confirmed_.store(false, std::memory_order_relaxed);
                        status_.store(Status::Active, std::memory_order_relaxed);
                    } else {
                        status_.store(Status::Failed, std::memory_order_relaxed);
                    }
                }
                sleep_ms(1000);
                continue;
            }

            const int block_ms = (1000 * percent) / 100;
            const int unblock_ms = 1000 - block_ms;
            if (blocked_) {
                if (!purge_all_rules()) {
                    status_.store(Status::Failed, std::memory_order_relaxed);
                    running_.store(false, std::memory_order_release);
                    break;
                }
                blocked_ = false;
                cleanup_confirmed_.store(true, std::memory_order_relaxed);
                status_.store(Status::Waiting, std::memory_order_relaxed);
                sleep_ms(unblock_ms);
            } else {
                // Never stack a second DROP rule after an uncertain cleanup.
                if (!purge_all_rules() || execute_root_status(kBlockCommand) != 0) {
                    status_.store(Status::Failed, std::memory_order_relaxed);
                    running_.store(false, std::memory_order_release);
                    break;
                }
                blocked_ = true;
                cleanup_confirmed_.store(false, std::memory_order_relaxed);
                status_.store(Status::Active, std::memory_order_relaxed);
                sleep_ms(block_ms);
            }
        }

        running_.store(false, std::memory_order_release);
        finish_cleanup("остановка");
    }

    void sleep_ms(int milliseconds) {
        while (milliseconds > 0 && running_.load(std::memory_order_acquire)) {
            const int step = milliseconds > 50 ? 50 : milliseconds;
            std::this_thread::sleep_for(std::chrono::milliseconds(step));
            milliseconds -= step;
        }
    }

    std::atomic<bool> enabled_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> worker_alive_{false};
    std::atomic<bool> cleanup_confirmed_{false};
    std::atomic<int> percent_{45};
    std::atomic<Status> status_{Status::Off};
    bool blocked_ = false;
    std::mutex thread_mutex_;
    std::thread thread_;
};

Controller& controller() {
    static Controller instance;
    return instance;
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
