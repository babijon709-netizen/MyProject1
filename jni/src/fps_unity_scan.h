#pragma once
// fps_unity_scan.h — поиск поля targetFrameRate в libunity.so НЕИЗВЕСТНОЙ
// версии движка.
//
// Зачем. Раньше разблокировка FPS работала только на двух образах libunity,
// прошитых в fps_memory.h по GNU build ID вместе с готовыми RVA. libunity.so
// не собирается под игру: это готовая библиотека из редактора Unity, одна на
// всю версию движка. Значит обновление игры НЕ меняет её (build ID тот же), а
// вот обновление Unity меняет её целиком — все RVA уезжают, таблица перестаёт
// совпадать, и разблокировка молча ничего не делает («не работает анлок фпс»,
// в журнале «fps: libunity не опознан»). Добавить новую строку в таблицу без
// самого файла невозможно, поэтому раскладку ищем на месте.
//
// Что ищем. Геттер Application::GetTargetFrameRate — тривиальная функция из
// трёх инструкций:
//     adrp x8, <страница>
//     ldr  w0, [x8, #<смещение>]
//     ret
// Пара adrp+ldr кодирует АДРЕС самого поля, поэтому найденный геттер сразу
// даёт и поле. Проверено на обеих известных сборках: 0x7390e0 -> 0x17b8184 и
// 0x7392f0 -> 0x17b8a74 раскодируются ровно в табличные значения.
//
// Как отсеиваем чужие геттеры (таких троек в движке много):
//   * поле обязано лежать в ЗАПИСЫВАЕМОМ сегменте образа;
//   * в файле оно инициализировано -1 — это документированное значение по
//     умолчанию Application.targetFrameRate («платформа решает сама»);
//   * в живом процессе в нём либо -1, либо правдоподобный предел кадров.
// Кандидат принимается, только если он ОДИН. Иначе ничего не патчим: писать
// число в случайную глобальную переменную движка хуже, чем не разблокировать
// FPS.
//
// Файл читается с диска (путь берётся из карты процесса), а не из памяти
// игры: так доступны заголовки ELF и исходное содержимое .data, и ни одного
// лишнего обращения к чужому процессу.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <vector>

