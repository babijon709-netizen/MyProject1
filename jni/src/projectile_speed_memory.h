#pragma once
// Fast physical projectiles.  This touches only the active weapon's verified
// ShaftedProjectile prefab in the managed heap; no code, hooks or RVA writes.
#include "viewmodel.h"
#include "game_offsets_active.h"
#include <cstdint>
#include <initializer_list>

namespace projectile_speed {
// Highest bounded launch speed used by the physical-projectile integrator.
// Keep it finite and fixed rather than multiplying a weapon-specific value.
inline constexpr float kMaxLaunchSpeed = 10000.f;
inline constexpr const char* kShaftedProjectile = "ShaftedProjectile";
inline constexpr const char* kOxide = "Oxide";

template<class IO> bool pointer(IO& io, uint64_t address, uint64_t& value) {
    if (!io.read(address, value)) return false;
    value &= 0x00ffffffffffffffULL;
    return true;
}

enum class Life { Alive, Gone, Unreadable };

struct Identity {
    uint64_t object = 0, klass = 0, native = 0;
    bool unity = false;

    template<class IO> bool capture(IO& io, uint64_t object_, bool unity_) {
        object = object_; unity = unity_;
        return object >= 0x10000 && pointer(io, object, klass) && klass >= 0x10000 &&
            (!unity || (pointer(io, object + go_active::MANAGED_CACHED_PTR, native) && native >= 0x10000));
    }
    template<class IO> Life life(IO& io) const {
        uint64_t value = 0;
        if (!pointer(io, object, value)) return Life::Unreadable;
        if (value != klass) return Life::Gone;
        if (unity) {
            if (!pointer(io, object + go_active::MANAGED_CACHED_PTR, value)) return Life::Unreadable;
            if (value != native) return Life::Gone;
        }
        return Life::Alive;
    }
    bool operator==(const Identity& other) const {
        return object == other.object && klass == other.klass && native == other.native && unity == other.unity;
    }
};

struct Context {
    Identity player, fp, weapon, prefab;
    uint64_t projectile_prefab_offset = 0;
    bool operator==(const Context& other) const {
        return player == other.player && fp == other.fp && weapon == other.weapon &&
               prefab == other.prefab && projectile_prefab_offset == other.projectile_prefab_offset;
    }
};

struct Scalar {
    uint64_t address = 0;
    float original = 0.f, last = 0.f;
    bool owned = false;

