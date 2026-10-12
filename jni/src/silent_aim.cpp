#include "silent_aim_core.h"
#include "game.h"      // esp_silent_* — память игры через выбранный транспорт
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace silent_aim {
namespace {
uint64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
// Память игры для тика сайлента — ТОЛЬКО через выбранный способ чтения
// (esp_silent_* поверх g_mem/транспорта, см. game.cpp). Прежний собственный
// /proc/<pid>/mem убран: в кернел-режимах (FT/RT-Dev/RT-Hook) он появлялся в
// /proc/<self>/fd у чита и молча не работал при закрытом прямом доступе.
// «Открытие» здесь — не открытие файла, а подтверждение, что игра
// привязана к этому pid выбранным транспортом (привязку держит g_mem).
class Memory {
public:
    bool open(int pid) { return esp_silent_target(pid); }
    void close() {}
    bool read(uint64_t address, void* out, size_t size) {
        return esp_silent_read(address, out, size);
    }
    bool write(uint64_t address, const void* in, size_t size) {
        return esp_silent_write(address, in, size);
    }
};
class Writer {
    std::mutex mutex_;
    // Serialize the synchronous frame write, worker tick, and clear. Otherwise
    // a worker already inside RPM/WPM can restore an OLD target after the new
    // frame has written its direction, or even after aiming was turned off.
    std::mutex io_mutex_;
    std::condition_variable wake_;
    std::thread thread_;
    Command command_{};
    Stats stats_{};
    uint64_t published_ = 0, session_ = 0, revision_ = 0;
    bool stopping_ = false;

    void invalidate_locked() {
        stats_.active = stats_.axis = false;
        ++session_; ++revision_;
        wake_.notify_all();
    }

    void loop() {
        Memory memory;
        int pid = -1;
        uint64_t session = 0;
        std::unique_lock<std::mutex> lock(mutex_);
        while (!stopping_) {
            if (!stats_.active) {
                memory.close(); pid = -1;
                wake_.wait(lock, [&] { return stopping_ || stats_.active; });
                continue;
            }
            if (!fresh(published_, now_ms())) {
                stats_.active = stats_.axis = false;
                continue;
            }
            if (pid != command_.pid || session != session_) {
                if (!memory.open(command_.pid)) {
                    ++stats_.failures;
                    stats_.active = stats_.axis = false;
                    continue;
                }
                pid = command_.pid; session = session_;
            }
            const Command command = command_;
            const uint64_t command_session = session_;
            const uint64_t command_revision = revision_;
            const uint64_t published = published_;
            lock.unlock();
            std::unique_lock<std::mutex> io(io_mutex_);
            lock.lock();
            // Recheck AFTER obtaining the IO lock. A newer frame/clear may
            // have completed while this tick was waiting for that lock.
            if (stopping_ || !stats_.active || session_ != command_session ||
                revision_ != command_revision || !fresh(published, now_ms())) {
                io.unlock();
                continue;
            }
            const uint64_t now = now_ms();
            lock.unlock();
            const Result result = tick(memory, command, (now - published) * 0.001f);
            lock.lock();
            // Neither publish_sync nor clear can pass the IO lock while tick
            // writes, so a completed old write cannot follow the new one.
            stats_.axis = result == Result::written;
            if (result == Result::written) { ++stats_.writes; }
            else {
                ++stats_.failures;
                if (result == Result::guard_failed) ++stats_.guard_failures;
                stats_.active = false; // нужен новый снимок кадра
            }
            io.unlock();
            const auto interval = std::chrono::microseconds(command_.recent_shot ? kHotUs : kTickUs);
            wake_.wait_for(lock, interval, [&] {
                return stopping_ || !stats_.active || revision_ != command_revision;
            });
        }
    }
public:
    ~Writer() { stop(); }
    bool publish_sync(const Command& command) {
        std::lock_guard<std::mutex> io(io_mutex_);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_ || !valid_command(command)) {
                invalidate_locked();
                return false;
            }
        }
        Memory memory;
        if (!memory.open(command.pid)) {
            std::lock_guard<std::mutex> lock(mutex_);
            ++stats_.failures;
            invalidate_locked();
            return false;
        }
        // Guard and write immediately in the publishing frame, before waking
        // the worker. The worker can never overwrite it with an older target.
        const Result result = tick(memory, command);
        std::lock_guard<std::mutex> lock(mutex_);
        if (result != Result::written || stopping_) {
            ++stats_.failures;
            if (result == Result::guard_failed) ++stats_.guard_failures;
            invalidate_locked();
            return false;
        }
        ++stats_.writes;
        if (!thread_.joinable()) {
            try { thread_ = std::thread(&Writer::loop, this); }
            catch (...) {
                ++stats_.failures;
                invalidate_locked();
                return false;
            }
        }
        command_ = command;
        published_ = now_ms();
        ++revision_;
        stats_.active = stats_.axis = true;
        wake_.notify_all();
        return true;
    }
    void clear() {
        std::lock_guard<std::mutex> io(io_mutex_);
        std::lock_guard<std::mutex> lock(mutex_);
        invalidate_locked();
    }
    void stop() {
        {
            std::lock_guard<std::mutex> io(io_mutex_);
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
            invalidate_locked();
        }
        if (thread_.joinable()) thread_.join();
    }
    Stats stats() { std::lock_guard<std::mutex> lock(mutex_); return stats_; }
};
Writer& writer() { static Writer value; return value; }
}
bool publish_sync(const Command& command) { return writer().publish_sync(command); }
void clear() { writer().clear(); }
void shutdown() { writer().stop(); }
Stats stats() { return writer().stats(); }
}
