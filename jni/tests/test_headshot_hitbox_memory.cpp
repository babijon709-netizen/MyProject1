#include "headshot_hitbox_memory.h"

#include <cstdio>
#include <cstring>
#include <map>

namespace {
struct FakeIO {
    std::map<uint64_t, unsigned char> mem;
    bool allow_write = true;

    template<class T> bool read(uint64_t address, T& value) {
        std::memset(&value, 0, sizeof(value));
        for (size_t i = 0; i < sizeof(value); ++i) {
            const auto it = mem.find(address + i);
            if (it == mem.end()) return false;
            reinterpret_cast<unsigned char*>(&value)[i] = it->second;
        }
        return true;
    }
    template<class T> void seed(uint64_t address, const T& value) {
        const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
        for (size_t i = 0; i < sizeof(value); ++i) mem[address + i] = bytes[i];
    }
    template<class T> bool write_heap(uint64_t address, const T& value) {
        if (!allow_write) return false;
        seed(address, value);
        return true;
    }
};

constexpr uint64_t kKcc = 0x10000;
constexpr uint64_t kRoot = 0x11000;
constexpr uint64_t kArray = 0x12000;
constexpr uint64_t kHead = 0x13000;
constexpr uint64_t kChest = 0x14000;
constexpr uint64_t kLeg = 0x15000;
constexpr uint64_t kClass = 0x20000;

int failures = 0;
#define CHECK(condition, label) do { \
    if (!(condition)) { ++failures; std::printf("FAIL: %s (line %d)\n", label, __LINE__); } \
} while (0)

void seed_hitbox(FakeIO& io, uint64_t hitbox, int32_t area, uint64_t native) {
    io.seed(hitbox, kClass);
    io.seed(hitbox + go_active::MANAGED_CACHED_PTR, native);
    io.seed(hitbox + go_active::HITBOX_AREA, area);
    const float size[3] = {0.4f, 0.6f, 0.35f};
    io.seed(hitbox + go_active::HITBOX_SIZE, size);
}

int32_t area(FakeIO& io, uint64_t hitbox) {
    int32_t value = -1;
    io.read(hitbox + go_active::HITBOX_AREA, value);
    return value;
}

void seed_scene(FakeIO& io) {
    io.seed(kKcc + go_active::KCC_HITBOX_ROOT, kRoot);
    io.seed(kRoot + go_active::HITBOX_ROOT_ARRAY, kArray);
    const int32_t count = 3;
    io.seed(kArray + go_active::IL2CPP_ARRAY_LENGTH, count);
    io.seed(kArray + go_active::IL2CPP_ARRAY_FIRST_ELEMENT + 0 * 8, kHead);
    io.seed(kArray + go_active::IL2CPP_ARRAY_FIRST_ELEMENT + 1 * 8, kChest);
    io.seed(kArray + go_active::IL2CPP_ARRAY_FIRST_ELEMENT + 2 * 8, kLeg);
    seed_hitbox(io, kHead, 0, 0x30000);
    seed_hitbox(io, kChest, 1, 0x31000);
    seed_hitbox(io, kLeg, 2, 0x32000);
}
} // namespace

int main() {
    // The toggle persists only in the v5 trailer; v4 profiles keep it off.
    {
        ViewmodelSettings settings;
        settings.headshot_body = true;
        const auto v5 = viewmodel::encode(settings);
        CHECK(viewmodel::decode(&v5, sizeof(v5)).headshot_body, "v5 headshot toggle persists");
        auto v4 = v5;
        v4.version = 4;
        v4.flags &= ~1024u;
        CHECK(!viewmodel::decode(&v4, sizeof(v4)).headshot_body, "v4 profile keeps toggle off");
    }

    FakeIO io;
    seed_scene(io);
    headshot_hitbox::Controller<FakeIO> controller(io);

    controller.begin_frame(true);
    CHECK(controller.apply_kcc(kKcc), "body hit areas remapped");
    controller.end_frame();
    CHECK(area(io, kHead) == 0, "native head untouched");
    CHECK(area(io, kChest) == 0 && area(io, kLeg) == 0, "body areas become head");
    CHECK(controller.tracked() == 2, "only changed body hitboxes tracked");

    // A regular next frame keeps ownership and does not lose restoration data.
    controller.begin_frame(true);
    controller.apply_kcc(kKcc);
    controller.end_frame();
    CHECK(controller.tracked() == 2, "tracked areas survive steady frames");

    // An area changed by the game after our write does not get overwritten by
    // restoration, while the still-owned entry restores exactly to its source.
    const int32_t game_value = 4;
    io.seed(kLeg + go_active::HITBOX_AREA, game_value);
    controller.begin_frame(false);
    controller.end_frame();
    CHECK(area(io, kChest) == 1, "owned chest area restored");
    CHECK(area(io, kLeg) == 4, "external replacement preserved");
    CHECK(controller.tracked() == 0, "disable clears restored entries");

    // A dead/recycled managed identity is never written during cleanup.
    seed_scene(io);
    controller.begin_frame(true);
    controller.apply_kcc(kKcc);
    controller.end_frame();
    const uint64_t recycled_class = 0x22000;
    io.seed(kChest, recycled_class);
    io.seed(kChest + go_active::MANAGED_CACHED_PTR, uint64_t{0x33000});
    controller.begin_frame(false);
    controller.end_frame();
    CHECK(area(io, kChest) == 0, "recycled identity is not restored into");

    if (failures) { std::printf("%d FAILURES\n", failures); return 1; }
    std::puts("ALL HEADSHOT-HITBOX TESTS PASS");
    return 0;
}
