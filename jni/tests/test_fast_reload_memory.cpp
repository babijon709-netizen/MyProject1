#include "fast_reload_memory.h"
#include "fast_fire_memory.h"
#include <cassert>
#include <cstring>
#include <map>
#include <string>
#include <limits>
#include <cstdio>
#include <vector>
#include <cmath>

bool near(float a, float b) { return std::fabs(a - b) < 1e-6f; }

struct FakeIO {
    std::map<uint64_t, uint8_t> mem;
    std::map<uint64_t, std::pair<std::string, std::string>> types;
    uint64_t fail_read = 0, fail_write = 0, fail_after_write = 0;
    unsigned writes = 0;
    std::vector<uint64_t> write_log;
    template<class T> void put(uint64_t a, T v) {
        auto* bytes = reinterpret_cast<const uint8_t*>(&v);
        for (size_t i = 0; i < sizeof(v); ++i) mem[a + i] = bytes[i];
    }
    template<class T> bool read(uint64_t a, T& v) {
        if (a == fail_read) return false;
        auto* bytes = reinterpret_cast<uint8_t*>(&v);
        for (size_t i = 0; i < sizeof(v); ++i) {
            auto it = mem.find(a + i);
            if (it == mem.end()) return false;
            bytes[i] = it->second;
        }
        return true;
    }
    template<class T> bool write_heap(uint64_t a, T v) {
        if (a == fail_write) return false;
        ++writes; write_log.push_back(a); put(a, v);
        if (a == fail_after_write) fail_read = a;
        return true;
    }
    bool exact(uint64_t a, const char* n, const char* ns) {
        auto it = types.find(a);
        return it != types.end() && it->second.first == n && it->second.second == ns;
    }
    void object(uint64_t a, const char* n, const char* ns) {
        types[a] = { n, ns };
        put<uint64_t>(a, a + 0x800000);
        put<uint64_t>(a + go_active::MANAGED_CACHED_PTR, a + 0x900000);
    }
    float value(uint64_t a) { float v = 0; assert(read(a, v)); return v; }
};

constexpr uint64_t player = 0x10000, net = 0x20000, fp = 0x30000, gun = 0x40000;
uint64_t reload_period() { return gun + go_active::FPWEAPON_EFFECTIVE_RELOAD_PERIOD; }
uint64_t loop_reload() { return gun + go_active::FPHITSCAN_EFFECTIVE_LOOP_RELOAD_PERIOD; }

struct Fixture {
    FakeIO io;
    fast_reload::Controller<FakeIO> c{ io };
    const char* name;
    Fixture(const char* n = "FPHitscan", const char* ns = "Oxide") : name(n) {
        io.object(player, "PlayerManager", "Oxide"); io.object(net, "NetworkIdentity", "Mirror");
        io.object(fp, "FPManager", "Oxide"); io.object(gun, name, ns);
        io.put<uint64_t>(player + go_active::NETWORK_BEHAVIOUR_IDENTITY, net);
        io.put<uint8_t>(net + go_active::NETWORK_IDENTITY_IS_LOCAL_PLAYER, 1);
        io.put<uint64_t>(player + go_active::PLAYER_FP_MANAGER, fp);
        io.put<uint64_t>(fp + go_active::FPMANAGER_CURRENT_OBJECT, gun);
        io.put<uint64_t>(gun + go_active::FPOBJECT_PLAYER_BACKREF, player);
        io.put<float>(reload_period(), 3.0f);
        if (std::strcmp(name, "FPHitscan") == 0) {
            io.put<float>(loop_reload(), 1.5f);
        }
    }
    void tick(bool on = true) { c.tick(1, 0x1000000, player, on); }
};

