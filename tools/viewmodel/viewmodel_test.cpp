#include "fps_memory.h"
#include "loot_memory.h"
#include "object_data_memory.h"
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
    std::string heap_maps = "150000-260000 rw-p 00000000 00:00 0 [heap]\n";
    uint64_t heap_next = 0x150000;
    uint64_t block_heap_write = 0;    // адрес, на котором write_heap врёт «нельзя»
    bool swallow_heap_writes = false; // write_heap «удачен», но ничего не пишет
    uint64_t next = 0x10000;
    unsigned reads = 0, writes = 0;
    uint64_t failRead = 0, failWrite = 0;
    uint64_t alloc(size_t size = 0x500) {
        uint64_t p = next; next += 0x1000;
        for (size_t i = 0; i < size; ++i) bytes[p+i] = 0;
        return p;
    }
    // Объекты «управляемой кучи»: только сюда пускает гейт write_heap.
    // Шаг округляется вверх под размер, чтобы большие массивы не наезжали
    // на объекты, выделенные следом.
    uint64_t halloc(size_t size = 0x200) {
        const size_t step = (size + 0x3FF) & ~size_t(0x3FF);
        uint64_t p = heap_next; heap_next += step;
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
    // Гейт как у HeapMemory в game_viewmodel.cpp: только анонимная куча.
    template<class T> bool write_heap(uint64_t p, const T& v) {
        if (p == block_heap_write || !object_data::heap_writable(heap_maps, p, sizeof(T)))
            return false;
        if (!swallow_heap_writes) set(p, v);
        return true;
    }
    // Классы для fast_loot::Controller: klass -> указатели на C-строки
    // имени/неймспейса, как в настоящем Il2CppClass (+0x10/+0x18).
    uint64_t cstring(const char* s) {
        uint64_t p = halloc(strlen(s) + 2);
        for (size_t i = 0; i <= strlen(s); ++i) bytes[p+i] = (uint8_t)s[i];
        return p;
    }
    uint64_t klass_of(const char* n, const char* ns) {
        uint64_t k = halloc(0x40);
        set(k + IL2CPP_CLASS_NAME, cstring(n));
        set(k + IL2CPP_CLASS_NAMESPACE, cstring(ns));
        return k;
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
        io.set(idle+TRANSFORMOFFSET_LERP_SPEED,7.f); io.set(run+TRANSFORMOFFSET_LERP_SPEED,9.f);
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
      assert(f.c.status.hands==Status::Active && near(f.position(),Position{0.2f,0.f,0.6f}));
      assert(f.io.get<float>(f.idle+TRANSFORMOFFSET_LERP_SPEED)==10000.f);
      // The real consumer clamps dt * speed; even at 144 FPS the first Update
      // takes the full step rather than interpolating over multiple frames.
      assert(f.io.get<float>(f.idle+TRANSFORMOFFSET_LERP_SPEED)/144.f >= 1.f);
      f.io.set(f.fp+FPMANAGER_AIM_BLEND,0.f); f.io.set(f.fp+FPMANAGER_WORLD_FOV_TARGET,70.f); f.tick(); assert(f.fov()==100.f);
      for (int i=0; i<5; ++i) f.tick();
      assert(f.io.get<float>(f.idle+TRANSFORMOFFSET_LERP_SPEED)==7.f);
      assert(f.io.get<float>(f.run+TRANSFORMOFFSET_LERP_SPEED)==9.f);
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
    { Fixture f; f.tick(); // Do not alter normal hip-fire pose animation.
      assert(f.io.get<float>(f.idle+TRANSFORMOFFSET_LERP_SPEED)==7.f);
      f.io.set(f.aim+ACTIVITY_ACTIVE_FLAG,uint8_t(1)); f.tick();
      f.io.set(f.aim+ACTIVITY_ACTIVE_FLAG,uint8_t(0)); f.tick(); // immediate zero blend exit
      assert(f.io.get<float>(f.idle+TRANSFORMOFFSET_LERP_SPEED)==10000.f);
      f.s.hands_enabled=false; f.io.failWrite=f.idle+TRANSFORMOFFSET_LERP_SPEED; f.tick();
      assert(f.c.status.hands==Status::Failed && same(f.position(),f.origin));
      f.io.failWrite=0; f.tick(); assert(f.io.get<float>(f.idle+TRANSFORMOFFSET_LERP_SPEED)==7.f);
    }
    { Fixture f; f.io.set(f.aim+ACTIVITY_ACTIVE_FLAG,uint8_t(1)); f.tick();
      f.io.set(f.aim+ACTIVITY_ACTIVE_FLAG,uint8_t(0)); f.tick();
      f.io.set(f.idle+TRANSFORMOFFSET_LERP_SPEED,11.f); // game replaces our value
      f.c.reset(true); assert(f.io.get<float>(f.idle+TRANSFORMOFFSET_LERP_SPEED)==11.f);
      assert(f.io.get<float>(f.run+TRANSFORMOFFSET_LERP_SPEED)==9.f);
    }
}
static void config_tests() {
    ViewmodelSettings s{true,true,103.f,10.f,-20.f,30.f}; s.fps_limit=144; auto e=encode(s); auto r=decode(&e,sizeof(e));
    assert(r.fps_limit==144 && r.fov_enabled && r.hands_enabled && r.fov==103.f && r.x==10.f && r.y==-20.f && r.z==30.f);
    for(size_t i=0;i<sizeof(e);++i) { auto d=decode(&e,i); assert(d.fps_limit==0 && !d.fov_enabled && !d.hands_enabled && d.x==0); }
    for (int limit : {0, 90, 120, 144}) {
        s.fps_limit=limit; auto saved=encode(s); auto loaded=decode(&saved,sizeof(saved));
        assert(loaded.fps_limit==limit && loaded.fov_enabled && loaded.hands_enabled);
        assert(loaded.fov==103.f && loaded.x==10.f && loaded.y==-20.f && loaded.z==30.f);
    }
    for (int invalid : {-1, 1, 60, 91, 145, 1000}) {
        s.fps_limit=invalid; assert(sanitize(s).fps_limit==0);
        auto saved=encode(s); assert(decode(&saved,sizeof(saved)).fps_limit==0);
    }
    s.fps_limit=144;
    // v1 instruction/loot flags must never silently activate replacements.
    for (uint32_t flags=0; flags<128; ++flags) {
        auto saved=encode(s); saved.version=1; saved.flags=flags;
        auto loaded=decode(&saved,sizeof(saved));
        assert(loaded.fov_enabled==bool(flags&1) && loaded.hands_enabled==bool(flags&2));
        assert(!loaded.no_recoil && !loaded.no_spread && !loaded.fast_loot);
        const int limits[]={0,144,90,120};
        assert(loaded.fps_limit==limits[(flags>>2)&3]);
        assert(loaded.fov==103.f && loaded.x==10.f && loaded.y==-20.f && loaded.z==30.f);
        assert(encode(loaded).flags==(flags&15u));
    }
    // v2: bit 4 was retired, so an old file carrying it must not gain the new
    // instant-pickup feature.
    for (uint32_t flags=0; flags<128; ++flags) {
        auto saved=encode(s); saved.version=2; saved.flags=flags;
        auto loaded=decode(&saved,sizeof(saved));
        assert(loaded.no_recoil==bool(flags&32) && loaded.no_spread==bool(flags&64));
        assert(!loaded.fast_loot);
        assert(encode(loaded).flags==(flags&~16u));
    }
    // v3: the same bit now means instant pickup, everything else is unchanged.
    for (uint32_t flags=0; flags<128; ++flags) {
        auto saved=encode(s); saved.flags=flags;
        assert(saved.version==3);
        auto loaded=decode(&saved,sizeof(saved));
        assert(loaded.no_recoil==bool(flags&32) && loaded.no_spread==bool(flags&64));
        assert(loaded.fast_loot==bool(flags&16));
        assert(loaded.fov_enabled==bool(flags&1) && loaded.hands_enabled==bool(flags&2));
        assert(encode(loaded).flags==flags);
    }
    for (int limit : {0,90,120,144}) for (bool nr : {false,true}) for (bool ns : {false,true})
        for (bool fl : {false,true}) {
        s.fps_limit=limit; s.no_recoil=nr; s.no_spread=ns; s.fast_loot=fl;
        auto saved=encode(s); auto loaded=decode(&saved,sizeof(saved));
        assert(loaded.no_recoil==nr && loaded.no_spread==ns && loaded.fps_limit==limit);
        assert(loaded.fast_loot==fl);
    }
    s.fast_loot=false;
    for (unsigned bit=7; bit<32; ++bit) {
        auto saved=encode(s); saved.flags=127u | (uint32_t(1)<<bit);
        auto loaded=decode(&saved,sizeof(saved));
        assert(!loaded.fov_enabled && !loaded.hands_enabled && !loaded.no_recoil && !loaded.no_spread && !loaded.fast_loot && loaded.fps_limit==0);
    }
    s.fps_limit=144; s.no_recoil=s.no_spread=false;
    e.flags=7; auto oldFps=decode(&e,sizeof(e));
    assert(!oldFps.no_recoil && !oldFps.no_spread && !oldFps.fast_loot && oldFps.fps_limit==144 && oldFps.fov_enabled && oldFps.hands_enabled);
    e.flags=3; auto legacy=decode(&e,sizeof(e));
    assert(legacy.fov_enabled && legacy.hands_enabled && legacy.fps_limit==0);
    e.version=4; assert(!decode(&e,sizeof(e)).fov_enabled); e=encode(s); e.flags=128; assert(!decode(&e,sizeof(e)).hands_enabled);
    e=encode(s); e.fov=std::numeric_limits<float>::quiet_NaN(); e.x=std::numeric_limits<float>::infinity(); e.y=-1000; e.z=1000;
    auto d=decode(&e,sizeof(e)); assert(d.fov==90 && d.x==0 && d.y==-50 && d.z==50);
}
static void fps_tests() {
    for (const auto& layout : kFpsLayouts) {
        FakeMemory io; FpsController<FakeMemory> c(io);
        const uint64_t base=0x10000000, target=base+layout.target, pace=base+layout.pacing_type;
        io.set(base+0x318,layout.build_id);
        io.set(base+layout.getter,std::array<uint32_t,3>{0xf00083e8,layout.load_instruction,0xd65f03c0});
        io.set(base+layout.pacing_update,uint32_t(0xf81e0ffe));
        io.set(pace,int32_t(1));
        io.set(target,int32_t(60));
        c.tick(1,base,0); assert(io.reads==0 && io.writes==0);
        c.tick(1,base,144);
        assert(c.status==Status::Active && io.get<int32_t>(target)==144 && io.get<int32_t>(pace)==0);
        // Settled mode is read-only: nothing keeps landing in the game process.
        for(int i=0;i<20;++i) c.tick(1,base,144);
        auto count=io.writes; c.tick(1,base,144); c.tick(1,base,144);
        assert(io.writes==count && c.status==Status::Active);
        c.tick(1,base,0); assert(io.get<int32_t>(target)==60 && io.get<int32_t>(pace)==1 && !c.has_state());
        // Switching custom limits must not replace the original snapshot with
        // our previous override. Default restores each real starting value.
        for (int original : {-1, 30, 60, 90, 120, 144, 300}) {
            io.set(target,int32_t(original));
            for (int limit : {90, 120, 144, 120, 90}) {
                c.tick(1,base,limit);
                assert(c.status==Status::Active && io.get<int32_t>(target)==limit && io.get<int32_t>(pace)==0);
            }
            c.tick(1,base,0);
            assert(c.status==Status::Off && !c.has_state() && io.get<int32_t>(target)==original && io.get<int32_t>(pace)==1);
        }
        // The pacing kind from the game is restored, not normalized to native.
        for (int kind : {1, 2}) { io.set(pace,int32_t(kind)); c.tick(1,base,0); assert(io.get<int32_t>(pace)==kind); }
        // A failed mode switch or reset retains the same original for retry.
        io.set(target,int32_t(60)); c.tick(1,base,90);
        io.failWrite=target; c.tick(1,base,120); assert(c.status==Status::Failed);
        io.failWrite=0; c.tick(1,base,120); assert(io.get<int32_t>(target)==120);
        io.failRead=target; c.tick(1,base,0); assert(c.status==Status::Failed && c.has_state());
        io.failRead=0; c.tick(1,base,0); assert(io.get<int32_t>(target)==60);
        c.tick(1,base,90); c.tick(1,base,91); // invalid request is fail-closed/default
        assert(c.status==Status::Off && io.get<int32_t>(target)==60);
        io.set(pace,int32_t(1)); // восстановленный игровой kind носит прежнее число
        for (int limit : {90, 120, 144}) {
            io.set(target,int32_t(-1)); c.tick(1,base,limit); c.reset(true);
            assert(io.get<int32_t>(target)==-1 && io.get<int32_t>(pace)==1);
        }
        // An unknown pacing kind is fail-closed: cap still applies, status says
        // the pacing half never did. The foreign value is never overwritten.
        io.set(pace,int32_t(7)); c.tick(1,base,90);
        assert(c.status==Status::Failed && io.get<int32_t>(target)==90 && io.get<int32_t>(pace)==7);
        io.set(pace,int32_t(1)); c.reset(true);
        io.set(target,int32_t(60)); c.tick(1,base,144);
        io.set(target,int32_t(300)); c.tick(1,base,144); c.tick(1,base,0);
        assert(io.get<int32_t>(target)==300); // new graphics preset, not obsolete 60
        c.tick(1,base,144); io.set(target,int32_t(90)); c.tick(1,base,0);
        assert(io.get<int32_t>(target)==90); // never clobber a newer game write
        c.tick(1,base,144); io.failWrite=target; c.tick(1,base,0);
        assert(c.has_state() && c.status==Status::Failed);
        io.failWrite=0; c.tick(1,base,0); assert(io.get<int32_t>(target)==90);
        io.failRead=target; count=io.writes; c.tick(1,base,144);
        assert(c.status==Status::Failed && io.writes==count); io.failRead=0;
        io.set(target,int32_t(0x12345678)); c.tick(1,base,144); assert(io.writes==count);
        io.set(target,int32_t(60)); c.tick(1,base,144); count=io.writes;
        io.set(base+0x318,uint64_t(0)); c.tick(1,base,0); assert(io.writes==count);
        c.reset(true); assert(io.writes==count); // unknown engine: no restore writes
        io.set(base+0x318,layout.build_id); io.set(base+layout.getter,uint32_t(0));
        c.tick(1,base,144); assert(io.writes==count); // code fingerprint mismatch
        io.set(base+layout.getter,std::array<uint32_t,3>{0xf00083e8,layout.load_instruction,0xd65f03c0});
        io.set(base+layout.pacing_update,uint32_t(0));
        c.tick(1,base,144); assert(io.writes==count); // pacing fingerprint mismatch
        io.set(base+layout.pacing_update,uint32_t(0xf81e0ffe));
        c.tick(1,base,144); count=io.writes; c.tick(2,0,144);
        assert(io.writes==count && !c.has_state()); // lost session: forget
    }
}
// ---- Быстрый сбор: fast_loot::Controller на фейковой памяти --------
// Проверяется вся цепочка до обещания и единственная запись (float темпа),
// включая отказы: чужая длительность, чужие классы, не-куча, сессии.
struct LootFixture {
    FakeMemory io;
    fast_loot::Controller<FakeMemory> c{io};
    const uint64_t base = 0x10000000;
    uint64_t helper_class = 0, statics = 0, runners = 0, promise_klass = 0,
             scaled_klass = 0, other_klass = 0;
    std::vector<uint64_t> items_arrays;   // loopItems каждого раннера (для add_item)
    std::vector<uint64_t> runner_objs;    // сами раннеры (для tail)
    LootFixture(int nrunners = 2, bool valid = true) {
        helper_class = hklass(valid ? "PlayerLoopHelper" : "Wrong",
                              "Cysharp.Threading.Tasks");
        io.set(base + PLAYERLOOPHELPER_TYPEINFO_RVA, helper_class);
        statics = io.halloc();
        io.set(helper_class + IL2CPP_CLASS_STATIC_FIELDS, statics);
        promise_klass = hklass("DelayIgnoreTimeScalePromise", "");
        scaled_klass = hklass("DelayPromise", "");
        other_klass = hklass("SomethingElse", "");
        runners = io.halloc();
        io.set(statics + PLAYERLOOPHELPER_RUNNERS, runners);
        io.set(runners + IL2CPP_ARRAY_LENGTH, int32_t(nrunners));
        for (int t = 0; t < nrunners; ++t) {
            uint64_t runner = io.halloc();
            io.set(runner, hklass("PlayerLoopRunner", "Cysharp.Threading.Tasks.Internal"));
            // С запасом под сотни элементов: массив не должен наезжать на
            // объекты, выделенные следом.
            uint64_t items = io.halloc(0x20 + 2048 * 8);
            io.set(runner + LOOPRUNNER_ITEMS, items);
            io.set(items + IL2CPP_ARRAY_LENGTH, int32_t(0));
            io.set(runner + LOOPRUNNER_TAIL, int32_t(0));
            io.set(runners + IL2CPP_ARRAY_FIRST_ELEMENT + (uint64_t)t * 8, runner);
            items_arrays.push_back(items);
            runner_objs.push_back(runner);
        }
    }
    uint64_t hklass(const char* n, const char* ns) { return io.klass_of(n, ns); }
    uint64_t add_item(int runner, uint64_t klass) {
        uint64_t items = items_arrays[runner];
        int32_t n = io.get<int32_t>(items + IL2CPP_ARRAY_LENGTH);
        io.set(items + IL2CPP_ARRAY_FIRST_ELEMENT + (uint64_t)n * 8, io.halloc());
        io.set(io.ptr(items + IL2CPP_ARRAY_FIRST_ELEMENT + (uint64_t)n * 8), klass);
        io.set(items + IL2CPP_ARRAY_LENGTH, int32_t(n + 1));
        io.set(runner_objs[runner] + LOOPRUNNER_TAIL, int32_t(n + 1));
        return io.ptr(items + IL2CPP_ARRAY_FIRST_ELEMENT + (uint64_t)n * 8);
    }
    uint64_t add_promise(int runner, float span, float elapsed = 0.01f,
                         int32_t frame = 100) {
        uint64_t p = add_item(runner, promise_klass);
        io.set(p + DELAYPROMISE_TIMESPAN, span);
        io.set(p + DELAYPROMISE_ELAPSED, elapsed);
        io.set(p + DELAYPROMISE_INITIAL_FRAME, frame);
        return p;
    }
    // Обычный (time-scaled) DelayPromise: тройка полей сдвинута на 4 байта.
    uint64_t add_scaled(int runner, float span, float elapsed = 0.01f,
                        int32_t frame = 100) {
        uint64_t p = add_item(runner, scaled_klass);
        io.set(p + DELAYPROMISE_SCALED_TIMESPAN, span);
        io.set(p + DELAYPROMISE_SCALED_ELAPSED, elapsed);
        io.set(p + DELAYPROMISE_SCALED_INITIAL_FRAME, frame);
        return p;
    }
    void tick(bool on = true) { c.tick(1, base, on); }
};
static void loot_tests() {
    using fast_loot::kCadenceBits;
    {   // Счастливый путь: два обещания темпа 0.35 обнулены, чужие не тронуты.
        LootFixture f;
        uint64_t a = f.add_promise(0, 0.35f);
        uint64_t b = f.add_item(0, f.other_klass);      // не обещание
        f.io.set(b + DELAYPROMISE_TIMESPAN, 0.35f);      // ...и всё равно мимо
        uint64_t d = f.add_promise(1, 0.35f);
        f.tick();
        assert(f.c.status == Status::Active && f.c.neutralized() == 2);
        assert(f.io.get<uint32_t>(a + DELAYPROMISE_TIMESPAN) == 0u);
        assert(f.io.get<uint32_t>(d + DELAYPROMISE_TIMESPAN) == 0u);
        assert(f.io.get<uint32_t>(b + DELAYPROMISE_TIMESPAN) == kCadenceBits);
        // Выключение: восстановлений нет — поле дальше пишет сама игра.
        f.tick(false);
        assert(f.c.status == Status::Off);
        assert(f.io.get<uint32_t>(a + DELAYPROMISE_TIMESPAN) == 0u);
    }
    {   // Time-scaled DelayPromise: та же побитовая сверка темпа, но поля
        // сдвинуты (span 0x1C/elapsed 0x20/frame 0x18) — запись идёт в своё
        // поле и не портит соседние.
        LootFixture f;
        uint64_t s = f.add_scaled(0, 0.35f);
        uint64_t i = f.add_promise(0, 0.35f);
        f.tick();
        assert(f.c.status == Status::Active && f.c.neutralized() == 2);
        assert(f.io.get<uint32_t>(s + DELAYPROMISE_SCALED_TIMESPAN) == 0u);
        assert(f.io.get<uint32_t>(i + DELAYPROMISE_TIMESPAN) == 0u);
        // Соседние поля не задеты: elapsed/frame на месте у обоих классов.
        assert(f.io.get<float>(s + DELAYPROMISE_SCALED_ELAPSED) == 0.01f);
        assert(f.io.get<int32_t>(i + DELAYPROMISE_INITIAL_FRAME) == 100);
        // Чужой темп у scaled — не трогаем.
        uint64_t s2 = f.add_scaled(0, 0.5f);
        f.tick();
        assert(f.io.get<uint32_t>(s2 + DELAYPROMISE_SCALED_TIMESPAN) == 0x3f000000u);
    }
    {   // Чужие длительности/значения — не трогаем даже у настоящего обещания.
        LootFixture f;
        uint64_t a = f.add_promise(0, 0.5f);
        uint64_t b = f.add_promise(0, 0.f);
        uint64_t nan = f.add_promise(0, std::numeric_limits<float>::quiet_NaN());
        uint64_t inf = f.add_promise(0, std::numeric_limits<float>::infinity());
        uint64_t big = f.add_promise(0, 3.5f);
        f.tick();
        assert(f.c.status == Status::Active && f.c.neutralized() == 0);
        assert(f.io.get<float>(a + DELAYPROMISE_TIMESPAN) == 0.5f);
        assert(f.io.get<float>(b + DELAYPROMISE_TIMESPAN) == 0.f);
        assert(!viewmodel::finite(f.io.get<float>(nan + DELAYPROMISE_TIMESPAN)));
        // NaN/Inf сверяются по битам: сборка Андроида идёт с -ffast-math
        // (-ffinite-math-only), при котором сравнение «x == infinity()»
        // компилятор вправе свернуть в false (на g++ >= 13 так и происходит).
        assert(f.io.get<uint32_t>(nan + DELAYPROMISE_TIMESPAN) == 0x7fc00000u);
        assert(f.io.get<uint32_t>(inf + DELAYPROMISE_TIMESPAN) == 0x7f800000u);
        assert(f.io.get<float>(big + DELAYPROMISE_TIMESPAN) == 3.5f);
    }
    {   // Невразумительные elapsed/initialFrame — пропуск без записи.
        LootFixture f;
        uint64_t a = f.add_promise(0, 0.35f, std::numeric_limits<float>::quiet_NaN());
        uint64_t b = f.add_promise(0, 0.35f, 0.4f);
        uint64_t d = f.add_promise(0, 0.35f, 0.01f, -5);
        f.tick();
        assert(f.c.neutralized() == 0);
        assert(f.io.get<uint32_t>(a + DELAYPROMISE_TIMESPAN) == kCadenceBits);
        assert(f.io.get<uint32_t>(b + DELAYPROMISE_TIMESPAN) == kCadenceBits);
        assert(f.io.get<uint32_t>(d + DELAYPROMISE_TIMESPAN) == kCadenceBits);
    }
    {   // Не тот билд: слот читается, но класс чужой — Failed, без записей.
        LootFixture f(1, false);
        f.add_promise(0, 0.35f);
        f.tick();
        assert(f.c.status == Status::Failed && f.c.neutralized() == 0);
    }
    {   // Слоты ещё не инициализированы — Waiting, без записей.
        LootFixture f;
        f.add_promise(0, 0.35f);
        f.io.set(f.base + PLAYERLOOPHELPER_TYPEINFO_RVA, uint64_t(0));
        f.tick(); assert(f.c.status == Status::Waiting && f.c.neutralized() == 0);
        f.io.set(f.base + PLAYERLOOPHELPER_TYPEINFO_RVA, f.helper_class);
        f.io.set(f.statics + PLAYERLOOPHELPER_RUNNERS, uint64_t(0));
        f.tick(); assert(f.c.status == Status::Waiting);
        f.io.set(f.statics + PLAYERLOOPHELPER_RUNNERS, f.runners);
        f.io.set(f.runners + IL2CPP_ARRAY_LENGTH, int32_t(0));
        f.tick(); assert(f.c.status == Status::Waiting);
    }
    {   // Раннер с чужим классом: его элементы не рассматриваются вовсе.
        LootFixture f;
        // У первого раннера подменяем класс: его элементы не рассматриваются.
        uint64_t wrong = f.io.klass_of("WrongRunner", "");
        f.io.set(f.io.ptr(f.runners + IL2CPP_ARRAY_FIRST_ELEMENT), wrong);
        f.add_promise(0, 0.35f);
        uint64_t ok = f.add_promise(1, 0.35f);
        f.tick();
        assert(f.c.neutralized() == 1);
        assert(f.io.get<uint32_t>(ok + DELAYPROMISE_TIMESPAN) == 0u);
    }
    {   // Гейт кучи: обещание вне анонимной кучи — запись запрещена, Failed.
        LootFixture f;
        uint64_t outside = f.io.alloc();
        f.io.set(outside, f.promise_klass);
        f.io.set(outside + DELAYPROMISE_TIMESPAN, 0.35f);
        f.io.set(outside + DELAYPROMISE_ELAPSED, 0.01f);
        f.io.set(outside + DELAYPROMISE_INITIAL_FRAME, int32_t(100));
        f.io.set(f.items_arrays[0] + IL2CPP_ARRAY_FIRST_ELEMENT, outside);
        f.io.set(f.items_arrays[0] + IL2CPP_ARRAY_LENGTH, int32_t(1));
        f.io.set(f.runner_objs[0] + LOOPRUNNER_TAIL, int32_t(1));
        f.tick();
        assert(f.c.status == Status::Failed && f.c.neutralized() == 0);
        assert(f.io.get<uint32_t>(outside + DELAYPROMISE_TIMESPAN) == kCadenceBits);
    }
    {   // tail < длины массива (ёмкость): за tail лежат устаревшие указатели
        // после компакции RunCore — обещание в мёртвом слоте не трогается,
        // даже если это переиспользованный пулом объект с темпом 0.35.
        LootFixture f;
        uint64_t live = f.add_promise(0, 0.35f);
        uint64_t stale = f.add_promise(0, 0.35f);
        // RunCore «удалил» второй: tail=1, но слот и длина остались.
        f.io.set(f.runner_objs[0] + LOOPRUNNER_TAIL, int32_t(1));
        f.tick();
        assert(f.c.status == Status::Active && f.c.neutralized() == 1);
        assert(f.io.get<uint32_t>(live + DELAYPROMISE_TIMESPAN) == 0u);
        assert(f.io.get<uint32_t>(stale + DELAYPROMISE_TIMESPAN) == kCadenceBits);
        // tail испорчен (отрицательный/больше длины) — откат к длине массива:
        // живой элемент в слоте [0] всё ещё обрабатывается.
        LootFixture f2;
        uint64_t a = f2.add_promise(0, 0.35f);
        f2.io.set(f2.runner_objs[0] + LOOPRUNNER_TAIL, int32_t(-7));
        f2.tick();
        assert(f2.c.neutralized() == 1);
        assert(f2.io.get<uint32_t>(a + DELAYPROMISE_TIMESPAN) == 0u);
    }
    {   // Запись «удачна», но память не изменилась (между нами писали/отворили
        // доступ) — перечитывание обязано это поймать: Failed, без счёта.
        LootFixture f;
        uint64_t a = f.add_promise(0, 0.35f);
        f.io.swallow_heap_writes = true;
        f.tick();
        assert(f.c.status == Status::Failed && f.c.neutralized() == 0);
        assert(f.io.get<uint32_t>(a + DELAYPROMISE_TIMESPAN) == kCadenceBits);
    }
    {   // Смена сессии: адреса забыты, всё резолвится заново и работает.
        LootFixture f;
        f.add_promise(0, 0.35f);
        f.c.tick(7, f.base, true);
        assert(f.c.status == Status::Active && f.c.neutralized() == 1);
        f.add_promise(0, 0.35f);
        f.c.tick(8, f.base, true);
        assert(f.c.status == Status::Active && f.c.neutralized() == 2);
    }
    {   // Кэш классов: после первого тика имена уже не нужны — вердикт по
        // указателю класса (переименование ничего не ломает).
        LootFixture f;
        f.add_promise(0, 0.35f);
        f.tick();
        assert(f.c.neutralized() == 1);
        // После первого тика вердикт по указателю класса уже в кэше: даже
        // «испорченные» строки имён не влияют на следующий проход.
        f.io.set(f.promise_klass + IL2CPP_CLASS_NAME, uint64_t(0));
        f.add_promise(0, 0.35f);
        f.tick();
        assert(f.c.neutralized() == 2);
    }
    {   // Бюджет тика: 600 обещаний в одном раннере — второй раннер не
        // голодает вечно благодаря вращению начала обхода.
        LootFixture f;
        for (int i = 0; i < 600; ++i) f.add_promise(0, 0.35f);
        uint64_t late = f.add_promise(1, 0.35f);
        for (int i = 0; i < 8; ++i) f.tick();
        assert(f.c.status == Status::Active);
        assert(f.io.get<uint32_t>(late + DELAYPROMISE_TIMESPAN) == 0u);
        assert(f.c.neutralized() == 601);
    }
    {   // Пустые раннеры и нулевые слоты — нормальная тишина, не Failed.
        LootFixture f(4);
        f.tick(); f.tick();
        assert(f.c.status == Status::Active && f.c.neutralized() == 0);
    }
    {   // Тегированный указатель класса (TBI/MTE) снимается маской.
        LootFixture f;
        f.add_promise(0, 0.35f);
        uint64_t items = f.items_arrays[0];
        f.io.set(items + IL2CPP_ARRAY_FIRST_ELEMENT,
                 f.io.ptr(items + IL2CPP_ARRAY_FIRST_ELEMENT) | 0xAB00000000000000ull);
        f.tick();
        assert(f.c.status == Status::Active && f.c.neutralized() == 1);
    }
}

int main() {
    config_tests(); fps_tests();
    go::SelectBuild(go::Build::Release); tests(); loot_tests();
    assert(go::BetaAvailable()); go::SelectBuild(go::Build::Beta); tests(); loot_tests();
    std::cout << "viewmodel: release/beta, lifecycle, instant ADS return, FPS90/120/144, delegates, fast pickup, config, NaN: OK\n";
}
