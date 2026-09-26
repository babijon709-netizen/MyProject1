// Стенд «игра обновилась»: автообновление оффсетов БЕЗ единой подсказки.
//
//   sh tools/offsets/metascan_test.sh
//
// Ровно тот случай, на котором автообновление встало 26.09 (см. лог
// benzware_offsets.log):
//
//   * вшитые TYPEINFO_RVA устарели — живого класса-якоря нет;
//   * в /proc/<pid>/maps НЕТ отображения с именем «global-metadata»: файл
//     метаданных лежит внутри .apk, да ещё и не с начала отображения
//     (страница внутри архива), а строки карты с «.apk» резолвер раньше
//     выбрасывал целиком;
//   * структуры Il2CppClass живут не в сегментах .so, а в анонимной куче.
//
// Раньше в такой конфигурации резолвер писал «имена зашифрованы, автообновление
// по именам невозможно» и оставлял все оффсеты вшитыми. Теперь он обязан:
//   1) найти блок метаданных по сигнатуре 0xFAB11BAF и разобрать заголовок,
//      взяв ТОЧНОЕ окно таблицы строк (stringOffset/stringSize);
//   2) найти в этом окне адреса табличных имён;
//   3) найти в куче указатели на них — это и есть живые Il2CppClass;
//   4) вычитать смещения полей из FieldInfo и перезаписать вшитые константы.
//
// Стенд гоняет НАСТОЯЩИЙ jni/src/offset_resolver.cpp по процессу-ребёнку
// через /proc/<pid>/mem — как на устройстве.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <time.h>

#include <string>

#include "mem_io.h"
#include "game_offsets_active.h"

// ---- окружение резолвера (в игре живёт в game.cpp) --------------------------
memio::Reader g_mem;
pid_t     g_pid = -1;
uint64_t  g_il2cpp_base = 0;

bool valid_obj(uint64_t p) {
    return p >= 0x10000 && p < 0x0001000000000000ULL && (p & 0x7) == 0;
}

double mono_seconds() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

std::string read_remote_string(uint64_t address, bool* readable) {
    if (readable) *readable = false;
    if (!address) return {};
    const size_t kMax = 95;
    char buffer[kMax + 1] = {};
    size_t got = 0;
    while (got < kMax) {
        const size_t want = (kMax - got < sizeof(uint64_t)) ? (kMax - got) : sizeof(uint64_t);
        if (!g_mem.read(address + got, buffer + got, want)) {
            if (got == 0) return {};
            size_t single = 0;
            while (single < want &&
                   g_mem.read_quiet(address + got + single, buffer + got + single, 1)) {
                if (buffer[got + single] == '\0') break;
                ++single;
            }
            got += single;
            break;
        }
        got += want;
        if (memchr(buffer + got - want, '\0', want)) break;
    }
    buffer[kMax] = '\0';
    if (readable) *readable = true;
    return std::string(buffer);
}

#include "../../jni/src/offset_resolver.cpp"

// ---- утилиты стенда ---------------------------------------------------------
static int s_checks = 0, s_fails = 0;
static void check(bool ok, const char* what) {
    ++s_checks;
    if (!ok) ++s_fails;
    printf("  %-66s %s\n", what, ok ? "ок" : "ПРОВАЛ");
}

static bool resolver_applied(std::uint64_t* slot) {
    for (int i = 0; i < s_applied_count; ++i)
        if (s_applied[i].slot == slot) return true;
    return false;
}

struct Handed {
    uint64_t il2cpp_base;
    uint64_t decoy_base;      // подставной блок метаданных протектора
    uint64_t apk_base;        // отображение «base.apk»
    uint64_t meta_header;     // заголовок метаданных внутри него
    uint64_t string_table;    // таблица строк
    uint64_t heap_base;
    uint64_t pm, nc, item;
};

// ---- ребёнок: «обновлённая игра» --------------------------------------------
//
// Раскладка файла «base.apk» (512 КБ):
//   0x00000              мусор архива (нули)
//   0x02000              Il2CppGlobalMetadataHeader (sanity 0xFAB11BAF, версия 31)
//   0x03000              таблица строк (0x8000 байт): имена классов и полей
//
// Заголовок ссылается на таблицу ОТНОСИТЕЛЬНО себя (как в настоящем файле):
// stringOffset = 0x1000, stringSize = 0x8000.
static const uint64_t kApkSize      = 512u << 10;
static const uint64_t kHeaderOff    = 0x2000;
static const uint32_t kStringRel    = 0x1000;
static const uint32_t kStringSize   = 0x8000;