    template<class IO> bool restore(IO& io, const Identity& owner) {
        if (!owned) { *this = {}; return true; }
        const Life life = owner.life(io);
        if (life == Life::Unreadable) return false;
        if (life == Life::Gone) { *this = {}; return true; }
        float current = 0.f;
        if (!io.read(address, current)) return false;
        if (!viewmodel::finite(current)) { *this = {}; return true; }
        // Do not overwrite a value replaced by a prefab/scene lifecycle or by
        // another writer after this controller last applied its own value.
        if (current == last && original != last) {
            if (!io.write_heap(address, original)) return false;
            if (!io.read(address, current) || current != original) return false;
        }
        *this = {};
        return true;
    }
};

template<class IO> class Controller {
    IO& io_;
    uint64_t session_ = 0, base_ = 0;
    Context context_{};
    Scalar speed_{};

    bool restore() { return speed_.restore(io_, context_.prefab); }

    bool resolve(uint64_t player, Context& out) {
        uint64_t network = 0, fp = 0, active = 0, weapon = 0, owner = 0;
        uint8_t local = 0;
        if (!player || !io_.named(player, "PlayerManager", "Oxide") ||
            !out.player.capture(io_, player, true) ||
            !pointer(io_, player + go_active::NETWORK_BEHAVIOUR_IDENTITY, network) ||
            !io_.named(network, "NetworkIdentity", "Mirror") ||
            !io_.read(network + go_active::NETWORK_IDENTITY_IS_LOCAL_PLAYER, local) || local != 1 ||
            !pointer(io_, player + go_active::PLAYER_FP_MANAGER, fp) ||
            !io_.named(fp, "FPManager", "Oxide") || !out.fp.capture(io_, fp, true))
            return false;

        // +0x50 is the authoritative slot.  The typed weapon cache is only a
        // compatibility fallback while it is absent, never while a tool/melee
        // object is explicitly equipped.
        const bool read_active = pointer(io_, fp + go_active::FPMANAGER_CURRENT_OBJECT, active);
        if (read_active && active) weapon = active;
        else if (!pointer(io_, fp + go_active::FPMANAGER_CURRENT_WEAPON, weapon) || !weapon) return false;
        if (!out.weapon.capture(io_, weapon, true) ||
            !pointer(io_, weapon + go_active::FPOBJECT_PLAYER_BACKREF, owner) || owner != player)
            return false;

        // All offsets below are from the supplied 11940 dump.  Do not select by
        // a guessed/obfuscated weapon class: only a strict ShaftedProjectile
        // class check permits a write, so unsupported weapons fail closed.
        for (uint64_t offset : {
                 go_active::FPBOW_PROJECTILE_PREFAB,
                 go_active::FPCROSSBOW_PROJECTILE_PREFAB,
                 go_active::FPSNOWBALL_PROJECTILE_PREFAB,
                 go_active::FPROCKET_PROJECTILE_PREFAB,
                 go_active::FPEXPLOSIVE_PROJECTILE_PREFAB,
                 go_active::FPSPEAR_PROJECTILE_PREFAB}) {
            uint64_t prefab = 0;
            if (!pointer(io_, weapon + offset, prefab) || !prefab ||
                !io_.named_strict(prefab, kShaftedProjectile, kOxide) ||
                !out.prefab.capture(io_, prefab, true))
                continue;
            out.projectile_prefab_offset = offset;
            return true;
        }
        return false;
    }

    bool apply() {
        if (context_.weapon.life(io_) != Life::Alive || context_.prefab.life(io_) != Life::Alive)
            return false;
        uint64_t current_prefab = 0;
        if (!pointer(io_, context_.weapon.object + context_.projectile_prefab_offset, current_prefab) ||
            current_prefab != context_.prefab.object)
            return false;

        const uint64_t address = context_.prefab.object + go_active::SHAFTEDPROJECTILE_LAUNCH_SPEED;
        float current = 0.f;
        // Projectiles with a zero/negative speed are not valid launch prefabs;
        // reject rather than turn an arbitrary float into a speed value.
        if (!io_.read(address, current) || !viewmodel::finite(current) ||
            current < 0.01f || current > 5000.f)
            return false;
        if (!speed_.owned || speed_.address != address || current != speed_.last)
            speed_.original = current;
        speed_.address = address;

        const float wanted = kMaxLaunchSpeed;
        speed_.owned = true; // retain restoration information on write failure
        if (current == wanted) { speed_.last = wanted; return true; }
        if (!io_.write_heap(address, wanted)) return false;
        if (!io_.read(address, current) || current != wanted) return false;
        speed_.last = wanted;
        return true;
    }

public:
    viewmodel::Status status = viewmodel::Status::Off;
    explicit Controller(IO& io) : io_(io) {}

    bool has_state() const { return speed_.owned; }
    bool reset(bool restore_original) {
        const bool ok = !restore_original || restore();
        context_ = {}; speed_ = {}; session_ = base_ = 0;
        status = viewmodel::Status::Off;
        return ok;
    }
    void tick(uint64_t session, uint64_t base, uint64_t player, bool enabled) {
        using viewmodel::Status;
        if (session != session_ || base != base_) { reset(false); session_ = session; base_ = base; }
        status = enabled ? Status::Waiting : Status::Off;
        if (!enabled) {
            if (!restore()) status = Status::Failed;
            return;
        }
        if (!base) return;

        Context next;
        if (!resolve(player, next)) {
            // Unequipping to a non-projectile weapon must not leave its old
            // prefab accelerated for the next draw. Restore the still-live
            // previous prefab before reporting that the new item is unsupported.
            if (!restore()) status = Status::Failed;
            else { context_ = {}; status = Status::Waiting; }
            return;
        }
        if (!(next == context_)) {
            if (!restore()) speed_ = {};
            context_ = next;
        }
        status = apply() ? Status::Active : Status::Failed;
    }
};
} // namespace projectile_speed
