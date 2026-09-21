#pragma once
// Write gate for object-data features ONLY. Existing FOV/FPS/aim adapters are
// separate. A fresh /proc/<pid>/maps snapshot is required at each actual write.
#include <cstdint>
#include <cstdio>
#include <string>
#include <limits>

namespace object_data {
inline bool heap_writable(const std::string& maps, uint64_t address, size_t size) {
    if (address < 0x10000 || !size || size > 64 ||
        address > std::numeric_limits<uint64_t>::max() - size) return false;
    uint64_t previous_end = 0;
    bool file_tail = false;
    size_t pos = 0;
    while (pos < maps.size()) {
        auto end = maps.find('\n', pos);
        if (end == std::string::npos) end = maps.size();
        const std::string line = maps.substr(pos, end - pos);
        pos = end + 1;
        unsigned long long lo = 0, hi = 0, offset = 0, inode = 0;
        unsigned major = 0, minor = 0;
        char perms[5]{};
        int consumed = 0;
        if (std::sscanf(line.c_str(), "%llx-%llx %4s %llx %x:%x %llu %n",
                        &lo, &hi, perms, &offset, &major, &minor, &inode, &consumed) != 7 ||
            !consumed || hi <= lo || lo < previous_end) return false;
        std::string name = line.substr(static_cast<size_t>(consumed));
        while (!name.empty() && (name.back() == ' ' || name.back() == '\r')) name.pop_back();
        const bool file = inode != 0 || name.find('/') != std::string::npos;
        const bool anonymous = name.empty() || name == "[heap]" ||
            name == "[anon:libc_malloc]" || name == "[anon:scudo:primary]" ||
            name == "[anon:scudo:secondary]";
        // ELF .bss may be inode=0, unnamed, and writable. Reject anonymous
        // tails contiguous with file mappings, including intervening guards.
        // Also intentionally reject unknown named mappings, ashmem and memfd.
        file_tail = file || (lo == previous_end && file_tail);
        previous_end = hi;
        if (address >= lo && address < hi) {
            return address + size <= hi && !file_tail && anonymous &&
                !inode && !offset && !major && !minor &&
                std::string(perms) == "rw-p";
        }
    }
    return false;
}
}
