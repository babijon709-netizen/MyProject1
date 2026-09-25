#include "silent_aim_core.h"
#include <cassert>
#include <cstdio>
#include <limits>
#include <map>
#include <vector>

struct Memory {
    std::map<uint64_t, std::vector<unsigned char>> data;
    unsigned writes = 0;
    bool fail_write = false;
    bool read(uint64_t a, void* out, size_t n) {
        auto it = data.find(a);
        if (it == data.end() || it->second.size() != n) return false;
        std::memcpy(out, it->second.data(), n); return true;
    }
    bool write(uint64_t a, const void* in, size_t n) {
        if (fail_write) return false;
        const auto* p = static_cast<const unsigned char*>(in);
        data[a] = {p, p + n}; ++writes; return true;
    }
    void pointer(uint64_t a, uint64_t p) { write(a, &p, sizeof(p)); }
};
int main() {
    using namespace silent_aim;
    Command c;
    c.pid = 42; c.player = 0x100000; c.handler = 0x200000; c.look = 0x300000;
    c.player_class = 0x400000; c.handler_class = 0x500000; c.look_class = 0x600000;
    c.fp_manager = 0x700000; c.weapon = 0x800000; c.target = 0x900000;
    c.player_handler_offset = 0x78; c.handler_player_offset = 0xd0;
    c.look_offset = 0x140; c.value_offset = 0x20; c.fp_manager_offset = 0x90; c.weapon_offset = 0x50;
    c.eye[1] = 2.f; c.aim[0] = 3.f; c.aim[1] = 2.f; c.aim[2] = 4.f;
    Memory m;
    m.pointer(c.player, c.player_class);
    m.pointer(c.handler, c.handler_class); m.pointer(c.look, c.look_class);
    m.pointer(c.player + c.player_handler_offset, c.handler | 0xab00000000000000ULL);
    m.pointer(c.handler + c.handler_player_offset, c.player);
    m.pointer(c.handler + c.look_offset, c.look);
    m.pointer(c.player + c.fp_manager_offset, c.fp_manager);
    m.pointer(c.fp_manager + c.weapon_offset, c.weapon);
    const Memory original = m;
    assert(tick(m, c) == Result::written);
    float dir[3]; assert(m.read(c.look + c.value_offset, dir, sizeof(dir)));
    assert(std::fabs(dir[0] - .6f) < 1e-5f && dir[1] == 0.f && std::fabs(dir[2] - .8f) < 1e-5f);
    // Each guard independently refuses a stale route/class/weapon, with no write.
    for (const auto& item : original.data) {
        m = original; m.pointer(item.first, 0xdead00);
        const auto before = m.writes;
        assert(tick(m, c) == Result::guard_failed && m.writes == before);
        m = original; m.data.erase(item.first);
        assert(tick(m, c) == Result::guard_failed);
    }
    m = original; m.fail_write = true;
    assert(tick(m, c) == Result::write_failed);
    m = original;
    for (float v : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), 2e6f}) {
        Command bad = c; bad.aim[0] = v;
        assert(tick(m, bad) == Result::invalid);
    }
    Command bad = c;
    for (int i = 0; i < 3; ++i) bad.aim[i] = bad.eye[i];
    assert(tick(m, bad) == Result::invalid);
    bad = c; bad.value_offset = 0;
    assert(tick(m, bad) == Result::invalid);
    bad = c; bad.pid = -1; assert(tick(m, bad) == Result::invalid);
    assert(fresh(1000, 1000) && fresh(1000, 1099));
    assert(!fresh(1000, 1100) && !fresh(1000, 999));
    std::puts("silent core: geometry, TBI, all guards, failed IO, NaN/Inf, lease OK");
}