namespace viewmodel {

struct UnityFpsCandidate {
    uint64_t getter_rva = 0;      // адрес геттера относительно базы образа
    uint64_t target_rva = 0;      // адрес поля targetFrameRate
    uint32_t load_instruction = 0;  // ldr w0,[xN,#imm] — его же сверяем в памяти
};

struct UnityFpsScan {
    std::vector<UnityFpsCandidate> candidates;
    int triples = 0;        // сколько троек-геттеров вообще встретилось
    bool file_ok = false;   // ELF разобран
};

// ---- разбор инструкций A64 --------------------------------------------------

// adrp xD, #imm  ->  страница; возвращает false, если это не adrp.
inline bool a64_adrp_page(uint64_t pc_rva, uint32_t insn, unsigned& rd, uint64_t& page) {
    if ((insn & 0x9F000000u) != 0x90000000u) return false;
    rd = insn & 0x1Fu;
    const int64_t immlo = (insn >> 29) & 3u;
    const int64_t immhi = (insn >> 5) & 0x7FFFFu;
    int64_t imm = (immhi << 2) | immlo;
    if (imm & (int64_t)(1ll << 20)) imm -= (int64_t)(1ll << 21);   // знаковое 21-битное
    page = (pc_rva & ~0xFFFull) + (uint64_t)(imm * 4096);
    return true;
}

// ldr wT, [xN, #imm12*4] — 32-битная форма с беззнаковым смещением.
inline bool a64_ldr32_imm(uint32_t insn, unsigned& rt, unsigned& rn, uint32_t& offset) {
    if ((insn & 0xFFC00000u) != 0xB9400000u) return false;
    rt = insn & 0x1Fu;
    rn = (insn >> 5) & 0x1Fu;
    offset = ((insn >> 10) & 0xFFFu) * 4u;
    return true;
}

inline bool a64_is_ret(uint32_t insn) { return insn == 0xD65F03C0u; }

// Три инструкции по адресу getter_rva — это «вернуть int32 из глобальной
// переменной»? Тогда target_rva — адрес этой переменной.
inline bool decode_int32_getter(uint64_t getter_rva, const uint32_t code[3], uint64_t& target_rva) {
    unsigned rd = 0, rt = 0, rn = 0;
    uint64_t page = 0;
    uint32_t offset = 0;
    if (!a64_adrp_page(getter_rva, code[0], rd, page)) return false;
    if (!a64_ldr32_imm(code[1], rt, rn, offset)) return false;
    if (!a64_is_ret(code[2])) return false;
    if (rn != rd || rt != 0) return false;   // та же база, результат в w0
    target_rva = page + offset;
    return true;
}

// Предел кадров, который мог выставить сам движок или игра.
inline bool looks_like_frame_rate(int32_t value) {
    if (value == -1) return true;
    static const int32_t kRates[] = {24, 25, 30, 32, 36, 40, 45, 48, 50, 55, 60,
                                     70, 72, 75, 80, 90, 100, 110, 120, 144, 165,
                                     180, 240, 360};
    for (int32_t rate : kRates) if (value == rate) return true;
    return false;
}

// ---- ELF --------------------------------------------------------------------

namespace elf64 {
struct Header {
    uint8_t  ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};
struct ProgramHeader {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
};
constexpr uint32_t kLoad = 1;
constexpr uint32_t kExec = 1, kWrite = 2;
}  // namespace elf64

// Скан файла libunity.so. Ошибка чтения/разбора — просто пустой результат.
inline UnityFpsScan scan_unity_fps(const char* path) {
    UnityFpsScan scan;
    if (!path || !path[0]) return scan;
    FILE* file = fopen(path, "rb");
    if (!file) return scan;

    elf64::Header header{};
    if (fread(&header, sizeof(header), 1, file) != 1 ||
        header.ident[0] != 0x7F || header.ident[1] != 'E' || header.ident[2] != 'L' ||
        header.ident[3] != 'F' || header.ident[4] != 2 /* ELFCLASS64 */ ||
        header.machine != 183 /* EM_AARCH64 */ ||
        header.phentsize != sizeof(elf64::ProgramHeader) ||
        header.phnum == 0 || header.phnum > 128) {
        fclose(file);
        return scan;
    }

    std::vector<elf64::ProgramHeader> segments;
    segments.resize(header.phnum);
    if (fseek(file, (long)header.phoff, SEEK_SET) != 0 ||
        fread(segments.data(), sizeof(elf64::ProgramHeader), header.phnum, file) != header.phnum) {
        fclose(file);
        return scan;
    }
    scan.file_ok = true;

    // Записываемые сегменты целиком в память: проверка «в файле лежит -1»
    // делается для каждой найденной тройки, и ходить за четырьмя байтами на
    // диск десять тысяч раз незачем. .data/.got у libunity — единицы мегабайт.
    struct WritableSegment { uint64_t vaddr, filesz, memsz; std::vector<uint8_t> bytes; };
    std::vector<WritableSegment> writable;
    {
        uint64_t budget = 16u << 20;
        for (const elf64::ProgramHeader& segment : segments) {
            if (segment.type != elf64::kLoad || !(segment.flags & elf64::kWrite)) continue;
            if (segment.filesz > budget) continue;
            WritableSegment loaded;
            loaded.vaddr = segment.vaddr;
            loaded.filesz = segment.filesz;
            loaded.memsz = segment.memsz;
            loaded.bytes.resize((size_t)segment.filesz);
            if (segment.filesz &&
                (fseek(file, (long)segment.offset, SEEK_SET) != 0 ||
                 fread(loaded.bytes.data(), 1, (size_t)segment.filesz, file) != segment.filesz))
                continue;
            budget -= segment.filesz;
            writable.push_back(std::move(loaded));
        }
    }

    // Исходное содержимое поля в файле: оно обязано быть -1.
    auto initial_value = [&](uint64_t vaddr, int32_t& value) {
        for (const WritableSegment& segment : writable) {
            if (vaddr < segment.vaddr || vaddr + 4 > segment.vaddr + segment.memsz) continue;
            // За filesz начинается .bss: там нулевая инициализация, а нам нужна -1.
            if (vaddr + 4 > segment.vaddr + segment.filesz) return false;
            memcpy(&value, segment.bytes.data() + (size_t)(vaddr - segment.vaddr), 4);
            return true;
        }
        return false;
    };

    // Потоковый проход по исполняемым сегментам: буфер 1 МиБ с перекрытием в
    // две инструкции, чтобы тройка не разрывалась на границе куска.
    constexpr size_t kWords = 256 * 1024;   // 1 МиБ
    std::vector<uint32_t> buffer(kWords);
    for (const elf64::ProgramHeader& segment : segments) {
        if (segment.type != elf64::kLoad || !(segment.flags & elf64::kExec)) continue;
        if (segment.filesz < 12) continue;
        uint64_t done = 0;                       // байт сегмента обработано
        while (done + 12 <= segment.filesz) {
            const uint64_t want = segment.filesz - done;
            const size_t words = (size_t)((want / 4 < kWords) ? want / 4 : kWords);
            if (words < 3) break;
            if (fseek(file, (long)(segment.offset + done), SEEK_SET) != 0) break;
            const size_t got = fread(buffer.data(), 4, words, file);
            if (got < 3) break;
            for (size_t i = 0; i + 2 < got; ++i) {
                if (!a64_is_ret(buffer[i + 2])) continue;
                const uint64_t getter_rva = segment.vaddr + done + (uint64_t)i * 4;
                uint64_t target_rva = 0;
                if (!decode_int32_getter(getter_rva, &buffer[i], target_rva)) continue;
                ++scan.triples;
                int32_t initial = 0;
                if (!initial_value(target_rva, initial) || initial != -1) continue;
                UnityFpsCandidate candidate;
                candidate.getter_rva = getter_rva;
                candidate.target_rva = target_rva;
                candidate.load_instruction = buffer[i + 1];
                scan.candidates.push_back(candidate);
                if (scan.candidates.size() >= 64) break;   // столько — уже мусор
            }
            if (scan.candidates.size() >= 64) break;
            // Перекрытие: последние две инструкции куска могут начинать тройку.
            done += (uint64_t)(got - 2) * 4;
        }
        if (scan.candidates.size() >= 64) break;
    }

    fclose(file);
    return scan;
}

}  // namespace viewmodel
