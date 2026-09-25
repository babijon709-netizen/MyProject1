// Adapted from silent.zip/DLP2.cpp (v149): repeated 12-byte LookDirection
// writes, camera-to-bone geometry and per-write ownership/class guards.
// See SILENT_AIM.md for intentionally omitted foreign-project dependencies.
#include "silent_aim_core.h"
#include <chrono>
#include <condition_variable>
#include <fcntl.h>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <cstdio>

namespace silent_aim {
namespace {
uint64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
// This fd is exclusively owned by the worker. No caches, auto-reopen, shared
// Reader state or logging in the hot loop (the UI reads counters instead).
class Memory {
    int fd_ = -1;
public:
    ~Memory() { close(); }
    void close() { if (fd_ >= 0) ::close(fd_); fd_ = -1; }
    bool open(int pid) {
        close();
        char path[64];
        std::snprintf(path, sizeof(path), "/proc/%d/mem", pid);
        fd_ = ::open(path, O_RDWR | O_CLOEXEC);
        return fd_ >= 0;
    }
    bool read(uint64_t addr, void* out, size_t size) {
        return fd_ >= 0 && pread64(fd_, out, size, static_cast<off64_t>(addr)) == static_cast<ssize_t>(size);
    }
    bool write(uint64_t addr, const void* in, size_t size) {
        return fd_ >= 0 && pwrite64(fd_, in, size, static_cast<off64_t>(addr)) == static_cast<ssize_t>(size);
    }
};
class Writer {
    std::mutex mutex_;
    std::condition_variable wake_;
    std::thread thread_;
    Command command_{};
    Stats stats_{};
    uint64_t published_ = 0, session_ = 0;
    bool stopping_ = false;

    void loop() {
        Memory mem;
        int pid = -1;
        uint64_t session = 0;
        std::unique_lock<std::mutex> lock(mutex_);
        while (!stopping_) {
            if (!stats_.active) {
                mem.close(); pid = -1;
                wake_.wait(lock, [&] { return stopping_ || stats_.active; });
                continue;
            }
            if (!fresh(published_, now_ms())) {
                stats_.active = stats_.axis = false;
                continue;
            }
            if (pid != command_.pid || session != session_) {
                if (!mem.open(command_.pid)) {
                    ++stats_.failures;
                    stats_.active = stats_.axis = false;
                    continue;
                }
                pid = command_.pid; session = session_;
            }
            // Mutex spans validation+write: clear/shutdown cannot race a
            // copied command. Only a handful of uncached pread calls here.
            const Result result = tick(mem, command_);
            stats_.axis = result == Result::written;
            if (result == Result::written) ++stats_.writes;
            else {
                ++stats_.failures;
                if (result == Result::guard_failed) ++stats_.guard_failures;
                stats_.active = false; // require a NEW frame snapshot
            }
            const auto interval = std::chrono::microseconds(command_.recent_shot ? kHotUs : kTickUs);
            wake_.wait_for(lock, interval, [&] { return stopping_ || !stats_.active; });
        }
    }
public:
    ~Writer() { stop(); }
    bool publish(const Command& command) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || !valid_command(command)) {
            stats_.active = stats_.axis = false;
            ++session_;
            wake_.notify_all();
            return false;
        }
        if (!thread_.joinable()) {
            try { thread_ = std::thread(&Writer::loop, this); }
            catch (...) { ++stats_.failures; return false; }
        }
        command_ = command;
        published_ = now_ms();
        stats_.active = true;
        wake_.notify_all();
        return true;
    }
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        stats_.active = stats_.axis = false;
        ++session_; // even same-PID reset must reopen the private fd
        wake_.notify_all();
    }
    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
            stats_.active = stats_.axis = false;
            wake_.notify_all();
        }
        if (thread_.joinable()) thread_.join();
    }
    Stats stats() { std::lock_guard<std::mutex> lock(mutex_); return stats_; }
};
Writer& writer() { static Writer w; return w; }
}
bool publish(const Command& c) { return writer().publish(c); }
void clear() { writer().clear(); }
void shutdown() { writer().stop(); }
Stats stats() { return writer().stats(); }
}
