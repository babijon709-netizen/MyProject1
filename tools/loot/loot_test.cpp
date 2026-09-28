#include "loot_memory.h"
#include <cassert>
#include <cstring>
#include <string>
#include <unordered_map>

struct FakeMemory {
    std::unordered_map<uint64_t,uint8_t> bytes;
    bool fail_write=false; int writes=0;
    template<class T> void set(uint64_t a,const T& v){const auto*p=(const uint8_t*)&v;for(size_t i=0;i<sizeof(v);++i)bytes[a+i]=p[i];}
    void text(uint64_t a,const char* s){for(size_t i=0;i<=std::strlen(s);++i)bytes[a+i]=(uint8_t)s[i];}
    template<class T> T get(uint64_t a){T v{};assert(read(a,v));return v;}
    template<class T> bool read(uint64_t a,T& v){auto*p=(uint8_t*)&v;for(size_t i=0;i<sizeof(v);++i){auto it=bytes.find(a+i);if(it==bytes.end())return false;p[i]=it->second;}return true;}
    template<class T> bool write_heap(uint64_t a,const T& v){++writes;if(fail_write)return false;set(a,v);return true;}
    void klass(uint64_t k,uint64_t namep,uint64_t nsp,const char* name,const char* ns){set(k+go_active::IL2CPP_CLASS_NAME,namep);set(k+go_active::IL2CPP_CLASS_NAMESPACE,nsp);text(namep,name);text(nsp,ns);}
};
struct Fixture {
    FakeMemory io; fast_loot::Controller<FakeMemory> c;
    uint64_t base=0x100000,helper=0x20000,statics=0x21000,runners=0x22000;
    uint64_t runner=0x23000,items=0x24000,promise=0x25000;
    uint64_t helperk=0x26000,runnerk=0x27000,promisek=0x28000;
    Fixture():c(io){
        io.klass(helperk,0x30000,0x30100,"PlayerLoopHelper","Cysharp.Threading.Tasks");
        io.set(base+go_active::PLAYER_LOOP_HELPER_TYPEINFO_RVA,helperk);
        io.set(helperk+go_active::IL2CPP_CLASS_STATIC_FIELDS_OFFSET,statics);
        io.set(statics+go_active::PLAYERLOOPHELPER_RUNNERS,runners);
        int32_t one=1; io.set(runners+go_active::IL2CPP_ARRAY_LENGTH,one); io.set(runners+go_active::IL2CPP_ARRAY_FIRST_ELEMENT,runner);
        io.klass(runnerk,0x30200,0x30300,"PlayerLoopRunner","Cysharp.Threading.Tasks.Internal"); io.set(runner,runnerk);
        io.set(runner+go_active::LOOPRUNNER_ITEMS,items); io.set(runner+go_active::LOOPRUNNER_TAIL,one);
        io.set(items+go_active::IL2CPP_ARRAY_LENGTH,one); io.set(items+go_active::IL2CPP_ARRAY_FIRST_ELEMENT,promise);
        io.klass(promisek,0x30400,0x30500,"DelayIgnoreTimeScalePromise",""); io.set(promise,promisek);
        io.set(promise+go_active::DELAYPROMISE_TIMESPAN,fast_loot::kCadenceBits);
        io.set(promise+go_active::DELAYPROMISE_ELAPSED,0.1f); io.set(promise+go_active::DELAYPROMISE_INITIAL_FRAME,int32_t(123));
    }
    void tick(bool enabled=true){c.tick(1,base,enabled);}
};
int main(){using viewmodel::Status;
    {Fixture f; f.tick(); assert(f.c.status==Status::Active); assert(f.io.get<uint32_t>(f.promise+go_active::DELAYPROMISE_TIMESPAN)==0); assert(f.c.neutralized()==1&&f.io.writes==1); f.tick(false); assert(f.c.status==Status::Off);}
    {Fixture f; f.io.set(f.promise+go_active::DELAYPROMISE_TIMESPAN,uint32_t(0x3e99999a)); f.tick(); assert(f.c.status==Status::Active&&f.io.writes==0&&f.c.neutralized()==0);}
    {Fixture f; f.io.fail_write=true; f.tick(); assert(f.c.status==Status::Failed&&f.c.neutralized()==0);}
    {Fixture f; f.io.text(0x30000,"OtherHelper"); f.tick(); assert(f.c.status==Status::Failed&&f.io.writes==0);}
    {Fixture f; f.io.set(f.runner+go_active::LOOPRUNNER_TAIL,int32_t(0)); f.tick(); assert(f.c.status==Status::Active&&f.io.writes==0);}
    {Fixture f; f.io.text(0x30400,"DelayPromise"); f.io.set(f.promise+go_active::DELAYPROMISE_SCALED_INITIAL_FRAME,int32_t(123)); f.io.set(f.promise+go_active::DELAYPROMISE_SCALED_TIMESPAN,fast_loot::kCadenceBits); f.io.set(f.promise+go_active::DELAYPROMISE_SCALED_ELAPSED,0.1f); f.tick(); assert(f.c.status==Status::Active&&f.io.get<uint32_t>(f.promise+go_active::DELAYPROMISE_SCALED_TIMESPAN)==0);}
}
