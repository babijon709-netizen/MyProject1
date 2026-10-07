#include "weapon_speed_memory.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <unordered_map>

struct FakeIO {
    std::unordered_map<std::uint64_t, float> values;

    template<class T> bool read(std::uint64_t address, T& out) {
        const auto it = values.find(address);
        if (it == values.end() || sizeof(T) != sizeof(float)) return false;
        std::memcpy(&out, &it->second, sizeof(T));
        return true;
    }

    template<class T> bool write_heap(std::uint64_t address, const T& value) {
        if (sizeof(T) != sizeof(float)) return false;
        float copied = 0.f;
        std::memcpy(&copied, &value, sizeof(copied));
        values[address] = copied;
        return true;
    }
};

static int failures = 0;
#define CHECK(expr, message) do { \
    if (!(expr)) { std::cerr << "FAIL: " << message << '\n'; ++failures; } \
} while (0)

int main() {
    constexpr std::uint64_t address = 0x100000;
    FakeIO io;
    weapon_speed::Scalar scalar;

    io.values[address] = 1.f;
    CHECK(scalar.sample(io, address, 0.f, 10.f), "capture original");
    CHECK(scalar.put(io, 0.2f), "apply accelerated value");
    CHECK(std::fabs(scalar.original - 1.f) < 0.0001f, "original retained after apply");

    // Game-side restoration is transient, not a new lifecycle original.
    io.values[address] = 1.f;
    CHECK(scalar.sample(io, address, 0.f, 10.f), "sample game restoration");
    CHECK(std::fabs(scalar.original - 1.f) < 0.0001f, "restoration does not compound original");
    CHECK(scalar.put(io, 0.2f), "reapply stable target");

    // An intermediate action-state value must not become a new baseline either.
    io.values[address] = 0.5f;
    CHECK(scalar.sample(io, address, 0.f, 10.f), "sample intermediate value");
    CHECK(std::fabs(scalar.original - 1.f) < 0.0001f, "intermediate value does not replace original");
    CHECK(scalar.put(io, 0.2f), "target remains based on lifecycle original");

    weapon_speed::Context before_shot;
    before_shot.player.object = 0x110000;
    before_shot.fp.object = 0x120000;
    before_shot.weapon.object = 0x130000;
    before_shot.config.object = 0x140000;
    weapon_speed::Context after_shot = before_shot;
    after_shot.burst_wait.object = 0x150000;
    CHECK(before_shot == after_shot,
          "lazy burst wait creation does not change weapon context");
    after_shot.weapon.object = 0x160000;
    CHECK(!(before_shot == after_shot), "active weapon change changes context");

    if (failures) return 1;
    std::cout << "ALL WEAPON-SPEED TESTS PASS\n";
    return 0;
}
