#include "fast_reload_memory.h"
#include <cassert>
#include <cstring>
#include <map>
#include <string>
#include <limits>
#include <cstdio>
#include <vector>
#include <cmath>

bool near(float a,float b) { return std::fabs(a-b)<1e-6f; }

struct FakeIO {
    std::map<uint64_t,uint8_t> mem;
    std::map<uint64_t,std::pair<std::string,std::string>> types;
    uint64_t fail_read=0, fail_write=0, fail_after_write=0;
    unsigned writes=0;
    std::vector<uint64_t> write_log;
    template<class T> void put(uint64_t a, T v) {
        auto* bytes=reinterpret_cast<const uint8_t*>(&v);
        for (size_t i=0;i<sizeof(v);++i) mem[a+i]=bytes[i];
    }
    template<class T> bool read(uint64_t a, T& v) {
        if (a==fail_read) return false;
        auto* bytes=reinterpret_cast<uint8_t*>(&v);
        for (size_t i=0;i<sizeof(v);++i) {
            auto it=mem.find(a+i);
            if (it==mem.end()) return false;
            bytes[i]=it->second;
        }
        return true;
    }
    template<class T> bool write_heap(uint64_t a, T v) {
        if (a==fail_write) return false;
        ++writes; write_log.push_back(a); put(a,v);
        if (a==fail_after_write) fail_read=a;
        return true;
    }
    bool exact(uint64_t a,const char* n,const char* ns) {
        auto it=types.find(a);
        return it!=types.end() && it->second.first==n && it->second.second==ns;
    }
    void object(uint64_t a,const char* n,const char* ns) {
        types[a]={n,ns}; put<uint64_t>(a,a+0x800000);
        put<uint64_t>(a+go_active::MANAGED_CACHED_PTR,a+0x900000);
    }
    float value(uint64_t a) { float v=0; assert(read(a,v)); return v; }
};
constexpr uint64_t player=0x10000, net=0x20000, fp=0x30000, gun=0x40000, cfg=0x70000;
uint64_t effective_reload() { return gun+go_active::FPWEAPON_EFFECTIVE_RELOAD_PERIOD; }
uint64_t source_reload()    { return gun+go_active::FPWEAPON_RELOAD_PERIOD; }
uint64_t loop_reload()      { return gun+go_active::FPHITSCAN_EFFECTIVE_LOOP_RELOAD_PERIOD; }
struct Fixture {
    FakeIO io;
    fast_reload::Controller<FakeIO> c{io};
    const char* name;
    Fixture(const char* n="FPHitscan",const char* ns="Oxide"):name(n) {
        io.object(player,"PlayerManager","Oxide"); io.object(net,"NetworkIdentity","Mirror");
        io.object(fp,"FPManager","Oxide"); io.object(gun,name,ns);
        io.put<uint64_t>(player+go_active::NETWORK_BEHAVIOUR_IDENTITY,net);
        io.put<uint8_t>(net+go_active::NETWORK_IDENTITY_IS_LOCAL_PLAYER,1);
        io.put<uint64_t>(player+go_active::PLAYER_FP_MANAGER,fp);
        io.put<uint64_t>(fp+go_active::FPMANAGER_CURRENT_OBJECT,gun);
        io.put<uint64_t>(gun+go_active::FPOBJECT_PLAYER_BACKREF,player);
        io.put<float>(effective_reload(),2.5f);
        io.put<float>(source_reload(),2.5f);
        // Shared config asset: the controller must never write it.
        io.object(cfg,"HitscanWeaponConfig","HyperHug.Games.Oxide.Features.Weapons");
        io.put<uint64_t>(gun+go_active::FPHITSCAN_CONFIG,cfg);
        io.put<float>(cfg+go_active::HITSCANCONFIG_FULL_RELOAD,2.4f);
        io.put<float>(cfg+go_active::HITSCANCONFIG_LOOP_RELOAD,0.7f);
        if (std::strcmp(n,"FPHitscan")==0) io.put<float>(loop_reload(),0.8f);
        else io.put<float>(loop_reload(),0.5f);  // alien value at the unused slot
    }
    void tick(bool on=true) { c.tick(1,0x1000000,player,on); }
    bool config_untouched() const {
        float full=0, loop=0;
        const_cast<FakeIO&>(io).read(cfg+go_active::HITSCANCONFIG_FULL_RELOAD,full);
        const_cast<FakeIO&>(io).read(cfg+go_active::HITSCANCONFIG_LOOP_RELOAD,loop);
        return full==2.4f && loop==0.7f;
    }
};

