#include "weapon_tuning_memory.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <map>
#include <vector>

using S = viewmodel::Status;
struct Memory {
    std::map<uint64_t, uint8_t> bytes;
    std::vector<uint64_t> writes;
    uint64_t fail_read = 0, fail_write = 0, fail_readback = 0;
    bool ambiguous_write = false;
    size_t reads = 0;
    template<class T> void set(uint64_t a, const T& value) {
        const auto* p = reinterpret_cast<const uint8_t*>(&value);
        for (size_t i=0;i<sizeof(T);++i) bytes[a+i]=p[i];
    }
    template<class T> bool read(uint64_t a, T& value) {
        ++reads;
        if (a==fail_read || (a==fail_readback && !writes.empty())) return false;
        auto* p = reinterpret_cast<uint8_t*>(&value);
        for (size_t i=0;i<sizeof(T);++i) {
            auto b=bytes.find(a+i); if(b==bytes.end()) return false;
            p[i]=b->second;
        }
        return true;
    }
    template<class T> bool write(uint64_t a, const T& value) {
        static_assert(sizeof(T)==4, "only aligned instruction writes");
        assert(a%4==0);
        if (a==fail_write) {
            if (ambiguous_write) { set(a,value); writes.push_back(a); }
            return false;
        }
        set(a,value); writes.push_back(a); return true;
    }
    uint32_t word(uint64_t a) {
        uint32_t w=0;
        for(unsigned i=0;i<4;++i) w|=uint32_t(bytes.at(a+i))<<(i*8);
        return w;
    }
};
struct Fixture {
    static constexpr uint64_t base=0x100000000;
    Memory io;
    weapon_tuning::Controller<Memory> c{io,true};
    const weapon_tuning::Layout& layout;
    explicit Fixture(const weapon_tuning::Layout& l) : layout(l) {
        io.set(base+0x308,weapon_tuning::kGnuNote); io.set(base+0x318,l.build_id);
        for (size_t i=0;i<3;++i) io.set(address(i)-12,l.sites[i].guard);
    }
    uint64_t address(size_t i) const {return base+layout.sites[i].rva;}
    void tick(bool recoil, bool spread) {c.tick(1,base,recoil,spread);}
    void check(bool recoil, bool spread) {
        for (size_t i=0;i<3;++i) {
            const auto& site=layout.sites[i];
            for (size_t j=0;j<8;++j) {
                const auto want=j==3 && (i==0 ? recoil : spread)
                    ? (i==0 ? weapon_tuning::kNoRecoil : weapon_tuning::kNoSpread) : site.guard[j];
                assert(io.word(address(i)-12+j*4)==want);
            }
        }
    }
};
int main() {
    static_assert(weapon_tuning::kNoRecoil==0xf900013f, "STR XZR,[X9]");
    static_assert(weapon_tuning::kNoSpread==0x1e2703e0, "FMOV S0,WZR");
    for (const auto& layout:weapon_tuning::kLayouts) {
        // Every transition; both features default off and are independent.
        for (int before=0;before<4;++before) for(int after=0;after<4;++after) {
            Fixture f(layout); f.tick(before&1,before&2); f.check(before&1,before&2);
            f.tick(after&1,after&2); f.check(after&1,after&2);
            assert(f.c.status.recoil==((after&1)?S::Active:S::Off));
            assert(f.c.status.spread==((after&2)?S::Active:S::Off));
            auto n=f.io.writes.size(); f.tick(after&1,after&2); assert(f.io.writes.size()==n);
            assert(f.c.reset(true)); f.check(false,false); assert(!f.c.has_state());
        }
        { Fixture f(layout); f.tick(false,false); assert(f.io.reads==0 && f.io.writes.empty()); }
        { Fixture f(layout); weapon_tuning::Controller<Memory> c(f.io,false);
          c.tick(1,Fixture::base,true,true); assert(c.status.recoil==S::Failed && c.status.spread==S::Failed);
          assert(f.io.writes.empty()); }
        for(uint64_t base:{uint64_t(0),Fixture::base+1,UINT64_MAX-3}) {
            Fixture f(layout); f.c.tick(1,base,true,true);
            assert(f.io.writes.empty() && f.io.reads==0);
            assert(f.c.status.recoil==(base?S::Failed:S::Waiting));
        }
        for (size_t i=0;i<20;++i) {
            Fixture f(layout); auto id=layout.build_id; id[i]^=0xff;
            f.io.set(Fixture::base+0x318,id); f.tick(true,true);
            assert(f.io.writes.empty() && f.c.status.recoil==S::Failed && f.c.status.spread==S::Failed);
        }
        for (size_t i=0;i<4;++i) {
            Fixture f(layout); f.io.set(Fixture::base+0x308+i*4,uint32_t(0)); f.tick(true,true);
            assert(f.io.writes.empty());
        }
        // A bad member must not cause the other half of No Spread to be written.
        for(size_t i=0;i<3;++i) for(size_t j=0;j<8;++j) {
            Fixture f(layout); f.io.set(f.address(i)-12+j*4,uint32_t(0));
            f.tick(i==0,i!=0); assert(f.io.writes.empty());
            assert((i==0 ? f.c.status.recoil : f.c.status.spread)==S::Failed);
        }
        for(size_t i=0;i<3;++i) {
            for(uint32_t foreign:{uint32_t(0xd503201f), i==0?weapon_tuning::kNoRecoil:weapon_tuning::kNoSpread}) {
                Fixture f(layout); f.io.set(f.address(i),foreign); f.tick(i==0,i!=0);
                assert(f.io.writes.empty()); f.tick(false,false);
                assert(f.io.word(f.address(i))==foreign); // no adopting another process's patch
            }
        }
        // Transient apply failures and ambiguous write reports: rollback, retry.
        for(size_t i=0;i<3;++i) {
            { Fixture f(layout); f.io.fail_read=f.address(i)-12; f.tick(i==0,i!=0);
              assert(f.io.writes.empty()); f.io.fail_read=0; f.tick(i==0,i!=0); f.check(i==0,i!=0); }
            for(bool ambiguous:{false,true}) {
                Fixture f(layout); f.io.fail_write=f.address(i); f.io.ambiguous_write=ambiguous;
                f.tick(i==0,i!=0); assert((i==0?f.c.status.recoil:f.c.status.spread)==S::Failed);
                f.check(false,false); // first spread write is rolled back if second fails
                f.io.fail_write=0; f.tick(i==0,i!=0); f.check(i==0,i!=0);
            }
            { Fixture f(layout); f.io.fail_readback=f.address(i); f.tick(i==0,i!=0);
              assert((i==0?f.c.status.recoil:f.c.status.spread)==S::Failed);
              f.io.fail_readback=0; f.tick(false,false); f.check(false,false); assert(!f.c.has_state()); }
        }
        // A failing feature never blocks the other one.
        { Fixture f(layout); f.io.fail_write=f.address(2); f.tick(true,true);
          assert(f.c.status.recoil==S::Active && f.c.status.spread==S::Failed); f.check(true,false); }
        { Fixture f(layout); f.io.fail_write=f.address(0); f.tick(true,true);
          assert(f.c.status.recoil==S::Failed && f.c.status.spread==S::Active); f.check(false,true); }
        // Restore retries; even a failed half must not stop restoration of the rest.
        for(size_t i=0;i<3;++i) {
            Fixture f(layout); f.tick(true,true); f.io.fail_write=f.address(i); f.tick(false,false);
            assert(f.c.has_state() && (i==0?f.c.status.recoil:f.c.status.spread)==S::Failed);
            for(size_t j=0;j<3;++j) if(j!=i) assert(f.io.word(f.address(j))==layout.sites[j].guard[3]);
            f.io.fail_write=0; f.tick(false,false); f.check(false,false); assert(!f.c.has_state());
        }
        { Fixture f(layout); f.tick(true,true); f.io.fail_read=f.address(1)-12; f.tick(false,false);
          assert(f.c.has_state() && f.c.status.spread==S::Failed && f.c.status.recoil==S::Off);
          f.io.fail_read=0; f.tick(false,false); f.check(false,false); }
        { Fixture f(layout); f.tick(true,true); f.io.set(f.address(1)-4,uint32_t(0)); f.tick(false,false);
          assert(f.c.has_state() && f.c.status.spread==S::Failed);
          f.io.set(f.address(1)-4,layout.sites[1].guard[2]); f.tick(false,false); f.check(false,false); }
        { Fixture f(layout); f.tick(true,true); f.io.set(f.address(1),uint32_t(0xd503201f)); f.tick(false,false);
          assert(f.io.word(f.address(1))==0xd503201f && !f.c.has_state()); }
        { Fixture f(layout); f.tick(true,true); f.io.set(f.address(1),layout.sites[1].guard[3]); f.tick(false,false);
          f.check(false,false); assert(!f.c.has_state()); }
        // Identity changes cannot reuse ownership, including another supported ID.
        for (const auto& other:weapon_tuning::kLayouts) if(&other!=&layout) {
            Fixture f(layout); f.tick(true,true); auto n=f.io.writes.size();
            f.io.set(Fixture::base+0x318,other.build_id); f.tick(false,false);
            assert(f.io.writes.size()==n && f.c.has_state() && f.c.status.recoil==S::Failed);
            f.io.set(Fixture::base+0x318,layout.build_id); f.tick(false,false); f.check(false,false);
        }
        { Fixture f(layout); f.tick(true,true); auto n=f.io.writes.size();
          f.c.tick(2,Fixture::base,false,false); assert(f.io.writes.size()==n && !f.c.has_state());
          f.tick(true,true); assert(f.io.writes.size()==n && f.c.status.recoil==S::Failed); }
        { Fixture f(layout); f.tick(true,true); auto n=f.io.writes.size();
          f.c.tick(1,Fixture::base+0x100000000,false,false); assert(f.io.writes.size()==n && !f.c.has_state()); }
        { Fixture f(layout); f.tick(true,true); auto n=f.io.writes.size();
          assert(f.c.reset(false)); assert(f.io.writes.size()==n); }
        { Fixture f(layout); f.tick(true,true); f.io.fail_write=f.address(1); assert(!f.c.reset(true)); }
    }
    std::cout << "weapons: both builds, independent toggles, exact guards, ownership, grouped rollback, restore/retry, lifecycle: OK\n";
}
