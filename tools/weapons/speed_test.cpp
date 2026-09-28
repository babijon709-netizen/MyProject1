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
    bool fail_write=false; uint64_t fail_read=0; int writes=0;
    template<class T> void set(uint64_t a,const T& v){const auto*p=(const uint8_t*)&v;for(size_t i=0;i<sizeof(v);++i)bytes[a+i]=p[i];}
    template<class T> T get(uint64_t a){T v{};assert(read(a,v));return v;}
    template<class T> bool read(uint64_t a,T& v){if(a==fail_read)return false;auto*p=(uint8_t*)&v;for(size_t i=0;i<sizeof(v);++i){auto it=bytes.find(a+i);if(it==bytes.end())return false;p[i]=it->second;}return true;}
    bool named(uint64_t o,const char*n,const char*ns){return names[o]==n&&spaces[o]==ns;}
    template<class T> bool write_heap(uint64_t a,const T& v){++writes;if(fail_write)return false;set(a,v);return true;}
    void object(uint64_t o,uint64_t k,const char*n,const char*ns,bool unity=true){set(o,k);if(unity)set(o+0x10,o+0x9000);names[o]=n;spaces[o]=ns;}
};
struct Fixture {
    FakeMemory io; weapon_speed::Controller<FakeMemory> c;
    uint64_t base=0x100000,player=0x20000,net=0x21000,fp=0x22000,weapon=0x23000,config=0x24000;
    Fixture():c(io){
        io.set(base+0x318,weapon_speed::kLayouts[0].build_id);
        io.object(player,0x30000,"PlayerManager","Oxide");
        io.object(net,0x31000,"NetworkIdentity","Mirror"); uint8_t local=1; io.set(net+0x4a,local); io.set(player+0x40,net);
        io.object(fp,0x32000,"FPManager","Oxide"); io.set(player+go_active::PLAYER_FP_MANAGER,fp);
        io.object(weapon,0x33000,"FPHitscan","Oxide"); io.set(fp+go_active::FPMANAGER_CURRENT_WEAPON,weapon);
        io.set(weapon+go_active::FPOBJECT_PLAYER_BACKREF,player);
        io.object(config,0x34000,"HitscanWeaponConfig",weapon_speed::kWeapons);
        io.set(weapon+go_active::FPHITSCAN_CONFIG,config);
        io.set(config+go_active::HITSCANCONFIG_FIRE_RATE,5.f);
        io.set(config+go_active::HITSCANCONFIG_FULL_RELOAD,4.f);
        io.set(config+go_active::HITSCANCONFIG_LOOP_RELOAD,2.f);
    }
    void tick(bool fire=true,bool reload=true,float fm=3,float rm=4){c.tick(1,base,player,fire,reload,fm,rm);}
};
int main(){
    using viewmodel::Status;
    { Fixture f; f.tick(); assert(f.c.fire==Status::Active&&f.c.reload==Status::Active); assert(f.io.get<float>(f.config+go_active::HITSCANCONFIG_FIRE_RATE)==15.f); assert(f.io.get<float>(f.config+go_active::HITSCANCONFIG_FULL_RELOAD)==1.f); assert(f.io.get<float>(f.config+go_active::HITSCANCONFIG_LOOP_RELOAD)==.5f); f.c.reset(true); assert(f.io.get<float>(f.config+go_active::HITSCANCONFIG_FIRE_RATE)==5.f); assert(f.io.get<float>(f.config+go_active::HITSCANCONFIG_FULL_RELOAD)==4.f); }
    { Fixture f; f.tick(); f.tick(false,true); assert(f.io.get<float>(f.config+go_active::HITSCANCONFIG_FIRE_RATE)==5.f); assert(f.io.get<float>(f.config+go_active::HITSCANCONFIG_FULL_RELOAD)==1.f); }
    { Fixture f; f.tick(); f.io.set(f.config+go_active::HITSCANCONFIG_FIRE_RATE,7.f); f.tick(true,false); assert(f.io.get<float>(f.config+go_active::HITSCANCONFIG_FIRE_RATE)==21.f); f.c.reset(true); assert(f.io.get<float>(f.config+go_active::HITSCANCONFIG_FIRE_RATE)==7.f); }
    { Fixture f; f.tick(); uint64_t neww=0x25000,newc=0x26000; f.io.object(neww,0x35000,"FPHitscan","Oxide"); f.io.object(newc,0x36000,"HitscanWeaponConfig",weapon_speed::kWeapons); f.io.set(neww+go_active::FPOBJECT_PLAYER_BACKREF,f.player); f.io.set(neww+go_active::FPHITSCAN_CONFIG,newc); f.io.set(newc+go_active::HITSCANCONFIG_FIRE_RATE,2.f); f.io.set(newc+go_active::HITSCANCONFIG_FULL_RELOAD,8.f); f.io.set(newc+go_active::HITSCANCONFIG_LOOP_RELOAD,4.f); f.io.set(f.fp+go_active::FPMANAGER_CURRENT_WEAPON,neww); f.tick(); assert(f.io.get<float>(f.config+go_active::HITSCANCONFIG_FIRE_RATE)==5.f); assert(f.io.get<float>(newc+go_active::HITSCANCONFIG_FIRE_RATE)==6.f); }
    { Fixture f; f.tick(); f.tick(true,false); f.io.fail_write=true; f.io.set(f.config+go_active::HITSCANCONFIG_FIRE_RATE,5.f); f.tick(true,false); assert(f.c.fire==Status::Failed); }
    { Fixture f; f.tick(); f.io.fail_read=f.config; f.tick(false,false); assert(f.c.fire==Status::Failed&&f.c.reload==Status::Failed&&f.c.has_state()); f.io.fail_read=0; f.tick(false,false); assert(!f.c.has_state()); assert(f.io.get<float>(f.config+go_active::HITSCANCONFIG_FIRE_RATE)==5.f); }
    { Fixture f; std::array<uint8_t,20> bad{}; f.io.set(f.base+0x318,bad); f.tick(); assert(f.c.fire==Status::Failed&&f.c.reload==Status::Failed&&f.io.writes==0); }
    { Fixture f; f.tick(); f.io.set(f.config,uint64_t(0xdead0)); int before=f.io.writes; f.c.reset(true); assert(f.io.writes==before); }
}
