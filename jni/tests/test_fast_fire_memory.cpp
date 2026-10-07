#include "fast_fire_memory.h"
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
constexpr uint64_t player=0x10000, net=0x20000, fp=0x30000, gun=0x40000, wait=0x50000;
uint64_t period() { return gun+go_active::FPWEAPON_SHOT_PERIOD; }
uint64_t source_period(const char* name) {
    using namespace go_active;
    if (std::strcmp(name,"FPHitscan")==0) return gun+FPHITSCAN_FIRE_DURATION;
    if (std::strcmp(name,"FPBow")==0) return gun+FPBOW_MIN_SHOT_PERIOD;
    if (std::strcmp(name,"FPCrossbow")==0) return gun+FPCROSSBOW_MIN_SHOT_PERIOD;
    if (std::strcmp(name,"FPRocketLauncher")==0) return gun+FPROCKETLAUNCHER_MIN_SHOT_PERIOD;
    if (std::strcmp(name,"FPSnowball")==0) return gun+FPSNOWBALL_MIN_THROW_PERIOD;
    if (std::strcmp(name,"FPExplosiveCharge")==0) return gun+FPEXPLOSIVECHARGE_MIN_THROW_PERIOD;
    if (std::strcmp(name,"FPSpear")==0 || std::strcmp(name,"FPMelee")==0 ||
        std::strcmp(name,"FPTool")==0 || std::strcmp(name,"FPTorch")==0 ||
        std::strcmp(name,"FPChainsaw")==0 || std::strcmp(name,"FPBuildingHammer")==0)
        return gun+FPMELEE_TIME_BETWEEN_ATTACKS;
    return 0;
}
struct Fixture {
    FakeIO io;
    fast_fire::Controller<FakeIO> c{io};
    const char* name;
    Fixture(const char* n="FPHitscan",const char* ns="Oxide"):name(n) {
        io.object(player,"PlayerManager","Oxide"); io.object(net,"NetworkIdentity","Mirror");
        io.object(fp,"FPManager","Oxide"); io.object(gun,name,ns);
        io.put<uint64_t>(player+go_active::NETWORK_BEHAVIOUR_IDENTITY,net);
        io.put<uint8_t>(net+go_active::NETWORK_IDENTITY_IS_LOCAL_PLAYER,1);
        io.put<uint64_t>(player+go_active::PLAYER_FP_MANAGER,fp);
        io.put<uint64_t>(fp+go_active::FPMANAGER_CURRENT_OBJECT,gun);
        io.put<uint64_t>(gun+go_active::FPOBJECT_PLAYER_BACKREF,player);
        io.put<float>(period(),0.6f);
        const uint64_t source=source_period(name);
        if (source) io.put<float>(source,0.6f);
        if (std::strcmp(name,"FPHitscan")==0) {
            io.put<uint32_t>(gun+go_active::FPHITSCAN_FIRE_MODE,fast_fire::kFullAuto);
            io.put<int32_t>(gun+go_active::FPHITSCAN_BURST_LENGTH,3);
            io.put<float>(gun+go_active::FPHITSCAN_BURST_DURATION,0.3f);
            io.put<float>(gun+go_active::FPHITSCAN_BURST_PAUSE,0.35f);
            io.put<uint64_t>(gun+go_active::FPHITSCAN_BURST_WAIT,0);
        }
        if (std::strcmp(name,"FPSpear")==0) {
            io.put<float>(gun+go_active::FPSPEAR_MIN_THROW_PERIOD,1.2f);
            io.put<float>(gun+go_active::FPSPEAR_CACHED_THROW_PERIOD,1.2f);
        }
    }
    void tick(bool on=true) { c.tick(1,0x1000000,player,on); }
    void burst() {
        io.object(wait,"WaitForSeconds","UnityEngine");
        io.put<uint64_t>(gun+go_active::FPHITSCAN_BURST_WAIT,wait);
        io.put<uint32_t>(gun+go_active::FPHITSCAN_FIRE_MODE,fast_fire::kBurst);
        io.put<int32_t>(gun+go_active::FPHITSCAN_BURST_LENGTH,3);
        io.put<float>(period(),0.65f);
        io.put<float>(gun+go_active::FPHITSCAN_BURST_DURATION,0.3f);
        io.put<float>(gun+go_active::FPHITSCAN_BURST_PAUSE,0.35f);
        io.put<float>(wait+go_active::WAITFORSECONDS_SECONDS,0.1f);
    }
};

