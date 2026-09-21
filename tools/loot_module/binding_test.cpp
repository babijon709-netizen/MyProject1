// Exercise the real binder against synthetic objects in THIS process only.
// Including the adapter permits read-only discovery / heap-CAS tests while
// its exported install API still refuses host-side native game execution.
#include "module.cpp"
#include <cassert>
#include <iostream>
#include <sys/mman.h>

int main() {
    const size_t page=size_t(sysconf(_SC_PAGESIZE));
    constexpr size_t bytes=0x10000;
    void* reservation=mmap(reinterpret_cast<void*>(0x3000000000ULL),bytes+page*2,PROT_NONE,
                           MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(reservation!=MAP_FAILED);
    auto start=reinterpret_cast<uint64_t>(reservation)+page;
    assert(mprotect(reinterpret_cast<void*>(start),bytes,PROT_READ|PROT_WRITE)==0);
    const auto put=[](uint64_t a,uint64_t value){std::memcpy(reinterpret_cast<void*>(a),&value,8);};
    const auto put32=[](uint64_t a,int32_t value){std::memcpy(reinterpret_cast<void*>(a),&value,4);};
    uint64_t next_klass=start+0xa000;
    const auto object=[&](uint64_t a,const char* name,const char* ns){
        uint64_t klass=next_klass; next_klass+=0x100;
        put(a,klass); put(klass+0x10,reinterpret_cast<uint64_t>(name));
        put(klass+0x18,reinterpret_cast<uint64_t>(ns));
    };
    const uint64_t view=start, button=start+0x1000, event=start+0x2000, calls=start+0x3000,
                   list=start+0x4000,array=start+0x5000,call=start+0x6000,action=start+0x7000,mi=start+0x8000;
    object(view,"LootScreenViewImpl",views); object(button,"Button","UnityEngine.UI");
    object(calls,"InvokableCallList","UnityEngine.Events");
    object(call,"InvokableCall","UnityEngine.Events");
    object(action,"UnityAction","UnityEngine.Events");
    put(view+0x10,0x70000000); put(button+0x10,0x70001000);
    put(view+0x48,button); put(button+0x100,event); put(event+0x10,calls);
    put(calls+0x18,list); put(list+0x10,array); put32(list+0x18,1); put(array+0x18,2);
    put(array+0x20,call); put(call+0x10,action);
    const uint64_t base=0x10000000, original=base+layouts[0].fn[StockClick].rva, replacement=0x20000100;
    put(action+0x10,original); put(action+0x18,original); put(action+0x20,view);
    put(action+0x28,mi); put(mi+8,original); put(action+0x40,view);
    assert(find_delegate(base,layouts[0],view)==action);
    put(action+0x78,start+0x9000); assert(!find_delegate(base,layouts[0],view)); put(action+0x78,0);
    put(action+0x20,button); assert(!find_delegate(base,layouts[0],view)); put(action+0x20,view);
    put(action+0x40,action); assert(!find_delegate(base,layouts[0],view)); put(action+0x40,view);
    put(mi+8,original+4); assert(!find_delegate(base,layouts[0],view)); put(mi+8,original);
    put32(list+0x18,33); assert(!find_delegate(base,layouts[0],view)); put32(list+0x18,1);
    put(array+0x28,call); put32(list+0x18,2);
    assert(!find_delegate(base,layouts[0],view)); put32(list+0x18,1);
    assert(memory.exchange_heap(action+0x18,original,replacement));
    assert(!find_delegate(base,layouts[0],view));
    assert(find_delegate(base,layouts[0],view,replacement)==action);
    assert(!memory.exchange_heap(action+0x18,original,original)); // stale expected pointer
    assert(memory.ptr(action+0x18)==replacement);
    put32(list+0x18,0); assert(!find_delegate(base,layouts[0],view,replacement)); // detached: not reachable
    put32(list+0x18,1);
    assert(memory.exchange_heap(action+0x18,replacement,original));
    assert(find_delegate(base,layouts[0],view)==action);
    assert(mprotect(reinterpret_cast<void*>(start),bytes,PROT_READ)==0);
    assert(!memory.exchange_heap(action+0x18,original,replacement));
    assert(memory.ptr(action+0x18)==original);
    FILE* file=tmpfile(); assert(file);
    assert(ftruncate(fileno(file),page)==0);
    void* backed=mmap(nullptr,page,PROT_READ|PROT_WRITE,MAP_PRIVATE,fileno(file),0);
    assert(backed!=MAP_FAILED);
    put(reinterpret_cast<uint64_t>(backed),original);
    assert(!memory.exchange_heap(reinterpret_cast<uint64_t>(backed),original,replacement));
    munmap(backed,page); fclose(file); munmap(reservation,bytes+page*2);
    std::cout<<"loot binder: exact listener/ABI shape, duplicates/multicast/detach rejected; heap CAS/restore; RO/file-backed writes rejected: OK\n";
}
