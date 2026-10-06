#pragma once
// Быстрая стрельба и перезарядка для уже созданного FPHitscan.
//
// Одних полей HitscanWeaponConfig недостаточно: FPHitscan.lMf/sPo копирует
// интервалы в живой FPWeaponBase/FPHitscan ещё при создании/доставании оружия.
// Поэтому контроллер синхронно держит и config, и подтверждённые ARM64-аудитом
// live-кэши. Инструкции/данные библиотек не меняются, а все адресные цепочки
// берутся только из выбранного go_active профиля Release/Beta.
#include "viewmodel.h"
#include "game_offsets_active.h"
#include <cstdint>

namespace weapon_speed {
inline constexpr const char* kWeapons = "HyperHug.Games.Oxide.Features.Weapons";
// Extreme 20x timing outruns the authoritative reload/fire state machine: the
// client restarts the animation before the server accepts the previous action.
// A bounded multiplier remains visibly fast without creating that feedback loop.
inline constexpr float kFireMultiplier = 5.f;
inline constexpr float kReloadMultiplier = 5.f;

template<class IO> bool pointer(IO& io, uint64_t address, uint64_t& value) {
    if (!io.read(address,value)) return false;
    value &= 0x00ffffffffffffffULL;
    return true;
}

enum class Life { Alive, Gone, Unreadable };

struct Identity {
    uint64_t object=0, klass=0, native=0;
    bool unity=false;

    template<class IO> bool capture(IO& io,uint64_t obj,bool is_unity) {
        object=obj; unity=is_unity;
        return obj>=0x10000 && pointer(io,obj,klass) && klass>=0x10000 &&
            (!unity || (pointer(io,obj+go_active::MANAGED_CACHED_PTR,native) && native>=0x10000));
    }
    template<class IO> Life life(IO& io) const {
        uint64_t k=0,n=0;
        if (!pointer(io,object,k)) return Life::Unreadable;
        if (k!=klass) return Life::Gone;
        if (unity) {
            if (!pointer(io,object+go_active::MANAGED_CACHED_PTR,n)) return Life::Unreadable;
            if (n!=native) return Life::Gone;
        }
        return Life::Alive;
    }
    bool operator==(const Identity& other) const {
        return object==other.object && klass==other.klass && native==other.native && unity==other.unity;
    }
};

struct Context {
    Identity player,fp,weapon,config,burst_wait;
    bool operator==(const Context& other) const {
        // WaitForSeconds is a lazily-created cache: it can be null before the
        // first shot and appear afterwards. It is not weapon identity. Including
        // it here made the controller tear down and rebuild every time the
        // burst coroutine created/replaced the wait object, which caused the
        // intermittent fast-fire/reload behaviour.
        return player==other.player && fp==other.fp && weapon==other.weapon &&
            config==other.config;
    }
};

inline bool finite_range(float value,float lo,float hi) {
    return viewmodel::finite(value) && value>=lo && value<=hi;
}

struct Scalar {
    uint64_t address=0;
    float original=0.f,last=0.f,current=0.f;
    bool owned=false;

