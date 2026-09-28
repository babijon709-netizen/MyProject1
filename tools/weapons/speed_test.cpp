#include "weapon_speed_memory.h"
#include <cassert>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

struct FakeMemory {
    std::unordered_map<uint64_t,uint8_t> bytes;
    std::unordered_map<uint64_t,std::string> names;
    std::unordered_map<uint64_t,std::string> spaces;
    std::vector<uint64_t> write_addresses;
    bool fail_write=false; uint64_t fail_read=0; int writes=0;
    template<class T> void set(uint64_t a,const T& v){const auto*p=(const uint8_t*)&v;for(size_t i=0;i<sizeof(v);++i)bytes[a+i]=p[i];}
    template<class T> T get(uint64_t a){T v{};assert(read(a,v));return v;}
    template<class T> bool read(uint64_t a,T& v){if(a==fail_read)return false;auto*p=(uint8_t*)&v;for(size_t i=0;i<sizeof(v);++i){auto it=bytes.find(a+i);if(it==bytes.end())return false;p[i]=it->second;}return true;}
    bool named(uint64_t o,const char*n,const char*ns){return names[o]==n&&spaces[o]==ns;}
    template<class T> bool write_heap(uint64_t a,const T& v){++writes;write_addresses.push_back(a);if(fail_write)return false;set(a,v);return true;}
    void object(uint64_t o,uint64_t k,const char*n,const char*ns,bool unity=true){set(o,k);if(unity)set(o+0x10,o+0x9000);names[o]=n;spaces[o]=ns;}
};

struct Fixture {
    FakeMemory io; weapon_speed::Controller<FakeMemory> c;
    uint64_t base=0x100000,player=0x20000,net=0x21000,fp=0x22000;
    uint64_t weapon=0x23000,config=0x24000,wait=0x25000;

    Fixture():c(io){
        io.object(player,0x30000,"PlayerManager","Oxide");
        io.object(net,0x31000,"NetworkIdentity","Mirror");
        uint8_t local=1; io.set(net+0x4a,local); io.set(player+0x40,net);
        io.object(fp,0x32000,"FPManager","Oxide"); io.set(player+go_active::PLAYER_FP_MANAGER,fp);
        make_gun(weapon,config,wait,5.f,4.f,2.f,12.f,5.f,2.f,.15f);
        io.set(fp+go_active::FPMANAGER_CURRENT_OBJECT,weapon);
        io.set(fp+go_active::FPMANAGER_CURRENT_WEAPON,weapon);
    }
    void make_gun(uint64_t w,uint64_t cfg,uint64_t burst,float fire,float full,float loop,
                  float shot,float effective_full,float effective_loop,float seconds) {
        io.object(w,w+0x10000,"FPHitscan","Oxide");
        io.set(w+go_active::FPOBJECT_PLAYER_BACKREF,player);
        io.object(cfg,cfg+0x10000,"HitscanWeaponConfig",weapon_speed::kWeapons);
        io.set(w+go_active::FPHITSCAN_CONFIG,cfg);
        io.set(cfg+go_active::HITSCANCONFIG_FIRE_RATE,fire);
        io.set(cfg+go_active::HITSCANCONFIG_FULL_RELOAD,full);
        io.set(cfg+go_active::HITSCANCONFIG_LOOP_RELOAD,loop);
        io.set(w+go_active::FPWEAPON_SHOT_PERIOD,shot);
        io.set(w+go_active::FPWEAPON_EFFECTIVE_RELOAD_PERIOD,effective_full);
        io.set(w+go_active::FPHITSCAN_EFFECTIVE_LOOP_RELOAD_PERIOD,effective_loop);
        io.object(burst,burst+0x10000,"WaitForSeconds","UnityEngine",false);
        io.set(burst+go_active::WAITFORSECONDS_SECONDS,seconds);
        io.set(w+go_active::FPHITSCAN_BURST_WAIT,burst);
    }
    void tick(bool fire=true,bool reload=true){c.tick(1,base,player,fire,reload);}
};

