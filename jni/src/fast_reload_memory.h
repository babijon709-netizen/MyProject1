#pragma once
// Fast reload: caps the per-instance reload timing fields of the active
// first-person weapon. Evidence from dump11946 / dumpbeta11943 is in
// reversing/NOTES_FAST_RELOAD.md. No code, timers, promises, ammo, animator
// objects or shared HitscanWeaponConfig assets are written.
#include "fast_fire_memory.h"
#include "weapon_tuning_memory.h"
#include <algorithm>
#include <array>
#include <cstdint>

namespace fast_reload {
using weapon_tuning::Identity;
using weapon_tuning::Life;
using weapon_tuning::pointer;
// Same cap as fast fire: the animator rate parameters divide by these periods
// (rate = clipDuration / period), so 30 ms bounds both the full-reload and the
// loop-reload step and stays above the game's own 5 ms validity floor.
inline constexpr float kPeriod = 0.03f;

struct Context {
    Identity player, fp, weapon;
    fast_fire::Kind kind = fast_fire::Kind::Unknown;
    bool operator==(const Context& b) const {
        return player==b.player && fp==b.fp && weapon==b.weapon && kind==b.kind;
    }
};
struct Saved {
    float original = 0;
    float applied = 0;
    bool owned = false;
};
struct Plan {
    std::array<float,3> target{};
    std::array<bool,3> active{};
};

template<class IO> class Controller {
    // 0: FPWeaponBase effective full-reload cache (rVA); recomputed from the
    //    instance source / config on every draw init, so it is restored last.
    // 1: FPWeaponBase instance source reloadPeriod (sry multiplies it by the
    //    weapon-mod factor; the hitscan override reads the config instead).
    // 2: FPHitscan effective loop-reload cache, copied from the shared config.
    static constexpr size_t kEffectiveReload = 0;
    static constexpr size_t kSourceReload = 1;
    static constexpr size_t kLoopReload = 2;

    IO& io;
    Context context_{};
    std::array<Saved,3> saved_{};
    uint64_t session_ = 0, base_ = 0;
    int stage_ = 0;

    bool fail(int stage) { stage_=stage; return false; }
    static bool valid_period(float v) {
        return viewmodel::finite(v) && v >= 0.f && v <= 60.f;
    }
    static float clamp_period(float v) { return v > kPeriod ? kPeriod : v; }

    uint64_t address(size_t i) const {
        using namespace go_active;
        if (i==kEffectiveReload)
            return context_.weapon.object+FPWEAPON_EFFECTIVE_RELOAD_PERIOD;
        if (i==kSourceReload)
            return context_.weapon.object+FPWEAPON_RELOAD_PERIOD;
        if (i==kLoopReload && context_.kind==fast_fire::Kind::Hitscan)
            return context_.weapon.object+FPHITSCAN_EFFECTIVE_LOOP_RELOAD_PERIOD;
        return 0;
    }
    bool used(size_t i) const { return address(i)!=0; }

    bool resolve(uint64_t player, Context& c) {
        using namespace go_active;
        uint64_t net=0, fp=0, weapon=0, back=0;
        uint8_t local=0;
        if (!io.exact(player,"PlayerManager","Oxide") || !c.player.capture(io,player,true) ||
            !pointer(io,player+NETWORK_BEHAVIOUR_IDENTITY,net) ||
            !io.exact(net,"NetworkIdentity","Mirror") ||
            !io.read(net+NETWORK_IDENTITY_IS_LOCAL_PLAYER,local) || local!=1) return fail(1);
        if (!pointer(io,player+PLAYER_FP_MANAGER,fp) || !io.exact(fp,"FPManager","Oxide") ||
            !c.fp.capture(io,fp,true)) return fail(2);
        // Never fall back to the stale typed slot after a failed/null active read.
        if (!pointer(io,fp+FPMANAGER_CURRENT_OBJECT,weapon) || !weapon) return fail(3);
        c.kind=fast_fire::classify(io,weapon);
        if (c.kind==fast_fire::Kind::Unknown) return fail(4);
        if (!c.weapon.capture(io,weapon,true) ||
            !pointer(io,weapon+FPOBJECT_PLAYER_BACKREF,back) || back!=player) return fail(5);
        return true;
    }

    Life life(size_t i) { (void)i; return context_.weapon.life(io); }

    bool build_plan(Plan& p) {
        for (size_t i=0;i<p.target.size();++i) {
            if (!used(i)) continue;
            float now=0;
            if (!io.read(address(i),now) || !valid_period(now)) return fail(7);
            // Only ever shorten: a value already at/below the cap keeps its
            // ownership released so restore never clobbers a game update.
            p.active[i]=true;
            p.target[i]=clamp_period(now);
        }
        return true;
    }

    bool restore_one(size_t i) {
        auto& s=saved_[i];
        if (!s.owned) return true;
        const Life state=life(i);
        if (state==Life::Unreadable) return false;
        if (state==Life::Gone) { s={}; return true; }
        float now=0;
        if (!io.read(address(i),now)) return false;
        // Never overwrite another writer's update or a destroyed object.
        if (now==s.applied) {
            if (!io.write_heap(address(i),s.original) ||
                !io.read(address(i),now) || now!=s.original) return false;
        }
        s={};
        return true;
    }
    bool restore() {
        bool ok=true;
        // Restore source fields first, the caches they feed are last.
        for (size_t i=1;i<saved_.size();++i) if (!restore_one(i)) ok=false;
        if (!restore_one(kEffectiveReload)) ok=false;
        return ok;
    }

    bool apply(size_t i, float target, uint64_t player) {
        if (!used(i)) return true;
        if (!valid_period(target)) return fail(7);
        float now=0;
        if (!io.read(address(i),now) || !valid_period(now)) return fail(7);
        auto& saved=saved_[i];
        if (now<=target) {
            if (saved.owned && now!=saved.applied) saved={};
            return true;
        }
        Context check;
        if (!resolve(player,check) || !(check==context_) || life(i)!=Life::Alive)
            return stage_ ? false : fail(8);
        if (!io.read(address(i),now) || !valid_period(now)) return fail(7);
        if (now<=target) {
            if (saved.owned && now!=saved.applied) saved={};
            return true;
        }
        saved={now,target,true};
        if (!io.write_heap(address(i),target) ||
            !io.read(address(i),now) || now!=target) return fail(10);
        return true;
    }
public:
    viewmodel::Status status = viewmodel::Status::Off;
    explicit Controller(IO& memory):io(memory) {}
    int stage() const { return stage_; }
    bool has_state() const {
        for (const auto& s:saved_) if (s.owned) return true;
        return false;
    }
    bool reset(bool restore_originals) {
        const bool ok=!restore_originals || restore();
        saved_={}; context_={}; session_=base_=0; stage_=0;
        status=viewmodel::Status::Off;
        return ok;
    }
    void tick(uint64_t session, uint64_t base, uint64_t player, bool enabled) {
        using viewmodel::Status;
        if (session_!=session || base_!=base) {
            reset(false); session_=session; base_=base;
        }
        stage_=0;
        if (!enabled) {
            status=restore()?Status::Off:Status::Failed;
            if (status==Status::Failed) stage_=9;
            return;
        }
        Context fresh;
        if (!resolve(player,fresh)) {
            // Restore old live objects even when the new equip chain is absent.
            const int failed_stage=stage_;
            const bool restored=restore();
            if (!restored) stage_=9;
            else stage_=failed_stage;
            status=restored && failed_stage<6 ? Status::Waiting : Status::Failed;
            return;
        }
        if (!(fresh==context_)) {
            if (!restore()) { stage_=9; status=Status::Failed; return; }
            context_=fresh;
        }
        Plan plan;
        if (!build_plan(plan)) {
            const int failed_stage=stage_;
            if (!restore()) stage_=9;
            else stage_=failed_stage;
            status=Status::Failed;
            return;
        }
        status=Status::Active;
        // Write source values first. The runtime caches are last because the
        // draw init copies initializer fields into them.
        for (size_t i=1;i<plan.target.size();++i) {
            if (plan.active[i] && !apply(i,plan.target[i],player)) {
                status=Status::Failed; return;
            }
        }
        if (plan.active[kEffectiveReload] &&
            !apply(kEffectiveReload,plan.target[kEffectiveReload],player))
            status=Status::Failed;
    }
};
}
