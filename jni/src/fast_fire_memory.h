#pragma once
#include "weapon_tuning_memory.h"
#include <algorithm>
#include <array>
#include <cstdint>

namespace fast_fire {
using weapon_tuning::Identity;
using weapon_tuning::Life;
using weapon_tuning::pointer;
inline constexpr float kPeriod = 0.03f;
inline constexpr uint32_t kNoFireMode = UINT32_MAX;
inline constexpr uint32_t kSemiAuto = 0;
inline constexpr uint32_t kBurst = 1;
inline constexpr uint32_t kFullAuto = 2;
inline constexpr uint32_t kMaxBurstLength = 64;
inline constexpr const char* kFirstPerson =
    "HyperHug.Games.Oxide.By_Namespace.Oxide.Player.First_Person";

enum class Kind {
    Unknown, Hitscan, Bow, Crossbow, RocketLauncher, ExplosiveCharge,
    Snowball, Spear, Melee
};

template<class IO> Kind classify(IO& io, uint64_t object) {
    if (io.exact(object, "FPHitscan", "Oxide")) return Kind::Hitscan;
    if (io.exact(object, "FPSpear", "Oxide")) return Kind::Spear;
    if (io.exact(object, "FPBow", "Oxide")) return Kind::Bow;
    if (io.exact(object, "FPCrossbow", kFirstPerson)) return Kind::Crossbow;
    if (io.exact(object, "FPRocketLauncher", "Oxide")) return Kind::RocketLauncher;
    if (io.exact(object, "FPExplosiveCharge", "Oxide")) return Kind::ExplosiveCharge;
    if (io.exact(object, "FPSnowball", kFirstPerson)) return Kind::Snowball;
    for (const char* name : {"FPMelee", "FPTool", "FPTorch", "FPChainsaw", "FPBuildingHammer"})
        if (io.exact(object, name, "Oxide")) return Kind::Melee;
    return Kind::Unknown;
}

struct Context {
    Identity player, fp, weapon, wait;
    Kind kind = Kind::Unknown;
    uint32_t fire_mode = kNoFireMode;
    uint32_t burst_length = 0;
    bool operator==(const Context& b) const {
        return player==b.player && fp==b.fp && weapon==b.weapon && wait==b.wait &&
            kind==b.kind && fire_mode==b.fire_mode && burst_length==b.burst_length;
    }
};
struct Saved {
    float original = 0;
    float applied = 0;
    bool owned = false;
};
struct Plan {
    std::array<float,5> target{};
    std::array<bool,5> active{};
};

template<class IO> class Controller {
    static constexpr size_t kCachedPeriod = 0;
    static constexpr size_t kSourcePeriod = 1;
    static constexpr size_t kSecondarySource = 2;
    static constexpr size_t kSecondaryCache = 3;
    static constexpr size_t kBurstWaitPeriod = 4;

    IO& io;
    Context context_{};
    std::array<Saved,5> saved_{};
    uint64_t session_ = 0, base_ = 0;
    int stage_ = 0;

    bool fail(int stage) { stage_=stage; return false; }
    static bool valid_period(float v) {
        return viewmodel::finite(v) && v >= 0.f && v <= 60.f;
    }
    static float clamp_period(float v) { return v > kPeriod ? kPeriod : v; }

    uint64_t source_period_offset() const {
        using namespace go_active;
        switch (context_.kind) {
            case Kind::Hitscan:
                if (context_.fire_mode==kSemiAuto) return FPHITSCAN_FIRE_DURATION;
                if (context_.fire_mode==kBurst) return FPHITSCAN_BURST_DURATION;
                return 0; // Full-auto derives its source rate from a shared config asset.
            case Kind::Bow: return FPBOW_MIN_SHOT_PERIOD;
            case Kind::Crossbow: return FPCROSSBOW_MIN_SHOT_PERIOD;
            case Kind::RocketLauncher: return FPROCKETLAUNCHER_MIN_SHOT_PERIOD;
            case Kind::ExplosiveCharge: return FPEXPLOSIVECHARGE_MIN_THROW_PERIOD;
            case Kind::Snowball: return FPSNOWBALL_MIN_THROW_PERIOD;
            case Kind::Spear:
            case Kind::Melee: return FPMELEE_TIME_BETWEEN_ATTACKS;
            default: return 0;
        }
    }

    uint64_t address(size_t i) const {
        using namespace go_active;
        if (i==kCachedPeriod) return context_.weapon.object+FPWEAPON_SHOT_PERIOD;
        if (i==kSourcePeriod) {
            const auto offset=source_period_offset();
            return offset ? context_.weapon.object+offset : 0;
        }
        if (i==kSecondarySource) {
            if (context_.kind==Kind::Hitscan && context_.fire_mode==kBurst)
                return context_.weapon.object+FPHITSCAN_BURST_PAUSE;
            if (context_.kind==Kind::Spear)
                return context_.weapon.object+FPSPEAR_MIN_THROW_PERIOD;
            return 0;
        }
        if (i==kSecondaryCache && context_.kind==Kind::Spear)
            return context_.weapon.object+FPSPEAR_CACHED_THROW_PERIOD;
        if (i==kBurstWaitPeriod && context_.kind==Kind::Hitscan &&
            context_.fire_mode==kBurst && context_.wait.object)
            return context_.wait.object+WAITFORSECONDS_SECONDS;
        return 0;
    }
    bool used(size_t i) const { return address(i)!=0; }

    bool resolve(uint64_t player, Context& c) {
        using namespace go_active;
        uint64_t net=0, fp=0, weapon=0, back=0, wait=0;
        uint8_t local=0;
        if (!io.exact(player,"PlayerManager","Oxide") || !c.player.capture(io,player,true) ||
            !pointer(io,player+NETWORK_BEHAVIOUR_IDENTITY,net) ||
            !io.exact(net,"NetworkIdentity","Mirror") ||
            !io.read(net+NETWORK_IDENTITY_IS_LOCAL_PLAYER,local) || local!=1) return fail(1);
        if (!pointer(io,player+PLAYER_FP_MANAGER,fp) || !io.exact(fp,"FPManager","Oxide") ||
            !c.fp.capture(io,fp,true)) return fail(2);
        // Never fall back to the stale typed slot after a failed/null active read.
        if (!pointer(io,fp+FPMANAGER_CURRENT_OBJECT,weapon) || !weapon) return fail(3);
        c.kind=classify(io,weapon);
        if (c.kind==Kind::Unknown) return fail(4);
        if (!c.weapon.capture(io,weapon,true) ||
            !pointer(io,weapon+FPOBJECT_PLAYER_BACKREF,back) || back!=player) return fail(5);
        if (c.kind==Kind::Hitscan) {
            if (!io.read(weapon+FPHITSCAN_FIRE_MODE,c.fire_mode) || c.fire_mode>kFullAuto)
                return fail(6);
            if (c.fire_mode==kBurst) {
                int32_t length=0;
                if (!io.read(weapon+FPHITSCAN_BURST_LENGTH,length) ||
                    length<1 || static_cast<uint32_t>(length)>kMaxBurstLength ||
                    !pointer(io,weapon+FPHITSCAN_BURST_WAIT,wait) || !wait ||
                    !io.exact(wait,"WaitForSeconds","UnityEngine") ||
                    !c.wait.capture(io,wait,false)) return fail(6);
                c.burst_length=static_cast<uint32_t>(length);
            }
        }
        return true;
    }

    Life life(size_t i) {
        const auto state=context_.weapon.life(io);
        if (state!=Life::Alive || i!=kBurstWaitPeriod) return state;
        uint64_t wait=0;
        if (!pointer(io,context_.weapon.object+go_active::FPHITSCAN_BURST_WAIT,wait))
            return Life::Unreadable;
        if (wait!=context_.wait.object) return Life::Gone;
        return context_.wait.life(io);
    }

    bool build_plan(Plan& p, bool bow_no_delay) {
        const uint64_t main_address=address(kCachedPeriod);
        float main_period=0;
        if (!io.read(main_address,main_period) || !valid_period(main_period)) return fail(7);
        const bool zero_bow = bow_no_delay && context_.kind == Kind::Bow;
        p.active[kCachedPeriod]=true;
        p.target[kCachedPeriod]=zero_bow ? 0.f : clamp_period(main_period);

        const uint64_t source=source_period_offset();
        if (source) {
            float current=0;
            if (!io.read(context_.weapon.object+source,current) || !valid_period(current))
                return fail(7);
            p.active[kSourcePeriod]=true;
            p.target[kSourcePeriod]=zero_bow ? 0.f : clamp_period(current);
        }

        if (context_.kind==Kind::Spear) {
            float throw_source=0, throw_cache=0;
            if (!io.read(context_.weapon.object+go_active::FPSPEAR_MIN_THROW_PERIOD,throw_source) ||
                !valid_period(throw_source) ||
                !io.read(context_.weapon.object+go_active::FPSPEAR_CACHED_THROW_PERIOD,throw_cache) ||
                !valid_period(throw_cache)) return fail(7);
            p.active[kSecondarySource]=p.active[kSecondaryCache]=true;
            p.target[kSecondarySource]=clamp_period(throw_source);
            p.target[kSecondaryCache]=clamp_period(throw_cache);
        }

        if (context_.kind==Kind::Hitscan && context_.fire_mode==kBurst) {
            float burst_duration=0, burst_pause=0, wait_seconds=0;
            const uint64_t wait_address=address(kBurstWaitPeriod);
            if (!io.read(context_.weapon.object+go_active::FPHITSCAN_BURST_DURATION,burst_duration) ||
                !valid_period(burst_duration) ||
                !io.read(context_.weapon.object+go_active::FPHITSCAN_BURST_PAUSE,burst_pause) ||
                !valid_period(burst_pause) || !wait_address ||
                !io.read(wait_address,wait_seconds) || !valid_period(wait_seconds)) return fail(7);

            const float length=static_cast<float>(context_.burst_length);
            const float duration_per_shot=burst_duration/length;
            if (!viewmodel::finite(duration_per_shot) || duration_per_shot<0.f || duration_per_shot>60.f)
                return fail(7);
            // The source builds the shared wait from burstDuration / burstLength.
            // Keep both representations consistent, and never lengthen a shorter value.
            const float new_wait=std::min(kPeriod,std::min(wait_seconds,duration_per_shot));
            const float new_duration=std::min(burst_duration,new_wait*length);
            const float new_pause=std::min(burst_pause,kPeriod);
            const float required_cycle=new_wait*length+new_pause;
            if (!viewmodel::finite(new_duration) || !viewmodel::finite(required_cycle) ||
                required_cycle<0.f || required_cycle>60.f) return fail(7);
            // A shorter cached cycle than the complete burst is already present
            // in the game state. Do not accelerate its coroutine into overlap.
            if (main_period+0.00001f<required_cycle) return fail(11);

            p.target[kCachedPeriod]=std::min(main_period,std::max(kPeriod,required_cycle));
            p.active[kSourcePeriod]=true;
            p.target[kSourcePeriod]=new_duration;
            p.active[kSecondarySource]=true;
            p.target[kSecondarySource]=new_pause;
            p.active[kBurstWaitPeriod]=true;
            p.target[kBurstWaitPeriod]=new_wait;
        }
        return true;
    }

    bool restore_one(size_t i) {
        auto& s=saved_[i];
        if (!s.owned) return true;
        const auto state=life(i);
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
        // Restore initializer/source values first, then the runtime cache they feed.
        for (size_t i=1;i<saved_.size();++i) if (!restore_one(i)) ok=false;
        if (!restore_one(kCachedPeriod)) ok=false;
        return ok;
    }

    bool apply(size_t i, float target, uint64_t player) {
        if (!used(i)) return true;
        if (!valid_period(target)) return fail(7);
        float now=0;
        if (!io.read(address(i),now) || !valid_period(now)) return fail(7);
        auto& saved=saved_[i];
        if (saved.owned && now!=saved.applied) saved={};
        if (saved.owned && now==saved.applied && now<=target) return true;
        if (!saved.owned && now<=target) return true;

        Context check;
        if (!resolve(player,check) || !(check==context_) || life(i)!=Life::Alive)
            return stage_ ? false : fail(8);
        if (!io.read(address(i),now) || !valid_period(now)) return fail(7);
        if (saved.owned && now!=saved.applied) saved={};
        if (saved.owned && now==saved.applied && now<=target) return true;
        if (!saved.owned && now<=target) return true;
        if (!saved.owned) saved={now,target,true};
        else saved.applied=target;
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
    void tick(uint64_t session, uint64_t base, uint64_t player, bool enabled,
              bool bow_no_delay = false, bool fast_fire_enabled = true) {
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
        if (bow_no_delay && !fast_fire_enabled && context_.kind != Kind::Bow) {
            status = restore() ? Status::Waiting : Status::Failed;
            return;
        }
        Plan plan;
        if (!build_plan(plan, bow_no_delay)) {
            const int failed_stage=stage_;
            if (!restore()) stage_=9;
            else stage_=failed_stage;
            status=Status::Failed;
            return;
        }
        status=Status::Active;
        // Write source values first. The runtime cache is last because On_Draw
        // copies these initializer fields into that cache.
        for (size_t i=1;i<plan.target.size();++i) {
            if (plan.active[i] && !apply(i,plan.target[i],player)) {
                status=Status::Failed; return;
            }
        }
        if (plan.active[kCachedPeriod] && !apply(kCachedPeriod,plan.target[kCachedPeriod],player))
            status=Status::Failed;
    }
};
}
