#include "no_ballistics_memory.h"
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>

struct FakeIO {
    std::map<uint64_t, uint8_t> mem;
    std::map<uint64_t, std::pair<std::string, std::string>> types;
    uint64_t fail_read = 0, fail_write = 0;
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
        return true;
    }
    bool named(uint64_t address, const char* name, const char* namespaze) {
        const auto it = types.find(address);
        return it != types.end() && it->second.first == name && it->second.second == namespaze;
    }
    bool exact(uint64_t address, const char* name, const char* namespaze) {
        return named(address, name, namespaze);
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
constexpr uint64_t kFpManager = 0x30000;
constexpr uint64_t kWeapon = 0x40000;
constexpr uint64_t kConfig = 0x50000;
constexpr uint64_t kProfile = 0x60000;
constexpr uint64_t kProjectile = 0x70000;
constexpr uint64_t kSecondWeapon = 0x80000;
constexpr uint64_t kSecondConfig = 0x90000;
constexpr uint64_t kSecondProfile = 0xA0000;
constexpr uint64_t kSecondProjectile = 0xB0000;
constexpr float kProfileValues[3] = {0.65f, 0.025f, 0.75f};
constexpr float kProjectileValues[3] = {9.81f, 0.12f, 0.44f};

uint64_t profile_address(uint64_t base, size_t index) {
    using namespace go_active;
    static const uint64_t offsets[3] = {
        BALLISTICPROFILE_GRAVITY,
        BALLISTICPROFILE_AIR_DRAG,
        BALLISTICPROFILE_UNDERWATER_DRAG,
    };
    return base + offsets[index];
}
uint64_t projectile_address(uint64_t base, size_t index) {
    using namespace go_active;
    static const uint64_t offsets[3] = {
        SHAFTEDPROJECTILE_GRAVITY,
        SHAFTEDPROJECTILE_AIR_DRAG,
        SHAFTEDPROJECTILE_UNDERWATER_DRAG,
    };
    return base + offsets[index];
}

class Fixture {
public:
    FakeIO io;
    no_ballistics::Controller<FakeIO> controller{io};

    Fixture(fast_fire::Kind kind) {
        io.object(kPlayer, "PlayerManager", "Oxide");
        io.object(kNetwork, "NetworkIdentity", "Mirror");
        io.object(kFpManager, "FPManager", "Oxide");
        io.put<uint64_t>(kPlayer + go_active::NETWORK_BEHAVIOUR_IDENTITY, kNetwork);
        io.put<uint8_t>(kNetwork + go_active::NETWORK_IDENTITY_IS_LOCAL_PLAYER, 1);
        io.put<uint64_t>(kPlayer + go_active::PLAYER_FP_MANAGER, kFpManager);
        install_weapon(kWeapon, kConfig, kProfile, kProjectile, kind);
        io.put<uint64_t>(kFpManager + go_active::FPMANAGER_CURRENT_OBJECT, kWeapon);
    }

    void install_weapon(uint64_t weapon, uint64_t config, uint64_t profile,
                       uint64_t projectile, fast_fire::Kind kind) {
        const char* name = "";
        const char* namespaze = "Oxide";
        switch (kind) {
            case fast_fire::Kind::Hitscan:        name = "FPHitscan"; break;
            case fast_fire::Kind::Bow:            name = "FPBow"; break;
            case fast_fire::Kind::Crossbow:       name = "FPCrossbow"; namespaze = fast_fire::kFirstPerson; break;
            case fast_fire::Kind::RocketLauncher: name = "FPRocketLauncher"; break;
            case fast_fire::Kind::ExplosiveCharge:name = "FPExplosiveCharge"; break;
            case fast_fire::Kind::Snowball:       name = "FPSnowball"; namespaze = fast_fire::kFirstPerson; break;
            case fast_fire::Kind::Spear:          name = "FPSpear"; break;
            default:                              name = "FPMelee"; break;
        }
        io.object(weapon, name, namespaze);
        io.put<uint64_t>(weapon + go_active::FPOBJECT_PLAYER_BACKREF, kPlayer);

        if (no_ballistics::has_profile(kind)) {
            io.object(config, "HitscanWeaponConfig", weapon_tuning::kWeapons);
            io.object(profile, "BallisticProfile", no_ballistics::kBallistics);
            io.put<uint64_t>(weapon + no_ballistics::config_offset(kind), config);
            io.put<uint64_t>(config + go_active::HITSCANCONFIG_BALLISTIC_PROFILE, profile);
            for (size_t i = 0; i < 3; ++i)
                io.put<float>(profile_address(profile, i), kProfileValues[i]);
        }
        if (no_ballistics::has_projectile(kind)) {
            io.object(projectile, "ShaftedProjectile", "Oxide");
            io.put<uint64_t>(weapon + no_ballistics::projectile_offset(kind), projectile);
            for (size_t i = 0; i < 3; ++i)
                io.put<float>(projectile_address(projectile, i), kProjectileValues[i]);
        }
    }

    void tick(bool enabled = true) {
        controller.tick(1, 0x1000000, kPlayer, enabled);
    }
};

void test_config_extension() {
    ViewmodelSettings settings;
    settings.no_ballistics = true;
    settings.no_spread = true;
    settings.fast_loot = true;
    auto encoded = viewmodel::encode(settings);
    assert(encoded.version == 6 && encoded.size == sizeof(encoded));
    assert((encoded.flags & 2048u) != 0);
    auto decoded = viewmodel::decode(&encoded, sizeof(encoded));
    assert(decoded.no_ballistics && decoded.no_spread && decoded.fast_loot);

    encoded.aim_lead_ms = 75.f;
    assert(viewmodel::decode_aim_lead_ms(&encoded, sizeof(encoded)) == 75.f);
    auto v5 = encoded;
    v5.version = 5;
    v5.flags &= ~2048u;
    decoded = viewmodel::decode(&v5, sizeof(v5));
    assert(!decoded.no_ballistics);
    assert(viewmodel::decode_aim_lead_ms(&v5, sizeof(v5)) == 75.f);

    auto old_unknown_flag = v5;
    old_unknown_flag.flags |= 2048u;
    decoded = viewmodel::decode(&old_unknown_flag, sizeof(old_unknown_flag));
    assert(!decoded.no_ballistics && !decoded.no_spread && !decoded.fast_loot);

    auto corrupt = encoded;
    corrupt.flags |= 4096u;
    decoded = viewmodel::decode(&corrupt, sizeof(corrupt));
    assert(!decoded.no_ballistics && !decoded.no_spread && !decoded.fast_loot);
}

void test_kind(fast_fire::Kind kind) {
    Fixture fixture(kind);
    fixture.tick();
    assert(fixture.controller.status == viewmodel::Status::Active);
    assert(fixture.controller.stage() == 0);

    if (no_ballistics::has_profile(kind)) {
        for (size_t i = 0; i < 3; ++i)
            assert(fixture.io.value(profile_address(kProfile, i)) == 0.f);
    }
    if (no_ballistics::has_projectile(kind)) {
        for (size_t i = 0; i < 3; ++i)
            assert(fixture.io.value(projectile_address(kProjectile, i)) == 0.f);
    }

    const unsigned writes = fixture.io.writes;
    fixture.tick();
    assert(fixture.io.writes == writes);

    fixture.tick(false);
    assert(fixture.controller.status == viewmodel::Status::Off);
    assert(!fixture.controller.has_state());
    if (no_ballistics::has_profile(kind)) {
        for (size_t i = 0; i < 3; ++i)
            assert(std::fabs(fixture.io.value(profile_address(kProfile, i)) - kProfileValues[i]) < 1e-6f);
    }
    if (no_ballistics::has_projectile(kind)) {
        for (size_t i = 0; i < 3; ++i)
            assert(std::fabs(fixture.io.value(projectile_address(kProjectile, i)) - kProjectileValues[i]) < 1e-6f);
    }
}

void test_restore_ownership() {
    Fixture fixture(fast_fire::Kind::Hitscan);
    fixture.tick();
    fixture.io.put<float>(profile_address(kProfile, 0), 4.25f);
    fixture.tick(false);
    assert(fixture.io.value(profile_address(kProfile, 0)) == 4.25f);
    assert(fixture.io.value(profile_address(kProfile, 1)) == kProfileValues[1]);
    assert(fixture.io.value(profile_address(kProfile, 2)) == kProfileValues[2]);
}

void test_debug_snapshot_and_write_failures() {
    Fixture fixture(fast_fire::Kind::Hitscan);
    const uint64_t gravity = profile_address(kProfile, 0);
    fixture.io.fail_write = gravity;
    fixture.tick();
    auto debug = fixture.controller.debug_info();
    assert(fixture.controller.status == viewmodel::Status::Failed);
    assert(fixture.controller.stage() == 16);
    assert(debug.error_stage == 16 && debug.error_address == gravity);
    assert(debug.kind == fast_fire::Kind::Hitscan && debug.probe_kind == fast_fire::Kind::Hitscan);
    assert(debug.probe_weapon == kWeapon && debug.probe_config == kConfig && debug.probe_profile == kProfile);
    assert(debug.target_mask == 0x07 && debug.saved_mask == 0x01);

    fixture.io.fail_write = 0;
    fixture.tick();
    debug = fixture.controller.debug_info();
    assert(fixture.controller.status == viewmodel::Status::Active);
    assert(debug.patched_mask == 0x07 && debug.saved_mask == 0x07);

    fixture.io.fail_write = gravity;
    fixture.tick(false);
    debug = fixture.controller.debug_info();
    assert(fixture.controller.status == viewmodel::Status::Failed);
    assert(debug.error_stage == 18 && debug.error_address == gravity);
    assert((debug.restore_attempted_mask & 0x01) != 0);

    fixture.io.fail_write = 0;
    fixture.tick(false);
    assert(fixture.controller.status == viewmodel::Status::Off);
    assert(!fixture.controller.has_state());
    for (size_t i = 0; i < 3; ++i)
        assert(std::fabs(fixture.io.value(profile_address(kProfile, i)) - kProfileValues[i]) < 1e-6f);
}

void test_weapon_switch() {
    Fixture fixture(fast_fire::Kind::Bow);
    fixture.tick();
    assert(fixture.io.value(projectile_address(kProjectile, 0)) == 0.f);

    fixture.install_weapon(kSecondWeapon, kSecondConfig, kSecondProfile,
                           kSecondProjectile, fast_fire::Kind::Hitscan);
    fixture.io.put<uint64_t>(kFpManager + go_active::FPMANAGER_CURRENT_OBJECT, kSecondWeapon);
    fixture.tick();
    for (size_t i = 0; i < 3; ++i) {
        assert(std::fabs(fixture.io.value(projectile_address(kProjectile, i)) - kProjectileValues[i]) < 1e-6f);
        assert(fixture.io.value(profile_address(kSecondProfile, i)) == 0.f);
    }

    fixture.tick(false);
    for (size_t i = 0; i < 3; ++i)
        assert(std::fabs(fixture.io.value(profile_address(kSecondProfile, i)) - kProfileValues[i]) < 1e-6f);
}

void test_switch_to_non_ballistic_weapon_restores() {
    Fixture fixture(fast_fire::Kind::Bow);
    fixture.tick();
    const unsigned patched_writes = fixture.io.writes;
    fixture.install_weapon(kSecondWeapon, kSecondConfig, kSecondProfile,
                           kSecondProjectile, fast_fire::Kind::Melee);
    fixture.io.object(kSecondWeapon, "FPBuildingHammer", "Oxide");
    fixture.io.put<uint64_t>(kFpManager + go_active::FPMANAGER_CURRENT_OBJECT, kSecondWeapon);
    fixture.tick();
    assert(fixture.controller.status == viewmodel::Status::Active);
    assert(!fixture.controller.has_state());
    assert(fixture.io.writes == patched_writes + 3);
    for (size_t i = 0; i < 3; ++i)
        assert(std::fabs(fixture.io.value(projectile_address(kProjectile, i)) - kProjectileValues[i]) < 1e-6f);
    fixture.tick(false);
    assert(fixture.io.writes == patched_writes + 3);
}

void test_melee_is_supported_without_writes() {
    for (const char* name : {"FPMelee", "FPBuildingHammer", "FPTool", "FPTorch", "FPChainsaw"}) {
        Fixture fixture(fast_fire::Kind::Melee);
        fixture.io.object(kWeapon, name, "Oxide");
        fixture.tick();
        assert(fixture.controller.status == viewmodel::Status::Active);
        assert(fixture.controller.stage() == 0);
        assert(fixture.io.writes == 0);
        fixture.tick(false);
        assert(fixture.controller.status == viewmodel::Status::Off);
    }
}

void test_fail_closed() {
    Fixture fixture(fast_fire::Kind::Bow);
    fixture.io.put<uint8_t>(kNetwork + go_active::NETWORK_IDENTITY_IS_LOCAL_PLAYER, 0);
    fixture.tick();
    assert(fixture.controller.status == viewmodel::Status::Waiting);
    assert(fixture.controller.stage() == 4);
    assert(fixture.io.writes == 0);
    assert(fixture.io.value(projectile_address(kProjectile, 0)) == kProjectileValues[0]);
}
}

