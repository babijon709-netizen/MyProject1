#pragma once
// Fast reload: reduces FPWeaponBase.effectiveReloadPeriod (the live cached
// reload duration the animator/StateMachineBehaviour reads) and, for hitscan
// weapons, the loop-reload period.  Same lifecycle as fast_fire — save
// originals, apply, restore on disable/weapon switch.
//
// All FPWeaponBase descendants carry effectiveReloadPeriod at +0xE8.
// Melee tools typically have reloadPeriod == 0 in their prefab, so the
// valid_period gate naturally rejects them without a per-type whitelist.
// FPHitscan additionally stores a loop-reload period at +0x2A0 (used for
// magazine-by-magazine reload like shotgun shells).
#include "weapon_tuning_memory.h"
#include <algorithm>
#include <array>
#include <cstdint>

namespace fast_reload {
using weapon_tuning::Identity;
using weapon_tuning::Life;
using weapon_tuning::pointer;

// Target: as fast as the game accepts without crashing the coroutine.
inline constexpr float kPeriod = 0.03f;

// Two writable slots: the base reload period on every weapon, and the
// hitscan-specific loop-reload period.
inline constexpr size_t kCachedReload = 0;
inline constexpr size_t kLoopReload   = 1;
inline constexpr size_t kSlotCount    = 2;

struct Context {
    Identity player, fp, weapon;
    bool hitscan = false;
    bool operator==(const Context& b) const {
        return player == b.player && fp == b.fp && weapon == b.weapon &&
               hitscan == b.hitscan;
    }
};

struct Saved {
    float original = 0;
    float applied  = 0;
    bool  owned    = false;
};

template<class IO> class Controller {
    IO& io;
    Context context_{};
    std::array<Saved, kSlotCount> saved_{};
    uint64_t session_ = 0, base_ = 0;
    int stage_ = 0;

    bool fail(int stage) { stage_ = stage; return false; }
    static bool valid_period(float v) {
        return viewmodel::finite(v) && v >= 0.f && v <= 60.f;
    }
    static float clamp_period(float v) { return v > kPeriod ? kPeriod : v; }

    // Address of each slot, or 0 if the slot does not apply to this weapon.
    uint64_t address(size_t i) const {
        using namespace go_active;
        if (i == kCachedReload)
            return context_.weapon.object + FPWEAPON_EFFECTIVE_RELOAD_PERIOD;
        if (i == kLoopReload && context_.hitscan)
            return context_.weapon.object + FPHITSCAN_EFFECTIVE_LOOP_RELOAD_PERIOD;
        return 0;
    }
    bool used(size_t i) const { return address(i) != 0; }

    // Resolve the weapon chain.  Accepts any FPWeaponBase descendant that
    // actually has a non-zero reload period.
    bool resolve(uint64_t player, Context& c) {
        using namespace go_active;
        uint64_t net = 0, fp = 0, weapon = 0, back = 0;
        uint8_t local = 0;
        // Player -> NetworkIdentity -> local player.
        if (!io.exact(player, "PlayerManager", "Oxide") ||
            !c.player.capture(io, player, true) ||
            !pointer(io, player + NETWORK_BEHAVIOUR_IDENTITY, net) ||
            !io.exact(net, "NetworkIdentity", "Mirror") ||
            !io.read(net + NETWORK_IDENTITY_IS_LOCAL_PLAYER, local) || local != 1)
            return fail(1);
        // FPManager -> active weapon.
        if (!pointer(io, player + PLAYER_FP_MANAGER, fp) ||
            !io.exact(fp, "FPManager", "Oxide") ||
            !c.fp.capture(io, fp, true))
            return fail(2);
        if (!pointer(io, fp + FPMANAGER_CURRENT_OBJECT, weapon) || !weapon)
            return fail(3);
        // The FPManager chain already guarantees this is a valid FPObject
        // descendant.  The backref confirms it belongs to this player.
        // Unlike fast_fire, no per-type classification is needed — all
        // FPWeaponBase descendants share the reload field at +0xE8.
        if (!c.weapon.capture(io, weapon, true))
            return fail(4);
        if (!pointer(io, weapon + FPOBJECT_PLAYER_BACKREF, back) || back != player)
            return fail(5);
        // Classify: FPHitscan gets the extra loop-reload slot.
        c.hitscan = io.exact(weapon, "FPHitscan", "Oxide");
        return true;
    }

    Life life(size_t /*i*/) {
        return context_.weapon.life(io);
    }

    bool restore_one(size_t i) {
        auto& s = saved_[i];
        if (!s.owned) return true;
        const auto state = life(i);
        if (state == Life::Unreadable) return false;
        if (state == Life::Gone) { s = {}; return true; }
        const uint64_t addr = address(i);
        if (!addr) { s = {}; return true; }
        float now = 0;
        if (!io.read(addr, now)) return false;
        if (now == s.applied) {
            if (!io.write_heap(addr, s.original) ||
                !io.read(addr, now) || now != s.original) return false;
        }
        s = {};
        return true;
    }

    bool restore() {
        bool ok = true;
        for (size_t i = 0; i < kSlotCount; ++i)
            if (!restore_one(i)) ok = false;
        return ok;
    }

    bool apply(size_t i, float target, uint64_t player) {
        const uint64_t addr = address(i);
        if (!addr) return true;
        if (!valid_period(target)) return fail(7);
        float now = 0;
        if (!io.read(addr, now) || !valid_period(now)) return fail(7);
        auto& saved = saved_[i];
        if (now <= target) {
            // Already fast enough — don't own it.
            if (saved.owned && now != saved.applied) saved = {};
            return true;
        }
        // Revalidate before write.
        Context check;
        if (!resolve(player, check) || !(check == context_) || life(i) != Life::Alive)
            return stage_ ? false : fail(8);
        if (!io.read(addr, now) || !valid_period(now)) return fail(7);
        if (now <= target) {
            if (saved.owned && now != saved.applied) saved = {};
            return true;
        }
        saved = { now, target, true };
        if (!io.write_heap(addr, target) ||
            !io.read(addr, now) || now != target) return fail(10);
        return true;
    }

public:
    viewmodel::Status status = viewmodel::Status::Off;
    explicit Controller(IO& memory) : io(memory) {}
    int stage() const { return stage_; }
    bool has_state() const {
        for (const auto& s : saved_) if (s.owned) return true;
        return false;
    }
    bool reset(bool restore_originals) {
        const bool ok = !restore_originals || restore();
        saved_ = {}; context_ = {}; session_ = base_ = 0; stage_ = 0;
        status = viewmodel::Status::Off;
        return ok;
    }
    void tick(uint64_t session, uint64_t base, uint64_t player, bool enabled) {
        using viewmodel::Status;
        if (session_ != session || base_ != base) {
            reset(false); session_ = session; base_ = base;
        }
        stage_ = 0;
        if (!enabled) {
            status = restore() ? Status::Off : Status::Failed;
            if (status == Status::Failed) stage_ = 9;
            return;
        }
        Context fresh;
        if (!resolve(player, fresh)) {
            const int failed_stage = stage_;
            const bool restored = restore();
            if (!restored) stage_ = 9;
            else stage_ = failed_stage;
            status = restored && failed_stage < 6 ? Status::Waiting : Status::Failed;
            return;
        }
        if (!(fresh == context_)) {
            if (!restore()) { stage_ = 9; status = Status::Failed; return; }
            context_ = fresh;
        }
        status = Status::Active;
        for (size_t i = 0; i < kSlotCount; ++i) {
            if (used(i) && !apply(i, clamp_period(kPeriod), player)) {
                status = Status::Failed; return;
            }
        }
    }
};
}