int main() {
    using namespace go_active;
    for (auto version : {GameVersion::Release,GameVersion::Beta}) {
        Select(version);
        for (const char* n : {"FPHitscan","FPBow","FPRocketLauncher","FPExplosiveCharge",
                             "FPSpear","FPMelee","FPTool","FPTorch","FPChainsaw","FPBuildingHammer",
                             "FPCrossbow","FPSnowball"}) {
            const bool moved=std::strcmp(n,"FPCrossbow")==0 || std::strcmp(n,"FPSnowball")==0;
            Fixture f(n,moved?fast_fire::kFirstPerson:"Oxide");
            f.tick(); assert(f.c.status==viewmodel::Status::Active);
            assert(f.io.value(period())==fast_fire::kPeriod);
            if (std::strcmp(n,"FPHitscan")!=0)
                assert(f.io.value(source_period(n))==fast_fire::kPeriod);
            const auto writes=f.io.writes; f.tick(); assert(f.io.writes==writes);
            if (std::strcmp(n,"FPSpear")==0) {
                assert(f.io.value(gun+FPSPEAR_MIN_THROW_PERIOD)==fast_fire::kPeriod);
                assert(f.io.value(gun+FPSPEAR_CACHED_THROW_PERIOD)==fast_fire::kPeriod);
            }
            f.tick(false); assert(f.io.value(period())==0.6f && !f.c.has_state());
            if (std::strcmp(n,"FPHitscan")!=0)
                assert(f.io.value(source_period(n))==0.6f);
            if (std::strcmp(n,"FPSpear")==0) {
                assert(f.io.value(gun+FPSPEAR_MIN_THROW_PERIOD)==1.2f);
                assert(f.io.value(gun+FPSPEAR_CACHED_THROW_PERIOD)==1.2f);
            }
        }
        { Fixture f; f.burst(); f.tick();
          assert(f.c.status==viewmodel::Status::Active);
          assert(near(f.io.value(period()),0.12f));
          assert(near(f.io.value(gun+FPHITSCAN_BURST_DURATION),0.09f));
          assert(f.io.value(gun+FPHITSCAN_BURST_PAUSE)==0.03f);
          assert(f.io.value(wait+WAITFORSECONDS_SECONDS)==0.03f);
          assert(f.c.reset(true));
          assert(f.io.value(period())==0.65f);
          assert(f.io.value(gun+FPHITSCAN_BURST_DURATION)==0.3f);
          assert(f.io.value(gun+FPHITSCAN_BURST_PAUSE)==0.35f);
          assert(f.io.value(wait+WAITFORSECONDS_SECONDS)==0.1f); }
        { Fixture f; f.burst(); f.io.put<float>(period(),0.05f); f.tick();
          assert(f.c.status==viewmodel::Status::Failed && f.c.stage()==11 && !f.io.writes); }
        { Fixture f; f.burst(); f.tick(); f.io.put<float>(period(),0.05f); f.tick();
          assert(f.c.status==viewmodel::Status::Failed && f.c.stage()==11);
          assert(!f.c.has_state() && f.io.value(period())==0.05f);
          assert(f.io.value(gun+FPHITSCAN_BURST_DURATION)==0.3f);
          assert(f.io.value(gun+FPHITSCAN_BURST_PAUSE)==0.35f);
          assert(f.io.value(wait+WAITFORSECONDS_SECONDS)==0.1f); }
        { Fixture f; f.burst(); f.io.put<int32_t>(gun+FPHITSCAN_BURST_LENGTH,0); f.tick();
          assert(f.c.status==viewmodel::Status::Failed && f.c.stage()==6 && !f.io.writes); }
        { Fixture f; f.burst(); f.tick(); f.io.put<uint32_t>(gun+FPHITSCAN_FIRE_MODE,fast_fire::kFullAuto);
          f.tick(); assert(f.c.status==viewmodel::Status::Active);
          assert(f.io.value(gun+FPHITSCAN_BURST_DURATION)==0.3f);
          assert(f.io.value(gun+FPHITSCAN_BURST_PAUSE)==0.35f);
          assert(f.io.value(wait+WAITFORSECONDS_SECONDS)==0.1f);
          f.tick(false); assert(f.io.value(period())==0.65f); }
        { Fixture f("FPHitscan");
          f.io.put<uint32_t>(gun+FPHITSCAN_FIRE_MODE,fast_fire::kSemiAuto);
          f.io.put<float>(gun+FPHITSCAN_FIRE_DURATION,0.5f); f.io.put<float>(period(),0.5f);
          f.tick(); assert(f.io.value(gun+FPHITSCAN_FIRE_DURATION)==fast_fire::kPeriod);
          assert(f.io.value(period())==fast_fire::kPeriod);
          f.tick(false); assert(f.io.value(gun+FPHITSCAN_FIRE_DURATION)==0.5f);
          assert(f.io.value(period())==0.5f); }
        { Fixture f; f.io.put<uint64_t>(fp+FPMANAGER_CURRENT_OBJECT,0);
          f.tick(); assert(f.io.value(period())==0.6f && f.c.status==viewmodel::Status::Waiting); }
        { Fixture f; f.io.fail_read=fp+FPMANAGER_CURRENT_OBJECT;
          f.tick(); assert(!f.io.writes); }
        { Fixture f; f.io.put<uint8_t>(net+NETWORK_IDENTITY_IS_LOCAL_PLAYER,0);
          f.tick(); assert(!f.io.writes); }
        { Fixture f; f.io.put<uint64_t>(gun+FPOBJECT_PLAYER_BACKREF,player+1);
          f.tick(); assert(!f.io.writes); }
        { Fixture f("FPObject"); f.tick(); assert(!f.io.writes); }
        { Fixture f("FPHitscan","WrongNamespace"); f.tick(); assert(!f.io.writes); }
        { Fixture f; f.io.types.erase(gun); f.tick(); assert(!f.io.writes); }
        { Fixture f; f.io.put<float>(period(),std::numeric_limits<float>::quiet_NaN());
          f.tick(); assert(!f.io.writes && f.c.status==viewmodel::Status::Failed); }
        for (float v : {-1.f,61.f,std::numeric_limits<float>::infinity()}) {
            Fixture f; f.io.put<float>(period(),v); f.tick(); assert(!f.io.writes);
        }
        { Fixture f; f.io.put<float>(period(),0.01f); f.tick(); f.tick(false);
          assert(!f.io.writes && f.io.value(period())==0.01f); }
        { Fixture f("FPMelee"); f.io.put<float>(source_period("FPMelee"),0.01f);
          f.io.put<float>(period(),0.01f);
          f.tick(); f.tick(false); assert(!f.io.writes); }
        { Fixture f; f.io.fail_write=period(); f.tick(); assert(f.c.has_state());
          f.io.fail_write=0; f.tick(false); assert(!f.c.has_state() && f.io.value(period())==0.6f); }
        { Fixture f; f.io.fail_after_write=period(); f.tick(); assert(f.c.has_state());
          f.io.fail_after_write=f.io.fail_read=0; f.tick(false);
          assert(f.io.value(period())==0.6f && !f.c.has_state()); }
        { Fixture f; f.io.fail_write=period(); f.tick();
          f.io.fail_write=0; f.tick(false); assert(!f.c.has_state()); }
        { Fixture f; f.tick(); f.io.fail_write=period(); f.tick(false);
          assert(f.c.has_state() && f.c.status==viewmodel::Status::Failed);
          f.io.fail_write=0; f.tick(false); assert(!f.c.has_state()); }
        { Fixture f; f.tick(); f.io.fail_read=gun; f.tick(false); assert(f.c.has_state());
          f.io.fail_read=0; f.tick(false); assert(f.io.value(period())==0.6f); }
        { Fixture f; f.tick(); f.io.put<float>(period(),0.4f); f.tick(false);
          assert(f.io.value(period())==0.4f); }
        { Fixture f; f.tick(); f.io.put<float>(period(),0.4f); f.tick(); f.tick(false);
          assert(f.io.value(period())==0.4f); }
        { Fixture f("FPMelee"); f.tick();
          f.io.put<float>(source_period("FPMelee"),0.4f);
          f.io.put<float>(period(),0.4f); f.tick(); f.tick(false);
          assert(f.io.value(source_period("FPMelee"))==0.4f && f.io.value(period())==0.4f); }
        { Fixture f; f.tick(); f.io.put<uint64_t>(gun+MANAGED_CACHED_PTR,0);
          auto writes=f.io.writes; f.tick(false); assert(f.io.writes==writes && !f.c.has_state()); }
        { Fixture f; f.tick(); auto writes=f.io.writes; f.c.tick(2,0x2000000,0,false);
          assert(f.io.writes==writes && !f.c.has_state()); }
        { Fixture f; f.burst(); f.tick(); f.io.put<uint64_t>(gun+FPHITSCAN_BURST_WAIT,0);
          f.tick(false); assert(f.io.value(period())==0.65f);
          assert(f.io.value(wait+WAITFORSECONDS_SECONDS)==fast_fire::kPeriod); }
        { Fixture f; f.tick(); f.io.object(0x60000,"FPBow","Oxide");
          f.io.put<uint64_t>(0x60000+MANAGED_CACHED_PTR,0x690000);
          f.io.put<uint64_t>(0x60000+FPOBJECT_PLAYER_BACKREF,player);
          f.io.put<float>(0x60000+FPWEAPON_SHOT_PERIOD,0.9f);
          f.io.put<float>(0x60000+FPBOW_MIN_SHOT_PERIOD,0.9f);
          f.io.put<uint64_t>(fp+FPMANAGER_CURRENT_OBJECT,0x60000);
          f.io.fail_write=period(); f.tick(); assert(f.c.status==viewmodel::Status::Failed);
          assert(f.io.value(0x60000+FPWEAPON_SHOT_PERIOD)==0.9f);
          f.io.fail_write=0; f.tick(); assert(f.io.value(period())==0.6f);
          assert(f.io.value(0x60000+FPWEAPON_SHOT_PERIOD)==fast_fire::kPeriod);
          assert(f.io.value(0x60000+FPBOW_MIN_SHOT_PERIOD)==fast_fire::kPeriod);
          f.tick(false); assert(f.io.value(0x60000+FPWEAPON_SHOT_PERIOD)==0.9f);
          assert(f.io.value(0x60000+FPBOW_MIN_SHOT_PERIOD)==0.9f); }
    }
    std::puts("fast_fire_memory: all tests passed");
}
