// Патч кода игры в памяти для сайлента (aarch64).
//
// Об устройстве сеттера
// ---------------------
// Сеттер оси выстрела — это Set у обобщённого класса GuI`1<Vector3>.
// В дампах он находится так: ищем в script.json
//   "Name": "<что-угодно>\\u003CUnityEngine.Vector3\\u003E$$Set
// (имя класса обфусцировано и между сборками меняется, а аргумент Vector3 —
// нет). Адрес совпадает и в релизе, и в бете: RVA 0x8f5dbdc. Проверено по
// обоим дампам из репозитория, поэтому это НЕ часть таблицы переключения
// версий (tools/offsets): там только то, что между сборками разъезжается.
//
// Пролог сеттера (16 байт, их мы и подменяем):
//   0x8f5dbdc  sub sp, sp, #0x40
//   0x8f5dbe0  str x30, [sp, #0x10]
//   0x8f5dbe4  stp x22, x21, [sp, #0x20]
//   0x8f5dbe8  stp x20, x19, [sp, #0x30]
// Дальше идёт тело: mov x22, x0; mov x21, x0; mov x20, x1;
// ldr x8, [x22, #0x20]! — то есть объект приходит в x0, а записываемый вектор
// лежит в s0/s1/s2. Значит, чтобы подменить значение, достаточно к моменту
// входа в тело поменять s0/s1/s2 — больше ничего.
//
// Что пишем вместо пролога (16 байт)
// ----------------------------------
//   +0  ldr x17, #8        ; загрузить адрес трамполина из следующего слова
//   +4  br  x17            ; перейти в трамполин
//   +8  <адрес трамполина, 8 байт>
//
// Трамполин
// ---------
//   movz/movk x17, <блок управления>
//   ldrb w16, [x17, #0]    ; флаг активности
//   cbz  w16, <пролог>     ; выключен — не трогаем регистры вовсе
//   ldr  s0, [x17, #8]     ; наша ось X
//   ldr  s1, [x17, #12]    ; наша ось Y
//   ldr  s2, [x17, #16]    ; наша ось Z
//   <пролог>:
//   sub  sp, sp, #0x40         ; вытесненные инструкции — 4 штуки, один в один
//   str  x30, [sp, #0x10]      ; и с тем же sp: кадр получается в точности
//   stp  x22, x21, [sp, #0x20] ; таким, каким его ждёт эпилог сеттера
//   stp  x20, x19, [sp, #0x30]
//   movz/movk x16, <сеттер+16> ; в тело сеттера, минуя вытесненный пролог
//   br   x16
//
// Почему трамполин не трогает стек: x16 и x17 в AArch64 — scratch-регистры
// (IP0/IP1), их значение не сохраняется через вызов, поэтому ни один вызывающий
// не вправе ждать от них чего-то после bl. Спасать их не нужно — а значит не
// нужно и двигать sp, из-за чего вытесненный пролог писал бы сохранённые
// регистры не туда, куда их потом забирает эпилог (игра бы падала). Проверено
// прогоном на эмуляторе: tools/silentpatch/emulate.py.
//
// Вектор направления здесь не нормализуется: его готовит aim.cpp, а FPHitscan
// всё равно нормализует ось сам.
#include "silent_patch.h"

#include <cstring>

#include <sys/mman.h>
#include <unistd.h>

#include "logfile.h"

