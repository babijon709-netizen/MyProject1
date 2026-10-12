#include "silent_aim_core.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <unordered_map>

namespace {
struct Memory {
    std::unordered_map<uint64_t, uint64_t> pointers;
    uint64_t last_write = 0;
    float value[3] = {};

    bool read(uint64_t address, void* out, size_t size) {
        const auto it = pointers.find(address);
        if (size != sizeof(uint64_t) || it == pointers.end()) return false;
        std::memcpy(out, &it->second, size);
        return true;
    }
    bool write(uint64_t address, const void* in, size_t size) {
        if (size != sizeof(value)) return false;
        last_write = address;
        std::memcpy(value, in, size);
        return true;
    }
};

silent_aim::Command command() {
    silent_aim::Command c;
    c.pid = 123;
    c.target = 0x10000;
    c.player = 0x11000;
    c.handler = 0x12000;
    c.look = 0x13000;
    c.player_class = 0x21000;
    c.handler_class = 0x22000;
    c.look_class = 0x23000;
    c.player_handler_offset = 0x78;
    c.handler_player_offset = 0x58;
    c.look_offset = 0x140;
    c.value_offset = 0x20;
    c.eye[0] = 1.f; c.eye[1] = 2.f; c.eye[2] = 3.f;
    c.aim[0] = 1.f; c.aim[1] = 2.f; c.aim[2] = 7.f;
    return c;
}

Memory guarded_memory(const silent_aim::Command& c) {
    Memory memory;
    memory.pointers[c.player] = c.player_class;
    memory.pointers[c.player + c.player_handler_offset] = c.handler;
    memory.pointers[c.handler] = c.handler_class;
    memory.pointers[c.handler + c.handler_player_offset] = c.player;
    memory.pointers[c.handler + c.look_offset] = c.look;
    memory.pointers[c.look] = c.look_class;
    return memory;
}
}

int main() {
    const auto c = command();

    // No FPObject/FPHitscan fields are needed: the shared player look chain is
    // sufficient for projectile weapons and remains guarded on every write.
    assert(silent_aim::valid_command(c));
    auto memory = guarded_memory(c);
    assert(silent_aim::tick(memory, c) == silent_aim::Result::written);
    assert(memory.last_write == c.look + c.value_offset);
    assert(std::fabs(memory.value[0]) < 0.00001f);
    assert(std::fabs(memory.value[1]) < 0.00001f);
    assert(std::fabs(memory.value[2] - 1.f) < 0.00001f);

    // At 100 m/s a runner at 5 m/s needs about 5 m of lead over 100 m,
    // even with gravity and drag disabled. The shooter running at 3 m/s
    // changes that lead; only the RELATIVE motion advances the worker axis.
    silent_aim::Command moving = c;
    const aim_ballistics::Parameters flat{100.f, 0.f, 0.f, 0.f, 200.f, true};
    assert(silent_aim::solve_axis({0.f, 1.5f, 0.f}, {0.f, 1.5f, 100.f},
                                  {5.f, 0.f, 0.f}, {3.f, 0.f, 0.f},
                                  flat, 0.f, 0.f, moving));
    memory = guarded_memory(moving);
    assert(silent_aim::tick(memory, moving) == silent_aim::Result::written);
    assert(memory.value[0] > 0.018f && memory.value[0] < 0.022f);
    const float first = memory.value[0];
    assert(silent_aim::tick(memory, moving, 0.05f) == silent_aim::Result::written);
    assert(memory.value[0] > first);
    assert(silent_aim::tick(memory, moving, 0.11f) == silent_aim::Result::invalid);
    assert(!silent_aim::solve_axis({0.f, 1.5f, 0.f}, {0.f, 1.5f, 100.f},
                                   {NAN, 0.f, 0.f}, {}, flat, 0.f, 0.f, moving));
    assert(!silent_aim::solve_axis({0.f, 1.5f, 0.f}, {0.f, 1.5f, 100.f},
                                   {}, {}, flat, 0.36f, 0.f, moving));
    // No weapon data: aim directly at the future bone, no fabricated speed.
    assert(silent_aim::solve_axis({0.f, 1.5f, 0.f}, {0.f, 1.5f, 100.f},
                                  {5.f, 0.f, 0.f}, {}, {}, 0.05f, 0.f, moving));
    assert(std::fabs(moving.aim[0] - 0.25f) < 0.001f);
    moving = c;
    assert(!silent_aim::solve_axis({0.f, 1.5f, 0.f}, {0.f, 1.5f, 300.f},
                                   {}, {}, flat, 0.f, 0.f, moving));

    memory = guarded_memory(c);
    memory.pointers[c.handler + c.look_offset] = 0x14000;
    assert(silent_aim::tick(memory, c) == silent_aim::Result::guard_failed);

    assert(!silent_aim::wants_no_spread(false, true, false));
    assert(!silent_aim::wants_no_spread(true, false, false));
    assert(!silent_aim::wants_no_spread(true, true, true));
    assert(silent_aim::wants_no_spread(true, true, false));
    std::puts("ALL SILENT-AIM TESTS PASS");
    return 0;
}
