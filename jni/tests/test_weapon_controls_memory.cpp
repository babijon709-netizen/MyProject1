#include "movement_tuning_memory.h"
#include "weapon_controls_memory.h"
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>

struct FakeIO {
    std::map<uint64_t, uint8_t> mem;
    std::map<uint64_t, std::pair<std::string, std::string>> types;
    uint64_t fail_read = 0, fail_write = 0, fail_after_write = 0;
    unsigned writes = 0;

    template<class T> void put(uint64_t address, T value) {
        const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
        for (size_t i = 0; i < sizeof(value); ++i) mem[address + i] = bytes[i];
    }
    template<class T> bool read(uint64_t address, T& value) {
        if (address == fail_read) return false;
        auto* bytes = reinterpret_cast<uint8_t*>(&value);
        for (size_t i = 0; i < sizeof(value); ++i) {
            const auto it = mem.find(address + i);
            if (it == mem.end()) return false;
            bytes[i] = it->second;
        }
        return true;
    }
    template<class T> bool write_heap(uint64_t address, T value) {
        if (address == fail_write) return false;
        ++writes;
        put(address, value);
        if (address == fail_after_write) fail_read = address;
        return true;
    }
    bool exact(uint64_t address, const char* name, const char* namespaze) {
        const auto it = types.find(address);
        return it != types.end() && it->second.first == name && it->second.second == namespaze;
    }
    void object(uint64_t address, const char* name, const char* namespaze) {
        types[address] = {name, namespaze};
        put<uint64_t>(address, address + 0x800000);
        put<uint64_t>(address + go_active::MANAGED_CACHED_PTR, address + 0x900000);
    }
    float value(uint64_t address) {
        float result = 0.f;
        assert(read(address, result));
        return result;
    }
};

namespace {
constexpr uint64_t kPlayer = 0x10000;
constexpr uint64_t kNetwork = 0x20000;
constexpr uint64_t kFp = 0x30000;
constexpr uint64_t kBow = 0x40000;
constexpr uint64_t kBow2 = 0x48000;
constexpr uint64_t kProjectile = 0x50000;
constexpr uint64_t kProjectile2 = 0x58000;

void install(FakeIO& io, uint64_t weapon, uint64_t projectile) {
    io.object(weapon, "FPBow", "Oxide");
    io.object(projectile, "ShaftedProjectile", "Oxide");
    io.put<uint64_t>(weapon + go_active::FPOBJECT_PLAYER_BACKREF, kPlayer);
    io.put<uint64_t>(weapon + go_active::FPBOW_PROJECTILE_PREFAB, projectile);
    io.put<uint8_t>(weapon + go_active::FPWEAPON_IS_AIMING, 1);
    io.put<uint8_t>(weapon + go_active::FPWEAPON_CAN_SPRINT_WHILE_RELOADING, 0);
    io.put<float>(weapon + go_active::FPWEAPON_SHOT_PERIOD, 0.6f);
    io.put<float>(weapon + go_active::FPBOW_MIN_SHOT_PERIOD, 0.6f);
    io.put<float>(weapon + go_active::FPBOW_AIM_SPEED, 2.f);
    io.put<float>(weapon + go_active::FPBOW_AIM_POWER, 3.f);
    io.put<float>(projectile + go_active::SHAFTEDPROJECTILE_LAUNCH_SPEED, 50.f);
}

struct Fixture {
    FakeIO io;
    movement_tuning::Controller<FakeIO> movement{io};
    weapon_controls::Controller<FakeIO> weapons{io};

    Fixture() {
        io.object(kPlayer, "PlayerManager", "Oxide");
        io.object(kNetwork, "NetworkIdentity", "Mirror");
        io.object(kFp, "FPManager", "Oxide");
        io.put<uint64_t>(kPlayer + go_active::NETWORK_BEHAVIOUR_IDENTITY, kNetwork);
        io.put<uint8_t>(kNetwork + go_active::NETWORK_IDENTITY_IS_LOCAL_PLAYER, 1);
        io.put<uint64_t>(kPlayer + go_active::PLAYER_FP_MANAGER, kFp);
        install(io, kBow, kProjectile);
        io.put<uint64_t>(kFp + go_active::FPMANAGER_CURRENT_OBJECT, kBow);
    }

