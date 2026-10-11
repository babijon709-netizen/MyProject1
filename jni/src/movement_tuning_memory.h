#pragma once
#include "viewmodel.h"
#include "game_offsets_active.h"
#include "fast_fire_memory.h"
#include <array>
#include <cstdint>

namespace movement_tuning {
using fast_fire::Kind;
using fast_fire::classify;
using weapon_tuning::Identity;
using weapon_tuning::Life;
using weapon_tuning::pointer;

struct Context {
    Identity player, fp, weapon;
    Kind kind = Kind::Unknown;
    bool operator==(const Context& b) const {
        return player == b.player && fp == b.fp && weapon == b.weapon && kind == b.kind;
    }
};

struct SavedBool {
    uint8_t original = 0;
    uint8_t applied = 0;
    bool owned = false;
};

inline bool valid_bool(uint8_t value) { return value <= 1; }

template<class IO> class Controller {
    static constexpr size_t kReloadSprint = 0;
    static constexpr size_t kAimSprint = 1;

    IO& io;
    Context context_{};
    std::array<SavedBool, 2> saved_{};
    uint64_t session_ = 0, base_ = 0;
    int stage_ = 0;

    bool fail(int stage) { stage_ = stage; return false; }

    uint64_t address(size_t index) const {
        using namespace go_active;
        return context_.weapon.object +
               (index == kReloadSprint ? FPWEAPON_CAN_SPRINT_WHILE_RELOADING
                                        : FPWEAPON_IS_AIMING);
    }

    Life life() const { return context_.weapon.life(io); }

    // The event-handler activity remains the visual ADS source while the
    // weapon backing flag is temporarily released for the sprint decision.
    bool read_aim_activity(bool& active) const {
        using namespace go_active;
        uint64_t handler = 0, back = 0, activity = 0;
        uint8_t value = 0;
        if (!pointer(io, context_.player.object + PLAYER_EVENT_HANDLER, handler) || !handler ||
            !pointer(io, handler + EVENT_HANDLER_MANAGER_BACKREF, back) || back != context_.player.object ||
            !pointer(io, handler + EVENT_HANDLER_AIM_ACTIVITY, activity) || !activity ||
            !io.read(activity + ACTIVITY_ACTIVE_FLAG, value) || value > 1)
            return false;
        active = value != 0;
        return true;
    }

    bool restore_one(size_t i) {
        auto& saved = saved_[i];
        if (!saved.owned) return true;
        const Life state = life();
        if (state == Life::Unreadable) return false;
        if (state == Life::Gone) { saved = {}; return true; }

        uint8_t now = 0;
        if (!io.read(address(i), now) || !valid_bool(now)) return false;
        if (i == kAimSprint && saved.original != 0) {
            bool aiming = true;
            if (read_aim_activity(aiming) && !aiming) {
                saved = {};
                return true;
            }
        }
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

    bool resolve(uint64_t player, Context& next) {
        using namespace go_active;
        uint64_t net = 0, fp = 0, weapon = 0, back = 0;
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
        return true;
    }

    bool apply_reload(uint64_t player, bool enabled) {
        if (!enabled) return restore_one(kReloadSprint);
        auto& saved = saved_[kReloadSprint];
        uint8_t now = 0;
        if (!io.read(address(kReloadSprint), now) || !valid_bool(now)) return fail(6);
        if (saved.owned && now != saved.applied) saved = {};
        if (now == 1) return true;

        Context fresh;
        if (!resolve(player, fresh) || !(fresh == context_)) return fail(7);
        if (!io.read(address(kReloadSprint), now) || !valid_bool(now)) return fail(6);
        if (saved.owned && now != saved.applied) saved = {};
        if (!saved.owned) saved.original = now;
        saved.applied = 1;
        saved.owned = true;
        if (!io.write_heap(address(kReloadSprint), uint8_t{1}) ||
            !io.read(address(kReloadSprint), now) || now != 1)
            return fail(8);
        return true;
    }

    bool apply_aim(uint64_t player, bool enabled) {
        if (!enabled) return restore_one(kAimSprint);
        auto& saved = saved_[kAimSprint];
        uint8_t now = 0;
        if (!io.read(address(kAimSprint), now) || !valid_bool(now)) return fail(6);
        if (now == 0) {
            bool aiming = true;
            if (saved.owned && read_aim_activity(aiming) && !aiming) saved = {};
            return true;
        }

        Context fresh;
        if (!resolve(player, fresh) || !(fresh == context_)) return fail(7);
        if (!io.read(address(kAimSprint), now) || !valid_bool(now)) return fail(6);
        if (now == 0) return true;
        if (!saved.owned) saved.original = now;
        saved.applied = 0;
        saved.owned = true;
        if (!io.write_heap(address(kAimSprint), uint8_t{0}) ||
            !io.read(address(kAimSprint), now) || now != 0)
            return fail(8);
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
              bool sprint_while_aiming, bool sprint_while_reloading) {
        using viewmodel::Status;
        const bool enabled = sprint_while_aiming || sprint_while_reloading;
        if (session_ != session || base_ != base) {
            reset(false);
            session_ = session;
            base_ = base;
        }
        stage_ = 0;
        if (!enabled) {
            status = restore() ? Status::Off : Status::Failed;
            return;
        }
        if (!base) {
            status = Status::Waiting;
            return;
        }
        Context fresh;
        if (!resolve(player, fresh)) {
            const int failed_stage = stage_;
            status = restore() ? Status::Waiting : Status::Failed;
            if (status == Status::Waiting) stage_ = failed_stage;
            return;
        }
        if (!(fresh == context_)) {
            if (!restore()) { status = Status::Failed; return; }
            context_ = fresh;
        }
        if (!apply_reload(player, sprint_while_reloading) ||
            !apply_aim(player, sprint_while_aiming)) {
            status = Status::Failed;
            return;
        }
        status = Status::Active;
    }
};
}