    template<class IO> bool sample(IO& io,uint64_t addr,float lo,float hi) {
        float value=0.f;
        if (!io.read(addr,value) || !finite_range(value,lo,hi)) return false;
        // Capture an original once per field identity. The game may restore a
        // config/cache between actions; adopting that transient value on every
        // mismatch compounds the multiplier and causes permanent animation
        // loops. Keep the lifecycle original and simply re-apply the target.
        if (!owned || address!=addr) original=value;
        address=addr; current=value;
        return true;
    }
    template<class IO> bool put(IO& io,float value) {
        if (!address || !viewmodel::finite(value)) return false;
        if (current==value) { last=value; owned=true; return true; }
        // Preserve ownership even on failure: reset can retry restoration of
        // any earlier writes from this same frame.
        owned=true;
        if (!io.write_heap(address,value)) return false;
        current=last=value;
        float check=0.f;
        if (!io.read(address,check) || check!=value) return false;
        return true;
    }
    template<class IO> bool normalize_copy(IO& io,uint64_t addr) const {
        if (!owned || !addr) return true;
        float value=0.f;
        if (!io.read(addr,value) || !viewmodel::finite(value)) return false;
        // A newly constructed weapon can copy our accelerated config/cache
        // before the next frame.  Only undo an exact copy of our last value;
        // an already-existing holstered weapon keeps its independent original.
        if (value==last && original!=last) {
            if (!io.write_heap(addr,original)) return false;
            if (!io.read(addr,value) || value!=original) return false;
        }
        return true;
    }
    template<class IO> bool restore(IO& io,const Identity& owner) {
        if (!owned) { *this={}; return true; }
        const Life life=owner.life(io);
        if (life==Life::Unreadable) return false;
        if (life==Life::Gone) { *this={}; return true; }
        float value=0.f;
        if (!io.read(address,value)) return false;
        if (!viewmodel::finite(value)) { *this={}; return true; }
        // Conditional restore: never overwrite a value subsequently changed
        // by the game or another lifecycle owner.
        if (value==last && original!=last) {
            if (!io.write_heap(address,original)) return false;
            if (!io.read(address,value) || value!=original) return false;
        }
        *this={};
        return true;
    }
};

template<class IO> class Controller {
    IO& io_;
    uint64_t session_=0,base_=0;
    Context context_{};

    // Config source values.
    Scalar config_fire_{},config_full_{},config_loop_{};
    // Values copied by FPHitscan.lMf/sPo into an already-live weapon.
    Scalar shot_period_{},effective_full_{},effective_loop_{},burst_seconds_{};

    bool restore_fire() {
        const bool a=config_fire_.restore(io_,context_.config);
        const bool b=shot_period_.restore(io_,context_.weapon);
        const bool c=burst_seconds_.restore(io_,context_.burst_wait);
        return a && b && c;
    }
    bool restore_reload() {
        const bool a=config_full_.restore(io_,context_.config);
        const bool b=config_loop_.restore(io_,context_.config);
        const bool c=effective_full_.restore(io_,context_.weapon);
        const bool d=effective_loop_.restore(io_,context_.weapon);
        return a && b && c && d;
    }
    bool restore_all() {
        const bool a=restore_fire();
        const bool b=restore_reload();
        return a && b;
    }
    void forget_values() {
        // A respawn destroys the old managed/native weapon graph.  Reads from
        // those addresses can then remain Unreadable forever, so restoration
        // must not pin the controller to the pre-death context.  This is only
        // used after a complete, independently validated replacement context
        // has already been resolved.
        config_fire_={}; config_full_={}; config_loop_={};
        shot_period_={}; effective_full_={}; effective_loop_={}; burst_seconds_={};
    }
    bool normalize_inherited_caches(const Context& next) {
        // Only migrate cached values when the config is shared but the live
        // weapon instance changed. This prevents a newly drawn instance from
        // inheriting an already accelerated cache and being scaled twice.
        if (!(context_.config==next.config) || context_.weapon==next.weapon) return true;
        const bool a=shot_period_.normalize_copy(
            io_,next.weapon.object+go_active::FPWEAPON_SHOT_PERIOD);
        const bool b=effective_full_.normalize_copy(
            io_,next.weapon.object+go_active::FPWEAPON_EFFECTIVE_RELOAD_PERIOD);
        const bool c=effective_loop_.normalize_copy(
            io_,next.weapon.object+go_active::FPHITSCAN_EFFECTIVE_LOOP_RELOAD_PERIOD);
        return a && b && c;
    }

    bool resolve(uint64_t player,Context& c) {
        uint64_t net=0,fp=0,weapon=0,back=0,config=0;
        uint8_t local=0;
        if (!player || !io_.named(player,"PlayerManager","Oxide") ||
            !c.player.capture(io_,player,true) ||
            !pointer(io_,player+go_active::NETWORK_BEHAVIOUR_IDENTITY,net) ||
            !io_.named(net,"NetworkIdentity","Mirror") ||
            !io_.read(net+go_active::NETWORK_IDENTITY_IS_LOCAL_PLAYER,local) || local!=1 ||
            !pointer(io_,player+go_active::PLAYER_FP_MANAGER,fp) ||
            !io_.named(fp,"FPManager","Oxide") || !c.fp.capture(io_,fp,true))
            return false;

        auto accept_weapon = [&](uint64_t candidate) {
            uint64_t owner=0;
            return candidate && io_.named(candidate,"FPHitscan","Oxide") &&
                pointer(io_,candidate+go_active::FPOBJECT_PLAYER_BACKREF,owner) &&
                owner==player && (weapon=candidate,true);
        };

        // _currentWeapon (+0x50) is the authoritative active FPObject.  A
        // valid but stale typed cache at +0x58 must not win after switching.
        // Fall back to +0x58 only while the authoritative slot is absent/null;
        // if +0x50 contains a melee/tool, do not patch the previously held gun.
        uint64_t active=0;
        const bool active_read=pointer(io_,fp+go_active::FPMANAGER_CURRENT_OBJECT,active);
        if (active_read && active) {
            if (!accept_weapon(active)) return false;
        } else {
            uint64_t typed=0;
            if (!pointer(io_,fp+go_active::FPMANAGER_CURRENT_WEAPON,typed) ||
                !accept_weapon(typed)) return false;
        }

        if (!c.weapon.capture(io_,weapon,true) ||
            !pointer(io_,weapon+go_active::FPOBJECT_PLAYER_BACKREF,back) || back!=player ||
            !pointer(io_,weapon+go_active::FPHITSCAN_CONFIG,config) ||
            !io_.named(config,"HitscanWeaponConfig",kWeapons) ||
            !c.config.capture(io_,config,true)) return false;

        // Burst cadence is cached in a managed WaitForSeconds.  It is optional
        // for guards/status, but when the exact object and a sane duration are
        // present it is owned/restored like every other live cache.
        uint64_t wait=0;
        float seconds=0.f;
        if (pointer(io_,weapon+go_active::FPHITSCAN_BURST_WAIT,wait) && wait &&
            io_.named(wait,"WaitForSeconds","UnityEngine") &&
            io_.read(wait+go_active::WAITFORSECONDS_SECONDS,seconds) &&
            finite_range(seconds,0.000001f,120.f)) {
            if (!c.burst_wait.capture(io_,wait,false)) return false;
        }
        return true;
    }

    bool apply_fire(float multiplier) {
        // Конфиг — источник истины, без него не работаем. А вот живые кэши
        // (период выстрела у ещё не стрелявшего ствола, burst-ожидание)
        // бывают нулевые/не инициализированные до первого выстрела или
        // доставании: раньше любой такой кэш ронял ВЕСЬ apply, и оружие
        // «не настраивалось», хотя конфиг был в порядке. Теперь нечитанный
        // кэш просто не трогаем — игру он догонит от ускоренного конфига
        // (lMf/sPo копирует конфиг в живые поля при создании/доставании).
        if (!config_fire_.sample(io_,context_.config.object+go_active::HITSCANCONFIG_FIRE_RATE,
                                0.0001f,10000.f)) return false;
        const bool have_shot=shot_period_.sample(io_,context_.weapon.object+go_active::FPWEAPON_SHOT_PERIOD,
                                 0.000001f,120.f);
        const bool have_burst=!context_.burst_wait.object ||
            burst_seconds_.sample(io_,context_.burst_wait.object+go_active::WAITFORSECONDS_SECONDS,
                                   0.000001f,120.f);

        float wanted=config_fire_.original>=1.f
            ? config_fire_.original*multiplier : config_fire_.original/multiplier;
        if (wanted<0.02f) wanted=0.02f;
        if (wanted>5000.f) wanted=5000.f;

        const bool a=config_fire_.put(io_,wanted);
        const bool b=!have_shot || shot_period_.put(io_,shot_period_.original/multiplier);
        const bool c=!have_burst || burst_seconds_.put(io_,burst_seconds_.original/multiplier);
        return a && b && c;
    }

    bool apply_reload(float multiplier) {
        if (!config_full_.sample(io_,context_.config.object+go_active::HITSCANCONFIG_FULL_RELOAD,
                                0.f,120.f) ||
            !config_loop_.sample(io_,context_.config.object+go_active::HITSCANCONFIG_LOOP_RELOAD,
                                0.f,120.f)) return false;
        // Do not take a transient zero as the "original" of a cache whose
        // config mode is enabled; sPo may initialize it later from our already
        // accelerated config, which would otherwise be accelerated a 2nd time.
        // Живые кэши перезарядки могут быть ещё не инициализированы (ствол не
        // перезаряжали) — как и в apply_fire, отсутствие кэша не роняет весь
        // apply: ускоренный конфиг игра скопирует в кэш первой же перезарядкой.
        const float full_min=config_full_.original>0.f ? 0.000001f : 0.f;
        const float loop_min=config_loop_.original>0.f ? 0.000001f : 0.f;
        const bool have_eff_full=effective_full_.sample(io_,context_.weapon.object+go_active::FPWEAPON_EFFECTIVE_RELOAD_PERIOD,
                                    full_min,120.f);
        const bool have_eff_loop=effective_loop_.sample(io_,context_.weapon.object+go_active::FPHITSCAN_EFFECTIVE_LOOP_RELOAD_PERIOD,
                                    loop_min,120.f);

        float full=config_full_.original>0.f ? config_full_.original/multiplier : 0.f;
        float loop=config_loop_.original>0.f ? config_loop_.original/multiplier : 0.f;
        if (full>0.f && full<0.15f) full=0.15f;
        if (loop>0.f && loop<0.08f) loop=0.08f;

        // The full period may include a player stat multiplier; scale the
        // captured effective value by the same config ratio instead of
        // replacing it with the raw config. Loop period is copied 1:1 today,
        // but ratio scaling also remains correct if that changes.
        const float full_scale=config_full_.original>0.f
            ? full/config_full_.original : 1.f/multiplier;
        const float loop_scale=config_loop_.original>0.f
            ? loop/config_loop_.original : 1.f/multiplier;
        const float effective_full=effective_full_.original>0.f
            ? effective_full_.original*full_scale : 0.f;
        const float effective_loop=effective_loop_.original>0.f
            ? effective_loop_.original*loop_scale : 0.f;

        const bool a=config_full_.put(io_,full);
        const bool b=config_loop_.put(io_,loop);
        const bool c=!have_eff_full || effective_full_.put(io_,effective_full);
        const bool d=!have_eff_loop || effective_loop_.put(io_,effective_loop);
        return a && b && c && d;
    }

public:
    viewmodel::Status fire=viewmodel::Status::Off,reload=viewmodel::Status::Off;
    explicit Controller(IO& io):io_(io){}

    bool has_state() const {
        return config_fire_.owned || config_full_.owned || config_loop_.owned ||
            shot_period_.owned || effective_full_.owned || effective_loop_.owned ||
            burst_seconds_.owned;
    }
    bool reset(bool restore) {
        const bool ok=!restore || restore_all();
        context_={};
        config_fire_={}; config_full_={}; config_loop_={};
        shot_period_={}; effective_full_={}; effective_loop_={}; burst_seconds_={};
        session_=base_=0;
        fire=reload=viewmodel::Status::Off;
        return ok;
    }
    void tick(uint64_t session,uint64_t base,uint64_t player,bool fast_fire,bool fast_reload) {
        using viewmodel::Status;
        if (session!=session_ || base!=base_) { reset(false); session_=session; base_=base; }
        fire=fast_fire?Status::Waiting:Status::Off;
        reload=fast_reload?Status::Waiting:Status::Off;
        if (!fast_fire && !restore_fire()) fire=Status::Failed;
        if (!fast_reload && !restore_reload()) reload=Status::Failed;
        // Failure to restore the disabled half commonly means its pre-death
        // owner vanished.  When the other half is enabled, continue resolving
        // so a verified replacement context can discard those stale addresses.
        if (!fast_fire && !fast_reload) return;
        if (!base) return;

        // No offset discovery: live reads only validate exact local objects
        // selected through release go_active constants.
        Context next;
        if (!resolve(player,next)) {
            // A single transport/class-name miss is not a lifecycle change.
            // Restoring every owned scalar here made the next successful tick
            // write all of them again, stalling the overlay and oscillating the
            // weapon state between original and accelerated timings.
            fire=fast_fire?Status::Waiting:fire;
            reload=fast_reload?Status::Waiting:reload;
            return;
        }
        if (!(next==context_)) {
            // A verified new context means an equip/respawn transition. Try a
            // conditional restore; a dead old graph must not pin the new one.
            const bool normalized=normalize_inherited_caches(next);
            const bool restored=restore_all();
            if (!normalized || !restored) forget_values();
            context_=next;
            if (!fast_fire) fire=Status::Off;
            if (!fast_reload) reload=Status::Off;
        } else if (!(next.burst_wait==context_.burst_wait)) {
            // WaitForSeconds is created/replaced lazily without changing the
            // weapon identity. Move burst ownership separately so it cannot
            // keep pointing at a pooled wait object.
            if (!burst_seconds_.restore(io_,context_.burst_wait)) burst_seconds_={};
            context_.burst_wait=next.burst_wait;
        }
        if (fast_fire) fire=apply_fire(kFireMultiplier)?Status::Active:Status::Failed;
        if (fast_reload) reload=apply_reload(kReloadMultiplier)?Status::Active:Status::Failed;
    }
};
}