    ViewmodelSettings settings() const {
        ViewmodelSettings value;
        value.projectile_speed_enabled = true;
        value.bow_draw_speed_enabled = true;
        value.bow_draw_power_enabled = true;
        value.projectile_speed_multiplier = 2.f;
        value.bow_draw_speed_multiplier = 2.f;
        value.bow_draw_power_multiplier = 2.f;
        return value;
    }
};

void test_offsets_and_config() {
    using namespace go_active;
    assert(FPWEAPON_CAN_SPRINT_WHILE_RELOADING == 0xFA);
    assert(FPBOW_MIN_SHOT_PERIOD == 0x138);
    assert(FPBOW_AIM_SPEED == 0x144);
    assert(FPBOW_AIM_POWER == 0x148);
    assert(SHAFTEDPROJECTILE_LAUNCH_SPEED == 0xA8);
    assert(FPWEAPON_CAN_SPRINT_METHOD_RVA == 0x68693CC ||
           FPWEAPON_CAN_SPRINT_METHOD_RVA == 0x68D5DF4);

    ViewmodelSettings settings;
    settings.sprint_while_aiming = true;
    settings.sprint_while_reloading = true;
    settings.bow_no_delay = true;
    settings.projectile_speed_enabled = true;
    settings.bow_draw_speed_enabled = true;
    settings.bow_draw_power_enabled = true;
    settings.projectile_speed_multiplier = 2.f;
    settings.bow_draw_speed_multiplier = 3.f;
    settings.bow_draw_power_multiplier = 4.f;
    const auto extension = viewmodel::encode_weapon(settings);
    assert(extension.version == 1 && extension.size == sizeof(extension));
    ViewmodelSettings decoded;
    uint8_t bytes[sizeof(extension)];
    std::memcpy(bytes, &extension, sizeof(bytes));
    assert(viewmodel::decode_weapon(bytes, sizeof(bytes), decoded));
    assert(decoded.sprint_while_aiming && decoded.sprint_while_reloading && decoded.bow_no_delay);
    assert(decoded.projectile_speed_enabled && decoded.bow_draw_speed_enabled && decoded.bow_draw_power_enabled);
    assert(decoded.projectile_speed_multiplier == 2.f);
    assert(decoded.bow_draw_speed_multiplier == 3.f);
    assert(decoded.bow_draw_power_multiplier == 4.f);
    auto corrupt = extension;
    corrupt.flags |= 64u;
    assert(!viewmodel::decode_weapon(&corrupt, sizeof(corrupt), decoded));

    auto legacy = viewmodel::encode(settings);
    assert(viewmodel::decode(&legacy, 32).sprint_while_aiming == false);
    assert(!viewmodel::decode_weapon(&legacy, 32, decoded));
}

void test_apply_readback_restore_and_switch() {
    Fixture fixture;
    auto settings = fixture.settings();
    fast_fire::Controller<FakeIO> rapid(fixture.io);
    rapid.tick(1, 0x1000000, kPlayer, true, true, false);
    assert(rapid.status == viewmodel::Status::Active);
    assert(fixture.io.value(kBow + go_active::FPBOW_MIN_SHOT_PERIOD) == 0.f);
    assert(fixture.io.value(kBow + go_active::FPWEAPON_SHOT_PERIOD) == 0.f);
    rapid.tick(1, 0x1000000, kPlayer, false, false, false);
    assert(fixture.io.value(kBow + go_active::FPBOW_MIN_SHOT_PERIOD) == 0.6f);
    assert(fixture.io.value(kBow + go_active::FPWEAPON_SHOT_PERIOD) == 0.6f);
    fixture.movement.tick(1, 0x1000000, kPlayer, true, true);
    assert(fixture.movement.status == viewmodel::Status::Active);
    assert(fixture.io.mem[kBow + go_active::FPWEAPON_IS_AIMING] == 0);
    assert(fixture.io.mem[kBow + go_active::FPWEAPON_CAN_SPRINT_WHILE_RELOADING] == 1);

    fixture.weapons.tick(1, 0x1000000, kPlayer, settings);
    assert(fixture.weapons.status == viewmodel::Status::Active);
    assert(fixture.io.value(kProjectile + go_active::SHAFTEDPROJECTILE_LAUNCH_SPEED) == 100.f);
    assert(fixture.io.value(kBow + go_active::FPBOW_AIM_SPEED) == 4.f);
    assert(fixture.io.value(kBow + go_active::FPBOW_AIM_POWER) == 6.f);

    fixture.movement.tick(1, 0x1000000, kPlayer, false, false);
    fixture.weapons.tick(1, 0x1000000, kPlayer, ViewmodelSettings{});
    assert(fixture.io.mem[kBow + go_active::FPWEAPON_IS_AIMING] == 1);
    assert(fixture.io.mem[kBow + go_active::FPWEAPON_CAN_SPRINT_WHILE_RELOADING] == 0);
    assert(fixture.io.value(kProjectile + go_active::SHAFTEDPROJECTILE_LAUNCH_SPEED) == 50.f);
    assert(fixture.io.value(kBow + go_active::FPBOW_AIM_SPEED) == 2.f);
    assert(fixture.io.value(kBow + go_active::FPBOW_AIM_POWER) == 3.f);

    install(fixture.io, kBow2, kProjectile2);
    fixture.io.put<uint64_t>(kFp + go_active::FPMANAGER_CURRENT_OBJECT, kBow2);
    fixture.movement.tick(1, 0x1000000, kPlayer, true, true);
    fixture.weapons.tick(1, 0x1000000, kPlayer, settings);
    assert(fixture.io.mem[kBow + go_active::FPWEAPON_IS_AIMING] == 1);
    assert(fixture.io.value(kProjectile2 + go_active::SHAFTEDPROJECTILE_LAUNCH_SPEED) == 100.f);
    fixture.movement.reset(true);
    fixture.weapons.reset(true);
    assert(fixture.io.mem[kBow2 + go_active::FPWEAPON_IS_AIMING] == 1);
    assert(fixture.io.mem[kBow2 + go_active::FPWEAPON_CAN_SPRINT_WHILE_RELOADING] == 0);
    assert(fixture.io.value(kProjectile2 + go_active::SHAFTEDPROJECTILE_LAUNCH_SPEED) == 50.f);
}

void test_foreign_update_and_fail_closed() {
    Fixture fixture;
    auto settings = fixture.settings();
    fixture.weapons.tick(1, 0x1000000, kPlayer, settings);
    fixture.io.put<float>(kProjectile + go_active::SHAFTEDPROJECTILE_LAUNCH_SPEED, 777.f);
    fixture.weapons.tick(1, 0x1000000, kPlayer, ViewmodelSettings{});
    assert(fixture.io.value(kProjectile + go_active::SHAFTEDPROJECTILE_LAUNCH_SPEED) == 777.f);
    assert(!fixture.weapons.has_state());

    Fixture failed;
    failed.io.fail_after_write = kProjectile + go_active::SHAFTEDPROJECTILE_LAUNCH_SPEED;
    failed.weapons.tick(1, 0x1000000, kPlayer, settings);
    assert(failed.weapons.status == viewmodel::Status::Failed && failed.weapons.stage() == 10);
    assert(failed.weapons.has_state());
    failed.io.fail_after_write = failed.io.fail_read = 0;
    failed.weapons.tick(1, 0x1000000, kPlayer, ViewmodelSettings{});
    assert(failed.weapons.status == viewmodel::Status::Off && !failed.weapons.has_state());
    assert(failed.io.value(kProjectile + go_active::SHAFTEDPROJECTILE_LAUNCH_SPEED) == 50.f);

    Fixture movement_failed;
    movement_failed.io.fail_write = kBow + go_active::FPWEAPON_CAN_SPRINT_WHILE_RELOADING;
    movement_failed.movement.tick(1, 0x1000000, kPlayer, false, true);
    assert(movement_failed.movement.status == viewmodel::Status::Failed);
    movement_failed.io.fail_write = 0;
    movement_failed.movement.tick(1, 0x1000000, kPlayer, false, false);
    assert(movement_failed.movement.status == viewmodel::Status::Off);
}

} // namespace

int main() {
    for (auto version : {go_active::GameVersion::Release, go_active::GameVersion::Beta}) {
        go_active::Select(version);
        test_offsets_and_config();
        test_apply_readback_restore_and_switch();
        test_foreign_update_and_fail_closed();
    }
    std::puts("weapon controls: all tests passed");
    return 0;
}