namespace {

// RVA сеттера оси выстрела (см. комментарий в начале файла).
constexpr std::uint64_t kSetterRva = 0x8f5dbdc;

// Вытесняемый пролог: 4 инструкции сеттера, 16 байт.
constexpr std::uint32_t kPrologue[4] = {
    0xd10103ffu,  // sub sp, sp, #0x40
    0xf9000bfeu,  // str x30, [sp, #0x10]
    0xa90257f6u,  // stp x22, x21, [sp, #0x20]
    0xa9034ff4u,  // stp x20, x19, [sp, #0x30]
};

// Трамполин: фиксированные инструкции (адреса подставляются отдельно).
constexpr std::uint32_t kLdrbActive  = 0x39400230u;  // ldrb w16, [x17, #0]
constexpr std::uint32_t kCbzActive   = 0x34000090u;  // cbz w16, #0x10 (на вытесненный пролог)
constexpr std::uint32_t kLdrS0       = 0xbd400a20u;  // ldr s0, [x17, #8]
constexpr std::uint32_t kLdrS1       = 0xbd400e21u;  // ldr s1, [x17, #12]
constexpr std::uint32_t kLdrS2       = 0xbd401222u;  // ldr s2, [x17, #16]
constexpr std::uint32_t kBrX16       = 0xd61f0200u;  // br x16
constexpr std::uint32_t kLdrLitX17   = 0x58000051u;  // ldr x17, #8
constexpr std::uint32_t kBrX17       = 0xd61f0220u;  // br x17

// Блок управления: его читает трамполин. Поля выровнены так, как их забирают
// инструкции выше, и правятся из нашего потока.
struct SilentCtl {
    volatile std::uint8_t active;      // +0
    std::uint8_t pad[7];               // выравнивание вектора до +8
    volatile float dir[3];             // +8, +12, +16
};

SilentCtl g_ctl;

std::uint64_t g_setter = 0;            // адрес сеттера в памяти процесса
std::uint32_t g_original[4] = {};      // сохранённый пролог сеттера
std::uint32_t* g_tramp = nullptr;      // страница трамполина
std::size_t g_tramp_size = 0;
bool g_installed = false;
bool g_tried = false;
std::uint64_t g_tried_base = 0;

std::uint32_t EmitMovz(std::uint8_t rd, std::uint16_t imm) {
    return 0xd2800000u | (static_cast<std::uint32_t>(imm) << 5) | rd;
}

std::uint32_t EmitMovk(std::uint8_t rd, std::uint16_t imm, int shift) {
    const std::uint32_t hw = static_cast<std::uint32_t>(shift / 16) << 21;
    return 0xf2800000u | hw | (static_cast<std::uint32_t>(imm) << 5) | rd;
}

// Загрузить 64-битную константу в регистр четырьмя инструкциями.
void EmitMov64(std::uint32_t* code, int& n, std::uint8_t rd, std::uint64_t value) {
    code[n++] = EmitMovz(rd, static_cast<std::uint16_t>(value & 0xffff));
    code[n++] = EmitMovk(rd, static_cast<std::uint16_t>((value >> 16) & 0xffff), 16);
    code[n++] = EmitMovk(rd, static_cast<std::uint16_t>((value >> 32) & 0xffff), 32);
    code[n++] = EmitMovk(rd, static_cast<std::uint16_t>((value >> 48) & 0xffff), 48);
}

// Сбросить кэш инструкций после записи кода (clang на aarch64).
void FlushCode(void* from, void* to) {
    __builtin___clear_cache(static_cast<char*>(from), static_cast<char*>(to));
}

std::size_t PageSize() {
    const long ps = sysconf(_SC_PAGESIZE);
    return ps > 0 ? static_cast<std::size_t>(ps) : 4096u;
}

bool MakeWritable(void* addr, std::size_t len, bool exec) {
    const std::size_t page = PageSize();
    const std::uintptr_t start = reinterpret_cast<std::uintptr_t>(addr) & ~(page - 1);
    const std::uintptr_t end = reinterpret_cast<std::uintptr_t>(addr) + len;
    const std::size_t size = static_cast<std::size_t>((end - start + page - 1) & ~(page - 1));
    const int prot = PROT_READ | PROT_WRITE | (exec ? PROT_EXEC : 0);
    return mprotect(reinterpret_cast<void*>(start), size, prot) == 0;
}

// Собрать трамполин и записать патч (страница сеттера уже доступна на запись).
void WritePatch(std::uint32_t* tramp, std::uint32_t* setter) {
    std::uint32_t code[32] = {};
    int n = 0;
    EmitMov64(code, n, 17, reinterpret_cast<std::uint64_t>(&g_ctl));
    code[n++] = kLdrbActive;
    code[n++] = kCbzActive;      // патч выключен — сразу на вытесненный пролог
    code[n++] = kLdrS0;
    code[n++] = kLdrS1;
    code[n++] = kLdrS2;
    for (int i = 0; i < 4; ++i) code[n++] = kPrologue[i];
    EmitMov64(code, n, 16, reinterpret_cast<std::uint64_t>(setter) + 16);
    code[n++] = kBrX16;
    std::memcpy(tramp, code, static_cast<std::size_t>(n) * sizeof(std::uint32_t));

    std::uint32_t patch[4] = {};
    patch[0] = kLdrLitX17;
    patch[1] = kBrX17;
    const std::uint64_t ta = reinterpret_cast<std::uint64_t>(tramp);
    std::memcpy(&patch[2], &ta, sizeof(ta));
    std::memcpy(setter, patch, sizeof(patch));
}

}  // namespace

