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
// /proc/<pid>/mem убран: в кернел-режимах (FT/QX/RT) он появлялся в
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
    std::condition_variable wake_;
    std::thread thread_;
    Command command_{};
    Stats stats_{};
    uint64_t published_ = 0, session_ = 0, revision_ = 0;
    bool stopping_ = false;

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
            lock.unlock();
            const Result result = tick(memory, command);
            lock.lock();
            // Do not publish status for a snapshot superseded while the transport
            // read/write was in flight; the next iteration uses the newest copy.
            if (stopping_ || session_ != command_session || revision_ != command_revision) continue;
            stats_.axis = result == Result::written;
            if (result == Result::written) ++stats_.writes;
            else {
                ++stats_.failures;
                if (result == Result::guard_failed) ++stats_.guard_failures;
                stats_.active = false; // нужен новый снимок кадра
            }
            const auto interval = std::chrono::microseconds(command_.recent_shot ? kHotUs : kTickUs);
            wake_.wait_for(lock, interval, [&] {
                return stopping_ || !stats_.active || revision_ != command_revision;
            });
        }
    }
public:
    ~Writer() { stop(); }
    bool publish(const Command& command) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || !valid_command(command)) {
            stats_.active = stats_.axis = false;
            ++session_; ++revision_;
            wake_.notify_all();
            return false;
        }
        if (!thread_.joinable()) {
            try { thread_ = std::thread(&Writer::loop, this); }
            catch (...) { ++stats_.failures; return false; }
        }
        command_ = command;
        published_ = now_ms();
        ++revision_;
        stats_.active = true;
        wake_.notify_all();
        return true;
    }
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        stats_.active = stats_.axis = false;
        ++session_; ++revision_;
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
Writer& writer() { static Writer value; return value; }
}
bool publish(const Command& command) { return writer().publish(command); }
void clear() { writer().clear(); }
void shutdown() { writer().stop(); }
Stats stats() { return writer().stats(); }
}
