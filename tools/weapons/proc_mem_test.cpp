// Exercise the real /proc/pid/mem adapter against RX (not writable) pages in
// a child. Does not execute ARM instructions on a non-ARM host and therefore
// does not replace an Android instruction-cache/rendering test.
#include "weapon_tuning_memory.h"
#include "mem_io.h"
#include <cassert>
#include <iostream>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <set>

struct Memory {
    memio::Reader reader;
    template<class T> bool read(uint64_t a, T& v) { return reader.read_quiet(a, &v, sizeof(v)); }
    template<class T> bool write(uint64_t a, const T& v) { return reader.write(a, &v, sizeof(v)); }
};
int main() {
    for (const auto& layout : weapon_tuning::kLayouts) {
        int to_child[2], from_child[2];
        assert(pipe(to_child)==0 && pipe(from_child)==0);
        const pid_t pid=fork(); assert(pid>=0);
        if (pid==0) {
            close(to_child[1]); close(from_child[0]);
            const size_t page=static_cast<size_t>(sysconf(_SC_PAGESIZE));
            std::set<size_t> pages;
            for (const auto& site : layout.sites) {
                pages.insert((site.rva-12) & ~(page-1));
                pages.insert((site.rva+19) & ~(page-1));
            }
            // Reserve virtual space; commit only the ID and instruction pages.
            auto* image=static_cast<uint8_t*>(mmap(nullptr, *pages.rbegin()+page, PROT_NONE,
                                                   MAP_PRIVATE|MAP_ANONYMOUS, -1, 0));
            if (image==MAP_FAILED) _exit(2);
            if (mprotect(image,page,PROT_READ|PROT_WRITE)!=0) _exit(3);
            for (auto offset:pages)
                if (mprotect(image+offset,page,PROT_READ|PROT_WRITE)!=0) _exit(3);
            std::memcpy(image,"\x7f" "ELF",4);
            std::memcpy(image+0x308,weapon_tuning::kGnuNote.data(),16);
            std::memcpy(image+0x318,layout.build_id.data(),20);
            for (const auto& site:layout.sites)
                std::memcpy(image+site.rva-12,site.guard.data(),32);
            if (mprotect(image,page,PROT_READ)!=0) _exit(4);
            for (auto offset:pages)
                if (mprotect(image+offset,page,PROT_READ|PROT_EXEC)!=0) _exit(4);
            uint64_t base=reinterpret_cast<uint64_t>(image);
            if (write(from_child[1],&base,sizeof(base))!=sizeof(base)) _exit(5);
            char cmd=0;
            while (read(to_child[0],&cmd,1)==1 && cmd!='q') {
                std::array<uint32_t,3> values{};
                for(size_t i=0;i<3;++i)
                    values[i]=*reinterpret_cast<volatile const uint32_t*>(image+layout.sites[i].rva);
                if (write(from_child[1],values.data(),sizeof(values))!=sizeof(values)) _exit(6);
            }
            _exit(0);
        }
        close(to_child[0]); close(from_child[1]);
        uint64_t base=0; assert(read(from_child[0],&base,sizeof(base))==sizeof(base));
        Memory io; assert(io.reader.bind(pid,base));
        // Capability is mocked: this host test only reads the bytes, never executes them.
        weapon_tuning::Controller<Memory> c(io,true);
        auto check=[&](bool recoil,bool spread) {
            const char cmd='r'; assert(write(to_child[1],&cmd,1)==1);
            std::array<uint32_t,3> values{};
            assert(read(from_child[0],values.data(),sizeof(values))==sizeof(values));
            for(size_t i=0;i<3;++i) {
                const uint32_t want=(i==0?recoil:spread)
                    ? (i==0?weapon_tuning::kNoRecoil:weapon_tuning::kNoSpread) : layout.sites[i].guard[3];
                assert(values[i]==want);
            }
        };
        check(false,false);
        for (int mask: {1,3,2,0}) {
            c.tick(1,base,mask&1,mask&2); check(mask&1,mask&2);
            assert(c.status.recoil==((mask&1)?viewmodel::Status::Active:viewmodel::Status::Off));
            assert(c.status.spread==((mask&2)?viewmodel::Status::Active:viewmodel::Status::Off));
        }
        c.tick(1,base,true,true); assert(c.reset(true)); check(false,false);
        io.reader.unbind();
        const char quit='q'; assert(write(to_child[1],&quit,1)==1);
        close(to_child[1]); close(from_child[0]);
        int status=0; assert(waitpid(pid,&status,0)==pid && WIFEXITED(status) && WEXITSTATUS(status)==0);
    }
    std::cout << "weapons: real child RX pages, /proc/pid/mem apply/readback/restore: OK\n";
}