int main() {
    for (auto version : {go_active::GameVersion::Release, go_active::GameVersion::Beta}) {
        go_active::Select(version);
        assert(go_active::FPBOW_PROJECTILE_PREFAB == 0x150);
        assert(go_active::FPCROSSBOW_CONFIG == 0x138);
        assert(go_active::FPCROSSBOW_PROJECTILE_PREFAB == 0x148);
        assert(go_active::FPROCKETLAUNCHER_CONFIG == 0x128);
        assert(go_active::FPROCKETLAUNCHER_PROJECTILE_PREFAB == 0x148);
        assert(go_active::FPEXPLOSIVECHARGE_PROJECTILE_PREFAB == 0x130);
        assert(go_active::FPSNOWBALL_PROJECTILE_PREFAB == 0x130);
        assert(go_active::FPSPEAR_PROJECTILE_PREFAB == 0x170);
        assert(go_active::HITSCANCONFIG_BALLISTIC_PROFILE == 0x60);
        assert(go_active::BALLISTICPROFILE_GRAVITY == 0x10);
        assert(go_active::SHAFTEDPROJECTILE_GRAVITY == 0xAC);

        for (auto kind : {fast_fire::Kind::Hitscan, fast_fire::Kind::Bow,
                          fast_fire::Kind::Crossbow, fast_fire::Kind::RocketLauncher,
                          fast_fire::Kind::ExplosiveCharge, fast_fire::Kind::Snowball,
                          fast_fire::Kind::Spear, fast_fire::Kind::Melee})
            test_kind(kind);
        test_restore_ownership();
        test_debug_snapshot_and_write_failures();
        test_weapon_switch();
        test_switch_to_non_ballistic_weapon_restores();
        test_melee_is_supported_without_writes();
        test_fail_closed();
    }
    test_config_extension();
    std::puts("ALL NO-BALLISTICS TESTS PASS");
    return 0;
}
