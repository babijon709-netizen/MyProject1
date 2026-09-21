// Exercise the same mapping gate and proc-memory transport on our own process.
// Does not attach to a game or change any library mapping.
#include "object_data_memory.h"
#include "mem_io.h"
#include <sys/mman.h>
#include <unistd.h>
#include <cassert>
#include <cstring>
#include <fstream>
#include <iterator>
#include <iostream>

static bool write_heap(memio::Reader& io, uint64_t a, float value) {
    std::ifstream file("/proc/self/maps");
    assert(file);
    std::string maps((std::istreambuf_iterator<char>(file)),{});
    return object_data::heap_writable(maps,a,sizeof(value)) && io.write(a,&value,sizeof(value));
}
int main() {
    const size_t page=static_cast<size_t>(sysconf(_SC_PAGESIZE));
    // Isolated anonymous reservation with guard pages: no adjacency to ELF bss.
    void* area=mmap(reinterpret_cast<void*>(0x3000000000ULL),page*3,PROT_NONE,
                    MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(area!=MAP_FAILED);
    auto* data=static_cast<unsigned char*>(area)+page;
    assert(mprotect(data,page,PROT_READ|PROT_WRITE)==0);
    std::memcpy(data,"\x7f" "ELF",4);
    const uint64_t address=reinterpret_cast<uint64_t>(data+0x100);
    const float original=2.5f;
    std::memcpy(data+0x100,&original,4);
    memio::Reader io;
    assert(io.bind(getpid(),reinterpret_cast<uint64_t>(data)));
    assert(write_heap(io,address,0.f));
    float observed=1.f;
    assert(io.read_quiet(address,&observed,4) && observed==0.f);
    assert(write_heap(io,address,original));
    assert(io.read_quiet(address,&observed,4) && observed==original);
    assert(mprotect(data,page,PROT_READ|PROT_EXEC)==0);
    assert(!write_heap(io,address,0.f)); // /proc/mem itself could force RX writes; gate must refuse
    assert(io.read_quiet(address,&observed,4) && observed==original);
    FILE* temporary=tmpfile(); assert(temporary);
    assert(ftruncate(fileno(temporary),page)==0);
    void* backed=mmap(nullptr,page,PROT_READ|PROT_WRITE,MAP_PRIVATE,fileno(temporary),0);
    assert(backed!=MAP_FAILED);
    assert(!write_heap(io,reinterpret_cast<uint64_t>(backed)+0x100,0.f));
    io.unbind(); munmap(backed,page); fclose(temporary); munmap(area,page*3);
    std::cout<<"weapons proc-mem: real anonymous data apply/restore; RX and file-backed writes refused: OK\n";
}
