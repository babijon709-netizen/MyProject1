#include "weapon_tuning_memory.h"
#include "object_data_memory.h"
#include <unordered_map>
#include <string>
#include <vector>
#include <cassert>
#include <functional>
#include <limits>
#include <iostream>
using namespace weapon_tuning;
using viewmodel::Status;
constexpr uint64_t base=0x10000000, player=0x101000, fp=0x102000, weapon=0x103000,
    config=0x104000, recoil=0x105000, dispersion=0x106000, calc=0x107000, net=0x108000;
struct Fake {
    std::unordered_map<uint64_t,uint8_t> bytes;
    std::unordered_map<uint64_t,std::pair<std::string,std::string>> names;
    std::vector<uint64_t> writes;
    uint64_t fail_read=0, fail_write=0;
    std::function<void(uint64_t)> before_read;
    std::string maps="100000-200000 rw-p 00000000 00:00 0 [heap]\n";
    template<class T> void set(uint64_t a,const T& v) {
        auto p=reinterpret_cast<const uint8_t*>(&v);
        for(size_t i=0;i<sizeof(T);++i) bytes[a+i]=p[i];
    }
    template<class T> bool read(uint64_t a,T& v) {
        if(before_read) before_read(a);
        if(fail_read && a<=fail_read && fail_read<a+sizeof(T)) return false;
        auto p=reinterpret_cast<uint8_t*>(&v);
        for(size_t i=0;i<sizeof(T);++i) { auto it=bytes.find(a+i); if(it==bytes.end()) return false; p[i]=it->second; }
        return true;
    }
    template<class T> T get(uint64_t a) { T v{}; assert(read(a,v)); return v; }
    template<class T> bool write_heap(uint64_t a,const T& v) {
        if(a==fail_write || !object_data::heap_writable(maps,a,sizeof(T))) return false;
        assert(a>=0x100000 && a<0x200000); writes.push_back(a); set(a,v); return true;
    }
    bool named(uint64_t o,const char* n,const char* ns) {
        uint64_t k=0; if(!read(o,k)) return false;
        auto it=names.find(k); return it!=names.end() && it->second.first==n && it->second.second==ns;
    }
    void object(uint64_t o,const char* n,const char* ns,bool unity=true) {
        uint64_t k=o+0x80000; set(o,k); names[k]={n,ns};
        if(unity) set(o+0x10,o+0x90000);
    }
    explicit Fake(const Layout& l) {
        set(base+0x318,l.build_id);
        for(size_t i=0;i<4;++i) set(base+l.guard_sites[i],l.guard_words[i]);
        object(player,"PlayerManager","Oxide"); object(fp,"FPManager","Oxide");
        object(weapon,"FPHitscan","Oxide"); object(config,"HitscanWeaponConfig",kWeapons);
        object(recoil,"RecoilConfig",kWeapons); object(dispersion,"DispersionConfig",kWeapons);
        object(calc,l.calculator,"",false); object(net,"NetworkIdentity","Mirror");
        set(player+0x40,net); set(net+0x4a,uint8_t(1)); set(player+0x90,fp);
        set(fp+0x58,weapon); set(weapon+0xc0,player); set(weapon+0x130,config);
        set(weapon+0x200,recoil); set(weapon+0x208,config); set(weapon+0x1f8,calc);
        set(config+0x68,recoil); set(config+0x60,dispersion);
        set(calc+0x18,config); set(calc+0x10,dispersion);
        for(unsigned i=0;i<6;++i) set(recoil+0x18+i*4,float(i+1));
        set(dispersion+0x20,0.5f); set(dispersion+0x24,4.f); set(calc+0x20,2.f);
        set(config+0x18,42.f); set(config+0x34,0.125f); // damage, fire rate unchanged
    }
};
static void maps_tests() {
    using object_data::heap_writable;
    for (const std::string label : {"", "[heap]", "[anon:libc_malloc]", "[anon:scudo:primary]", "[anon:scudo:secondary]"}) {
        std::string m="100000-200000 rw-p 0 00:00 0 "+label+"\n";
        assert(heap_writable(m,0x100010,4)); assert(heap_writable(m,0x1ffffc,4));
        assert(!heap_writable(m,0x1ffffe,4)); assert(!heap_writable(m,0x200000,4));
        assert(!heap_writable(m,0x100010,0)); assert(!heap_writable(m,0x100010,65));
        assert(!heap_writable(m,UINT64_MAX-1,4));
    }
    for (const std::string line : {"rw-p 0 08:01 10 /libil2cpp.so", "rw-p 0 08:01 10 /libunity.so (deleted)",
         "rw-p 0 00:00 1 /memfd:heap", "rwxp 0 00:00 0", "r--p 0 00:00 0", "rw-s 0 00:00 0",
         "rw-p 0 00:00 0 [anon:.bss]", "rw-p 1000 00:00 0", "rw-p 0 00:00 1", "rw-p 0 00:00 0 [stack]"})
        assert(!heap_writable("100000-200000 "+line+"\n",0x100010,4));
    const std::string prefix="100000-110000 rw-p 0 08:01 1 /system/lib64/libother.so\n";
    assert(!heap_writable(prefix+"110000-200000 rw-p 0 00:00 0\n",0x110010,4));
    assert(!heap_writable(prefix+"110000-120000 ---p 0 00:00 0\n120000-200000 rw-p 0 00:00 0\n",0x120010,4));
    assert(heap_writable(prefix+"120000-200000 rw-p 0 00:00 0\n",0x120010,4));
    assert(!heap_writable("garbage\n100000-200000 rw-p 0 00:00 0\n",0x100010,4));
}
static void tests(const Layout& l) {
    {
        Fake io(l); Controller<Fake> c(io);
        c.tick(1,base,player,false,false); assert(io.writes.empty());
        c.tick(1,base,player,true,false); assert(c.recoil==Status::Active && c.spread==Status::Off && io.writes.size()==6);
        assert(io.get<float>(calc+0x20)==2.f);
        c.tick(1,base,player,true,true); assert(c.spread==Status::Active && io.writes.size()==9);
        for(int i=0;i<10;++i) c.tick(1,base,player,true,true);
        assert(io.writes.size()==9); // no continuous pwrite flood
        assert(io.get<float>(config+0x18)==42.f && io.get<float>(config+0x34)==0.125f);
        c.tick(1,base,player,false,true); assert(io.get<float>(recoil+0x18)==1.f && io.get<float>(calc+0x20)==0.f);
        c.tick(1,base,player,false,false); assert(!c.has_state());
        assert(io.get<float>(calc+0x20)==2.f && io.get<float>(dispersion+0x24)==4.f);
        assert(io.writes.size()==18);
    }
    for (int issue=0;issue<8;++issue) {
        Fake io(l); Controller<Fake> c(io);
        if(issue==0) io.set(base+0x318,std::array<uint8_t,20>{});
        if(issue==1) io.set(weapon+0xc0,uint64_t(0x199000));
        if(issue==2) io.set(net+0x4a,uint8_t(0));
        if(issue==3) io.set(config+0x68,uint64_t(0));
        if(issue==4) io.set(weapon+0x10,uint64_t(0));
        if(issue==5) io.set(base+l.guard_sites[0],uint32_t(0));
        if(issue==6) io.set(base+l.guard_sites[3],uint32_t(0x1400005d));
        if(issue==7) io.maps="100000-200000 rw-p 0 08:01 1 /some-library.so\n";
        c.tick(1,base,player,true,true); assert(io.writes.empty());
        assert(c.recoil!=Status::Active && c.spread!=Status::Active);
    }
    {
        Fake io(l); Controller<Fake> c(io); int seen=0;
        io.before_read=[&](uint64_t a){ if(a==weapon+0xc0 && ++seen==2) io.set(a,uint64_t(0)); };
        c.tick(1,base,player,true,true); assert(io.writes.empty()); // ownership changed just before apply
    }
    {
        Fake io(l); Controller<Fake> c(io); c.tick(1,base,player,true,true);
        io.set(recoil+0x18,7.f); c.tick(1,base,player,false,false);
        assert(io.get<float>(recoil+0x18)==7.f); // external updates win on restore
    }
    {
        Fake io(l); Controller<Fake> c(io); c.tick(1,base,player,true,false);
        io.set(recoil+0x18,7.f); c.tick(1,base,player,true,false);
        c.tick(1,base,player,false,false); assert(io.get<float>(recoil+0x18)==7.f); // recapture game update
    }
    {
        Fake io(l); Controller<Fake> c(io); c.tick(1,base,player,true,true);
        io.fail_read=recoil; c.tick(1,base,player,false,false);
        assert(c.has_state() && c.recoil==Status::Failed);
        io.fail_read=0; c.tick(1,base,player,false,false);
        assert(!c.has_state() && io.get<float>(recoil+0x18)==1.f);
    }
    {
        Fake io(l); Controller<Fake> c(io); c.tick(1,base,player,true,true);
        io.fail_write=recoil+0x18; c.tick(1,base,player,false,false);
        assert(c.has_state() && c.recoil==Status::Failed);
        io.fail_write=0; c.tick(1,base,player,false,false); assert(!c.has_state());
    }
    {
        Fake io(l); Controller<Fake> c(io); io.fail_write=recoil+0x1c;
        c.tick(1,base,player,true,false); assert(c.recoil==Status::Failed && c.has_state());
        io.fail_write=0; c.tick(1,base,player,false,false);
        assert(io.get<float>(recoil+0x18)==1.f && io.get<float>(recoil+0x1c)==2.f && !c.has_state());
    }
    {
        Fake io(l); Controller<Fake> c(io); c.tick(1,base,player,true,true);
        io.set(weapon+0x10,uint64_t(0)); c.tick(1,base,0,false,false);
        assert(!c.has_state() && io.get<float>(recoil+0x18)==1.f); // asset survives owner
        assert(io.get<float>(dispersion+0x24)==4.f && io.get<float>(calc+0x20)==0.f); // no dead calculator write
    }
    {
        Fake io(l); Controller<Fake> c(io); c.tick(1,base,player,true,true);
        io.set(recoil+0x10,uint64_t(0x900123)); c.tick(1,base,player,false,false);
        assert(io.get<float>(recoil+0x18)==0.f); // reused asset identity: drop, don't restore
    }
    {
        Fake io(l); Controller<Fake> c(io); c.tick(1,base,player,true,true);
        io.set(fp+0x58,uint64_t(0)); c.tick(1,base,player,true,true);
        assert(!c.has_state() && c.recoil==Status::Waiting && io.get<float>(calc+0x20)==2.f);
        io.set(fp+0x58,weapon); c.tick(1,base,player,true,true); assert(c.recoil==Status::Active);
        assert(c.reset(true)); assert(io.get<float>(recoil+0x18)==1.f);
    }
    {
        Fake io(l); Controller<Fake> c(io); c.tick(1,base,player,true,true); auto n=io.writes.size();
        c.tick(2,base,0,false,false); assert(io.writes.size()==n && !c.has_state());
    }
    {
        Fake io(l); Controller<Fake> c(io);
        io.set(player+0x90,fp | 0xab00000000000000ULL);
        io.set(weapon+0x200,recoil | 0xcd00000000000000ULL);
        c.tick(1,base,player,true,true); assert(c.recoil==Status::Active && c.spread==Status::Active);
        assert(c.reset(true));
    }
    {
        Fake io(l); Controller<Fake> c(io); c.tick(1,base,player,true,false);
        io.set(recoil+0x18,std::numeric_limits<float>::quiet_NaN());
        auto n=io.writes.size(); c.tick(1,base,player,false,false);
        assert(io.writes.size()==n+5 && !c.has_state());
    }
    for(float v : {std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),-2000.f,2000.f}) {
        Fake io(l); Controller<Fake> c(io); io.set(recoil+0x18,v);
        c.tick(1,base,player,true,false); assert(c.recoil==Status::Failed && io.writes.empty());
    }
}
int main() { maps_tests(); for(const auto& l:kLayouts) tests(l); std::cout<<"weapons: heap-only gate, both builds, ownership, transitions, restoration, failures, finite checks: OK\n"; }