struct StrTab {
    uint8_t* base;
    uint64_t addr;
    size_t   off;
};

static uint64_t put_str(StrTab& t, const char* s) {
    const uint64_t a = t.addr + t.off;
    const size_t n = strlen(s) + 1;
    memcpy(t.base + t.off, s, n);
    t.off += n;                 // строки идут ВПРИТЫК, как в таблице строк
    return a;
}

struct PField { const char* name; int32_t offset; };

static uint64_t build_class(uint8_t* heap, uint64_t heap_addr, size_t& hoff,
                            uint64_t image, uint64_t name, uint64_t ns,
                            const PField* fields, int nfields) {
    hoff = (hoff + 0x1FFu) & ~(size_t)0x1FF;
    uint8_t* k = heap + hoff;
    const uint64_t klass = heap_addr + hoff;
    hoff += 0x200;
    memset(k, 0, 0x200);
    *(uint64_t*)(k + 0x00) = image;
    *(uint64_t*)(k + IL2CPP_CLASS_NAME) = name;
    *(uint64_t*)(k + IL2CPP_CLASS_NAMESPACE) = ns;
    *(uint64_t*)(k + 0xB8) = klass + 0x140;          // static_fields
    *(uint16_t*)(k + IL2CPP_CLASS_FIELD_COUNT) = (uint16_t)nfields;
    if (nfields <= 0) return klass;

    uint8_t* farr = heap + hoff;
    const uint64_t farr_addr = heap_addr + hoff;
    hoff += 0x20 * (size_t)nfields + 0x40;
    *(uint64_t*)(k + IL2CPP_CLASS_FIELDS) = farr_addr;
    uint8_t* tarr = heap + hoff;
    const uint64_t tarr_addr = heap_addr + hoff;
    hoff += 0x10 * (size_t)nfields + 0x40;
    memset(farr, 0, 0x20 * (size_t)nfields + 0x40);
    memset(tarr, 0, 0x10 * (size_t)nfields + 0x40);
    for (int i = 0; i < nfields; ++i) {
        uint8_t* f = farr + 0x20 * i;
        *(uint64_t*)(f + IL2CPP_FIELDINFO_NAME)   = 0;   // имя проставит вызывающий
        *(uint64_t*)(f + IL2CPP_FIELDINFO_TYPE)   = tarr_addr + 0x10 * (uint64_t)i;
        *(uint64_t*)(f + IL2CPP_FIELDINFO_PARENT) = klass;
        *(int32_t*)(f + IL2CPP_FIELDINFO_OFFSET)  = fields[i].offset;
        *(uint32_t*)(f + 0x1C) = 0x04000000u + (uint32_t)i;
    }
    return klass;
}

static void set_field_name(uint8_t* heap, uint64_t heap_addr, uint64_t klass,
                           int idx, uint64_t name_addr) {
    const uint64_t farr = *(uint64_t*)(heap + (klass - heap_addr) + IL2CPP_CLASS_FIELDS);
    uint8_t* f = heap + (farr - heap_addr) + 0x20 * (size_t)idx;
    *(uint64_t*)(f + IL2CPP_FIELDINFO_NAME) = name_addr;
}

