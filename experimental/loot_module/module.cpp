#include "api.h"
#include "batch.h"
#include "layouts.h"
#include "object_data_memory.h"
#include <array>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <fcntl.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sys/syscall.h>

#define EXPORT extern "C" __attribute__((visibility("default")))
extern "C" __attribute__((visibility("hidden"))) void arena_loot_entry(void*,const void*);
namespace {
using namespace loot_module;
constexpr const char* views="HyperHug.Games.Oxide.Features.Loot.UI.Views";
constexpr const char* presenters="HyperHug.Games.Oxide.Features.Loot.UI.Presenters";
std::recursive_mutex mutex;
// No automatic game code execution, threads, scanning or patching on dlopen.
struct Memory {
    int fd=-1;
    template<class T> bool read(uint64_t a,T& out) {
        if(a<0x10000 || a>0x00ffffffffffffffULL-sizeof(T)) return false;
        if(fd<0) fd=open("/proc/self/mem",O_RDONLY|O_CLOEXEC);
        return fd>=0 && pread(fd,&out,sizeof(T),static_cast<off_t>(a))==ssize_t(sizeof(T));
    }
    uint64_t ptr(uint64_t a) {
        uint64_t p=0;
        // This prototype rejects tagged pointers rather than stripping MTE tags
        // and subsequently handing invalid native pointers to game functions.
        return read(a,p) && p>=0x10000 && !(p&0xff00000000000000ULL) && !(p&3) ? p : 0;
    }
    bool text(uint64_t a,const char* expected) {
        size_t n=std::strlen(expected)+1; char value[128]{};
        if(n>sizeof(value) || !a || fd<0) return false;
        return pread(fd,value,n,static_cast<off_t>(a))==ssize_t(n) && std::memcmp(value,expected,n)==0;
    }
    bool klass_named(uint64_t klass,const char* name,const char* ns) {
        // String addresses need not be aligned, unlike object pointers.
        uint64_t np=0,sp=0;
        return klass && read(klass+0x10,np) && read(klass+0x18,sp) && text(np,name) && text(sp,ns);
    }
    bool named(uint64_t obj,const char* name,const char* ns) {
        return klass_named(ptr(obj),name,ns);
    }
    uint64_t unity(uint64_t obj,const char* name,const char* ns) {
        return named(obj,name,ns)?ptr(obj+0x10):0;
    }
    bool exchange_heap(uint64_t address,uint64_t expected,uint64_t desired) try {
        if(address&7) return false;
        std::ifstream file("/proc/self/maps");
        if(!file) return false;
        std::string maps,line;
        while(std::getline(file,line)) { maps+=line; maps+='\n'; if(maps.size()>(8u<<20)) return false; }
        if(!file.eof() || !object_data::heap_writable(maps,address,8)) return false;
        // Only existing delegate.invoke_impl in anonymous RW heap is changed.
        // No executable-page permissions or code-cache operations are involved.
        return __atomic_compare_exchange_n(reinterpret_cast<uint64_t*>(address),&expected,desired,
                                            false,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);
    } catch(...) { return false; }
} memory;
uint64_t image=0;
const Layout* layout=nullptr;
long ui_tid=0;
bool enabled=false, busy=false;
int32_t last_frame=-1;
struct Binding {
    uint64_t view=0, native=0, action=0, klass=0, method=0, original=0;
} binding;
ArenaLootReportV1 report{sizeof(ArenaLootReportV1),ARENA_LOOT_OFF,0,0,0,0,0};

long tid() { return syscall(SYS_gettid); }
bool unity_thread() {
#if defined(__ANDROID__) && defined(__aarch64__)
    char name[16]{};
    return prctl(PR_GET_NAME,name)==0 && std::strcmp(name,"UnityMain")==0;
#else
    return false; // host builds are for compile/link checks, never native calls
#endif
}
const Layout* recognize(uint64_t base) {
    if(base<0x10000 || (base&0xfff) || base>0x00ffffffefffffffULL) return nullptr;
    std::array<uint8_t,20> id{};
    if(!memory.read(base+0x318,id)) return nullptr;
    for(const auto& l:layouts) if(l.build_id==id) {
        uint32_t wait=0;
        if(!memory.read(base+l.legacy_wait,wait) || wait!=l.original_wait) return nullptr;
        for(const auto& f:l.fn) {
            std::array<uint32_t,4> code{};
            if(!memory.read(base+f.rva,code) || code!=f.head) return nullptr;
        }
        return &l;
    }
    return nullptr;
}
template<class F> F function(Function index) { return reinterpret_cast<F>(image+layout->fn[index].rva); }

bool delegate_matches(uint64_t a,uint64_t view,uint64_t original,uint64_t expected_invoke) {
    uint64_t multicast=1;
    if(!memory.named(a,"UnityAction","UnityEngine.Events") ||
       memory.ptr(a+0x10)!=original || memory.ptr(a+0x18)!=expected_invoke ||
       memory.ptr(a+0x20)!=view || memory.ptr(a+0x40)!=view ||
       !memory.read(a+0x78,multicast) || multicast) return false;
    auto method=memory.ptr(a+0x28);
    return method && memory.ptr(method+8)==original;
}
uint64_t find_delegate(uint64_t base,const Layout& l,uint64_t view,uint64_t expected_invoke=0) {
    if(!memory.unity(view,"LootScreenViewImpl",views)) return 0;
    auto button=memory.ptr(view+0x48);
    if(!memory.unity(button,"Button","UnityEngine.UI")) return 0;
    auto event=memory.ptr(button+0x100), calls=memory.ptr(event+0x10);
    if(!memory.named(calls,"InvokableCallList","UnityEngine.Events")) return 0;
    auto list=memory.ptr(calls+0x18), array=memory.ptr(list+0x10);
    int32_t size=0; uint64_t capacity=0;
    if(!list || !array || !memory.read(list+0x18,size) || size<1 || size>32 ||
       !memory.read(array+0x18,capacity) || capacity<uint64_t(size) || capacity>256) return 0;
    uint64_t found=0, original=base+l.fn[StockClick].rva;
    if(!expected_invoke) expected_invoke=original;
    for(int32_t i=0;i<size;++i) {
        auto call=memory.ptr(array+0x20+8*uint64_t(i));
        if(!memory.named(call,"InvokableCall","UnityEngine.Events")) continue;
        auto action=memory.ptr(call+0x10);
        if(delegate_matches(action,view,original,expected_invoke)) {
            if(found) return 0; // ambiguous bindings: never replace all listeners
            found=action;
        }
    }
    return found;
}
struct Host {
    uint64_t view;
    bool snapshot(Context& c) {
        auto& a=c.identity;
        a[0]=view; a[1]=memory.unity(view,"LootScreenViewImpl",views);
        a[18]=memory.ptr(view+0x30); a[19]=memory.unity(a[18],"LootContainerView",views);
        a[2]=memory.ptr(a[18]+0x20); a[3]=memory.unity(a[2],"ItemContainer","Oxide.GUISystem");
        a[4]=memory.ptr(a[2]+0x80);
        a[16]=memory.ptr(view+0x48); a[17]=memory.unity(a[16],"Button","UnityEngine.UI");
        uint8_t kind=0,locked=1;
        if(!a[1] || !a[3] || !a[19] || !a[17] || !memory.named(a[4],"Inventory","Oxide") ||
           !memory.read(a[2]+0x70,locked) || locked || !memory.read(a[2]+0xf0,kind) || kind!=1 ||
           !memory.read(a[2]+0x8c,c.count)) return false; // only the currently open loot source
        const auto slots=memory.ptr(a[4]+0x10); uint64_t capacity=0;
        if(!slots || !memory.read(slots+0x18,capacity) || capacity>kMaxSlots ||
           c.count<0 || uint64_t(c.count)>capacity) return false;
        a[14]=kind;
        auto klass=memory.ptr(image+layout->game_typeinfo);
        if(!memory.klass_named(klass,"GameControllerBase","Oxide")) return false;
        const auto sf=memory.ptr(klass+0xb8);
        a[5]=memory.ptr(sf+0x10); a[6]=memory.unity(a[5],"PlayerManager","Oxide");
        a[7]=memory.ptr(a[5]+0x98); a[8]=memory.unity(a[7],"PlayerInventory","Oxide");
        auto net=memory.ptr(a[5]+0x40); uint8_t local=0;
        if(!a[6] || !a[8] || !memory.named(net,"NetworkIdentity","Mirror") ||
           !memory.read(net+0x4a,local) || local!=1) return false;
        a[15]=memory.ptr(view+0x90);
        if(!memory.named(a[15],"Action","System")) return false;
        a[9]=memory.ptr(a[15]+0x20);
        uint64_t multicast=1;
        const auto action_method=memory.ptr(a[15]+0x28), action_fn=memory.ptr(a[15]+0x10);
        if(!memory.read(a[15]+0x78,multicast) || multicast || !action_method || !action_fn ||
           memory.ptr(action_method+8)!=action_fn || memory.ptr(a[15]+0x18)!=action_fn ||
           memory.ptr(a[15]+0x40)!=a[9]) return false;
        if(!memory.named(a[9],"LootScreenPresenter",presenters) || memory.ptr(a[9]+0x10)!=view) return false;
        a[10]=memory.ptr(a[9]+0x30); a[11]=memory.ptr(a[9]+0x50);
        uint8_t collecting=1,disposed=1; int32_t cancel_state=-1;
        if(!memory.named(a[10],layout->controller,"") || !memory.read(a[10]+0x10,collecting) || collecting ||
           !memory.named(a[11],"CancellationTokenSource","System.Threading") ||
           !memory.read(a[11]+0x20,cancel_state) || cancel_state!=1 ||
           !memory.read(a[11]+0x28,disposed) || disposed) return false;
        // The generic MethodInfo must already be initialized by the game. Do
        // not initialize or overwrite any library-owned metadata ourselves.
        auto mi=memory.ptr(memory.ptr(image+layout->gui_method_got));
        if(!mi || memory.ptr(mi)!=image+layout->fn[GetGui].rva) return false;
        a[12]=reinterpret_cast<uint64_t>(function<void*(*)(const void*)>(GetGui)(reinterpret_cast<void*>(mi)));
        a[13]=memory.unity(a[12],"InventoryController","Oxide");
        if(!a[13]) return false;
        return !function<bool(*)(void*,const void*)>(GuiBlocked)(reinterpret_cast<void*>(a[12]),nullptr);
    }
    bool slot(const Context& c,int i,Slot& s) {
        auto holder=function<void*(*)(void*,int32_t,const void*)>(GetSlot)(reinterpret_cast<void*>(c.identity[2]),i,nullptr);
        s.holder=reinterpret_cast<uint64_t>(holder);
        if(!memory.named(s.holder,layout->holder,"") ||
           memory.ptr(s.holder+0x20)!=c.identity[4] || !memory.read(s.holder+0x18,s.index)) return false;
        s.item=memory.ptr(s.holder+0x10);
        if(!s.item) { uint64_t raw=1; return memory.read(s.holder+0x10,raw) && raw==0; }
        if(!memory.named(s.item,"Item","Oxide")) return false;
        s.eligible=function<bool(*)(void*,const void*)>(SlotHasItem)(holder,nullptr) &&
                   function<bool(*)(void*,const void*)>(ItemFits)(reinterpret_cast<void*>(s.item),nullptr);
        return true;
    }
    void transfer(const Context& c,int index) {
        function<void(*)(void*,uint8_t,int32_t,const void*)>(TransferSlot)(
            reinterpret_cast<void*>(c.identity[7]),uint8_t(c.identity[14]),index,nullptr);
    }
};

void collect_click_impl(void* view,const void* method) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if(!layout || recognize(image)!=layout) { report.status=ARENA_LOOT_UNSUPPORTED; return; }
    if(tid()!=ui_tid) { report.status=ARENA_LOOT_WRONG_THREAD; return; }
    if(!enabled || reinterpret_cast<uint64_t>(view)!=binding.view) {
        function<void(*)(void*,const void*)>(StockClick)(view,method);
        return;
    }
    const int32_t frame=function<int32_t(*)(const void*)>(FrameCount)(nullptr);
    if(frame==last_frame) return; // duplicated/copied listeners must not replay the click
    last_frame=frame;
    Host host{reinterpret_cast<uint64_t>(view)};
    auto r=collect(host,busy);
    report.batch_result=uint32_t(r.result); report.inspected=r.inspected;
    report.attempted=r.attempted; report.returned=r.returned;
    report.status=r.result==Result::Complete?ARENA_LOOT_READY:ARENA_LOOT_NOT_READY;
    // No replay after a partial/throwing batch. Preflight NotReady is safe to
    // delegate back to the stock handler (same UI click, no prior transfer).
    if(r.result==Result::NotReady && r.attempted==0)
        function<void(*)(void*,const void*)>(StockClick)(view,method);
}
}

