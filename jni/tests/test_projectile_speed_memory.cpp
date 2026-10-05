#include "projectile_speed_memory.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <utility>

namespace {
struct FakeIO {
    std::map<uint64_t, unsigned char> mem;
    std::map<uint64_t, std::pair<std::string, std::string>> names;

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
        seed(address, value);
        return true;
    }
    bool named(uint64_t object, const char* name, const char* namespaze) const {
        const auto it = names.find(object);
        return it != names.end() && it->second.first == name && it->second.second == namespaze;
    }
    bool named_strict(uint64_t object, const char* name, const char* namespaze) const {
        return named(object, name, namespaze);
    }
};

constexpr uint64_t kPlayer = 0x10000;
constexpr uint64_t kNetwork = 0x11000;
constexpr uint64_t kFp = 0x12000;
constexpr uint64_t kWeapon = 0x13000;
constexpr uint64_t kPrefab = 0x14000;

void seed_identity(FakeIO& io, uint64_t object, uint64_t klass, uint64_t native) {
    io.seed(object, klass);
    io.seed(object + go_active::MANAGED_CACHED_PTR, native);
}

void seed_scene(FakeIO& io, uint64_t projectile_prefab_offset) {
    seed_identity(io, kPlayer, 0x21000, 0x31000);
    seed_identity(io, kNetwork, 0x22000, 0x32000);
    seed_identity(io, kFp, 0x23000, 0x33000);
    seed_identity(io, kWeapon, 0x24000, 0x34000);
    seed_identity(io, kPrefab, 0x25000, 0x35000);
    io.names[kPlayer] = {"PlayerManager", "Oxide"};
    io.names[kNetwork] = {"NetworkIdentity", "Mirror"};
    io.names[kFp] = {"FPManager", "Oxide"};
    io.names[kPrefab] = {"ShaftedProjectile", "Oxide"};
    io.seed(kPlayer + go_active::NETWORK_BEHAVIOUR_IDENTITY, kNetwork);
    const uint8_t local = 1;
    io.seed(kNetwork + go_active::NETWORK_IDENTITY_IS_LOCAL_PLAYER, local);
    io.seed(kPlayer + go_active::PLAYER_FP_MANAGER, kFp);
    io.seed(kFp + go_active::FPMANAGER_CURRENT_OBJECT, kWeapon);
    io.seed(kWeapon + go_active::FPOBJECT_PLAYER_BACKREF, kPlayer);
    io.seed(kWeapon + projectile_prefab_offset, kPrefab);
    const float launch_speed = 50.f;
    io.seed(kPrefab + go_active::SHAFTEDPROJECTILE_LAUNCH_SPEED, launch_speed);
}

int failures = 0;
#define CHECK(condition, label) do { \
    if (!(condition)) { ++failures; std::printf("FAIL: %s (line %d)\n", label, __LINE__); } \
} while (0)

void check_offset(uint64_t offset, const char* label) {
    FakeIO io;
    seed_scene(io, offset);
    projectile_speed::Controller<FakeIO> controller(io);
    controller.tick(1, 0x40000, kPlayer, true);
    float speed = 0.f;
    CHECK(controller.status == viewmodel::Status::Active, label);
    CHECK(io.read(kPrefab + go_active::SHAFTEDPROJECTILE_LAUNCH_SPEED, speed), "accelerated speed readable");
    CHECK(speed == 50.f * projectile_speed::kMultiplier, "launch speed accelerated");
    const uint64_t none = 0;
    io.seed(kFp + go_active::FPMANAGER_CURRENT_OBJECT, none);
    controller.tick(1, 0x40000, kPlayer, true);
    CHECK(controller.status == viewmodel::Status::Waiting, "non-projectile weapon -> Waiting");
    CHECK(io.read(kPrefab + go_active::SHAFTEDPROJECTILE_LAUNCH_SPEED, speed), "unequip restoration readable");
    CHECK(speed == 50.f, "unequip restores launch speed");
    controller.tick(1, 0x40000, kPlayer, false);
    CHECK(controller.status == viewmodel::Status::Off, "disable -> Off");
}
} // namespace

int main() {
    check_offset(go_active::FPBOW_PROJECTILE_PREFAB, "bow prefab offset");
    check_offset(go_active::FPCROSSBOW_PROJECTILE_PREFAB, "crossbow prefab offset");
    check_offset(go_active::FPSNOWBALL_PROJECTILE_PREFAB, "snowball prefab offset");
    check_offset(go_active::FPROCKET_PROJECTILE_PREFAB, "rocket prefab offset");
    check_offset(go_active::FPEXPLOSIVE_PROJECTILE_PREFAB, "charge prefab offset");
    check_offset(go_active::FPSPEAR_PROJECTILE_PREFAB, "spear prefab offset");
    if (failures) { std::printf("%d FAILURES\n", failures); return 1; }
    std::puts("ALL PROJECTILE-SPEED TESTS PASS");
    return 0;
}