// Сценарий 2 («метадата зашифрована»): заголовка 0xFAB11BAF в памяти нет
// вовсе, а имена классов встречаются только ВНУТРИ структур — 40-байтовыми
// записями с инлайновым именем, как в дампе с устройства. Указателей на сами
// имена нет: класс лежит рядом, в той же записи.
static void child_encrypted(int out_fd) {
    char dir[] = "/tmp/metascan_enc.XXXXXX";
    if (!mkdtemp(dir)) _exit(90);
    char il_path[160];
    snprintf(il_path, sizeof(il_path), "%s/libil2cpp.so", dir);
    const size_t kIlSize = 256u << 10;
    int il_fd = open(il_path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (il_fd < 0 || ftruncate(il_fd, (off_t)kIlSize)) _exit(91);
    uint8_t* il = (uint8_t*)mmap(NULL, kIlSize, PROT_READ | PROT_WRITE, MAP_SHARED, il_fd, 0);
    if (il == MAP_FAILED) _exit(93);
    memcpy(il, "\x7f" "ELF", 4);

    const size_t kHeapSize = 4u << 20;
    uint8_t* heap = (uint8_t*)mmap(NULL, kHeapSize, PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (heap == MAP_FAILED) _exit(94);
    memset(heap, 0, kHeapSize);
    const uint64_t il_base = (uint64_t)(uintptr_t)il;
    const uint64_t heap_addr = (uint64_t)(uintptr_t)heap;

    // Имена полей — в отдельном куске кучи (их читает уже phase_fields).
    StrTab names = { heap + 0x800, heap_addr + 0x800, 0 };
    const uint64_t f_teamname = put_str(names, "teamName");
    const uint64_t f_clanid = put_str(names, "clanId");
    const uint64_t f_clantag = put_str(names, "clanTag");
    const uint64_t f_spawned = put_str(names, "spawned");

    size_t hoff = 0x1000;
    const PField pm_fields[] = {
        { "teamName", 0x2A0 }, { "clanId", 0x2B0 }, { "clanTag", 0x2B8 },
    };
    const uint64_t pm = build_class(heap, heap_addr, hoff, il_base + 0x1000,
                                    0, 0, pm_fields, 3);
    set_field_name(heap, heap_addr, pm, 0, f_teamname);
    set_field_name(heap, heap_addr, pm, 1, f_clanid);
    set_field_name(heap, heap_addr, pm, 2, f_clantag);
    const PField nc_fields[] = { { "spawned", 0x48 } };
    const uint64_t nc = build_class(heap, heap_addr, hoff, il_base + 0x1000,
                                    0, 0, nc_fields, 1);
    set_field_name(heap, heap_addr, nc, 0, f_spawned);

    // Пул сериализации: записи по 0x60 байт — нули, GUID, два указателя,
    // инлайновое имя (ровно та раскладка, что в benzhack_meta.bin).
    hoff = (hoff + 0xFFFu) & ~(size_t)0xFFF;
    struct Rec { const char* name; uint64_t klass; };
    const Rec recs[] = { { "PlayerManager", pm }, { "NetworkClient", nc } };
    for (size_t i = 0; i < sizeof(recs) / sizeof(recs[0]); ++i) {
        uint8_t* r = heap + hoff;
        memset(r, 0, 0x60);
        memset(r + 0x10, 0xA5, 16);                       // «GUID» сборки
        *(uint64_t*)(r + 0x28) = recs[i].klass;           // указатель на класс
        *(uint64_t*)(r + 0x30) = heap_addr + 0x800;       // соседний указатель
        memcpy(r + 0x38, recs[i].name, strlen(recs[i].name) + 1);
        *(uint16_t*)(r + 0x57) = 0x4B12;
        hoff += 0x60;
    }

    Handed hd = {};
    hd.il2cpp_base = il_base;
    hd.heap_base = heap_addr;
    hd.pm = pm; hd.nc = nc;
    if (write(out_fd, &hd, sizeof(hd)) != sizeof(hd)) _exit(95);
    raise(SIGSTOP);
    _exit(0);
}

// Сценарий 3 («мусор»): метаданных нет вовсе, а в куче лежат тысячи структур,
// у которых на месте имени — короткая строка из таблицы резолвера («gL»).
// Ровно это случилось на устройстве 26.09 19:46: окно имён задалось по такому
// «классу», 769 структур сошли за классы, из них посчитались 254 «смещения»,
// их применили и записали в кэш — ESP поехал, игра залагала, чит упал.
// Резолвер обязан НИЧЕГО не применить и НЕ записать кэш.
static void child_garbage(int out_fd) {
    char dir[] = "/tmp/metascan_junk.XXXXXX";
    if (!mkdtemp(dir)) _exit(90);
    char il_path[160];
    snprintf(il_path, sizeof(il_path), "%s/libil2cpp.so", dir);
    const size_t kIlSize = 256u << 10;
    int il_fd = open(il_path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (il_fd < 0 || ftruncate(il_fd, (off_t)kIlSize)) _exit(91);
    uint8_t* il = (uint8_t*)mmap(NULL, kIlSize, PROT_READ | PROT_WRITE, MAP_SHARED, il_fd, 0);
    if (il == MAP_FAILED) _exit(93);
    memcpy(il, "\x7f" "ELF", 4);

    const size_t kHeapSize = 8u << 20;
    uint8_t* heap = (uint8_t*)mmap(NULL, kHeapSize, PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (heap == MAP_FAILED) _exit(94);
    memset(heap, 0, kHeapSize);
    const uint64_t il_base = (uint64_t)(uintptr_t)il;
    const uint64_t heap_addr = (uint64_t)(uintptr_t)heap;

    // Короткие ротированные имена, которые ЕСТЬ в таблице резолвера.
    StrTab junk = { heap + 0x400, heap_addr + 0x400, 0 };
    const uint64_t s_gl = put_str(junk, "gL");
    const uint64_t s_edg = put_str(junk, "eDg");
    const uint64_t s_field = put_str(junk, "team");

    size_t hoff = 0x2000;
    for (int i = 0; i < 4000 && hoff + 0x200 < kHeapSize; ++i) {
        uint8_t* d = heap + hoff;
        memset(d, 0, 0x140);
        *(uint64_t*)(d + 0x00) = il_base + 0x1000;                  // «образ»
        *(uint64_t*)(d + IL2CPP_CLASS_NAME) = (i & 1) ? s_gl : s_edg;
        // Массив «полей»: одна запись с читаемым именем и мусорным смещением.
        uint8_t* f = d + 0x160;
        *(uint64_t*)(d + IL2CPP_CLASS_FIELDS) = heap_addr + hoff + 0x160;
        *(uint16_t*)(d + IL2CPP_CLASS_FIELD_COUNT) = 1;
        *(uint64_t*)(f + IL2CPP_FIELDINFO_NAME) = s_field;
        *(uint64_t*)(f + IL2CPP_FIELDINFO_TYPE) = heap_addr + 0x800;
        *(uint64_t*)(f + IL2CPP_FIELDINFO_PARENT) = heap_addr + hoff;
        *(int32_t*)(f + IL2CPP_FIELDINFO_OFFSET) = 0x2a8;           // «смещение» из лога
        hoff += 0x200;
    }

    Handed hd = {};
    hd.il2cpp_base = il_base;
    hd.heap_base = heap_addr;
    if (write(out_fd, &hd, sizeof(hd)) != sizeof(hd)) _exit(95);
    raise(SIGSTOP);
    _exit(0);
}

static void child_main(int out_fd) {
    char dir[] = "/tmp/metascan.XXXXXX";
    if (!mkdtemp(dir)) _exit(90);
    char il_path[160], apk_path[160], decoy_path[160];
    snprintf(il_path, sizeof(il_path), "%s/libil2cpp.so", dir);
    snprintf(apk_path, sizeof(apk_path), "%s/base.apk", dir);
    snprintf(decoy_path, sizeof(decoy_path), "%s/assets.dat", dir);

    const size_t kIlSize = 256u << 10;
    int il_fd = open(il_path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    int apk_fd = open(apk_path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    int decoy_fd = open(decoy_path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (il_fd < 0 || apk_fd < 0 || decoy_fd < 0) _exit(91);
    if (ftruncate(il_fd, (off_t)kIlSize) || ftruncate(apk_fd, (off_t)kApkSize) ||
        ftruncate(decoy_fd, (off_t)kApkSize)) _exit(92);

    uint8_t* il  = (uint8_t*)mmap(NULL, kIlSize, PROT_READ | PROT_WRITE, MAP_SHARED, il_fd, 0);
    uint8_t* apk = (uint8_t*)mmap(NULL, kApkSize, PROT_READ | PROT_WRITE, MAP_SHARED, apk_fd, 0);
    uint8_t* decoy = (uint8_t*)mmap(NULL, kApkSize, PROT_READ | PROT_WRITE, MAP_SHARED, decoy_fd, 0);
    if (il == MAP_FAILED || apk == MAP_FAILED || decoy == MAP_FAILED) _exit(93);
    memcpy(il, "\x7f" "ELF", 4);

    // Куча игры: отдельное АНОНИМНОЕ отображение — именно там живут классы.
    const size_t kHeapSize = 4u << 20;
    uint8_t* heap = (uint8_t*)mmap(NULL, kHeapSize, PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (heap == MAP_FAILED) _exit(94);
    memset(heap, 0, kHeapSize);

    const uint64_t il_base = (uint64_t)(uintptr_t)il;
    const uint64_t apk_base = (uint64_t)(uintptr_t)apk;
    const uint64_t decoy_base = (uint64_t)(uintptr_t)decoy;
    const uint64_t heap_addr = (uint64_t)(uintptr_t)heap;

    // --- заголовок метаданных ---
    uint8_t* hdr = apk + kHeaderOff;
    uint32_t* h = (uint32_t*)hdr;
    h[0] = 0xFAB11BAFu;      // sanity
    h[1] = 31;               // версия (Unity 6)
    for (int i = 0; i < 30; ++i) { h[2 + 2 * i] = 0x100; h[3 + 2 * i] = 0x10; }
    h[6] = kStringRel;       // stringOffset
    h[7] = kStringSize;      // stringSize

    // --- подставной блок протектора: заголовок в самом начале отображения,
    // таблица строк настоящая по форме, но имён из таблицы резолвера в ней нет.
    {
        uint32_t* d = (uint32_t*)decoy;
        d[0] = 0xFAB11BAFu;
        d[1] = 29;
        for (int i = 0; i < 30; ++i) { d[2 + 2 * i] = 0x100; d[3 + 2 * i] = 0x10; }
        d[6] = kStringRel;
        d[7] = kStringSize;
        StrTab dt = { decoy + kStringRel, decoy_base + kStringRel, 0 };
        static const char* const kJunk[] = {
            "DecoyType", "SomethingElse", "AnotherName", "ProtectorStub",
            "FakeManager", "NotAClass", "Placeholder", "Whatever"
        };
        for (size_t i = 0; i < sizeof(kJunk) / sizeof(kJunk[0]); ++i) put_str(dt, kJunk[i]);
    }

    StrTab tab = { apk + kHeaderOff + kStringRel, apk_base + kHeaderOff + kStringRel, 0 };

    const uint64_t s_pm = put_str(tab, "PlayerManager");
    const uint64_t s_oxide = put_str(tab, "Oxide");
    const uint64_t s_nc = put_str(tab, "NetworkClient");
    const uint64_t s_mirror = put_str(tab, "Mirror");
    const uint64_t s_item = put_str(tab, "Item");
    const uint64_t s_empty = put_str(tab, "");
    const uint64_t f_teamname = put_str(tab, "teamName");
    const uint64_t f_clanid = put_str(tab, "clanId");
    const uint64_t f_clantag = put_str(tab, "clanTag");
    const uint64_t f_pos = put_str(tab, "lastSavedPosition");
    const uint64_t f_seat = put_str(tab, "seatID");
    const uint64_t f_spawned = put_str(tab, "spawned");
    const uint64_t f_itemid = put_str(tab, "id");

    // --- классы в куче ---
    size_t hoff = 0x1000;
    const PField pm_fields[] = {
        { "teamName",          0x2A0 },   // вшито 0x280
        { "clanId",            0x2B0 },   // вшито 0x290
        { "clanTag",           0x2B8 },   // вшито 0x298
        { "lastSavedPosition", 0x1E4 },   // вшито 0x1D4
        { "seatID",            0x310 },
    };
    const uint64_t pm = build_class(heap, heap_addr, hoff, il_base + 0x1000,
                                    s_pm, s_oxide, pm_fields, 5);
    set_field_name(heap, heap_addr, pm, 0, f_teamname);
    set_field_name(heap, heap_addr, pm, 1, f_clanid);
    set_field_name(heap, heap_addr, pm, 2, f_clantag);
    set_field_name(heap, heap_addr, pm, 3, f_pos);
    set_field_name(heap, heap_addr, pm, 4, f_seat);

    const PField nc_fields[] = { { "spawned", 0x48 } };
    const uint64_t nc = build_class(heap, heap_addr, hoff, il_base + 0x1000,
                                    s_nc, s_mirror, nc_fields, 1);
    set_field_name(heap, heap_addr, nc, 0, f_spawned);

    const PField item_fields[] = { { "id", 0x38 } };
    const uint64_t item = build_class(heap, heap_addr, hoff, il_base + 0x1000,
                                      s_item, s_empty, item_fields, 1);
    set_field_name(heap, heap_addr, item, 0, f_itemid);

    // «Суп из указателей»: 3000 структур, у которых на месте имени класса
    // стоит указатель на настоящее имя «PlayerManager», образ валиден, а
    // массива полей нет (мусор). Ровно этим забился массив классов на
    // устройстве (лог 26.09: 4135 «живых классов», потолок 1024 из одних
    // тёзок, 5 констант из 160 вместо 153). Резолвер обязан их отсеять.
    {
        hoff = (hoff + 0xFFFu) & ~(size_t)0xFFF;
        for (int i = 0; i < 3000; ++i) {
            uint8_t* d = heap + hoff;
            if (hoff + 0x200 >= kHeapSize) break;
            memset(d, 0, 0x140);
            *(uint64_t*)(d + 0x00) = il_base + 0x1000;            // образ валиден
            *(uint64_t*)(d + IL2CPP_CLASS_NAME) = s_pm;           // указатель на имя
            *(uint64_t*)(d + IL2CPP_CLASS_FIELDS) = heap_addr + 0x200000;  // «поля» — нули
            *(uint16_t*)(d + IL2CPP_CLASS_FIELD_COUNT) = 7;
            hoff += 0x140;
        }
    }

    Handed hd = {};
    hd.il2cpp_base = il_base;
    hd.decoy_base = decoy_base;
    hd.apk_base = apk_base;
    hd.meta_header = apk_base + kHeaderOff;
    hd.string_table = tab.addr;
    hd.heap_base = heap_addr;
    hd.pm = pm; hd.nc = nc; hd.item = item;
    if (write(out_fd, &hd, sizeof(hd)) != sizeof(hd)) _exit(95);

    raise(SIGSTOP);
    _exit(0);
}

int main(int argc, char** argv) {
    const bool encrypted = (argc > 1 && strcmp(argv[1], "encrypted") == 0);
    const bool garbage = (argc > 1 && strcmp(argv[1], "garbage") == 0);
    int fds[2];
    if (pipe(fds)) { perror("pipe"); return 1; }
    const pid_t child = fork();
    if (child < 0) { perror("fork"); return 1; }
    if (child == 0) {
        close(fds[0]);
        if (garbage) child_garbage(fds[1]);
        else if (encrypted) child_encrypted(fds[1]);
        else child_main(fds[1]);
        _exit(0);
    }
    close(fds[1]);

    Handed h;
    if (read(fds[0], &h, sizeof(h)) != sizeof(h)) {
        fprintf(stderr, "ребёнок не отдал адреса\n");
        kill(child, SIGKILL);
        return 1;
    }
    if (encrypted)
        printf("стенд «метадата зашифрована»: ребёнок %d, .so@0x%llx, куча@0x%llx\n",
               (int)child, (unsigned long long)h.il2cpp_base,
               (unsigned long long)h.heap_base);
    else
    printf("стенд «игра обновилась»: ребёнок %d, .so@0x%llx, base.apk@0x%llx, "
           "заголовок метаданных@0x%llx, декой@0x%llx, куча@0x%llx\n",
           (int)child, (unsigned long long)h.il2cpp_base,
           (unsigned long long)h.apk_base, (unsigned long long)h.meta_header,
           (unsigned long long)h.decoy_base, (unsigned long long)h.heap_base);

    if (!g_mem.bind(child, h.il2cpp_base)) {
        fprintf(stderr, "привязка не удалась\n");
        kill(child, SIGKILL);
        return 1;
    }
    g_pid = child;
    g_il2cpp_base = h.il2cpp_base;

    offset_resolver_reset();

    const double t0 = mono_seconds();
    for (;;) {
        g_mem.frame_begin();
        offset_resolver_tick();
        if (s_phase == PH_DONE) break;
        if (mono_seconds() - t0 > 180.0) {
            char st[128] = {};
            offset_resolver_status(st, sizeof(st));
            printf("  ТАЙМАУТ: фаза %d, статус «%s»\n", (int)s_phase, st);
            break;
        }
    }
    printf("резолв занял %.1f с\n", mono_seconds() - t0);

    if (garbage) {
        check(go_active::PLAYER_TEAM_NAME != 0x2a8,
              "мусорное смещение из фальшивых классов НЕ применено");
        check(!no_key_class_found() || s_live_resolved == 0,
              "без ключевых классов живых значений не осталось");
        check(offset_resolver_class("PlayerManager", nullptr) == 0,
              "фальшивый класс не выдан за PlayerManager");
        struct stat st {};
        check(stat((std::string(OR_CACHE_DIR) + ".offs").c_str(), &st) != 0,
              "кэш НЕ записан по мусорному резолву");
        kill(child, SIGCONT); kill(child, SIGKILL);
        int status = 0; waitpid(child, &status, 0);
        printf(s_fails ? "СТЕНД ПРОВАЛЕН (%d из %d проверок)\n" : "СТЕНД ПРОЙДЕН (%d проверок)\n",
               s_fails ? s_fails : s_checks, s_checks);
        return s_fails ? 1 : 0;
    }

    if (encrypted) {
        check(!s_meta_from_sig && s_meta_sig_done,
              "заголовка метаданных в памяти нет — сигнатурный поиск отработал вхолостую");
        check(s_wanted_str_found > 0,
              "имя класса найдено в памяти, хотя вокруг него нули (старый фильтр выбрасывал)");
        check(offset_resolver_class("PlayerManager", nullptr) == h.pm,
              "класс найден по СОСЕДСТВУ с инлайновым именем");
        check(offset_resolver_class("NetworkClient", nullptr) == h.nc,
              "второй класс найден тем же путём");
        check(go_active::PLAYER_TEAM_NAME == 0x2A0 &&
              resolver_applied(&go_active::PLAYER_TEAM_NAME),
              "teamName переписан вшитый -> 0x2A0");
        check(go_active::PLAYER_CLAN_ID == 0x2B0, "clanId переписан -> 0x2B0");
        check(go_active::PLAYER_CLAN_TAG == 0x2B8, "clanTag переписан -> 0x2B8");
        kill(child, SIGCONT);
        kill(child, SIGKILL);
        int st = 0;
        waitpid(child, &st, 0);
        printf(s_fails ? "СТЕНД ПРОВАЛЕН (%d из %d проверок)\n" : "СТЕНД ПРОЙДЕН (%d проверок)\n",
               s_fails ? s_fails : s_checks, s_checks);
        return s_fails ? 1 : 0;
    }

    check(s_meta_from_sig, "блок метаданных найден по сигнатуре (без имени в карте)");
    check(s_meta_tries >= 1 && s_meta_bad_count >= 1,
          "подставной блок протектора отвергнут, поиск продолжен");
    check(s_meta_blob == h.meta_header, "адрес заголовка совпал с подложенным");
    check(s_meta_version == 31, "версия метаданных прочитана (31)");
    check(s_meta_begin == h.string_table &&
          s_meta_end == h.string_table + kStringSize,
          "окно имён = таблица строк из заголовка, а не весь файл");

    check(offset_resolver_class("PlayerManager", "Oxide") == h.pm,
          "PlayerManager найден в куче по указателю на имя");
    {
        int same = 0;
        for (int i = 0; i < s_class_count; ++i)
            if (strcmp(s_classes[i].name, "PlayerManager") == 0) ++same;
        check(same <= 6 && s_class_count < 64,
              "3000 мусорных тёзок отсеяны по форме, массив классов не забит");
    }
    check(offset_resolver_class("NetworkClient", "Mirror") == h.nc,
          "NetworkClient найден в куче по указателю на имя");
    check(offset_resolver_class("Item", nullptr) == h.item,
          "Item найден в куче по указателю на имя");

    check(go_active::PLAYER_TEAM_NAME == 0x2A0 &&
          resolver_applied(&go_active::PLAYER_TEAM_NAME),
          "teamName переписан вшитый -> 0x2A0");
    check(go_active::PLAYER_CLAN_ID == 0x2B0, "clanId переписан -> 0x2B0");
    check(go_active::PLAYER_CLAN_TAG == 0x2B8, "clanTag переписан -> 0x2B8");
    check(go_active::PLAYER_POSITION == 0x1E4, "lastSavedPosition переписан -> 0x1E4");
    check(go_active::PLAYER_SEAT_ID == 0x310, "seatID переписан -> 0x310");

    // Классов, которых в игре нет, резолвер выдумывать не должен.
    check(offset_resolver_class("FPManager", nullptr) == 0,
          "отсутствующий класс не выдумывается");

    kill(child, SIGCONT);
    kill(child, SIGKILL);
    int status = 0;
    waitpid(child, &status, 0);

    printf(s_fails ? "СТЕНД ПРОВАЛЕН (%d из %d проверок)\n" : "СТЕНД ПРОЙДЕН (%d проверок)\n",
           s_fails ? s_fails : s_checks, s_checks);
    return s_fails ? 1 : 0;
}
