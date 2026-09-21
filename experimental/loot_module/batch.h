#pragma once
// Synchronous request batch, invoked ONLY by the stock UI callback on Unity's
// thread. No inventory fabrication, timers, coroutine-state edits or retries.
#include <array>
#include <cstdint>

namespace loot_module {
constexpr int kMaxSlots = 256;
enum class Result : uint32_t { Complete, NotReady, InvalidSlots, Changed, Reentrant, Exception };
struct Report {
    Result result = Result::NotReady;
    uint32_t inspected = 0, attempted = 0, returned = 0;
};
struct Context {
    // Includes object/native identities, player, inventory, source, presenter,
    // cancellation source and container kind. Adapter must revalidate live UI.
    std::array<uint64_t,20> identity{};
    int count = 0;
    bool operator==(const Context& b) const { return identity==b.identity && count==b.count; }
};
struct Slot {
    uint64_t holder = 0, item = 0;
    int index = -1;
    bool eligible = false;
    bool operator==(const Slot& b) const { return holder==b.holder && item==b.item && index==b.index; }
};
// Host contract: snapshot() requires open/local/unlocked/not-cancelled context;
// slot() returns live holder+item identity and the stock eligibility predicate;
// transfer() invokes the stock *single-slot* request, not a server-side handler.
// No allocations or worker queue here. A click produces at most one bounded pass.
template<class Host> Report collect(Host& host, bool& busy) {
    if (busy) return {Result::Reentrant};
    busy=true;
    struct Release { bool& b; ~Release() { b=false; } } release{busy};
    Report out;
    try {
        Context initial;
        if (!host.snapshot(initial)) return out;
        if (initial.count<0 || initial.count>kMaxSlots) { out.result=Result::InvalidSlots; return out; }
        std::array<Slot,kMaxSlots> slots{};
        // Capture all indices before the first request. Refuse malformed or
        // duplicate indices rather than sending a partial speculative batch.
        for (int i=initial.count-1;i>=0;--i) {
            Context fresh;
            if (!host.snapshot(fresh) || !(fresh==initial)) { out.result=Result::Changed; return out; }
            if (!host.slot(initial,i,slots[i])) { out.result=Result::InvalidSlots; return out; }
            ++out.inspected;
            if (!slots[i].holder || slots[i].index<0) { out.result=Result::InvalidSlots; return out; }
            for (int j=i+1;j<initial.count;++j)
                if (slots[i].index==slots[j].index || slots[i].holder==slots[j].holder) { out.result=Result::InvalidSlots; return out; }
        }
        for (int i=initial.count-1;i>=0;--i) {
            Context fresh;
            if (!host.snapshot(fresh) || !(fresh==initial)) { out.result=Result::Changed; return out; }
            Slot now;
            if (!host.slot(initial,i,now) || !(now==slots[i])) { out.result=Result::Changed; return out; }
            if (!now.item || !now.eligible) continue;
            // Recheck UI/source/cancellation after native eligibility callbacks.
            if (!host.snapshot(fresh) || !(fresh==initial)) { out.result=Result::Changed; return out; }
            ++out.attempted; // a throwing request might already have side effects
            host.transfer(initial,now.index);
            ++out.returned; // does NOT mean the server accepted the transfer
        }
        out.result=Result::Complete;
    } catch (...) { out.result=Result::Exception; }
    return out;
}
}