static void expect_fast(Fixture& f,uint64_t w,uint64_t cfg,uint64_t wait) {
    assert(f.io.get<float>(cfg+go_active::HITSCANCONFIG_FIRE_RATE)==100.f);
    assert(f.io.get<float>(w+go_active::FPWEAPON_SHOT_PERIOD)==.6f);
    const float burst=f.io.get<float>(wait+go_active::WAITFORSECONDS_SECONDS);
    assert(burst>.00749f && burst<.00751f);
    assert(f.io.get<float>(cfg+go_active::HITSCANCONFIG_FULL_RELOAD)==.2f);
    assert(f.io.get<float>(cfg+go_active::HITSCANCONFIG_LOOP_RELOAD)==.1f);
    // Full reload keeps the player multiplier captured in the effective cache.
    assert(f.io.get<float>(w+go_active::FPWEAPON_EFFECTIVE_RELOAD_PERIOD)==.25f);
    assert(f.io.get<float>(w+go_active::FPHITSCAN_EFFECTIVE_LOOP_RELOAD_PERIOD)==.1f);
}

int main(){
    using viewmodel::Status;

    // Both toggles patch config and already-created live caches immediately,
    // then restore every owned value.
    {
        Fixture f; f.tick();
        assert(f.c.fire==Status::Active&&f.c.reload==Status::Active);
        expect_fast(f,f.weapon,f.config,f.wait);
        assert(f.c.reset(true));
        assert(f.io.get<float>(f.config+go_active::HITSCANCONFIG_FIRE_RATE)==5.f);
        assert(f.io.get<float>(f.weapon+go_active::FPWEAPON_SHOT_PERIOD)==12.f);
        assert(f.io.get<float>(f.wait+go_active::WAITFORSECONDS_SECONDS)==.15f);
        assert(f.io.get<float>(f.config+go_active::HITSCANCONFIG_FULL_RELOAD)==4.f);
        assert(f.io.get<float>(f.weapon+go_active::FPWEAPON_EFFECTIVE_RELOAD_PERIOD)==5.f);
        assert(f.io.get<float>(f.weapon+go_active::FPHITSCAN_EFFECTIVE_LOOP_RELOAD_PERIOD)==2.f);
    }

    // Independent toggles restore only their own config/runtime fields.
    {
        Fixture f; f.tick(); f.tick(false,true);
        assert(f.io.get<float>(f.config+go_active::HITSCANCONFIG_FIRE_RATE)==5.f);
        assert(f.io.get<float>(f.weapon+go_active::FPWEAPON_SHOT_PERIOD)==12.f);
        assert(f.io.get<float>(f.wait+go_active::WAITFORSECONDS_SECONDS)==.15f);
        assert(f.io.get<float>(f.config+go_active::HITSCANCONFIG_FULL_RELOAD)==.2f);
        assert(f.io.get<float>(f.weapon+go_active::FPWEAPON_EFFECTIVE_RELOAD_PERIOD)==.25f);
    }

    // Game-side replacement while owned becomes the new conditional original.
    {
        Fixture f; f.tick();
        f.io.set(f.config+go_active::HITSCANCONFIG_FIRE_RATE,7.f);
        f.tick(true,false);
        assert(f.io.get<float>(f.config+go_active::HITSCANCONFIG_FIRE_RATE)==140.f);
        assert(f.c.reset(true));
        assert(f.io.get<float>(f.config+go_active::HITSCANCONFIG_FIRE_RATE)==7.f);
    }

    // Switching guns restores the old weapon and patches the new one in the
    // same tick. +0x50 wins even while +0x58 remains a valid stale gun.
    {
        Fixture f; f.tick();
        const uint64_t w=0x26000,cfg=0x27000,wait=0x28000;
        f.make_gun(w,cfg,wait,2.f,8.f,4.f,30.f,8.f,4.f,.2f);
        f.io.set(f.fp+go_active::FPMANAGER_CURRENT_OBJECT,w);
        // FPMANAGER_CURRENT_WEAPON intentionally remains f.weapon.
        f.tick();
        assert(f.io.get<float>(f.config+go_active::HITSCANCONFIG_FIRE_RATE)==5.f);
        assert(f.io.get<float>(f.weapon+go_active::FPWEAPON_SHOT_PERIOD)==12.f);
        assert(f.io.get<float>(cfg+go_active::HITSCANCONFIG_FIRE_RATE)==40.f);
        assert(f.io.get<float>(w+go_active::FPWEAPON_SHOT_PERIOD)==1.5f);
        assert(f.io.get<float>(w+go_active::FPWEAPON_EFFECTIVE_RELOAD_PERIOD)==.4f);
    }

    // A non-hitscan authoritative current object blocks stale-gun fallback.
    {
        Fixture f; f.tick();
        const uint64_t melee=0x29000;
        f.io.object(melee,0x39000,"FPTool","Oxide");
        f.io.set(melee+go_active::FPOBJECT_PLAYER_BACKREF,f.player);
        f.io.set(f.fp+go_active::FPMANAGER_CURRENT_OBJECT,melee);
        f.tick();
        assert(f.io.get<float>(f.config+go_active::HITSCANCONFIG_FIRE_RATE)==5.f);
        assert(f.io.get<float>(f.weapon+go_active::FPWEAPON_SHOT_PERIOD)==12.f);
    }

    // A genuinely null authoritative slot can still use the typed fallback.
    {
        Fixture f; uint64_t zero=0;
        f.io.set(f.fp+go_active::FPMANAGER_CURRENT_OBJECT,zero);
        f.tick();
        assert(f.c.fire==Status::Active&&f.c.reload==Status::Active);
    }

    // Respawn can construct a new weapon from the still-patched shared config
    // before our next frame. Exact inherited cache values are normalized first,
    // preventing accidental 20x-on-20x acceleration, then re-applied once.
    {
        Fixture f; f.tick();
        const uint64_t w=0x2a000,wait=0x2b000;
        // Same config; runtime values were copied while it was accelerated.
        f.io.object(w,0x3a000,"FPHitscan","Oxide");
        f.io.set(w+go_active::FPOBJECT_PLAYER_BACKREF,f.player);
        f.io.set(w+go_active::FPHITSCAN_CONFIG,f.config);
        f.io.set(w+go_active::FPWEAPON_SHOT_PERIOD,.6f);
        f.io.set(w+go_active::FPWEAPON_EFFECTIVE_RELOAD_PERIOD,.25f);
        f.io.set(w+go_active::FPHITSCAN_EFFECTIVE_LOOP_RELOAD_PERIOD,.1f);
        f.io.object(wait,0x3b000,"WaitForSeconds","UnityEngine",false);
        f.io.set(wait+go_active::WAITFORSECONDS_SECONDS,.15f);
        f.io.set(w+go_active::FPHITSCAN_BURST_WAIT,wait);
        uint64_t gone=0; f.io.set(f.weapon+go_active::MANAGED_CACHED_PTR,gone);
        f.io.set(f.fp+go_active::FPMANAGER_CURRENT_OBJECT,w);
        f.io.set(f.fp+go_active::FPMANAGER_CURRENT_WEAPON,w);
        f.tick();
        expect_fast(f,w,f.config,wait);
    }

    // Write and transient restore failures remain visible and retryable.
    {
        Fixture f; f.tick(); f.tick(true,false);
        f.io.fail_write=true;
        f.io.set(f.config+go_active::HITSCANCONFIG_FIRE_RATE,5.f);
        f.tick(true,false);
        assert(f.c.fire==Status::Failed);
    }
    {
        Fixture f; f.tick();
        f.io.fail_read=f.config;
        f.tick(false,false);
        assert(f.c.fire==Status::Failed&&f.c.reload==Status::Failed&&f.c.has_state());
        f.io.fail_read=0;
        f.tick(false,false);
        assert(!f.c.has_state());
        assert(f.io.get<float>(f.config+go_active::HITSCANCONFIG_FIRE_RATE)==5.f);
    }

    // Reused/dead config identity is never overwritten on reset.
    {
        Fixture f; f.tick();
        f.io.set(f.config,uint64_t(0xdead0));
        assert(f.c.reset(true));
        for(uint64_t a:f.io.write_addresses)
            assert(a!=f.config+go_active::HITSCANCONFIG_FIRE_RATE ||
                   f.io.get<float>(a)==100.f);
    }
}
