#include "silent_aim.h"
#include <array>
#include <cassert>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <thread>
#include <unistd.h>

// Exercise the actual private-fd writer against our own allocated objects.
// Only inspect counters while active; object data is read after synchronous clear.
int main() {
    using namespace silent_aim;
    std::array<unsigned char, 1024> player{}, handler{}, look{}, fp{};
    uint64_t player_class = 0, handler_class = 0, look_class = 0, weapon = 0, target = 0;
    auto addr = [](const void* p) { return reinterpret_cast<uint64_t>(p); };
    auto ptr = [](auto& object, size_t offset, uint64_t value) {
        std::memcpy(object.data() + offset, &value, sizeof(value));
    };
    Command c;
    c.pid = getpid(); c.player = addr(player.data()); c.handler = addr(handler.data()); c.look = addr(look.data());
    c.player_class = addr(&player_class); c.handler_class = addr(&handler_class); c.look_class = addr(&look_class);
    c.fp_manager = addr(fp.data()); c.weapon = addr(&weapon); c.target = addr(&target);
    c.player_handler_offset = 0x78; c.handler_player_offset = 0xd0;
    c.look_offset = 0x140; c.value_offset = 0x20; c.fp_manager_offset = 0x90; c.weapon_offset = 0x50;
    c.aim[0] = 3.f; c.aim[2] = 4.f;
    ptr(player, 0, c.player_class); ptr(handler, 0, c.handler_class); ptr(look, 0, c.look_class);
    ptr(player, 0x78, c.handler); ptr(handler, 0xd0, c.player); ptr(handler, 0x140, c.look);
    ptr(player, 0x90, c.fp_manager); ptr(fp, 0x50, c.weapon);
    auto await = [](auto predicate) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!predicate() && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        assert(predicate());
    };
    clear();
    assert(publish(c));
    await([] { return stats().writes > 0; });
    clear();
    float dir[3]; std::memcpy(dir, look.data() + 0x20, sizeof(dir));
    assert(dir[0] > .599f && dir[0] < .601f && dir[2] > .799f && dir[2] < .801f);
    auto count = stats().writes;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    assert(stats().writes == count && !stats().active);
    for (int i = 0; i < 50; ++i) {
        assert(publish(c)); clear();
        assert(!stats().active);
    }
    assert(publish(c));
    Command invalid = c; invalid.pid = -1;
    assert(!publish(invalid) && !stats().active);
    count = stats().writes;
    // Resume, expire automatically, then resume again (same PID).
    assert(publish(c)); await([&] { return stats().writes > count; });
    await([] { return !stats().active; });
    count = stats().writes;
    assert(publish(c)); await([&] { return stats().writes > count; });
    clear();
    ptr(fp, 0x50, 0); // dropped weapon => fail closed
    const auto guards = stats().guard_failures;
    assert(publish(c)); await([&] { return stats().guard_failures > guards; });
    assert(!stats().active);
    shutdown(); shutdown();
    assert(!publish(c));
    std::puts("silent worker: private fd, write, clear, expiry, restart, weapon guard, join OK");
}
