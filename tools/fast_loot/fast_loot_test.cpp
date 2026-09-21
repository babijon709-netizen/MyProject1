#include "fast_loot_memory.h"
#include <cassert>
#include <iostream>
#include <map>
#include <vector>

using viewmodel::Status;
struct Memory {
    std::map<uint64_t, uint8_t> bytes;
    std::vector<uint64_t> writes;
    unsigned reads = 0;
    uint64_t fail_read = 0, fail_write = 0;
    bool fail_readback = false;
    template<class T> void set(uint64_t addr, const T& value) {
        const auto* b = reinterpret_cast<const uint8_t*>(&value);
        for (size_t i=0; i<sizeof(T); ++i) bytes[addr+i] = b[i];
    }
    template<class T> bool read(uint64_t addr, T& value) {
        ++reads;
        if (addr == fail_read) return false;
        auto* b = reinterpret_cast<uint8_t*>(&value);
        for (size_t i=0; i<sizeof(T); ++i) {
            auto it=bytes.find(addr+i); if (it==bytes.end()) return false;
            b[i]=it->second;
        }
        return true;
    }
    template<class T> bool write(uint64_t addr, const T& value) {
        assert(sizeof(T)==4 && (addr&3u)==0); // exactly one aligned instruction
        if (addr==fail_write) return false;
        writes.push_back(addr); set(addr,value);
        if (fail_readback) fail_read=addr;
        return true;
    }
    uint32_t word(uint64_t addr) { uint32_t value=0; assert(read(addr,value)); return value; }
};
struct Fixture {
    const fast_loot::Layout& layout;
    Memory io;
    fast_loot::Controller<Memory> c{io,true};
    static constexpr uint64_t base=0x10000000;
    uint64_t site, guard;
    explicit Fixture(const fast_loot::Layout& l) : layout(l), site(base+l.site), guard(site-12) {
        io.set(base+0x318,l.build_id); io.set(guard,l.guard);
    }
    void tick(bool on) { c.tick(1,base,on); }
    void check_original() {
        std::array<uint32_t,8> code{}; assert(io.read(guard,code)); assert(code==layout.guard);
    }
};
int main() {
    // FMOV S0,WZR: baseline ARM64, source WZR yields exact positive zero.
    const uint32_t instruction=fast_loot::kFastInstruction;
    assert((instruction & ~0x3ffu)==0x1e270000);
    assert(((instruction>>5)&31u)==31u && (instruction&31u)==0u);
    assert(fast_loot::kDelayMilliseconds==0);
    for (const auto& layout : fast_loot::kLayouts) {
        { Fixture f(layout); fast_loot::Controller<Memory> unsupported(f.io,false);
          unsupported.tick(1,Fixture::base,true);
          assert(unsupported.status==Status::Failed && f.io.writes.empty() && !unsupported.has_state());
          unsupported.tick(1,Fixture::base,false); assert(unsupported.status==Status::Off);
        }
        { Fixture f(layout); f.tick(false); assert(f.io.reads==0 && f.io.writes.empty());
          f.tick(true); assert(f.c.status==Status::Active && f.c.has_state());
          assert(f.io.word(f.site)==fast_loot::kFastInstruction);
          for(int i=0;i<100;++i) f.tick(true);
          assert(f.io.writes.size()==1 && f.io.writes[0]==f.site);
          f.tick(false); assert(f.c.status==Status::Off && !f.c.has_state()); f.check_original();
          f.tick(true); assert(f.c.reset(true)); f.check_original();
        }
        { Fixture f(layout); f.tick(true); auto writes=f.io.writes.size();
          f.c.tick(2,0,true); assert(f.io.writes.size()==writes && !f.c.has_state());
          assert(f.c.status==Status::Waiting); // no write through an old session
        }
        { Fixture f(layout); f.tick(true); auto writes=f.io.writes.size();
          f.c.tick(1,Fixture::base+0x1000,true); assert(f.io.writes.size()==writes && !f.c.has_state());
        }
        { Fixture f(layout); f.io.set(Fixture::base+0x318,std::array<uint8_t,20>{}); f.tick(true);
          assert(f.c.status==Status::Failed && f.io.writes.empty());
        }
        for(size_t i=0;i<8;++i) {
            Fixture f(layout); f.io.set(f.guard+i*4,uint32_t(0)); f.tick(true);
            assert(f.c.status==Status::Failed && f.io.writes.empty());
        }
        for (uint32_t foreign : {fast_loot::kFastInstruction, 0x0f03fd40u, 0x1e281000u}) {
          Fixture f(layout); f.io.set(f.site,foreign); f.tick(true);
          assert(f.c.status==Status::Failed && f.io.writes.empty());
          f.tick(false); assert(f.io.word(f.site)==foreign); // never adopt or undo a previous process's patch
        }
        { Fixture f(layout); f.io.fail_read=Fixture::base+0x318; f.tick(true);
          assert(f.c.status==Status::Failed && f.io.writes.empty()); f.io.fail_read=0; f.tick(true);
          assert(f.c.status==Status::Active);
        }
        { Fixture f(layout); f.io.fail_read=f.guard; f.tick(true); assert(f.io.writes.empty()); }
        { Fixture f(layout); f.io.fail_write=f.site; f.tick(true);
          assert(f.c.status==Status::Failed && !f.c.has_state()); f.io.fail_write=0; f.tick(true);
          assert(f.c.status==Status::Active);
          f.io.fail_write=f.site; f.tick(false); assert(f.c.status==Status::Failed && f.c.has_state());
          f.io.fail_write=0; f.tick(false); assert(f.c.status==Status::Off); f.check_original();
        }
        { Fixture f(layout); f.io.fail_readback=true; f.tick(true);
          assert(f.c.status==Status::Failed && f.c.has_state());
          f.io.fail_readback=false; f.io.fail_read=0; f.tick(false); f.check_original();
        }
        { Fixture f(layout); f.tick(true); f.io.fail_readback=true; f.tick(false);
          assert(f.c.status==Status::Failed && f.c.has_state());
          f.io.fail_readback=false; f.io.fail_read=0; f.tick(false);
          assert(!f.c.has_state()); f.check_original();
        }
        { Fixture f(layout); f.tick(true); f.io.set(f.site,uint32_t(0xd503201f));
          f.tick(false); assert(f.c.status==Status::Failed && !f.c.has_state());
          assert(f.io.word(f.site)==0xd503201f && f.io.writes.size()==1); // foreign NOP untouched
        }
        { Fixture f(layout); f.tick(true); f.io.set(f.guard,uint32_t(0)); f.tick(false);
          assert(f.c.status==Status::Failed && f.c.has_state() && f.io.writes.size()==1);
          f.io.set(f.guard,layout.guard[0]); f.tick(false); f.check_original();
        }
        { Fixture f(layout); f.tick(true); f.io.fail_write=f.site;
          assert(!f.c.reset(true)); // shutdown reports a failed restore, never claims success
        }
    }
    std::cout << "fast loot: release/beta, zero wait, architecture guard, exact guards, one-word patch, restore, failures, lifecycle: OK\n";
}
