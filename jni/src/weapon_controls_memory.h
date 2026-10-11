#pragma once
#include "viewmodel.h"
#include "game_offsets_active.h"
#include "fast_fire_memory.h"
#include <array>
#include <cstdint>

namespace weapon_controls {
using fast_fire::Kind;
using fast_fire::classify;
using weapon_tuning::Identity;
using weapon_tuning::Life;
using weapon_tuning::pointer;

inline uint64_t projectile_offset(Kind kind) {
    using namespace go_active;
    switch (kind) {
        case Kind::Bow:             return FPBOW_PROJECTILE_PREFAB;
        case Kind::Crossbow:        return FPCROSSBOW_PROJECTILE_PREFAB;
        case Kind::RocketLauncher:  return FPROCKETLAUNCHER_PROJECTILE_PREFAB;
        case Kind::ExplosiveCharge: return FPEXPLOSIVECHARGE_PROJECTILE_PREFAB;
        case Kind::Snowball:        return FPSNOWBALL_PROJECTILE_PREFAB;
        case Kind::Spear:           return FPSPEAR_PROJECTILE_PREFAB;
        default:                    return 0;
    }
}

inline bool has_projectile(Kind kind) { return projectile_offset(kind) != 0; }

struct Context {
    Identity player, fp, weapon, projectile;
    Kind kind = Kind::Unknown;
    bool operator==(const Context& b) const {
        return player == b.player && fp == b.fp && weapon == b.weapon &&
               projectile == b.projectile && kind == b.kind;
    }
};

struct SavedScalar {
    float original = 0.f;
    float applied = 0.f;
    bool owned = false;
};

inline bool valid_scalar(float value) {
    return viewmodel::finite(value) && value >= 0.f && value <= 100000.f;
}
inline bool valid_multiplier(float value) {
    return viewmodel::finite(value) && value >= 0.1f && value <= 10.f;
}

template<class IO> class Controller {
    static constexpr size_t kLaunchSpeed = 0;
    static constexpr size_t kAimSpeed = 1;
    static constexpr size_t kAimPower = 2;

    IO& io;
    Context context_{};
    std::array<SavedScalar, 3> saved_{};
    uint64_t session_ = 0, base_ = 0;
    int stage_ = 0;

    bool fail(int stage) { stage_ = stage; return false; }

    bool active(size_t index, const ViewmodelSettings& settings) const {
        if (index == kLaunchSpeed) return settings.projectile_speed_enabled && context_.projectile.object;
        if (context_.kind != Kind::Bow) return false;
        return index == kAimSpeed ? settings.bow_draw_speed_enabled
                                  : settings.bow_draw_power_enabled;
    }

    uint64_t address(size_t index) const {
        using namespace go_active;
        if (index == kLaunchSpeed)
            return context_.projectile.object + SHAFTEDPROJECTILE_LAUNCH_SPEED;
        return context_.weapon.object + (index == kAimSpeed ? FPBOW_AIM_SPEED : FPBOW_AIM_POWER);
    }

    float multiplier(size_t index, const ViewmodelSettings& settings) const {
        if (index == kLaunchSpeed) return settings.projectile_speed_multiplier;
        return index == kAimSpeed ? settings.bow_draw_speed_multiplier
                                   : settings.bow_draw_power_multiplier;
    }

    bool restore_one(size_t i) {
        auto& saved = saved_[i];
        if (!saved.owned) return true;
        const Identity& owner = i == kLaunchSpeed ? context_.projectile : context_.weapon;
        const Life state = owner.life(io);
        if (state == Life::Unreadable) return false;
        if (state == Life::Gone) { saved = {}; return true; }
        float now = 0.f;
        if (!io.read(address(i), now) || !valid_scalar(now)) return false;
        // Restore only the value still owned by this controller.
        if (now == saved.applied &&
            (!io.write_heap(address(i), saved.original) ||
             !io.read(address(i), now) || now != saved.original))
            return false;
        saved = {};
        return true;
    }

    bool restore() {
        bool ok = true;
        for (size_t i = 0; i < saved_.size(); ++i)
            if (!restore_one(i)) ok = false;
        return ok;
    }

    bool resolve(uint64_t player, const ViewmodelSettings& settings, Context& next) {
        using namespace go_active;
        uint64_t net = 0, fp = 0, weapon = 0, back = 0, projectile = 0;
        uint8_t local = 0;
        next = {};
        if (!player || !io.exact(player, "PlayerManager", "Oxide") ||
            !next.player.capture(io, player, true) ||
            !pointer(io, player + NETWORK_BEHAVIOUR_IDENTITY, net) ||
            !io.exact(net, "NetworkIdentity", "Mirror") ||
            !io.read(net + NETWORK_IDENTITY_IS_LOCAL_PLAYER, local) || local != 1)
            return fail(1);
        if (!pointer(io, player + PLAYER_FP_MANAGER, fp) ||
            !io.exact(fp, "FPManager", "Oxide") || !next.fp.capture(io, fp, true))
            return fail(2);
        if (!pointer(io, fp + FPMANAGER_CURRENT_OBJECT, weapon) || !weapon)
            return fail(3);
        next.kind = classify(io, weapon);
        if (next.kind == Kind::Unknown) return fail(4);
        if (!next.weapon.capture(io, weapon, true) ||
            !pointer(io, weapon + FPOBJECT_PLAYER_BACKREF, back) || back != player)
            return fail(5);

        if (settings.projectile_speed_enabled && has_projectile(next.kind)) {
            const uint64_t field = projectile_offset(next.kind);
            if (!pointer(io, weapon + field, projectile) || !projectile)
                return fail(6);
            if (!io.exact(projectile, "ShaftedProjectile", "Oxide") ||
                !next.projectile.capture(io, projectile, true))
                return fail(6);
        }
        return true;
    }

    bool apply_one(size_t index, const ViewmodelSettings& settings, uint64_t player) {
        if (!active(index, settings)) return true;
        const float factor = multiplier(index, settings);
        if (!valid_multiplier(factor)) return fail(7);
        auto& saved = saved_[index];
        float now = 0.f;
        if (!io.read(address(index), now) || !valid_scalar(now)) return fail(8);
        if (saved.owned && now != saved.applied) saved = {};
        const float original = saved.owned ? saved.original : now;
        const float target = original * factor;
        if (!viewmodel::finite(target) || target < 0.f || target > 100000.f) return fail(8);
        if (now == target) {
            if (saved.owned) saved.applied = now;
            return true;
        }

        Context fresh;
        if (!resolve(player, settings, fresh) || !(fresh == context_)) return fail(9);
        if (!io.read(address(index), now) || !valid_scalar(now)) return fail(8);
        if (saved.owned && now != saved.applied) {
            saved = {};
            const float retry_target = now * factor;
            if (!viewmodel::finite(retry_target) || retry_target < 0.f || retry_target > 100000.f)
                return fail(8);
            if (now == retry_target) return true;
            saved.original = now;
            saved.owned = true;
            saved.applied = retry_target;
        } else {
            if (!saved.owned) saved.original = now;
            saved.applied = target;
            saved.owned = true;
        }
        const float write_value = saved.applied;
        if (!io.write_heap(address(index), write_value) ||
            !io.read(address(index), now) || now != write_value)
            return fail(10);
        return true;
    }

public:
    viewmodel::Status status = viewmodel::Status::Off;
    explicit Controller(IO& memory) : io(memory) {}

    int stage() const { return stage_; }
    bool has_state() const {
        for (const auto& saved : saved_) if (saved.owned) return true;
        return false;
    }

    bool reset(bool restore_originals) {
        const bool ok = !restore_originals || restore();
        saved_ = {};
        context_ = {};
        session_ = base_ = 0;
        stage_ = 0;
        status = viewmodel::Status::Off;
        return ok;
    }

    void tick(uint64_t session, uint64_t base, uint64_t player,
              const ViewmodelSettings& settings) {
        using viewmodel::Status;
        const bool requested = settings.projectile_speed_enabled ||
                               (settings.bow_draw_speed_enabled && settings.bow_draw_speed_multiplier != 1.f) ||
                               (settings.bow_draw_power_enabled && settings.bow_draw_power_multiplier != 1.f);
        if (session_ != session || base_ != base) {
            reset(false);
            session_ = session;
            base_ = base;
        }
        stage_ = 0;
        if (!requested) {
            status = restore() ? Status::Off : Status::Failed;
            return;
        }
        if (!base) {
            status = Status::Waiting;
            return;
        }
        Context fresh;
        if (!resolve(player, settings, fresh)) {
            const int failed_stage = stage_;
            status = restore() ? Status::Waiting : Status::Failed;
            if (status == Status::Waiting) stage_ = failed_stage;
            return;
        }
        if (!(fresh == context_)) {
            if (!restore()) { status = Status::Failed; return; }
            context_ = fresh;
        }
        bool any_target = false;
        for (size_t i = 0; i < saved_.size(); ++i) {
            if (!active(i, settings)) {
                if (!restore_one(i)) { status = Status::Failed; return; }
                continue;
            }
            any_target = true;
            if (!apply_one(i, settings, player)) {
                status = Status::Failed;
                return;
            }
        }
        status = any_target ? Status::Active : Status::Waiting;
    }
};
}