// Unity's invoker uses BR x2, not a C++ BLR. Accept both jump and call
// branch types on BTI-enabled devices. This is OUR module's entry point;
// no instructions in the game or another library are modified.
extern "C" __attribute__((visibility("hidden"))) void arena_loot_dispatch(void* view,const void* method) {
    collect_click_impl(view,method);
}
#if defined(__aarch64__)
__asm__(
    ".text\n"
    ".p2align 2\n"
    ".global arena_loot_entry\n"
    ".hidden arena_loot_entry\n"
    ".type arena_loot_entry, %function\n"
    "arena_loot_entry:\n"
    ".inst 0xd50324df\n" // bti jc; safe as a hint on pre-BTI hardware
    "b arena_loot_dispatch\n"
    ".size arena_loot_entry, .-arena_loot_entry\n"
);
#else
extern "C" __attribute__((visibility("hidden"))) void arena_loot_entry(void* view,const void* method) {
    arena_loot_dispatch(view,method);
}
#endif

EXPORT int arena_loot_probe_v1(uint64_t base,uint64_t view) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    const auto* l=recognize(base);
    if(!l) return ARENA_LOOT_UNSUPPORTED;
    return find_delegate(base,*l,view)?ARENA_LOOT_READY:ARENA_LOOT_NOT_READY;
}
EXPORT int arena_loot_install_v1(uint64_t base,uint64_t view) {
    if(!unity_thread()) return ARENA_LOOT_WRONG_THREAD;
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if(ui_tid && tid()!=ui_tid) return ARENA_LOOT_WRONG_THREAD;
    if(binding.action || busy) return ARENA_LOOT_BINDING_CHANGED; // disable first
    const auto* l=recognize(base);
    if(!l || (image && image!=base)) return ARENA_LOOT_UNSUPPORTED;
    // Never change build under possible copied callbacks left in the game.
    if(layout && layout!=l) return ARENA_LOOT_UNSUPPORTED;
    uint64_t action=find_delegate(base,*l,view);
    if(!action) return ARENA_LOOT_NOT_READY;
    image=base; layout=l;
    Host host{view}; Context c;
    try { if(!host.snapshot(c)) return ARENA_LOOT_NOT_READY; }
    catch(...) { return ARENA_LOOT_NOT_READY; }
    auto original=base+l->fn[StockClick].rva;
    Binding next{view,memory.ptr(view+0x10),action,memory.ptr(action),memory.ptr(action+0x28),original};
    if(!delegate_matches(action,view,original,original)) return ARENA_LOOT_BINDING_CHANGED;
    // Publication is on the same Unity thread as invocation; no half-filled
    // delegate (method/target/code stay original), just one aligned pointer CAS.
    binding=next; ui_tid=tid(); last_frame=-1; enabled=true;
    if(!memory.exchange_heap(action+0x18,original,reinterpret_cast<uint64_t>(&arena_loot_entry))) {
        binding={}; enabled=false; return ARENA_LOOT_MEMORY_ERROR;
    }
    report={sizeof(report),ARENA_LOOT_READY,0,0,0,0,action};
    return ARENA_LOOT_READY;
}
EXPORT int arena_loot_disable_v1() {
    if(!unity_thread()) return ARENA_LOOT_WRONG_THREAD;
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if(busy) return ARENA_LOOT_BINDING_CHANGED;
    if(ui_tid && tid()!=ui_tid) return ARENA_LOOT_WRONG_THREAD;
    enabled=false;
    report.status=ARENA_LOOT_OFF;
    if(binding.action) {
        // If Unity destroyed the view or replaced its delegate, never write
        // through the stale address. NODELETE keeps any outstanding copy safe.
        const bool same=find_delegate(image,*layout,binding.view,reinterpret_cast<uint64_t>(&arena_loot_entry))==binding.action &&
                        memory.ptr(binding.view+0x10)==binding.native &&
                        memory.ptr(binding.action)==binding.klass &&
                        memory.ptr(binding.action+0x28)==binding.method &&
                        delegate_matches(binding.action,binding.view,binding.original,
                                         reinterpret_cast<uint64_t>(&arena_loot_entry));
        if(same && !memory.exchange_heap(binding.action+0x18,reinterpret_cast<uint64_t>(&arena_loot_entry),binding.original)) {
            report.status=ARENA_LOOT_MEMORY_ERROR;
            return ARENA_LOOT_MEMORY_ERROR;
        }
    }
    binding={}; report.status=ARENA_LOOT_OFF; report.delegate_address=0;
    return ARENA_LOOT_OFF;
}
EXPORT int arena_loot_report_v1(ArenaLootReportV1* out,uint32_t size) {
    if(!out || size!=sizeof(ArenaLootReportV1)) return ARENA_LOOT_UNSUPPORTED;
    std::lock_guard<std::recursive_mutex> lock(mutex);
    *out=report; return int(report.status);
}
