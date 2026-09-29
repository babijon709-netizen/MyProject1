#pragma once
// Write gate for object-data features ONLY. Existing FOV/FPS/aim adapters are
// separate. A /proc/<pid>/maps snapshot is required at each actual write; the
// caller refreshes it whenever an address is not confirmed by the current one.
//
// Разбор идёт без выделений памяти: карта процесса игры — это тысячи строк, и
// прежний вариант создавал на каждую строку по два std::string. На горячем
// пути (быстрая стрельба/перезарядка пишут несколько полей в кадре) это
// съедало кадр оверлея целиком.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <limits>

namespace object_data {
// Подтверждает адрес и заодно отдаёт границы подтверждённого отображения:
// вызывающий кэширует диапазон и не разбирает карту на каждую запись (разбор
// нескольких сотен килобайт стоит миллисекунды — это и был лаг оверлея).
inline bool heap_writable_range(const std::string& maps, uint64_t address, size_t size,
                                uint64_t& range_lo, uint64_t& range_hi) {
    range_lo = range_hi = 0;
    if (address < 0x10000 || !size || size > 64 ||
        address > std::numeric_limits<uint64_t>::max() - size) return false;
    uint64_t previous_end = 0;
    bool file_tail = false;
    size_t pos = 0;
    while (pos < maps.size()) {
        size_t end = maps.find('\n', pos);
        if (end == std::string::npos) end = maps.size();
        const char* const line_begin = maps.data() + pos;
        const size_t line_length = end - pos;
        pos = end + 1;

        // sscanf требует нуль-терминированную строку, но копировать нужно
        // только числовую шапку: имя отображения может быть сколь угодно
        // длинным (dex-в-памяти, длинные пути APK). Прежняя копия строки
        // целиком в буфер 512 байт отвергала ВСЮ карту из-за одной длинной
        // строки, и тогда переставали работать все записи в кучу — отдача,
        // разброс и темп оружия молча оставались невыключенными.
        char header[160];
        const size_t header_length = line_length < sizeof(header) - 1
            ? line_length : sizeof(header) - 1;
        std::memcpy(header, line_begin, header_length);
        header[header_length] = '\0';

        unsigned long long lo = 0, hi = 0, offset = 0, inode = 0;
        unsigned major = 0, minor = 0;
        char perms[5]{};
        int consumed = 0;
        if (std::sscanf(header, "%llx-%llx %4s %llx %x:%x %llu %n",
                        &lo, &hi, perms, &offset, &major, &minor, &inode, &consumed) != 7 ||
            !consumed || (size_t)consumed > line_length ||
            hi <= lo || lo < previous_end) return false;

        const char* name = line_begin + consumed;
        size_t name_length = line_length - (size_t)consumed;
        while (name_length && (name[name_length - 1] == ' ' || name[name_length - 1] == '\r'))
            --name_length;
        auto name_is = [&](const char* expected) {
            const size_t length = std::strlen(expected);
            return name_length == length && std::memcmp(name, expected, length) == 0;
        };
        const bool file = inode != 0 || std::memchr(name, '/', name_length) != nullptr;
        const bool anonymous = name_length == 0 || name_is("[heap]") ||
            name_is("[anon:libc_malloc]") || name_is("[anon:scudo:primary]") ||
            name_is("[anon:scudo:secondary]");
        // ELF .bss may be inode=0, unnamed, and writable. Reject anonymous
        // tails contiguous with file mappings, including intervening guards.
        // Also intentionally reject unknown named mappings, ashmem and memfd.
        file_tail = file || (lo == previous_end && file_tail);
        previous_end = hi;
        if (address >= lo && address < hi) {
            const bool ok = address + size <= hi && !file_tail && anonymous &&
                !inode && !offset && !major && !minor &&
                std::strcmp(perms, "rw-p") == 0;
            if (ok) { range_lo = lo; range_hi = hi; }
            return ok;
        }
    }
    return false;
}

inline bool heap_writable(const std::string& maps, uint64_t address, size_t size) {
    uint64_t lo = 0, hi = 0;
    return heap_writable_range(maps, address, size, lo, hi);
}
}
