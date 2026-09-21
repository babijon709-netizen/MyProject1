// Exercise the real /proc/pid/mem adapter against RX (not writable) pages in
// a child. Does not execute ARM instructions on a non-ARM host and therefore
// does not replace an Android instruction-cache/rendering test.
#include "fast_loot_memory.h"
#include "mem_io.h"
#include <cassert>
#include <iostream>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

struct Memory {
    memio::Reader reader;
    template<class T> bool read(uint64_t a, T& v) { return reader.read_quiet(a, &v, sizeof(v)); }
    template<class T> bool write(uint64_t a, const T& v) { return reader.write(a, &v, sizeof(v)); }
};
int main() {
    for (const auto& layout : fast_loot::kLayouts) {
        int to_child[2], from_child[2];
        assert(pipe(to_child)==0 && pipe(from_child)==0);
        const pid_t pid=fork(); assert(pid>=0);
        if (pid==0) {
            close(to_child[1]); close(from_child[0]);
            const size_t page=static_cast<size_t>(sysconf(_SC_PAGESIZE));
            const size_t code_page=layout.site & ~(page-1);
            // Reserve virtual address space only; commit just two pages.
            auto* image=static_cast<uint8_t*>(mmap(nullptr, code_page+page, PROT_NONE,
                                                   MAP_PRIVATE|MAP_ANONYMOUS, -1, 0));
            if (image==MAP_FAILED) _exit(2);
            if (mprotect(image,page,PROT_READ|PROT_WRITE)!=0 ||
                mprotect(image+code_page,page,PROT_READ|PROT_WRITE)!=0) _exit(3);
            std::memcpy(image,"\x7f" "ELF",4);
            std::memcpy(image+0x318,layout.build_id.data(),20);
            std::memcpy(image+layout.site-12,layout.guard.data(),32);
            std::memcpy(image+layout.site+fast_loot::kResumeOffset,fast_loot::kResumeGuard.data(),16);
            if (mprotect(image,page,PROT_READ)!=0 ||
                mprotect(image+code_page,page,PROT_READ|PROT_EXEC)!=0) _exit(4);
            uint64_t base=reinterpret_cast<uint64_t>(image);
            if (write(from_child[1],&base,sizeof(base))!=sizeof(base)) _exit(5);
            char cmd=0;
            while (read(to_child[0],&cmd,1)==1 && cmd!='q') {
                uint32_t value=*reinterpret_cast<volatile const uint32_t*>(image+layout.site);
                if (write(from_child[1],&value,sizeof(value))!=sizeof(value)) _exit(6);
            }
            _exit(0);
        }
        close(to_child[0]); close(from_child[1]);
        uint64_t base=0; assert(read(from_child[0],&base,sizeof(base))==sizeof(base));
        Memory io; assert(io.reader.bind(pid,base));
        // Capability is mocked: this host test only reads the bytes, never executes them.
        fast_loot::Controller<Memory> c(io,true);
        auto child_word=[&]() {
            const char cmd='r'; assert(write(to_child[1],&cmd,1)==1);
            uint32_t value=0; assert(read(from_child[0],&value,sizeof(value))==sizeof(value));
            return value;
        };
        assert(child_word()==layout.guard[3]);
        c.tick(1,base,true); assert(c.status==viewmodel::Status::Active);
        assert(child_word()==fast_loot::kFastInstruction);
        c.tick(1,base,false); assert(c.status==viewmodel::Status::Off);
        assert(child_word()==layout.guard[3]);
        c.tick(1,base,true); assert(c.reset(true)); assert(child_word()==layout.guard[3]);
        io.reader.unbind();
        const char quit='q'; assert(write(to_child[1],&quit,1)==1);
        close(to_child[1]); close(from_child[0]);
        int status=0; assert(waitpid(pid,&status,0)==pid && WIFEXITED(status) && WEXITSTATUS(status)==0);
    }
    std::cout << "fast loot: real child RX pages, /proc/pid/mem apply/readback/restore: OK\n";
}
