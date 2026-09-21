#include "viewmodel_memory.h"
#include <cassert>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <vector>

using namespace go_active;
using namespace viewmodel;
struct FakeMemory {
    std::map<uint64_t, uint8_t> bytes;
    std::map<uint64_t, std::string> names;
    uint64_t next = 0x10000;
    unsigned reads = 0, writes = 0;
    uint64_t failRead = 0, failWrite = 0;
    uint64_t alloc(size_t size = 0x500) {
        uint64_t p = next; next += 0x1000;
        for (size_t i = 0; i < size; ++i) bytes[p+i] = 0;
        return p;
    }
    template<class T> bool read(uint64_t p, T& v) {
        ++reads; if (p == failRead) return false;
        auto out = reinterpret_cast<uint8_t*>(&v);
        for (size_t i=0; i<sizeof(T); ++i) { auto it=bytes.find(p+i); if(it==bytes.end()) return false; out[i]=it->second; }
        return true;
    }
    template<class T> void set(uint64_t p, T v) {
        auto in = reinterpret_cast<const uint8_t*>(&v);
        for (size_t i=0; i<sizeof(T); ++i) bytes[p+i]=in[i];
    }
    template<class T> bool write(uint64_t p, const T& v) {
        if (p==failWrite) return false;
        ++writes; set(p,v); return true;
    }
    uint64_t ptr(uint64_t p) { uint64_t v=0; return read(p,v)?v:0; }
    bool is(uint64_t p, const char* name) { return names[p]==name; }
    uint64_t object(const char* name) {
        uint64_t p=alloc(); names[p]=name; set(p,alloc()); set(p+MANAGED_CACHED_PTR,alloc()); return p;
    }
    template<class T> T get(uint64_t p) { T v{}; assert(read(p,v)); return v; }
};
struct Fixture {
    FakeMemory io;
    Controller<FakeMemory> c{io};
    uint64_t player=io.object("PlayerManager"), fp=io.object("FPManager"), handler=io.object("Handler"), aim=io.alloc();
    uint64_t weapon=io.object("FPHitscan"), motion=io.object("FPMotion"), event=io.alloc(), action=io.alloc();
    uint64_t idle=io.object("TransformOffset"), run=io.object("TransformOffset");
    ViewmodelSettings s{true,true,100.f,10.f,-20.f,30.f};
    const Position origin{0.1f,0.2f,0.3f};
    Fixture() {
        io.set(player+PLAYER_FP_MANAGER,fp); io.set(player+PLAYER_EVENT_HANDLER,handler);
        io.set(handler+EVENT_HANDLER_MANAGER_BACKREF,player); io.set(handler+EVENT_HANDLER_AIM_ACTIVITY,aim);
        io.set(fp+FPMANAGER_AIM_BLEND,0.f); io.set(fp+FPMANAGER_WORLD_FOV_TARGET,70.f);
        io.set(fp+FPMANAGER_CURRENT_OBJECT,weapon); io.set(weapon+FPOBJECT_PLAYER_BACKREF,player);
        io.set(weapon+FPOBJECT_DRAW_EVENT,event); io.set(event+FPDRAW_ACTION,action);
        io.set(action+DELEGATE_TARGET,motion); io.set(motion+FPMOTION_OBJECT,weapon); io.set(motion+FPMOTION_PLAYER,handler);
        io.set(motion+FPMOTION_IDLE_OFFSET,idle); io.set(motion+FPMOTION_RUN_OFFSET,run);
        io.set(idle+TRANSFORMOFFSET_POSITION,origin); io.set(run+TRANSFORMOFFSET_POSITION,origin);
    }
    void tick() { c.tick(1,player,s); }
    Position position() { return io.get<Position>(idle+TRANSFORMOFFSET_POSITION); }
    float fov() { return io.get<float>(fp+FPMANAGER_WORLD_FOV_TARGET); }
};
static bool near(float a, float b) { return a-b < 1e-6f && b-a < 1e-6f; }
static bool near(Position a, Position b) { return near(a.x,b.x) && near(a.y,b.y) && near(a.z,b.z); }
static void tests() {
    { Fixture f; f.tick(); assert(f.c.status.fov==Status::Active && f.c.status.hands==Status::Active);
      Position expected{0.2f,0.f,0.6f}; assert(near(f.position(),expected)); assert(f.fov()==100.f);
      expected=f.position();
      for(int i=0;i<200;++i) { f.tick(); }
      assert(same(f.position(),expected)); // no drift
      f.s.x=20.f; f.tick(); assert(near(f.position().x,0.3f));
      f.s.fov_enabled=f.s.hands_enabled=false; f.tick(); assert(same(f.position(),f.origin)); assert(f.fov()==70.f);
    }
    { Fixture f; f.tick(); f.io.set(f.aim+ACTIVITY_ACTIVE_FLAG,uint8_t(1));
      f.io.set(f.fp+FPMANAGER_WORLD_FOV_TARGET,25.f); f.tick();
      assert(f.fov()==25.f); assert(same(f.position(),f.origin)); assert(f.c.status.fov==Status::ADS);
      f.io.set(f.aim+ACTIVITY_ACTIVE_FLAG,uint8_t(0)); f.io.set(f.fp+FPMANAGER_AIM_BLEND,0.5f); f.tick(); assert(f.fov()==25.f);
      f.io.set(f.fp+FPMANAGER_AIM_BLEND,0.f); f.io.set(f.fp+FPMANAGER_WORLD_FOV_TARGET,70.f); f.tick(); assert(f.fov()==100.f);
    }
    { Fixture f; f.tick(); f.io.set(f.fp+FPMANAGER_WORLD_FOV_TARGET,80.f);
      Position newOrigin{0.4f,0.5f,0.6f}; f.io.set(f.idle+TRANSFORMOFFSET_POSITION,newOrigin); f.tick();
      f.s={}; f.tick(); assert(f.fov()==80.f); assert(same(f.position(),newOrigin));
    }
    { Fixture f; auto list=f.io.alloc(), other=f.io.alloc(), root=f.io.alloc();
      f.io.set(f.event+FPDRAW_ACTION,root); f.io.set(root+MULTICAST_DELEGATES,list);
      f.io.set(list+IL2CPP_ARRAY_LENGTH,uint64_t(2)); f.io.set(list+IL2CPP_ARRAY_FIRST_ELEMENT,other);
      f.io.set(list+IL2CPP_ARRAY_FIRST_ELEMENT+8,f.action); f.tick(); assert(f.c.status.hands==Status::Active);
    }
    { Fixture f; auto list=f.io.alloc(); f.io.set(f.action+DELEGATE_TARGET,uint64_t(0));
      f.io.set(f.action+MULTICAST_DELEGATES,list); f.io.set(list+IL2CPP_ARRAY_LENGTH,uint64_t(1));
      f.io.set(list+IL2CPP_ARRAY_FIRST_ELEMENT,f.action); f.tick(); assert(f.c.status.hands==Status::Waiting); assert(f.io.reads<200);
    }
    { Fixture f; f.io.set(f.motion+FPMOTION_RUN_OFFSET,f.idle); f.tick(); auto before=f.position();
      f.tick(); assert(same(before,f.position())); f.s={}; f.tick(); assert(same(f.position(),f.origin));
    }
    { Fixture f; f.io.set(f.motion+FPMOTION_IDLE_OFFSET,uint64_t(0)); f.tick();
      auto before=f.io.get<Position>(f.run+TRANSFORMOFFSET_POSITION);
      f.io.set(f.motion+FPMOTION_IDLE_OFFSET,f.run); f.tick();
      assert(same(before,f.io.get<Position>(f.run+TRANSFORMOFFSET_POSITION)));
      f.s={}; f.tick(); assert(same(f.origin,f.io.get<Position>(f.run+TRANSFORMOFFSET_POSITION)));
    }
    { Fixture f; f.s.hands_enabled=false; f.tick(); assert(f.fov()==100.f && same(f.position(),f.origin)); }
    { Fixture f; f.s.fov_enabled=false; f.tick(); assert(f.fov()==70.f && f.c.status.hands==Status::Active); }
    { Fixture f; f.tick(); auto newWeapon=f.io.object("FPBow");
      f.io.set(newWeapon+FPOBJECT_PLAYER_BACKREF,f.player); f.io.set(f.fp+FPMANAGER_CURRENT_OBJECT,newWeapon); f.tick();
      assert(same(f.position(),f.origin)); assert(f.c.status.hands==Status::Waiting);
    }
    { Fixture f; f.tick(); f.io.set(f.motion+MANAGED_CACHED_PTR,uint64_t(0)); auto count=f.io.writes;
      f.s={}; f.tick(); assert(f.io.writes==count+1); // only live FPManager restored
    }
    { Fixture f; f.tick(); auto count=f.io.writes; f.c.tick(2,0,f.s); assert(count==f.io.writes); // rebind must not restore old addresses
    }
    { Fixture f; f.tick(); auto count=f.io.writes; f.c.tick(1,0,f.s); assert(count==f.io.writes); // scene/local identity disappeared
    }
    { Fixture f; f.io.names[f.motion]="WrongClass"; f.tick(); assert(f.c.status.hands==Status::Waiting); assert(same(f.position(),f.origin)); }
    { Fixture f; f.io.set(f.motion+FPMOTION_OBJECT,f.fp); f.tick(); assert(f.c.status.hands==Status::Waiting); }
    { Fixture f; f.io.failRead=f.aim+ACTIVITY_ACTIVE_FLAG; f.tick(); assert(f.io.writes==0); }
    { Fixture f; f.tick(); f.s={}; f.io.failRead=f.player+PLAYER_FP_MANAGER; f.tick();
      assert(f.c.has_state()); // failed lookup must not discard pending originals
      f.io.failRead=0; f.tick(); assert(!f.c.has_state()); assert(f.fov()==70.f); assert(same(f.position(),f.origin));
    }
    { Fixture f; f.io.failWrite=f.idle+TRANSFORMOFFSET_POSITION; f.tick(); assert(f.c.status.hands==Status::Failed);
      f.io.failWrite=0; f.tick(); assert(f.c.status.hands==Status::Active);
      f.s={}; f.io.failWrite=f.idle+TRANSFORMOFFSET_POSITION; f.tick(); assert(f.c.status.hands==Status::Failed);
      f.io.failWrite=0; f.tick(); assert(same(f.position(),f.origin));
    }
    { Fixture f; f.io.set(f.fp+FPMANAGER_WORLD_FOV_TARGET,std::numeric_limits<float>::quiet_NaN()); f.tick(); assert(f.c.status.fov==Status::Failed); }
    { Fixture f; f.io.set(f.idle+TRANSFORMOFFSET_POSITION,Position{std::numeric_limits<float>::infinity(),0,0}); f.tick(); assert(f.c.status.hands==Status::Failed); }
    { Fixture f; f.tick(); f.c.reset(true); assert(same(f.position(),f.origin)); assert(f.fov()==70.f); }
}
static void config_tests() {
    ViewmodelSettings s{true,true,103.f,10.f,-20.f,30.f}; auto e=encode(s); auto r=decode(&e,sizeof(e));
    assert(r.fov_enabled && r.hands_enabled && r.fov==103.f && r.x==10.f && r.y==-20.f && r.z==30.f);
    for(size_t i=0;i<sizeof(e);++i) { auto d=decode(&e,i); assert(!d.fov_enabled && !d.hands_enabled && d.x==0); }
    e.version=2; assert(!decode(&e,sizeof(e)).fov_enabled); e=encode(s); e.flags=8; assert(!decode(&e,sizeof(e)).hands_enabled);
    e=encode(s); e.fov=std::numeric_limits<float>::quiet_NaN(); e.x=std::numeric_limits<float>::infinity(); e.y=-1000; e.z=1000;
    auto d=decode(&e,sizeof(e)); assert(d.fov==90 && d.x==0 && d.y==-50 && d.z==50);
}
int main() {
    config_tests();
    go::SelectBuild(go::Build::Release); tests();
    assert(go::BetaAvailable()); go::SelectBuild(go::Build::Beta); tests();
    std::cout << "viewmodel: release/beta, lifecycle, ADS, delegates, config, NaN: OK\n";
}
