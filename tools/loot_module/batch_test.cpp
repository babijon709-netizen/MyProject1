#include "batch.h"
#include <cassert>
#include <functional>
#include <stdexcept>
#include <vector>
#include <iostream>
using namespace loot_module;
struct Fake {
    Context context;
    std::vector<Slot> slots;
    std::vector<int> sent;
    std::function<void()> on_transfer, on_snapshot;
    bool open=true, throw_transfer=false, throw_snapshot=false;
    int bad_slot=-1;
    explicit Fake(int n=4) {
        context.count=n; context.identity[0]=0x10000;
        for(int i=0;i<n;++i) slots.push_back({uint64_t(0x20000+i*8),uint64_t(0x30000+i*8),100+i,true});
    }
    bool snapshot(Context& c) {
        if(throw_snapshot) throw std::runtime_error("read failed");
        if(on_snapshot) on_snapshot();
        c=context;
        return open;
    }
    bool slot(const Context&,int i,Slot& s) {
        if(i==bad_slot || i<0 || i>=int(slots.size())) return false;
        s=slots[i]; return true;
    }
    void transfer(const Context&,int index) {
        sent.push_back(index);
        if(on_transfer) on_transfer();
        if(throw_transfer) throw std::runtime_error("command may have been queued");
    }
};
int main() {
    bool busy=false;
    {
        Fake f; auto r=collect(f,busy);
        assert(r.result==Result::Complete && r.inspected==4 && r.attempted==4 && r.returned==4);
        assert((f.sent==std::vector<int>{103,102,101,100}) && !busy);
    }
    for(int n : {0,1,36,128,kMaxSlots}) {
        Fake f(n); auto r=collect(f,busy);
        assert(r.result==Result::Complete && r.returned==unsigned(n));
        // All requests have happened before collect returns: no queued ticks.
        assert(f.sent.size()==unsigned(n));
    }
    for(int n : {-1,kMaxSlots+1}) {
        Fake f; f.context.count=n; auto r=collect(f,busy);
        assert(r.result==Result::InvalidSlots && f.sent.empty());
    }
    {
        Fake f; f.slots[1].item=0; f.slots[2].eligible=false;
        auto r=collect(f,busy); assert(r.returned==2 && (f.sent==std::vector<int>{103,100}));
    }
    {
        Fake f; f.slots[0].index=f.slots[3].index;
        assert(collect(f,busy).result==Result::InvalidSlots && f.sent.empty());
    }
    {
        Fake f; f.bad_slot=0;
        assert(collect(f,busy).result==Result::InvalidSlots && f.sent.empty());
    }
    {
        Fake f; f.open=false;
        auto r=collect(f,busy); assert(r.result==Result::NotReady && r.attempted==0);
    }
    {
        Fake f; f.on_transfer=[&]{ f.open=false; };
        auto r=collect(f,busy); assert(r.result==Result::Changed && r.returned==1 && f.sent.size()==1);
    }
    for(int identity : {0,2,4,5,9,10,11,14,16}) {
        Fake f; f.on_transfer=[&]{ ++f.context.identity[identity]; };
        auto r=collect(f,busy); assert(r.result==Result::Changed && r.returned==1);
    }
    {
        Fake f; f.on_transfer=[&]{ ++f.slots[2].item; };
        auto r=collect(f,busy); assert(r.result==Result::Changed && r.returned==1);
    }
    {
        Fake f; f.on_transfer=[&]{ for(auto& s:f.slots) s.eligible=false; };
        auto r=collect(f,busy); assert(r.result==Result::Complete && r.returned==1);
    }
    {
        Fake f; f.throw_transfer=true;
        auto r=collect(f,busy);
        assert(r.result==Result::Exception && r.attempted==1 && r.returned==0 && f.sent.size()==1 && !busy);
    }
    {
        Fake f; f.throw_snapshot=true;
        auto r=collect(f,busy); assert(r.result==Result::Exception && r.attempted==0 && !busy);
    }
    {
        Fake f; f.on_transfer=[&]{ auto nested=collect(f,busy); assert(nested.result==Result::Reentrant); };
        auto r=collect(f,busy); assert(r.returned==4 && f.sent.size()==4 && !busy);
    }
    {
        Fake f; int snapshots=0;
        f.on_snapshot=[&]{ if(++snapshots==3) f.open=false; };
        auto r=collect(f,busy); assert(r.result==Result::Changed && f.sent.empty());
    }
    {
        Fake f; f.slots[0].index=-1;
        assert(collect(f,busy).result==Result::InvalidSlots && f.sent.empty());
    }
    {
        Fake f; f.slots[0].holder=0;
        assert(collect(f,busy).result==Result::InvalidSlots && f.sent.empty());
    }
    {
        Fake f; f.slots[0].holder=f.slots[3].holder;
        assert(collect(f,busy).result==Result::InvalidSlots && f.sent.empty());
    }
    {
        Fake f; f.on_transfer=[&]{ --f.context.count; };
        auto r=collect(f,busy); assert(r.result==Result::Changed && r.returned==1);
    }
    {
        Fake f; f.on_transfer=[&]{ if(f.sent.size()==2) throw std::runtime_error("partial send"); };
        auto r=collect(f,busy);
        assert(r.result==Result::Exception && r.attempted==2 && r.returned==1 && !busy);
    }
    std::cout<<"loot module: synchronous batch, bounds, stock eligibility, cancellation/context changes, no replay, reentrancy: OK\n";
}