int main() {
    using namespace go_active;
    for (auto version : { GameVersion::Release, GameVersion::Beta }) {
        Select(version);

        // Basic: hitscan reduces both reload and loop-reload.
        {
            Fixture f;
            f.tick();
            assert(f.c.status == viewmodel::Status::Active);
            assert(near(f.io.value(reload_period()), fast_reload::kPeriod));
            assert(near(f.io.value(loop_reload()), fast_reload::kPeriod));
            // No repeated writes.
            const auto writes = f.io.writes; f.tick();
            assert(f.io.writes == writes);
            // Restore on disable.
            f.tick(false);
            assert(near(f.io.value(reload_period()), 3.0f));
            assert(near(f.io.value(loop_reload()), 1.5f));
            assert(!f.c.has_state());
        }

        // Non-hitscan: only the base reload period is touched.
        {
            Fixture f("FPBow", "Oxide");
            f.tick();
            assert(f.c.status == viewmodel::Status::Active);
            assert(near(f.io.value(reload_period()), fast_reload::kPeriod));
            const auto writes = f.io.writes;
            // Loop-reload slot is not used for non-hitscan.
            assert(writes == 1);
            f.tick(false);
            assert(near(f.io.value(reload_period()), 3.0f));
        }

        // Melee tools with zero reload period: not reduced (0 < kPeriod → already fast).
        {
            Fixture f("FPMelee", "Oxide");
            f.io.put<float>(reload_period(), 0.0f);
            f.tick();
            assert(f.c.status == viewmodel::Status::Active);
            // 0.0 <= kPeriod → no write.
            assert(near(f.io.value(reload_period()), 0.0f));
            assert(f.io.writes == 0);
        }

        // Already-fast period: no write.
        {
            Fixture f;
            f.io.put<float>(reload_period(), 0.01f);
            f.io.put<float>(loop_reload(), 0.01f);
            f.tick();
            assert(f.c.status == viewmodel::Status::Active);
            assert(f.io.writes == 0);
            f.tick(false);
            assert(near(f.io.value(reload_period()), 0.01f));
        }

        // Null weapon slot → waiting.
        {
            Fixture f;
            f.io.put<uint64_t>(fp + FPMANAGER_CURRENT_OBJECT, 0);
            f.tick();
            assert(f.c.status == viewmodel::Status::Waiting);
            assert(f.io.value(reload_period()) == 3.0f);
        }

        // Fail to read FPMANAGER_CURRENT_OBJECT → no writes.
        {
            Fixture f;
            f.io.fail_read = fp + FPMANAGER_CURRENT_OBJECT;
            f.tick();
            assert(!f.io.writes);
        }

        // Not local player → no writes.
        {
            Fixture f;
            f.io.put<uint8_t>(net + NETWORK_IDENTITY_IS_LOCAL_PLAYER, 0);
            f.tick();
            assert(!f.io.writes);
        }

        // Wrong back-ref → no writes.
        {
            Fixture f;
            f.io.put<uint64_t>(gun + FPOBJECT_PLAYER_BACKREF, player + 1);
            f.tick();
            assert(!f.io.writes);
        }

        // Weapon captured through FPManager: class name is irrelevant for
        // reload (all FPWeaponBase descendants share the field at +0xE8).
        // "FPObject" and "WrongNamespace" tests are intentionally absent —
        // fast_reload does not classify by weapon type (unlike fast_fire).

        // Invalid float → no writes.
        {
            Fixture f;
            f.io.put<float>(reload_period(), std::numeric_limits<float>::quiet_NaN());
            f.tick();
            assert(!f.io.writes && f.c.status == viewmodel::Status::Failed);
        }
        for (float v : { -1.f, 61.f, std::numeric_limits<float>::infinity() }) {
            Fixture f;
            f.io.put<float>(reload_period(), v);
            f.tick();
            assert(!f.io.writes);
        }

        // External update: game changed the value → our write was overwritten.
        {
            Fixture f;
            f.tick();
            f.io.put<float>(reload_period(), 2.0f);
            f.tick(false);
            assert(near(f.io.value(reload_period()), 2.0f));
        }

        // Write failure: restore still works later.
        {
            Fixture f;
            f.io.fail_write = reload_period();
            f.tick();
            assert(f.c.has_state());
            f.io.fail_write = 0;
            f.tick(false);
            assert(!f.c.has_state() && near(f.io.value(reload_period()), 3.0f));
        }

        // Read-after-write failure: restore succeeds when the read is unblocked.
        {
            Fixture f;
            f.io.fail_after_write = reload_period();
            f.tick();
            assert(f.c.has_state());
            f.io.fail_after_write = f.io.fail_read = 0;
            f.tick(false);
            assert(near(f.io.value(reload_period()), 3.0f) && !f.c.has_state());
        }

        // Destroyed weapon (native ptr zeroed) → restore forgone.
        {
            Fixture f;
            f.tick();
            f.io.put<uint64_t>(gun + MANAGED_CACHED_PTR, 0);
            auto writes = f.io.writes;
            f.tick(false);
            assert(f.io.writes == writes && !f.c.has_state());
        }

        // Session change without disable → resets state.
        {
            Fixture f;
            f.tick();
            auto writes = f.io.writes;
            f.c.tick(2, 0x2000000, 0, false);
            assert(f.io.writes == writes && !f.c.has_state());
        }

        // Weapon switch: old weapon restored, new weapon reduced.
        {
            Fixture f;
            f.tick();
            f.io.object(0x60000, "FPBow", "Oxide");
            f.io.put<uint64_t>(0x60000 + MANAGED_CACHED_PTR, 0x690000);
            f.io.put<uint64_t>(0x60000 + FPOBJECT_PLAYER_BACKREF, player);
            f.io.put<float>(0x60000 + FPWEAPON_EFFECTIVE_RELOAD_PERIOD, 5.0f);
            f.io.put<uint64_t>(fp + FPMANAGER_CURRENT_OBJECT, 0x60000);
            f.io.fail_write = reload_period();
            f.tick();
            assert(f.c.status == viewmodel::Status::Failed);
            assert(near(f.io.value(0x60000 + FPWEAPON_EFFECTIVE_RELOAD_PERIOD), 5.0f));
            f.io.fail_write = 0;
            f.tick();
            assert(near(f.io.value(reload_period()), 3.0f));
            assert(near(f.io.value(0x60000 + FPWEAPON_EFFECTIVE_RELOAD_PERIOD), fast_reload::kPeriod));
            f.tick(false);
            assert(near(f.io.value(0x60000 + FPWEAPON_EFFECTIVE_RELOAD_PERIOD), 5.0f));
        }

        // Crossbow and snowball have different namespace.
        for (const char* n : { "FPCrossbow", "FPSnowball" }) {
            Fixture f(n, fast_fire::kFirstPerson);
            f.tick();
            assert(f.c.status == viewmodel::Status::Active);
            assert(near(f.io.value(reload_period()), fast_reload::kPeriod));
            f.tick(false);
            assert(near(f.io.value(reload_period()), 3.0f));
        }

        // Spear, melee, tool, torch, chainsaw, building hammer.
        for (const char* n : { "FPSpear", "FPMelee", "FPTool", "FPTorch", "FPChainsaw", "FPBuildingHammer" }) {
            Fixture f(n, "Oxide");
            f.tick();
            assert(f.c.status == viewmodel::Status::Active);
            assert(near(f.io.value(reload_period()), fast_reload::kPeriod));
            f.tick(false);
            assert(near(f.io.value(reload_period()), 3.0f));
        }
    }

    std::puts("fast_reload_memory: all tests passed");
}