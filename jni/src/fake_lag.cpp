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
    "iptables -I INPUT ! -i lo -p udp -m length --length 80:65535 -j DROP";
constexpr const char* kUnblockCommand =
    "iptables -D INPUT ! -i lo -p udp -m length --length 80:65535 -j DROP 2>/dev/null";

bool execute_root(const char* command) {
    char buffer[512];
    std::snprintf(buffer, sizeof(buffer), "su -c '%s'", command);
    FILE* pipe = popen(buffer, "r");
    if (!pipe) return false;
    const int result = pclose(pipe);
    return result != -1 && WIFEXITED(result) && WEXITSTATUS(result) == 0;
}

void remove_rule_reliably() {
    for (int attempt = 0; attempt < 6; ++attempt) {
        if (execute_root(kUnblockCommand)) return;
        if (attempt != 5) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    LogLine("FakeLag: не удалось подтвердить удаление INPUT-правила");
}

class Controller {
public:
    ~Controller() { shutdown(); }

    void update(bool enabled, int percent, pid_t) {
        percent_.store(clamp_percent(percent), std::memory_order_relaxed);
        apply(enabled);
    }

    void shutdown() { apply(false); }

    Status status() const { return status_.load(std::memory_order_relaxed); }

private:
    void apply(bool enable) {
        if (enable) {
            if (running_.load(std::memory_order_acquire)) return;
            std::lock_guard<std::mutex> lock(thread_mutex_);
            if (running_.load(std::memory_order_relaxed)) return;
            if (thread_.joinable()) thread_.join();
            enabled_.store(true, std::memory_order_relaxed);
            running_.store(true, std::memory_order_release);
            blocked_ = false;
            status_.store(Status::Waiting, std::memory_order_relaxed);
            thread_ = std::thread(&Controller::loop, this);
            return;
        }

        enabled_.store(false, std::memory_order_relaxed);
        running_.store(false, std::memory_order_release);
        std::lock_guard<std::mutex> lock(thread_mutex_);
        if (thread_.joinable()) thread_.join();
        if (blocked_) {
            remove_rule_reliably();
            blocked_ = false;
        }
        status_.store(Status::Off, std::memory_order_relaxed);
    }

    void loop() {
        LogLine("FakeLag: запущен, INPUT UDP 80:65535, цикл 1000мс");
        while (running_.load(std::memory_order_acquire)) {
            if (!enabled_.load(std::memory_order_relaxed)) break;

            const int percent = percent_.load(std::memory_order_relaxed);
            if (percent >= 90) {
                if (!blocked_) {
                    execute_root(kBlockCommand);
                    blocked_ = true;
                    status_.store(Status::Active, std::memory_order_relaxed);
                }
                sleep_ms(1000);
                continue;
            }

            const int block_ms = (1000 * percent) / 100;
            const int unblock_ms = 1000 - block_ms;
            if (blocked_) {
                remove_rule_reliably();
                blocked_ = false;
                status_.store(Status::Waiting, std::memory_order_relaxed);
                sleep_ms(unblock_ms);
            } else {
                execute_root(kBlockCommand);
                blocked_ = true;
                status_.store(Status::Active, std::memory_order_relaxed);
                sleep_ms(block_ms);
            }
        }
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
