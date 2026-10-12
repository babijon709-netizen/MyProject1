// Integration test for synchronous vs worker write ordering (no game process).
#include "silent_aim.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {
std::mutex gate_mutex;
std::condition_variable gate_cv;
std::unordered_map<uint64_t, uint64_t> pointers;
std::vector<float> written_x;
bool block_worker = false, entered = false, release_worker = false;

silent_aim::Command make_command(float x, uint64_t target) {
    silent_aim::Command c;
    c.pid = 123;
    c.player = 0x11000; c.handler = 0x12000; c.look = 0x13000;
    c.player_class = 0x21000; c.handler_class = 0x22000; c.look_class = 0x23000;
    c.player_handler_offset = 0x78; c.handler_player_offset = 0x58;
    c.look_offset = 0x140; c.value_offset = 0x20;
    c.target = target;
    c.eye[0] = 0.f; c.eye[1] = 0.f; c.eye[2] = 0.f;
    c.aim[0] = x; c.aim[1] = 0.f; c.aim[2] = x == 0.f ? 10.f : 0.f;
    return c;
}
void install(const silent_aim::Command& c) {
    pointers[c.player] = c.player_class;
    pointers[c.player + c.player_handler_offset] = c.handler;
    pointers[c.handler] = c.handler_class;
    pointers[c.handler + c.handler_player_offset] = c.player;
    pointers[c.handler + c.look_offset] = c.look;
    pointers[c.look] = c.look_class;
}
void block_next_worker() {
    std::lock_guard<std::mutex> lock(gate_mutex);
    block_worker = true;
    entered = release_worker = false;
}
void wait_until_blocked() {
    std::unique_lock<std::mutex> lock(gate_mutex);
    assert(gate_cv.wait_for(lock, std::chrono::seconds(2), [] { return entered; }));
}
void release() {
    std::lock_guard<std::mutex> lock(gate_mutex);
    release_worker = true;
    block_worker = false;
    gate_cv.notify_all();
}
} // namespace

bool esp_silent_target(int pid) { return pid == 123; }
bool esp_silent_read(uint64_t address, void* out, size_t size) {
    std::lock_guard<std::mutex> lock(gate_mutex);
    const auto found = pointers.find(address);
    if (size != sizeof(uint64_t) || found == pointers.end()) return false;
    std::memcpy(out, &found->second, size);
    return true;
}
bool esp_silent_write(uint64_t address, const void* in, size_t size) {
    if (address != 0x13020 || size != 3 * sizeof(float)) return false;
    float direction[3];
    std::memcpy(direction, in, size);
    std::unique_lock<std::mutex> lock(gate_mutex);
    if (block_worker) {
        entered = true;
        gate_cv.notify_all();
        assert(gate_cv.wait_for(lock, std::chrono::seconds(2), [] { return release_worker; }));
    }
    written_x.push_back(direction[0]);
    return true;
}

int main() {
    const auto old_target = make_command(0.f, 0x31000);
    const auto new_target = make_command(10.f, 0x32000);
    install(old_target);
    assert(silent_aim::publish_sync(old_target));
    block_next_worker();
    wait_until_blocked(); // the old worker is blocked inside its write

    std::atomic<bool> published{false};
    std::thread next([&] { published.store(silent_aim::publish_sync(new_target)); });
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    assert(!published.load()); // cannot publish behind an in-flight old write
    release();
    next.join();
    assert(published);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    {
        std::lock_guard<std::mutex> lock(gate_mutex);
        assert(!written_x.empty());
        assert(std::fabs(written_x.back() - 1.f) < 0.001f);
        // Once the new write has completed, no old direction follows it.
        bool saw_new = false;
        for (float x : written_x) {
            if (x > 0.9f) saw_new = true;
            else if (saw_new) assert(false && "stale worker wrote after new target");
        }
    }

    block_next_worker();
    wait_until_blocked();
    std::atomic<bool> cleared{false};
    std::thread off([&] { silent_aim::clear(); cleared.store(true); });
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    assert(!cleared.load());
    release();
    off.join();
    assert(cleared);
    size_t count = 0;
    { std::lock_guard<std::mutex> lock(gate_mutex); count = written_x.size(); }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    { std::lock_guard<std::mutex> lock(gate_mutex); assert(written_x.size() == count); }
    silent_aim::shutdown();
    std::puts("ALL SILENT-AIM WRITER TESTS PASS");
}
