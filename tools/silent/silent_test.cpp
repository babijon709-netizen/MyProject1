#include "silent_aim_core.h"
#include <cassert>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <vector>

struct FakeMemory {
    std::unordered_map<uint64_t,uint8_t> bytes;
    int writes=0;
    template<class T> void set(uint64_t a,const T& v) {
        const auto* p=reinterpret_cast<const uint8_t*>(&v);
        for(size_t i=0;i<sizeof(v);++i) bytes[a+i]=p[i];
    }
    bool read(uint64_t a,void* out,size_t n) {
        auto* p=static_cast<uint8_t*>(out);
        for(size_t i=0;i<n;++i) { auto it=bytes.find(a+i); if(it==bytes.end()) return false; p[i]=it->second; }
        return true;
    }
    bool write(uint64_t a,const void* in,size_t n) {
        ++writes; const auto* p=static_cast<const uint8_t*>(in);
        for(size_t i=0;i<n;++i) bytes[a+i]=p[i];
        return true;
    }
};

static silent_aim::Command fixture(FakeMemory& m) {
    silent_aim::Command c;
    c.pid=7; c.player=0x10000; c.handler=0x11000; c.look=0x12000;
    c.player_class=0x21000; c.handler_class=0x22000; c.look_class=0x23000;
    c.fp_manager=0x13000; c.weapon=0x14000; c.target=0x15000;
    c.fp_manager_class=0x24000; c.weapon_class=0x25000;
    c.player_handler_offset=0x78; c.handler_player_offset=0xd0;
    c.look_offset=0x140; c.value_offset=0x20;
    c.fp_manager_offset=0x90; c.weapon_offset=0x50; c.weapon_player_offset=0xc0;
    c.eye[0]=1; c.eye[1]=2; c.eye[2]=3;
    c.aim[0]=4; c.aim[1]=6; c.aim[2]=3;
    m.set(c.player,c.player_class); m.set(c.player+c.player_handler_offset,c.handler);
    m.set(c.handler,c.handler_class); m.set(c.handler+c.handler_player_offset,c.player);
    m.set(c.handler+c.look_offset,c.look); m.set(c.look,c.look_class);
    m.set(c.player+c.fp_manager_offset,c.fp_manager); m.set(c.fp_manager,c.fp_manager_class);
    m.set(c.fp_manager+c.weapon_offset,c.weapon); m.set(c.weapon,c.weapon_class);
    m.set(c.weapon+c.weapon_player_offset,c.player);
    return c;
}
int main() {
    using namespace silent_aim;
    assert(fresh(100,100) && fresh(100,199) && !fresh(100,200) && !fresh(101,100));
    {
        FakeMemory m; auto c=fixture(m);
        assert(valid_command(c)); assert(tick(m,c)==Result::written); assert(m.writes==1);
        float d[3]{}; assert(m.read(c.look+c.value_offset,d,sizeof(d)));
        assert(std::fabs(d[0]-0.6f)<1e-5f && std::fabs(d[1]-0.8f)<1e-5f && std::fabs(d[2])<1e-5f);
    }
    {
        FakeMemory m; auto c=fixture(m); uint64_t other=0x16000;
        m.set(c.fp_manager+c.weapon_offset,other);
        assert(tick(m,c)==Result::guard_failed && m.writes==0);
    }
    {
        FakeMemory m; auto c=fixture(m); uint64_t other=0x26000;
        m.set(c.handler,other);
        assert(tick(m,c)==Result::guard_failed && m.writes==0);
    }
    {
        FakeMemory m; auto c=fixture(m); uint64_t other=0x16000;
        m.set(c.weapon+c.weapon_player_offset,other);
        assert(tick(m,c)==Result::guard_failed && m.writes==0);
    }
    {
        FakeMemory m; auto c=fixture(m); uint64_t other=0x26000;
        m.set(c.weapon,other);
        assert(tick(m,c)==Result::guard_failed && m.writes==0);
    }
    {
        FakeMemory m; auto c=fixture(m); c.aim[0]=std::numeric_limits<float>::quiet_NaN();
        assert(!valid_command(c)); assert(tick(m,c)==Result::invalid && m.writes==0);
    }
    {
        FakeMemory m; auto c=fixture(m); c.aim[0]=c.eye[0]; c.aim[1]=c.eye[1]; c.aim[2]=c.eye[2];
        assert(!valid_command(c));
    }
}