bool SilentPatchInstall(std::uint64_t il2cpp_base) {
    if (g_installed) return true;
    if (il2cpp_base == 0) return false;
    if (g_tried && g_tried_base == il2cpp_base) return false;  // уже пробовали и не вышло
    g_tried = true;
    g_tried_base = il2cpp_base;

    std::uint32_t* setter = reinterpret_cast<std::uint32_t*>(
        static_cast<std::uintptr_t>(il2cpp_base + kSetterRva));
    std::uint32_t prologue[4] = {};
    std::memcpy(prologue, setter, sizeof(prologue));
    if (std::memcmp(prologue, kPrologue, sizeof(prologue)) != 0) {
        LogLine("сайлент: патч не поставлен — пролог сеттера другой (%08x %08x %08x %08x)",
                prologue[0], prologue[1], prologue[2], prologue[3]);
        return false;
    }

    const std::size_t page = PageSize();
    void* mem = mmap(nullptr, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        LogLine("сайлент: патч не поставлен — mmap не дал страницу");
        return false;
    }

    if (!MakeWritable(setter, 16, true)) {
        munmap(mem, page);
        LogLine("сайлент: патч не поставлен — mprotect не пустил");
        return false;
    }

    WritePatch(static_cast<std::uint32_t*>(mem), setter);
    FlushCode(mem, static_cast<char*>(mem) + page);
    FlushCode(setter, setter + 4);

    g_setter = reinterpret_cast<std::uint64_t>(setter);
    std::memcpy(g_original, prologue, sizeof(prologue));
    g_tramp = static_cast<std::uint32_t*>(mem);
    g_tramp_size = page;
    g_installed = true;
    LogLine("сайлент: патч оси поставлен, сеттер=%llx трамполин=%llx",
            static_cast<unsigned long long>(g_setter),
            static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(mem)));
    return true;
}

void SilentPatchRestore() {
    g_ctl.active = 0;
    if (!g_installed) return;
    std::uint32_t* setter = reinterpret_cast<std::uint32_t*>(static_cast<std::uintptr_t>(g_setter));
    std::memcpy(setter, g_original, sizeof(g_original));
    FlushCode(setter, setter + 4);
    if (g_tramp != nullptr) {
        munmap(g_tramp, g_tramp_size);
        g_tramp = nullptr;
        g_tramp_size = 0;
    }
    g_installed = false;
    g_tried = false;
    g_tried_base = 0;
    LogLine("сайлент: патч оси снят, байты игры вернули");
}

bool SilentPatchInstalled() {
    return g_installed;
}

void SilentPatchSet(bool active, float x, float y, float z) {
    g_ctl.dir[0] = x;
    g_ctl.dir[1] = y;
    g_ctl.dir[2] = z;
    g_ctl.active = active ? 1u : 0u;
}