int main() {
    using namespace go_active;
    for (auto version : {GameVersion::Release,GameVersion::Beta}) {
        Select(version);
        for (const char* n : {"FPHitscan","FPBow","FPRocketLauncher","FPExplosiveCharge",
                              "FPSpear","FPMelee","FPTool","FPTorch","FPChainsaw","FPBuildingHammer",
                              "FPCrossbow","FPSnowball"}) {
            const bool hitscan=std::strcmp(n,"FPHitscan")==0;
            const bool moved=std::strcmp(n,"FPCrossbow")==0 || std::strcmp(n,"FPSnowball")==0;
            Fixture f(n,moved?fast_fire::kFirstPerson:"Oxide");
            f.tick(); assert(f.c.status==viewmodel::Status::Active);
            assert(f.io.value(effective_reload())==fast_reload::kPeriod);
            assert(f.io.value(source_reload())==fast_reload::kPeriod);
            if (hitscan) assert(f.io.value(loop_reload())==fast_reload::kPeriod);
            else assert(f.io.value(loop_reload())==0.5f);
            assert(f.config_untouched());
            const auto writes=f.io.writes; f.tick(); assert(f.io.writes==writes);
            f.tick(false); assert(f.io.value(effective_reload())==2.5f);
            assert(f.io.value(source_reload())==2.5f);
            if (hitscan) assert(f.io.value(loop_reload())==0.8f);
            else assert(f.io.value(loop_reload())==0.5f);
            assert(!f.c.has_state() && f.c.status==viewmodel::Status::Off);
            assert(f.config_untouched());
        }
        { Fixture f; f.io.put<float>(effective_reload(),0.01f);
          f.io.put<float>(source_reload(),0.01f); f.io.put<float>(loop_reload(),0.01f);
          f.tick(); assert(f.io.writes==0 && !f.c.has_state());
          assert(f.c.status==viewmodel::Status::Active);
          f.tick(false); assert(f.io.writes==0);
          assert(f.io.value(effective_reload())==0.01f);
          assert(f.io.value(source_reload())==0.01f);
          assert(f.io.value(loop_reload())==0.01f); }
        { Fixture f; f.io.put<float>(effective_reload(),std::numeric_limits<float>::quiet_NaN());
          f.tick(); assert(!f.io.writes && f.c.status==viewmodel::Status::Failed && f.c.stage()==7); }
        for (float v : {-1.f,61.f,std::numeric_limits<float>::infinity()}) {
            Fixture f; f.io.put<float>(effective_reload(),v); f.tick();
            assert(!f.io.writes && f.c.status==viewmodel::Status::Failed);
        }
        { Fixture f; f.io.put<float>(loop_reload(),std::numeric_limits<float>::quiet_NaN());
          f.tick(); assert(!f.io.writes && f.c.status==viewmodel::Status::Failed); }
        { Fixture f; f.io.put<uint64_t>(fp+FPMANAGER_CURRENT_OBJECT,0);
          f.tick(); assert(!f.io.writes && f.c.status==viewmodel::Status::Waiting); }
        { Fixture f; f.io.fail_read=fp+FPMANAGER_CURRENT_OBJECT;
          f.tick(); assert(!f.io.writes); }
        { Fixture f; f.io.put<uint8_t>(net+NETWORK_IDENTITY_IS_LOCAL_PLAYER,0);
          f.tick(); assert(!f.io.writes); }
        { Fixture f; f.io.put<uint64_t>(gun+FPOBJECT_PLAYER_BACKREF,player+1);
          f.tick(); assert(!f.io.writes); }
        { Fixture f("FPObject"); f.tick(); assert(!f.io.writes && f.c.stage()==4); }
        { Fixture f("FPHitscan","WrongNamespace"); f.tick(); assert(!f.io.writes); }
        { Fixture f; f.io.types.erase(gun); f.tick(); assert(!f.io.writes); }
        { Fixture f; f.tick(); f.io.fail_write=effective_reload(); f.tick(false);
          // restore source first, then the failed cache slot stays owned
          assert(f.c.has_state() && f.c.status==viewmodel::Status::Failed);
          assert(f.io.value(source_reload())==2.5f);
          assert(f.io.value(effective_reload())==fast_reload::kPeriod);
          f.io.fail_write=0; f.tick(false);
          assert(!f.c.has_state());
          assert(f.io.value(effective_reload())==2.5f);
          assert(f.io.value(source_reload())==2.5f); }
        { Fixture f; f.io.fail_write=effective_reload(); f.tick();
          // apply's cache write failed: ownership kept, original untouched
          assert(f.c.has_state());
          f.io.fail_write=0; f.tick(false);
          assert(!f.c.has_state());
          assert(f.io.value(effective_reload())==2.5f);
          assert(f.io.value(source_reload())==2.5f); }
        { Fixture f; f.io.fail_after_write=effective_reload(); f.tick();
          assert(f.c.has_state());
          f.io.fail_after_write=f.io.fail_read=0; f.tick(false);
          assert(!f.c.has_state());
          assert(f.io.value(effective_reload())==2.5f);
          assert(f.io.value(source_reload())==2.5f); }
        { Fixture f; f.tick(); f.io.fail_write=source_reload(); f.tick(false);
          // source restore failed -> retry on the next tick succeeds
          assert(f.c.has_state());
          f.io.fail_write=0; f.tick(false);
          assert(!f.c.has_state() && f.io.value(source_reload())==2.5f); }
        { Fixture f; f.tick(); f.io.put<float>(effective_reload(),0.4f);
          f.tick(false);  // another writer's update is never overwritten
          assert(f.io.value(effective_reload())==0.4f);
          assert(f.io.value(source_reload())==2.5f); }
        { Fixture f; f.tick(); f.io.put<float>(effective_reload(),0.4f);
          f.tick();  // still enabled: recapped, new original tracked
          assert(f.io.value(effective_reload())==fast_reload::kPeriod);
          f.tick(false); assert(f.io.value(effective_reload())==0.4f); }
        { Fixture f; f.tick(); f.io.put<float>(source_reload(),0.4f);
          f.tick(); assert(f.io.value(source_reload())==fast_reload::kPeriod);
          f.tick(false); assert(f.io.value(source_reload())==0.4f); }
        { Fixture f; f.tick(); f.io.put<uint64_t>(gun+MANAGED_CACHED_PTR,0);
          auto writes=f.io.writes; f.tick(false);
          assert(f.io.writes==writes && !f.c.has_state()); }
        { Fixture f; f.tick(); auto writes=f.io.writes;
          f.c.tick(2,0x2000000,0,false);
          assert(f.io.writes==writes && !f.c.has_state()); }
        { Fixture f; f.tick();  // equip switch: old weapon restored, new capped
          f.io.object(0x60000,"FPBow","Oxide");
          f.io.put<uint64_t>(0x60000+MANAGED_CACHED_PTR,0x690000);
          f.io.put<uint64_t>(0x60000+FPOBJECT_PLAYER_BACKREF,player);
          f.io.put<float>(0x60000+FPWEAPON_EFFECTIVE_RELOAD_PERIOD,1.8f);
          f.io.put<float>(0x60000+FPWEAPON_RELOAD_PERIOD,1.8f);
          f.io.put<uint64_t>(fp+FPMANAGER_CURRENT_OBJECT,0x60000);
          f.tick();
          assert(f.io.value(effective_reload())==2.5f);
          assert(f.io.value(source_reload())==2.5f);
          assert(f.io.value(0x60000+FPWEAPON_EFFECTIVE_RELOAD_PERIOD)==fast_reload::kPeriod);
          assert(f.io.value(0x60000+FPWEAPON_RELOAD_PERIOD)==fast_reload::kPeriod);
          f.tick(false);
          assert(f.io.value(0x60000+FPWEAPON_EFFECTIVE_RELOAD_PERIOD)==1.8f);
          assert(f.io.value(0x60000+FPWEAPON_RELOAD_PERIOD)==1.8f); }
        { Fixture f; f.tick();  // restore of the old weapon fails -> new not applied
          f.io.object(0x60000,"FPBow","Oxide");
          f.io.put<uint64_t>(0x60000+MANAGED_CACHED_PTR,0x690000);
          f.io.put<uint64_t>(0x60000+FPOBJECT_PLAYER_BACKREF,player);
          f.io.put<float>(0x60000+FPWEAPON_EFFECTIVE_RELOAD_PERIOD,1.8f);
          f.io.put<float>(0x60000+FPWEAPON_RELOAD_PERIOD,1.8f);
          f.io.put<uint64_t>(fp+FPMANAGER_CURRENT_OBJECT,0x60000);
          f.io.fail_write=source_reload();
          f.tick();
          assert(f.c.status==viewmodel::Status::Failed && f.c.stage()==9);
          assert(f.io.value(0x60000+FPWEAPON_EFFECTIVE_RELOAD_PERIOD)==1.8f);
          f.io.fail_write=0; f.tick();
          assert(f.io.value(0x60000+FPWEAPON_EFFECTIVE_RELOAD_PERIOD)==fast_reload::kPeriod);
          assert(f.io.value(effective_reload())==2.5f); }
        { Fixture f; f.tick();  // game reinit restored the cache: recapped next tick
          f.io.put<float>(effective_reload(),2.5f);
          f.io.fail_write=effective_reload();
          f.tick();
          assert(f.c.status==viewmodel::Status::Failed && f.c.stage()==10);
          f.io.fail_write=0; f.tick();
          assert(f.io.value(effective_reload())==fast_reload::kPeriod);
          f.tick(false);
          assert(f.io.value(effective_reload())==2.5f); }
    }
    std::puts("fast_reload_memory: all tests passed");
}
