#include "no_recoil_memory.h"
#include <cassert>
#include <cstring>
#include <map>
#include <string>
#include <cstdio>
#include <cmath>

struct FakeIO {
    std::map<uint64_t,uint8_t> mem;
    std::map<uint64_t,std::pair<std::string,std::string>> types;
    uint64_t fail_read=0, fail_write=0;
    unsigned writes=0;
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
        ++writes; put(a,v);
        return true;
    }
    bool named(uint64_t a,const char* n,const char* ns) {
        auto it=types.find(a);
        return it!=types.end() && it->second.first==n && it->second.second==ns;
    }
    void object(uint64_t a,const char* n,const char* ns) {
        types[a]={n,ns}; put<uint64_t>(a,a+0x800000);
        put<uint64_t>(a+go_active::MANAGED_CACHED_PTR,a+0x900000);
    }
    float value(uint64_t a) { float v=0; assert(read(a,v)); return v; }
};

constexpr uint64_t player=0x10000, net=0x20000, fp=0x30000;
constexpr uint64_t gun=0x40000, gun2=0x48000, cfg=0x50000, cfg2=0x58000, recoil=0x60000, recoil2=0x68000;

static const float kSrc[6] = {0.1f,0.2f,0.3f,0.4f,0.05f,0.15f};

static uint64_t recoil_off(size_t i) {
    using namespace go_active;
    static const uint64_t offs[6] = {
        RECOILCONFIG_MIN_RECOIL, RECOILCONFIG_MIN_RECOIL+4,
        RECOILCONFIG_MAX_RECOIL, RECOILCONFIG_MAX_RECOIL+4,
        RECOILCONFIG_MIN_SIDE, RECOILCONFIG_MAX_SIDE,
    };
    return recoil+offs[i];
}
static void put_recoil(FakeIO& io, uint64_t base, float scale) {
    for (size_t i=0;i<6;++i) io.put<float>(base+ (recoil_off(i)-recoil), kSrc[i]*scale);
}

struct Fixture {
    FakeIO io;
    no_recoil::Controller<FakeIO> c{io};
    Fixture(bool hitscan=true) {
        using namespace go_active;
        io.object(player,"PlayerManager","Oxide"); io.object(net,"NetworkIdentity","Mirror");
        io.object(fp,"FPManager","Oxide");
        io.put<uint64_t>(player+NETWORK_BEHAVIOUR_IDENTITY,net);
        io.put<uint8_t>(net+NETWORK_IDENTITY_IS_LOCAL_PLAYER,1);
        io.put<uint64_t>(player+PLAYER_FP_MANAGER,fp);
        if (hitscan) {
            io.object(gun,"FPHitscan","Oxide"); io.object(cfg,"HitscanWeaponConfig",no_recoil::kWeapons);
            io.object(recoil,"RecoilConfig",no_recoil::kWeapons);
            io.put<uint64_t>(fp+FPMANAGER_CURRENT_OBJECT,gun);
            io.put<uint64_t>(gun+FPOBJECT_PLAYER_BACKREF,player);
            io.put<uint64_t>(gun+FPHITSCAN_CONFIG,cfg);
            io.put<uint64_t>(cfg+HITSCANCONFIG_RECOIL_CONFIG,recoil);
            put_recoil(io,recoil,1.f);
        } else {
            io.object(gun,"FPBow","Oxide");
            io.put<uint64_t>(fp+FPMANAGER_CURRENT_OBJECT,gun);
            io.put<uint64_t>(gun+FPOBJECT_PLAYER_BACKREF,player);
        }
    }
    void tick(bool on=true) { c.tick(1,0x1000000,player,on); }
    void arm_second_weapon() {
        using namespace go_active;
        io.object(gun2,"FPHitscan","Oxide"); io.object(cfg2,"HitscanWeaponConfig",no_recoil::kWeapons);
        io.object(recoil2,"RecoilConfig",no_recoil::kWeapons);
        io.put<uint64_t>(gun2+FPOBJECT_PLAYER_BACKREF,player);
        io.put<uint64_t>(gun2+FPHITSCAN_CONFIG,cfg2);
        io.put<uint64_t>(cfg2+HITSCANCONFIG_RECOIL_CONFIG,recoil2);
        put_recoil(io,recoil2,2.f);
        io.put<uint64_t>(fp+FPMANAGER_CURRENT_OBJECT,gun2);
    }
};

int main() {
    using namespace go_active;
    for (auto version : {GameVersion::Release,GameVersion::Beta}) {
        Select(version);
        {   // full chain: zeroes all six floats, originals land back on off
            Fixture f;
            f.tick();
            assert(f.c.status==viewmodel::Status::Active && f.c.stage()==0);
            for (size_t i=0;i<6;++i) assert(f.io.value(recoil_off(i))==0.f);
            const unsigned w=f.io.writes; f.tick(); assert(f.io.writes==w); // idempotent
            f.tick(false);
            assert(f.c.status==viewmodel::Status::Off && !f.c.has_state());
            for (size_t i=0;i<6;++i) assert(std::fabs(f.io.value(recoil_off(i))-kSrc[i])<1e-6f);
        }
        {   // toggle back on after off rezeroes from the restored originals
            Fixture f;
            f.tick(); f.tick(false); f.tick(true);
            assert(f.c.status==viewmodel::Status::Active);
            for (size_t i=0;i<6;++i) assert(f.io.value(recoil_off(i))==0.f);
        }
        {   // weapon switch: old asset restored, fresh asset intercepted
            Fixture f;
            f.tick();
            f.arm_second_weapon();
            f.tick();
            assert(f.c.status==viewmodel::Status::Active);
            for (size_t i=0;i<6;++i) {
                assert(std::fabs(f.io.value(recoil_off(i))-kSrc[i])<1e-6f);      // old back
                assert(f.io.value(recoil2+(recoil_off(i)-recoil))==0.f);          // new zeroed
            }
        }
        {   // foreign update in between: restore must not clobber it
            Fixture f;
            f.tick();
            f.io.put<float>(recoil_off(2),7.7f);
            f.tick(false);
            assert(f.io.value(recoil_off(2))==7.7f);
            for (size_t i=0;i<6;++i) if (i!=2) assert(std::fabs(f.io.value(recoil_off(i))-kSrc[i])<1e-6f);
        }
        {   // bow in hands: honest Waiting at stage 6, no writes anywhere
            Fixture f(false);
            const unsigned w=f.io.writes;
            f.tick();
            assert(f.c.status!=viewmodel::Status::Active && f.c.stage()==6);
            assert(f.io.writes==w);
        }
        {   // backlink mismatch: stage 7
            Fixture f;
            f.io.put<uint64_t>(gun+FPOBJECT_PLAYER_BACKREF,0xBAD);
            f.tick();
            assert(f.c.status!=viewmodel::Status::Active && f.c.stage()==7);
        }
        {   // unreadable recoil scalar: stage 10 Failed, nothing half-written
            Fixture f;
            f.io.fail_read=recoil_off(0);
            f.tick();
            assert(f.c.status==viewmodel::Status::Failed && f.c.stage()==10);
        }
        {   // session change resets without touching originals of the old session
            Fixture f;
            f.tick();
            f.c.tick(2,0x1000000,player,true);
            assert(f.c.status==viewmodel::Status::Active); // fresh session rezeroes from zero state
        }
    }
    std::printf("ALL NO-RECOIL TESTS PASS\n");
    return 0;
}
