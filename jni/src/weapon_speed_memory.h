#pragma once
// Быстрая стрельба и перезарядка из onesource: меняются только поля живого
// HitscanWeaponConfig в управляемой куче. Инструкции и данные библиотек не
// записываются; исходные значения возвращаются условно при выключении/смене.
#include "viewmodel.h"
#include "game_offsets_active.h"
#include <array>
#include <cstdint>
#include <cstring>

namespace weapon_speed {
inline constexpr const char* kWeapons = "HyperHug.Games.Oxide.Features.Weapons";
struct Layout { std::array<uint8_t,20> build_id; };
inline constexpr Layout kLayouts[] = {
    {{0xd8,0x5e,0x63,0xcd,0x8d,0xcb,0xd4,0x81,0x69,0x50,0x12,0xef,0x2e,0x28,0x4f,0x0e,0xeb,0x9e,0xf3,0x07}},
    {{0xe1,0x07,0x95,0x34,0x9b,0xd7,0xc2,0xcd,0xc7,0xd8,0x52,0x75,0x97,0xc6,0x43,0x95,0xa4,0xef,0xed,0x79}},
};
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
            (!unity || (pointer(io,obj+0x10,native) && native>=0x10000));
    }
    template<class IO> Life life(IO& io) const {
        uint64_t k=0,n=0;
        if (!pointer(io,object,k)) return Life::Unreadable;
        if (k!=klass) return Life::Gone;
        if (unity) {
            if (!pointer(io,object+0x10,n)) return Life::Unreadable;
            if (n!=native) return Life::Gone;
        }
        return Life::Alive;
    }
    bool operator==(const Identity& other) const {
        return object==other.object && klass==other.klass && native==other.native && unity==other.unity;
    }
};
struct Context {
    Identity player,fp,weapon,config;
    bool operator==(const Context& other) const {
        return player==other.player && fp==other.fp && weapon==other.weapon && config==other.config;
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
        if (!owned || address!=addr || value!=last) original=value;
        address=addr; current=value;
        return true;
    }
    template<class IO> bool put(IO& io,float value) {
        if (!address || !viewmodel::finite(value)) return false;
        if (current==value) { last=value; owned=true; return true; }
        // Сохраняем ownership и оригинал даже при ошибке: следующий reset
        // сможет повторить восстановление уже записанных полей.
        owned=true;
        if (!io.write_heap(address,value)) return false;
        // Mark the value before verification: if the write landed but the
        // readback transiently failed, reset still knows what it must undo.
        current=last=value;
        float check=0.f;
        if (!io.read(address,check) || check!=value) return false;
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
    Scalar fire_{},full_{},loop_{};
    bool restore_fire() { return fire_.restore(io_,context_.config); }
    bool restore_reload() {
        const bool a=full_.restore(io_,context_.config);
        const bool b=loop_.restore(io_,context_.config);
        return a && b;
    }
    bool restore_all() { const bool a=restore_fire(); const bool b=restore_reload(); return a&&b; }
    bool resolve(uint64_t player,Context& c) {
        uint64_t net=0,fp=0,weapon=0,back=0,config=0;
        uint8_t local=0;
        return player && io_.named(player,"PlayerManager","Oxide") && c.player.capture(io_,player,true) &&
            pointer(io_,player+0x40,net) && io_.named(net,"NetworkIdentity","Mirror") &&
            io_.read(net+0x4a,local) && local==1 &&
            pointer(io_,player+go_active::PLAYER_FP_MANAGER,fp) && io_.named(fp,"FPManager","Oxide") && c.fp.capture(io_,fp,true) &&
            pointer(io_,fp+go_active::FPMANAGER_CURRENT_WEAPON,weapon) && io_.named(weapon,"FPHitscan","Oxide") && c.weapon.capture(io_,weapon,true) &&
            pointer(io_,weapon+go_active::FPOBJECT_PLAYER_BACKREF,back) && back==player &&
            pointer(io_,weapon+go_active::FPHITSCAN_CONFIG,config) &&
            io_.named(config,"HitscanWeaponConfig",kWeapons) && c.config.capture(io_,config,true);
    }
    bool apply_fire(float multiplier) {
        if (!fire_.sample(io_,context_.config.object+go_active::HITSCANCONFIG_FIRE_RATE,0.0001f,10000.f)) return false;
        float wanted=fire_.original>=1.f ? fire_.original*multiplier : fire_.original/multiplier;
        if (wanted<0.01f) wanted=0.01f;
        if (wanted>5000.f) wanted=5000.f;
        return fire_.put(io_,wanted);
    }
    bool apply_reload(float multiplier) {
        if (!full_.sample(io_,context_.config.object+go_active::HITSCANCONFIG_FULL_RELOAD,0.f,120.f) ||
            !loop_.sample(io_,context_.config.object+go_active::HITSCANCONFIG_LOOP_RELOAD,0.f,120.f)) return false;
        float full=full_.original>0.f ? full_.original/multiplier : 0.f;
        float loop=loop_.original>0.f ? loop_.original/multiplier : 0.f;
        if (full>0.f && full<0.03f) full=0.03f;
        if (loop>0.f && loop<0.01f) loop=0.01f;
        return full_.put(io_,full) && loop_.put(io_,loop);
    }
public:
    viewmodel::Status fire=viewmodel::Status::Off,reload=viewmodel::Status::Off;
    explicit Controller(IO& io):io_(io){}
    bool has_state() const { return fire_.owned || full_.owned || loop_.owned; }
    bool reset(bool restore) {
        const bool ok=!restore || restore_all();
        context_={}; fire_={}; full_={}; loop_={}; session_=base_=0;
        fire=reload=viewmodel::Status::Off;
        return ok;
    }
    void tick(uint64_t session,uint64_t base,uint64_t player,bool fast_fire,bool fast_reload,
              float fire_multiplier,float reload_multiplier) {
        using viewmodel::Status;
        if (session!=session_ || base!=base_) { reset(false); session_=session; base_=base; }
        fire=fast_fire?Status::Waiting:Status::Off;
        reload=fast_reload?Status::Waiting:Status::Off;
        if (!fast_fire && !restore_fire()) fire=Status::Failed;
        if (!fast_reload && !restore_reload()) reload=Status::Failed;
        if ((!fast_fire && !fast_reload) || fire==Status::Failed || reload==Status::Failed) return;
        fire_multiplier=viewmodel::bounded(fire_multiplier,1.f,20.f,3.f);
        reload_multiplier=viewmodel::bounded(reload_multiplier,1.f,20.f,4.f);
        std::array<uint8_t,20> id{};
        const Layout* layout=nullptr;
        if (base && io_.read(base+0x318,id))
            for (const auto& candidate:kLayouts) if (candidate.build_id==id) layout=&candidate;
        if (!layout) {
            restore_all();
            if (fast_fire) fire=Status::Failed;
            if (fast_reload) reload=Status::Failed;
            return;
        }
        Context next;
        if (!resolve(player,next)) {
            restore_all();
            return;
        }
        if (!(next==context_)) {
            if (!restore_all()) { fire=reload=Status::Failed; return; }
            context_=next;
        }
        if (fast_fire) fire=apply_fire(fire_multiplier)?Status::Active:Status::Failed;
        if (fast_reload) reload=apply_reload(reload_multiplier)?Status::Active:Status::Failed;
    }
};
}
