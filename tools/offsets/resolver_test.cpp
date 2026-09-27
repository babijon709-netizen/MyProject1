// Стенд автообновления оффсетов (jni/src/offset_resolver.cpp).
//
//   sh tools/offsets/resolver_test.sh
//
// Устройство повторяет то, что видит резолвер на телефоне: процесс-ребёнок,
// у которого в карте есть файл «.../libil2cpp.so» (mmap подложенного файла)
// и окно «.../global-metadata.dat», а в них — синтетические слоты классов и
// массивы полей с импровизированными смещениями. Родитель привязывается к
// ребёнку тем же слоем /proc/<pid>/mem и гонит НАСТОЯЩИЙ резолвер через все
// фазы: карты -> скан слотов -> калибровка массива полей -> резолв таблицы.
//
// Что проверяется:
//   * классы находятся по имени без всяких TYPEINFO_RVA (в т.ч. тёзка
//     «PlayerManager» без полей не мешает резолву — поле ищется в том из
//     классов, где оно есть);
//   * смещения полей берутся из FieldInfo и ПЕРЕЗАПИСЫВАЮТ вшитые константы
//     (подложены значения, отличающиеся от табличных);
//   * статик-поля резолвятся тем же механизмом;
//   * константы классов, которых в ребёнке нет, остаются на вшитых значениях;
//   * найденные классы отдаются по запросу (замена typeinfo-слотов);
//   * reapply возвращает найденное после порчи слота;
//   * после reset резолвер проходится заново и приходит к тем же значениям.
//
// Нечитаемые имена (устройство с отказом метаданных) здесь не смоделировать —
// память ребёнка читается вся; на устройстве этот случай оставляет вшитые
// значения (резолвер ничего не находит и ничего не портит).

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>

#include <string>

#include "mem_io.h"        // настоящий слой чтения (как на устройстве)
#include "game_offsets_active.h"

// ---- то, что резолвер ждёт от окружения (в игре живёт в game.cpp) ----------
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

// Та же семантика, что у read_remote_string в game.cpp.
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

// Настоящий резолвер (компилируется вместе со стендом).
#include "../../jni/src/offset_resolver.cpp"

// ---- утилиты стенда ----------------------------------------------------------
static int s_checks = 0, s_fails = 0;
static void check(bool ok, const char* what) {
    ++s_checks;
    if (!ok) ++s_fails;
    printf("  %-64s %s\n", what, ok ? "ок" : "ПРОВАЛ");
}

static bool resolver_applied(std::uint64_t* slot) {
    for (int i = 0; i < s_applied_count; ++i)
        if (s_applied[i].slot == slot) return true;
    return false;
}

static int resolver_field_index(std::uint64_t* slot) {
    for (int i = 0; i < kOrFieldCount; ++i)
        if (kOrFields[i].slot == slot) return i;
    return -1;
}

struct Handed {
    uint64_t il2cpp_base, meta_base;
    uint64_t pm_real, pm_decoy, teammember, netclient;
    uint64_t fpmanager, item, itemdata;
    uint64_t treehs, orehs, vec3, hitmarkeritem, oremark, itempickup, playerweapon;
    uint64_t teamcomponent, teamdata, networkbehaviour, networkidentity, dictionary;
};

// Строка в окне метаданных; возвращает её адрес.
static uint64_t put_string(uint8_t* meta, size_t& off, const char* s) {
    uint64_t addr = (uint64_t)(uintptr_t)(meta + off);
    memcpy(meta + off, s, strlen(s) + 1);
    off += strlen(s) + 1;
    off = (off + 7) & ~7u;
    return addr;
}

// Поле синтетического класса.
//   name    — имя поля (может быть «ротированным» обфускатором);
//   offset  — смещение, которое резолвер должен вычитать;
//   tid     — индекс определения типа (0 = примитив); у ссылочных полей
//             равен byval_arg класса-типа (резолвер сверяет именно его);
//   tenum   — Il2CppTypeEnum, если tid == 0 (0x0C float, 0x02 bool, ...).
struct PField { const char* name; int32_t offset; int32_t tid; uint32_t tenum; };
#define PF(n, o) { n, o, 0, 0 }   // поле с читаемым именем: тип не важен

// Класс в куче ребёнка: Il2CppClass + массив FieldInfo. Возвращает адрес класса.
//   tid — byval_arg этого класса (klassIndex): им резолвер узнаёт тип поля.
static uint64_t build_class(uint8_t* heap, size_t& hoff,
                            uint64_t image_ptr, uint64_t name_ptr, uint64_t ns_ptr,
                            const PField* fields, int nfields, uint8_t* meta, size_t& moff,
                            int32_t tid) {
    hoff = (hoff + 0x1FFu) & ~0x1FFu;
    uint8_t* k = heap + hoff;
    const uint64_t klass = (uint64_t)(uintptr_t)k;
    hoff += 0x200;
    memset(k, 0, 0x200);
    *(uint64_t*)(k + 0x00) = image_ptr;          // image — в окно libil2cpp
    *(uint64_t*)(k + 0x10) = name_ptr;           // name  — в окно метаданных
    *(uint64_t*)(k + 0x18) = ns_ptr;
    *(uint64_t*)(k + 0x58) = 0;                  // parent
    *(uint64_t*)(k + 0xB8) = klass + 0x140;      // static_fields — валидный адрес
    // byval_arg класса: Il2CppType (0x10) — data.klassIndex@0x0, биты@0x8.
    // Резолвер берёт отсюда индекс типа и сверяет с FieldInfo.type полей.
    *(int32_t*)(k + 0x20) = tid;
    *(uint32_t*)(k + 0x28) = 0x11u << 16;        // VALUETYPE/CLASS — не важно
    *(uint16_t*)(k + 0x124) = (uint16_t)nfields;

    if (nfields > 0) {
        uint8_t* farr_p = heap + hoff;
        const uint64_t farr = (uint64_t)(uintptr_t)farr_p;
        hoff += 0x20 * nfields + 0x40;
        *(uint64_t*)(k + 0x80) = farr;           // fields
        // По одному Il2CppType на поле: резолвер различает поля именно по типу.
        uint8_t* tarr_p = heap + hoff;
        hoff += 0x10 * (size_t)nfields + 0x40;
        for (int i = 0; i < nfields; ++i) {
            uint8_t* f = farr_p + 0x20 * i;
            uint8_t* t = tarr_p + 0x10 * (size_t)i;
            *(int32_t*)(t + 0x00) = fields[i].tid;
            *(uint32_t*)(t + 0x08) = fields[i].tenum << 16;
            *(uint64_t*)(f + 0x00) = put_string(meta, moff, fields[i].name);
            *(uint64_t*)(f + 0x08) = (uint64_t)(uintptr_t)t;
            *(uint64_t*)(f + 0x10) = klass;      // parent == класс
            *(int32_t*)(f + 0x18)  = fields[i].offset;
            *(uint32_t*)(f + 0x1C) = 0x04000000u + i;
        }
    }
    return klass;
}

static void child_main(int out_fd) {
    char dir[] = "/tmp/resolver_bench.XXXXXX";
    if (!mkdtemp(dir)) _exit(90);
    char il_path[128], meta_path[128];
    snprintf(il_path, sizeof(il_path), "%s/libil2cpp.so", dir);
    snprintf(meta_path, sizeof(meta_path), "%s/global-metadata.dat", dir);

    const size_t kIlSize = 512u << 10, kMetaSize = 64u << 10;
    int il_fd = open(il_path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    int meta_fd = open(meta_path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (il_fd < 0 || meta_fd < 0) _exit(91);
    if (ftruncate(il_fd, kIlSize) || ftruncate(meta_fd, kMetaSize)) _exit(92);

    uint8_t* il   = (uint8_t*)mmap(NULL, kIlSize, PROT_READ | PROT_WRITE, MAP_SHARED, il_fd, 0);
    uint8_t* meta = (uint8_t*)mmap(NULL, kMetaSize, PROT_READ | PROT_WRITE, MAP_SHARED, meta_fd, 0);
    if (il == MAP_FAILED || meta == MAP_FAILED) _exit(93);
    memcpy(il, "\x7f" "ELF", 4);   // bind() пробует читать начало образа

    // Куча ребёнка под классы/поля.
    static uint8_t heap[1u << 20];
    size_t hoff = 0x1000, moff = 0x100;

    const uint64_t il_base  = (uint64_t)(uintptr_t)il;
    const uint64_t meta_base = (uint64_t)(uintptr_t)meta;

    // Имена классов и пространств имён — в окне метаданных.
    const uint64_t s_pm   = put_string(meta, moff, "PlayerManager");
    const uint64_t s_gc   = put_string(meta, moff, "GameControllerBase");
    const uint64_t s_oxide = put_string(meta, moff, "Oxide");
    const uint64_t s_tm   = put_string(meta, moff, "TeamMember");
    const uint64_t s_teamcomponent = put_string(meta, moff, "Reo");
    const uint64_t s_teamdata = put_string(meta, moff, "nX");
    const uint64_t s_networkbehaviour = put_string(meta, moff, "NetworkBehaviour");
    const uint64_t s_networkidentity = put_string(meta, moff, "NetworkIdentity");
    const uint64_t s_dictionary = put_string(meta, moff, "Dictionary`2");
    const uint64_t s_generic_dictionary = put_string(meta, moff, "System.Collections.Generic");
    const uint64_t s_empty = put_string(meta, moff, "");
    const uint64_t s_nc   = put_string(meta, moff, "NetworkClient");
    const uint64_t s_mirror = put_string(meta, moff, "Mirror");
    // Классы для проверки поиска поля по ТИПУ (имя поля «ротировано»).
    const uint64_t s_fpm  = put_string(meta, moff, "FPManager");
    const uint64_t s_item = put_string(meta, moff, "Item");
    const uint64_t s_itemdata = put_string(meta, moff, "ItemData");
    // Классы автофарма: имена полей ротированы («XeK» вместо «XeK»), ищутся по
    // типу. Тип-классы названы ПОЛНО, как в дампе (резолвер сверяет хвост за
    // подчёркиванием: «c:OreHitstreaksMarker» ↔ «..._OreHitstreaksMarker»).
    const uint64_t s_treehs = put_string(meta, moff, "MineableObjectExtension_TreeHitstreaks");
    const uint64_t s_orehs  = put_string(meta, moff, "MineableObjectExtension_OreHitstreaks");
    const uint64_t s_vec3   = put_string(meta, moff, "Vector3");
    const uint64_t s_hmitem = put_string(meta, moff, "MineableObjectExtension_HitMarkerItem");
    const uint64_t s_oremark = put_string(meta, moff, "MineableObjectExtension_OreHitstreaksMarker");
    const uint64_t s_pickup = put_string(meta, moff, "ItemPickup");
    const uint64_t s_weapon = put_string(meta, moff, "PlayerWeapon");
    // Классы с ротирующимися именами (цепочки via): лежат только в куче
    // ребёнка, а не в слотах — по имени кэш их не знает, находит только
    // охота по индексу типа (byval_arg — индекс типа поля-ссылки).
    const uint64_t s_peg  = put_string(meta, moff, "eDg");   // PlayerEventHandler
    const uint64_t s_act  = put_string(meta, moff, "eDw");   // Activity

    // Окно данных «.so»: сюда кладём слоты-указатели, которые ищет сканер.
    uint64_t* slots = (uint64_t*)(il + 0x40000);
    const size_t kSlots = 4096;

    // Синтетические team/network классы проверяют новые runtime-записи:
    // имена полей на ротируемых TeamComponent/TeamData не совпадают с релизом,
    // поэтому они должны разрешиться через живую type/via-цепочку и тип.
    const int32_t tid_teamcomponent = 0x303;
    const int32_t tid_teamdata = 0x304;
    const int32_t tid_networkidentity = 0x305;
    const int32_t tid_networkbehaviour = 0x306;
    const PField teamdata_fields[] = {
        {"FLy", 0x18, 0, 0x0E},   // string: TeamData name; вшито 0x10
        {"FLV", 0x28, 0, 0x0E},   // второй string, не должен быть выбран
        {"FLv", 0x30, 0, 0x12},   // единственный Il2CppObject*: roster list; вшито 0x20
    };
    uint64_t teamdata = build_class(heap, hoff, il_base + 0x1000, s_teamdata, s_empty,
                                    teamdata_fields, 3, meta, moff, tid_teamdata);
    const PField teamcomponent_fields[] = {
        {"TeammateStates", 0x78, 0, 0x12},          // Dictionary; live-resolved (real dump: 0x70)
        {"player", 0x98, 0x101, 0x12},               // PlayerManager back-reference
        {"team", 0xA0, tid_teamdata, 0x12},          // TeamData reference; вшито 0x90
    };
    uint64_t teamcomponent = build_class(heap, hoff, il_base + 0x1000, s_teamcomponent, s_empty,
                                         teamcomponent_fields, 3, meta, moff, tid_teamcomponent);
    const PField dictionary_fields[] = {
        {"_entries", 0x28, 0, 0x12},                 // object array; standard dump offset 0x18
        {"_count", 0x38, 0, 0x08},                   // int32; standard dump offset 0x20
    };
    uint64_t dictionary = build_class(heap, hoff, il_base + 0x1000,
                                      s_dictionary, s_generic_dictionary,
                                      dictionary_fields, 2, meta, moff, 0x307);
    const PField networkidentity_fields[] = {
        {"_netId_k__BackingField", 0x60, 0, 0x09},   // uint; вшито 0x58
    };
    uint64_t networkidentity = build_class(heap, hoff, il_base + 0x1000,
                                           s_networkidentity, s_mirror,
                                           networkidentity_fields, 1, meta, moff,
                                           tid_networkidentity);
    const PField networkbehaviour_fields[] = {
        {"<netIdentity>k__BackingField", 0x48, tid_networkidentity, 0x12},
    };
    uint64_t networkbehaviour = build_class(heap, hoff, il_base + 0x1000,
                                            s_networkbehaviour, s_mirror,
                                            networkbehaviour_fields, 1, meta, moff,
                                            tid_networkbehaviour);

    // Настоящий PlayerManager: смещения ОТЛИЧАЮТСЯ от вшитых, чтобы видеть
    // именно перезапись, а не совпадение.
    const PField pm_fields[] = {
        PF("teamName",           0x2A0),   // вшито 0x280
        PF("clanId",             0x2B0),   // вшито 0x290
        PF("clanTag",            0x2B8),   // вшито 0x298
        PF("nicklabel",          0x138),   // вшито 0x130
        {"team", 0x128, tid_teamcomponent, 0x12}, // Reo/euD via-chain type
        PF("clientPlayerList",   0x18),    // статик; вшито 0x10
        {"playerEventHandler", 0x348, 0x301, 0x12},  // c:eDg; вшито 0x78; вход в цепочки via
    };
    uint64_t pm_real = build_class(heap, hoff, il_base + 0x1000, s_pm, s_oxide,
                                   pm_fields, 7, meta, moff, 0x101);
    const PField gc_fields[] = {
        {"_BCX_k__BackingField", 0x18, 0x101, 0x12}, // static PlayerManager; вшито 0x10
    };
    uint64_t gamecontroller = build_class(heap, hoff, il_base + 0x1000, s_gc, s_oxide,
                                          gc_fields, 1, meta, moff, 0x10A);

    // Тёзка без полей: резолв не обязан на него попасть.
    uint64_t pm_decoy = build_class(heap, hoff, il_base + 0x1000, s_pm, s_oxide,
                                    NULL, 0, meta, moff, 0x102);

    const PField tm_fields[] = {
        PF("nick",    0x30),   // вшито 0x20
        PF("clanTag", 0x28),   // вшито 0x18
        {"netId",    0x34, 0, 0x09}, // uint; вшито 0x2C
    };
    uint64_t teammember = build_class(heap, hoff, il_base + 0x1000, s_tm, s_empty,
                                      tm_fields, 3, meta, moff, 0x103);

    const PField nc_fields[] = {
        PF("spawned", 0x40),   // статик
    };
    uint64_t netclient = build_class(heap, hoff, il_base + 0x1000, s_nc, s_mirror,
                                     nc_fields, 1, meta, moff, 0x104);

    // ItemData — класс-тип поля Item._BKs_k__BackingField (в таблице резолвера
    // «c:ItemData»). Его byval_arg и есть тот klassIndex, который резолвер
    // будет искать в FieldInfo.type.
    const int32_t tid_itemdata = 0x205;
    uint64_t itemdata = build_class(heap, hoff, il_base + 0x1000, s_itemdata, s_empty,
                                    NULL, 0, meta, moff, tid_itemdata);

    // FPManager: ДВА поля с РОТИРОВАННЫМИ именами (_XqK/_YqK вместо _BYK/BYu)
    // типа float. По имени их не найти — резолвер берёт оба по типу и разводит
    // между собой: оба уехали на +0x08 (вшито 0xA8/0x9C -> 0xB0/0xA4).
    const PField fpm_fields[] = {
        {"_XqK_k__BackingField", 0xB0, 0, 0x0C},   // float; вшито 0xA8
        {"_YqK_k__BackingField", 0xA4, 0, 0x0C},   // float; вшито 0x9C
    };
    uint64_t fpmanager = build_class(heap, hoff, il_base + 0x1000, s_fpm, s_oxide,
                                     fpm_fields, 2, meta, moff, 0x206);

    // Item: поле с ротированным именем типа ItemData (c:ItemData).
    const int32_t tid_item = 0x207;
    const PField item_fields[] = {
        {"_ZzQ_k__BackingField", 0x30, tid_itemdata, 0x12},  // class; вшито 0x20
    };
    uint64_t item = build_class(heap, hoff, il_base + 0x1000, s_item, s_oxide,
                                item_fields, 1, meta, moff, tid_item);

    // Классы-ТИПЫ для полей автофарма: свои поля не нужны, важен только
    // byval_arg (klassIndex) — по нему резолвер узнаёт тип поля. Имена полные
    // («..._OreHitstreaksMarker»), чтобы в кэше сработал хвост за подчёркиванием.
    const int32_t tid_vec3    = 0x20A;
    const int32_t tid_hmitem  = 0x20B;
    const int32_t tid_oremark = 0x20C;
    uint64_t vec3       = build_class(heap, hoff, il_base + 0x1000, s_vec3, s_empty,
                                      NULL, 0, meta, moff, tid_vec3);
    uint64_t hitmarkeritem = build_class(heap, hoff, il_base + 0x1000, s_hmitem, s_oxide,
                                         NULL, 0, meta, moff, tid_hmitem);
    // …_OreHitstreaksMarker: два float-поля с ротированными именами (XeS/XeE).
    // Оба уехали так, что любая раскладка стоит одинаково (0x58/0x48 ->
    // 0x68/0x88): развести их нечем, и резолвер обязан оставить вшитые
    // значения, а не угадать.
    const PField oremark_fields[] = {
        {"XqA", 0x68, 0, 0x0C},   // float; вшито 0x58 (OREMARK_AGE)
        {"XqB", 0x88, 0, 0x0C},   // float; вшито 0x48 (OREMARK_SMOOTH)
    };
    uint64_t oremark    = build_class(heap, hoff, il_base + 0x1000, s_oremark, s_oxide,
                                      oremark_fields, 2, meta, moff, tid_oremark);

    // MineableObjectExtension_TreeHitstreaks: ВСЕ имена ротированы (в таблице
    // «Xze»/«Xzw»/«Xzv»/«XzL»), поля находятся только по типу. Два поля типа
    // Vector3 уехали ОДИНАКОВО (+0x10 — будто выше по классу вставили поле):
    // точного совпадения со вшитым смещением нет ни у одного, и разводить их
    // надо вместе, задачей о назначениях.
    const PField treehs_fields[] = {
        {"movingMethod", 0xE8, 0, 0x08},   // i4; имя читаемое — берётся по имени
        {"hitStreakMarkerOriginal", 0xA8, tid_hmitem, 0x12},  // TREEHS_MARKER_TEMPLATE
        {"XqS", 0x90, tid_hmitem, 0x12},   // c:HitMarkerItem;  вшито 0x50
        {"XqR", 0x98, tid_vec3,   0x12},   // c:Vector3;        вшито 0x88
        {"XqV", 0xB4, tid_vec3,   0x12},   // c:Vector3;        вшито 0xA4
        {"XqW", 0xE0, 0,          0x08},   // i4; имя ротировано — только по типу
    };
    uint64_t treehs = build_class(heap, hoff, il_base + 0x1000, s_treehs, s_oxide,
                                  treehs_fields, 6, meta, moff, 0x208);

    // MineableObjectExtension_OreHitstreaks: крестик автофарма. Одно поле с
    // читаемым именем (hitStreakMarker, TREEHS/OREHS_MARKER_TEMPLATE), второе —
    // с ротированным (XeK -> XqT): первое находится по имени и «занимает» своё
    // смещение, второе — по типу из оставшихся.
    const PField orehs_fields[] = {
        {"hitStreakMarker", 0x58, tid_oremark, 0x12},  // OREHS_MARKER_TEMPLATE
        {"XqT", 0x50, tid_oremark, 0x12},  // c:OreHitstreaksMarker; вшито 0x30
    };
    uint64_t orehs = build_class(heap, hoff, il_base + 0x1000, s_orehs, s_oxide,
                                 orehs_fields, 2, meta, moff, 0x209);

    // ItemPickup: поле с ротированным именем (_BQL -> XqC) типа Item. Полей
    // такого типа в классе ДВА (0xB0 и 0xC0), вшитое смещение 0xA8: точного
    // совпадения нет, резолвер берёт ближайшее — и только если оно единственное.
    const PField pickup_fields[] = {
        {"XqC", 0xB0, tid_item, 0x12},   // c:Item; вшито 0xA8
        {"XqD", 0xC0, tid_item, 0x12},   // c:Item; второй такой же
    };
    uint64_t itempickup = build_class(heap, hoff, il_base + 0x1000, s_pickup, s_oxide,
                                      pickup_fields, 2, meta, moff, 0x20D);

    // PlayerWeapon: обратная ссылка на игрока (c:PlayerManager). В кэше ДВА
    // класса с именем PlayerManager (пустышка-тёзка лежит раньше настоящего), и
    // byval_arg у них разный: резолвер обязан перебрать оба индекса, иначе поле
    // не найдётся ни одного. Имя поля ротировано (_player -> XqE).
    const PField weapon_fields[] = {
        {"XqE", 0x140, 0x101, 0x12},   // c:PlayerManager (настоящий); вшито 0x138
    };
    uint64_t playerweapon = build_class(heap, hoff, il_base + 0x1000, s_weapon, s_oxide,
                                        weapon_fields, 1, meta, moff, 0x20E);

    // PlayerEventHandler (ротированное имя eDg): первый шаг цепочек via. В
    // слотах его НЕТ — кэш по именам его не знает, находит только охота по
    // индексу типа: byval_arg 0x301 = тип PlayerManager.playerEventHandler.
    const PField peg_fields[] = {
        {"Aim",               0x40, 0x302, 0x12},  // Activity; имя читаемое; вшито 0x290
        {"XqU",               0xD8, 0x101, 0x12},  // c:PlayerManager; имя ротировано ("manager")
        {"LastLocalHitPoint", 0x180, 0x104, 0x12}, // имя читаемое; вшито 0x170
        {"LookDirection",     0x140, 0x105, 0x12}, // имя читаемое; вшито 0x140
        {"XqV",               0x188, 0x106, 0x12}, // балласт: не даём «вынужденному выбору»
    };
    uint64_t playerhandler = build_class(heap, hoff, il_base + 0x1000, s_peg, s_oxide,
                                         peg_fields, 5, meta, moff, 0x301);

    // Activity (ротированное имя eDw): ВТОРОЙ шаг двухшаговой цепочки — тип
    // поля eDg.Aim (0x302). Два bool-поля с ротированными именами, смещения
    // уехали от вшитого 0x10: резолвер берёт ближе, если оно единственное.
    const PField act_fields[] = {
        {"_XqS_k__BackingField", 0x90, 0, 0x02},  // bool; вшито 0x10
        {"_XqT_k__BackingField", 0xA0, 0, 0x02},  // bool; второй такой же
    };
    uint64_t activity = build_class(heap, hoff, il_base + 0x1000, s_act, s_oxide,
                                    act_fields, 2, meta, moff, 0x302);
    (void)playerhandler; (void)activity;

    // Живой PlayerManager -> Reo -> nX объектный путь, как в игре. Это
    // гарантирует, что двухшаговый via-поиск не зависит от порядка адресов,
    // в котором phase_kid случайно встретит классы в куче.
    auto align_heap = [&] { hoff = (hoff + 0x1Fu) & ~0x1Fu; };
    align_heap();
    const uint64_t local_player = (uint64_t)(uintptr_t)(heap + hoff);
    memset(heap + hoff, 0, 0x180); hoff += 0x180;
    *(uint64_t*)(uintptr_t)local_player = pm_real;
    align_heap();
    const uint64_t local_teamcomponent = (uint64_t)(uintptr_t)(heap + hoff);
    memset(heap + hoff, 0, 0x180); hoff += 0x180;
    *(uint64_t*)(uintptr_t)local_teamcomponent = teamcomponent;
    *(uint64_t*)(uintptr_t)(local_player + 0x128) = local_teamcomponent;
    *(uint64_t*)(uintptr_t)(local_teamcomponent + 0x98) = local_player;
    align_heap();
    const uint64_t local_teamdata = (uint64_t)(uintptr_t)(heap + hoff);
    memset(heap + hoff, 0, 0x80); hoff += 0x80;
    *(uint64_t*)(uintptr_t)local_teamdata = teamdata;
    *(uint64_t*)(uintptr_t)(local_teamcomponent + 0xA0) = local_teamdata;
    *(uint64_t*)(uintptr_t)(gamecontroller + 0x140 + 0x18) = local_player;

    // Слоты: настоящие классы вперемешку с мусором (выравнивание, битые
    // указатели, указатели в никуда) — фильтры сканера обязаны его отсеять.
    size_t sn = 0;
    slots[sn++] = pm_decoy;           // тёзка — раньше настоящего
    slots[sn++] = 0;
    slots[sn++] = il_base + 0x1234;   // указатель внутрь .so — не класс
    slots[sn++] = 0xdeadbeef000ull;   // валидный по форме, чтений нет
    slots[sn++] = meta_base + 0x40;   // в метаданные — тоже не класс
    slots[sn++] = pm_real;
    slots[sn++] = gamecontroller;
    // Мусор: значения проходят форму указателя, но читаются как нули
    // (выровнены на 64 КБ, где в ребёнке ничего нет). НЕ в начало кучи:
    // там лежит настоящий указатель, который сканер честно примет за класс.
    for (int i = 0; i < 700; ++i) slots[sn++] = 0x400000000ull + (uint64_t)(i * 8);
    slots[sn++] = teammember;
    slots[sn++] = netclient;
    slots[sn++] = teamcomponent;
    slots[sn++] = teamdata;
    slots[sn++] = dictionary;
    slots[sn++] = networkidentity;
    slots[sn++] = networkbehaviour;
    slots[sn++] = fpmanager;
    slots[sn++] = item;
    slots[sn++] = itemdata;
    slots[sn++] = treehs;
    slots[sn++] = orehs;
    slots[sn++] = vec3;
    slots[sn++] = hitmarkeritem;
    slots[sn++] = oremark;
    slots[sn++] = itempickup;
    slots[sn++] = playerweapon;
    slots[sn++] = 0xFFFFFFFFFFFFFFF8ull;
    if (sn > kSlots) _exit(94);

    Handed h;
    h.il2cpp_base = il_base;
    h.meta_base = meta_base;
    h.pm_real = pm_real; h.pm_decoy = pm_decoy;
    h.teammember = teammember; h.netclient = netclient;
    h.fpmanager = fpmanager; h.item = item; h.itemdata = itemdata;
    h.treehs = treehs; h.orehs = orehs; h.vec3 = vec3;
    h.hitmarkeritem = hitmarkeritem; h.oremark = oremark; h.itempickup = itempickup; h.playerweapon = playerweapon;
    h.teamcomponent = teamcomponent; h.teamdata = teamdata;
    h.networkbehaviour = networkbehaviour; h.networkidentity = networkidentity;
    h.dictionary = dictionary;
    if (write(out_fd, &h, sizeof(h)) != sizeof(h)) _exit(95);

    raise(SIGSTOP);   // родитель читает память, пока мы спим
    _exit(0);
}

int main() {
    int fds[2];
    if (pipe(fds)) { perror("pipe"); return 1; }
    const pid_t child = fork();
    if (child < 0) { perror("fork"); return 1; }
    if (child == 0) { close(fds[0]); child_main(fds[1]); _exit(0); }
    close(fds[1]);

    Handed h;
    if (read(fds[0], &h, sizeof(h)) != sizeof(h)) {
        fprintf(stderr, "ребёнок не отдал адреса\n");
        kill(child, SIGKILL);
        return 1;
    }
    printf("стенд автообновления оффсетов: ребёнок %d, .so@0x%llx, метаданные@0x%llx\n",
           (int)child, (unsigned long long)h.il2cpp_base, (unsigned long long)h.meta_base);

    if (!g_mem.bind(child, h.il2cpp_base)) {
        fprintf(stderr, "привязка не удалась\n");
        kill(child, SIGKILL);
        return 1;
    }
    g_pid = child;
    g_il2cpp_base = h.il2cpp_base;

    // Регрессия: кэш совпал, но якорь появится позже. Между попытками
    // резолвер ОБЯЗАН оставаться в PH_WAIT, а не начинать полный скан на
    // следующем кадре (именно так происходило на устройстве при перезаходе).
    offset_resolver_reset();
    s_bound_pid = child; s_bound_base = h.il2cpp_base;
    s_cache_checked = true; s_cache_retryable = true;
    s_cache_wait_since = mono_seconds();
    s_cache_retry_at = s_cache_wait_since + 10.0;
    offset_resolver_tick();
    check(s_phase == PH_WAIT && offset_resolver_busy(),
          "кэш ждёт якорь без перехода к полному скану");
    offset_resolver_reset();

    // ---- накопитель строк лога: обрезка не должна уводить позицию за буфер --
    // Ловушка snprintf (он возвращает ЖЕЛАЕМУЮ длину) роняла чит на устройстве:
    // «FORTIFY: vsnprintf: size 18446744073709551614 > SSIZE_MAX».
    {
        char small[16];
        size_t used = 0;
        small[0] = '\0';
        for (int i = 0; i < 20; ++i)
            log_append(small, sizeof(small), used, "оченьдлинноеимя%d, ", i);
        check(used < sizeof(small), "накопитель лога не уводит позицию за буфер");
        check(small[sizeof(small) - 1] == '\0', "накопитель лога держит строку завершённой");
        size_t zero_used = 0;
        char one[1] = {'\0'};
        log_append(one, sizeof(one), zero_used, "%s", "x");
        check(zero_used == 0 && one[0] == '\0', "буфер в один байт не переполняется");
    }

    // Вшитые значения констант, которые не должны измениться (классов нет).
    const uint64_t mineable_before = go_active::MINEABLE_ENTITY_TYPE;

    // ГОНЯЕМ резолвер до готовности (порции по 4 мс, как на устройстве).
    const double t0 = mono_seconds();
    for (;;) {
        g_mem.frame_begin();
        offset_resolver_tick();
        char st[96] = {};
        offset_resolver_status(st, sizeof(st));
        if (strstr(st, "готов")) break;
        if (mono_seconds() - t0 > 30.0) {
            printf("ПРОВАЛ: резолвер не завершился за 30 с (%s)\n", st);
            offset_resolver_debug_dump();
            // Диагностика: ЧИТАЕМ НАПРЯМУЮ (в обход кэша блоков), чтобы видеть
            // правду, а не закэшированный резолвером ноль.
            char mpath[64];
            snprintf(mpath, sizeof(mpath), "/proc/%d/mem", (int)child);
            int mfd = open(mpath, O_RDONLY);
            if (mfd >= 0) {
                uint32_t magic = 0;
                uint64_t slot0 = 0;
                pread(mfd, &magic, 4, (off_t)h.il2cpp_base);
                pread(mfd, &slot0, 8, (off_t)(h.il2cpp_base + 0x40000));
                printf("  прямой pread: магическое %08x, слот @+0x40000=%016llx\n",
                       magic, (unsigned long long)slot0);
                close(mfd);
            } else {
                printf("  прямой pread: /proc/%d/mem НЕ открылся (ребёнок мёртв?)\n", (int)child);
            }
            snprintf(mpath, sizeof(mpath), "/proc/%d/maps", (int)child);
            FILE* mf = fopen(mpath, "r");
            if (mf) {
                char line[512];
                while (fgets(line, sizeof(line), mf))
                    if (strstr(line, "libil2cpp") || strstr(line, "global-metadata"))
                        printf("  maps: %s", line);
                fclose(mf);
            } else {
                printf("  maps: /proc/%d/maps НЕ открылся (ребёнок мёртв?)\n", (int)child);
            }
            kill(child, SIGKILL);
            return 1;
        }
        struct timespec ts { 0, 2 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    char st[128] = {};
    offset_resolver_status(st, sizeof(st));
    printf("  резолвер: %s (%.2f с)\n", st, mono_seconds() - t0);

    // ---- классы по именам (замена TYPEINFO_RVA) ----------------------------
    const uint64_t pm_found = offset_resolver_class("PlayerManager", "Oxide");
    check(pm_found == h.pm_real || pm_found == h.pm_decoy,
          "PlayerManager найден по имени (без RVA)");
    check(offset_resolver_class_nth("PlayerManager", "Oxide", 0) == h.pm_decoy &&
          offset_resolver_class_nth("PlayerManager", "Oxide", 1) == h.pm_real,
          "класс-тёзка: второй кандидат доступен при пустой статике первого");
    check(offset_resolver_class("TeamMember", "") == h.teammember,
          "TeamMember найден по имени");
    check(offset_resolver_class("NetworkClient", "Mirror") == h.netclient,
          "NetworkClient найден по имени");
    check(offset_resolver_class("Dictionary`2", "System.Collections.Generic") == h.dictionary,
          "open generic Dictionary class is visible to the runtime resolver");
    check(offset_resolver_class("GameControllerBase", "Oxide") != 0,
          "GameControllerBase anchor found for the live team chain");
    check(offset_resolver_class("NotARealClass", "") == 0,
          "отсутствующий класс не выдумывается");

    // ---- смещения полей: подложенные значения вместо вшитых ----------------
    check(go_active::PLAYER_TEAM_NAME == 0x2A0, "teamName переписан 0x280 -> 0x2A0");
    check(go_active::PLAYER_CLAN_ID == 0x2B0,   "clanId переписан 0x290 -> 0x2B0");
    check(go_active::PLAYER_CLAN_TAG == 0x2B8,  "clanTag переписан 0x298 -> 0x2B8");
    check(go_active::PLAYER_NICKLABEL == 0x138, "nicklabel переписан 0x130 -> 0x138");
    check(go_active::PLAYER_TEAM_OBJECT == 0x128, "team переписан 0x120 -> 0x128");
    check(go_active::GAME_CONTROLLER_LOCAL_PLAYER_FIELD == 0x18,
          "local PlayerManager static link переписан 0x10 -> 0x18");
    check(go_active::PLAYER_MANAGER_STATIC_FIELDS_LIST == 0x18,
          "статик clientPlayerList переписан 0x10 -> 0x18");
    check(go_active::TEAM_MEMBER_NICK == 0x30,    "TeamMember.nick переписан 0x20 -> 0x30");
    check(go_active::TEAM_MEMBER_CLAN_TAG == 0x28, "TeamMember.clanTag 0x18 -> 0x28");
    check(go_active::PLAYER_NETWORK_IDENTITY == 0x48,
          "NetworkBehaviour.netIdentity разрешён по полю базы 0x40 -> 0x48");
    check(go_active::NETID_NET_ID == 0x60,
          "NetworkIdentity.netId разрешён по имени 0x58 -> 0x60");
    check(go_active::TEAM_MEMBER_NET_ID == 0x34,
          "TeamMember.netId разрешён по имени 0x2C -> 0x34");
    check(go_active::TEAM_OBJECT_PLAYER == 0x98,
          "TeamComponent.player разрешён через PlayerManager.team 0x80 -> 0x98");
    check(go_active::TEAM_OBJECT_DATA == 0xA0,
          "TeamComponent.team разрешён через PlayerManager.team 0x90 -> 0xA0");
    check(go_active::TEAM_OBJECT_STATES == 0x78 &&
          resolver_applied(&go_active::TEAM_OBJECT_STATES),
          "TeamComponent.TeammateStates resolved live through PlayerManager.team");
    check(go_active::DICT_ENTRIES == 0x28 && go_active::DICT_COUNT == 0x38 &&
          resolver_applied(&go_active::DICT_ENTRIES) &&
          resolver_applied(&go_active::DICT_COUNT),
          "generic Dictionary _entries/_count resolved by live class and field name");
    const int dict_entries_idx = resolver_field_index(&go_active::DICT_ENTRIES);
    const int dict_count_idx = resolver_field_index(&go_active::DICT_COUNT);
    const int applied_before_bad_offset = s_applied_count;
    if (dict_entries_idx >= 0) apply_resolution(kOrFields[dict_entries_idx], 0);
    check(go_active::DICT_ENTRIES == 0x28 && s_applied_count == applied_before_bad_offset,
          "zero instance offset cannot overwrite a valid Dictionary slot");
    check(!resolved_offset_is_valid(false, 0, "Dictionary`2") &&
          resolved_offset_is_valid(false, 0, "RaycastHit") &&
          resolved_offset_is_valid(false, 0x0C, "RaycastHit") &&
          resolved_offset_is_valid(false, 0, "WeaponPiece"),
          "managed object header is rejected but inline value-type offsets remain valid");
    check(go_active::TEAM_DATA_NAME == 0x18,
          "TeamData name найден типом через двухшаговый путь 0x10 -> 0x18");
    check(go_active::TEAM_DATA_MEMBERS == 0x30 &&
          resolver_applied(&go_active::TEAM_DATA_MEMBERS),
          "TeamData member collection: unique reference resolved 0x20 -> 0x30 via-path");

    // ---- поля с ротированными именами ищутся по ТИПУ ------------------------
    // Имя _XqK_k__BackingField в таблице нет (обфускатор переименовал), но тип
    // float тот же — резолвер берёт смещение по типу.
    check(go_active::FPMANAGER_AIM_BLEND == 0xB0 && go_active::FPMANAGER_WORLD_FOV_TARGET == 0xA4,
          "FPManager: два float по типу разведены 0xA8/0x9C -> 0xB0/0xA4");
    check(go_active::ITEM_DATA == 0x30,
          "Item: поле по типу ItemData (имя ротировано) 0x20 -> 0x30");
    check(offset_resolver_class("ItemData", "") == h.itemdata,
          "класс-тип поля тоже доступен по имени");

    // ---- автофарм: короткие ротированные имена находятся по ТИПУ ------------
    // В таблице эти константы держались на вшитых значениях: генератор молча
    // выбрасывал поля с именами вида «XeK» (короче трёх символов). Теперь поле
    // попадает в таблицу, если известен его тип, — крестик автофарма снова
    // находится после апдейта игры.
    check(go_active::OREHS_MARKER == 0x50,
          "OreHitstreaks: XeK по типу OreHitstreaksMarker 0x30 -> 0x50");
    check(go_active::OREHS_MARKER_TEMPLATE == 0x58,
          "OreHitstreaks: hitStreakMarker по имени 0x28 -> 0x58");
    check(go_active::TREEHS_MARKER == 0x90,
          "TreeHitstreaks: Xze по типу HitMarkerItem 0x50 -> 0x90");
    check(go_active::TREEHS_MARKER_TEMPLATE == 0xA8,
          "TreeHitstreaks: hitStreakMarkerOriginal по имени 0x28 -> 0xA8");
    check(go_active::TREEHS_STREAK == 0xE0,
          "TreeHitstreaks: XzL по типу i4 0x48 -> 0xE0");
    check(go_active::TREEHS_MOVING_METHOD == 0xE8,
          "TreeHitstreaks: movingMethod по имени 0x20 -> 0xE8");
    check(go_active::ITEMPICKUP_ITEM_OBJECT == 0xB0,
          "ItemPickup: _BQL по типу Item, ближайшее из двух 0xA8 -> 0xB0");
    check(go_active::PLAYERWEAPON_PLAYER_BACKREF == 0x140,
          "PlayerWeapon: _player по типу PlayerManager сквозь тёзку 0x138 -> 0x140");
    // Два поля одного типа Vector3, уехавшие на одно расстояние: разводим по
    // вшитым смещениям (0x88 -> 0x98, 0xA4 -> 0xB4) и не путаем местами.
    check(go_active::TREEHS_SPOT_A == 0x98 && go_active::TREEHS_SPOT_B == 0xB4,
          "TreeHitstreaks: два Vector3 разведены 0x98 / 0xB4");
    check(offset_resolver_class("MineableObjectExtension_OreHitstreaksMarker", "Oxide") == h.oremark,
          "класс-тип крестика найден по полному имени");
    check(offset_resolver_class("Vector3", "") == h.vec3,
          "класс-тип Vector3 найден по имени");
    // Система не угадывает: два float-поля, которые нельзя развести, остаются
    // на вшитых значениях. Если резолвер начнёт писать сюда наугад — упадёт.
    check(go_active::OREMARK_AGE == 0x58 && go_active::OREMARK_SMOOTH == 0x48,
          "OreHitstreaksMarker: неразводимые float оставлены вшитыми");

    // ---- цепочки via: классы с ротирующимися именами ------------------------
    // В кэше PlayerManager-тёзка (пустышка без полей) лежит РАНЬШЕ настоящего:
    // вход в цепочку обязан найти у настоящего. Целевые классы (ротированные
    // имена eDg/eDw) в кэше нет вовсе — только охота по индексу типа их
    // находит в куче ребёнка.
    check(go_active::PLAYER_EVENT_HANDLER == 0x348,
          "playerEventHandler по имени 0x78 -> 0x348 (вход в цепочку)");
    check(go_active::EVENT_HANDLER_AIM_ACTIVITY == 0x40,
          "eDg.Aim через цепочку по имени 0x290 -> 0x40");
    check(go_active::EVENT_HANDLER_MANAGER_BACKREF == 0xD8,
          "eDg: back-ref по типу PlayerManager 0xD0 -> 0xD8 (имя ротировано)");
    check(go_active::EVENT_HANDLER_LAST_HIT_POINT == 0x180,
          "eDg.LastLocalHitPoint через цепочку (без класса) 0x170 -> 0x180");
    check(go_active::ACTIVITY_ACTIVE_FLAG == 0x90,
          "Activity (двухшаговая цепочка): bool по типу, ближе из двух 0x10 -> 0x90");
    check(go_active::EVENT_HANDLER_LAST_HIT_TIME == 0x178 &&
          go_active::GUM_AIM_RAYCAST == 0x168 && go_active::GUM_RAYCAST_DATA == 0x160,
          "неразводимые via-поля оставлены вшитыми (не угадываем)");

    // ---- чего нет в ребёнке — остаётся вшитым ------------------------------
    check(go_active::MINEABLE_ENTITY_TYPE == mineable_before,
          "нерезолвлённая константа не тронута");

    // ---- reapply: после порчи слота найденное возвращается ------------------
    go_active::PLAYER_TEAM_NAME = 0x1234;
    offset_resolver_reapply();
    check(go_active::PLAYER_TEAM_NAME == 0x2A0, "reapply вернул найденное значение");

    // ---- reset: проход заново даёт тот же результат -------------------------
    offset_resolver_reset();
    // Порча и «трудных» констант: повторный проход обязан найти их заново, в
    // том числе два поля одного типа (иначе проверка выше ничего не значила бы).
    go_active::TREEHS_SPOT_A = 0x1234;
    go_active::TREEHS_SPOT_B = 0x1234;
    go_active::OREHS_MARKER = 0x1234;
    const double t1 = mono_seconds();
    for (;;) {
        g_mem.frame_begin();
        offset_resolver_tick();
        char st2[96] = {};
        offset_resolver_status(st2, sizeof(st2));
        if (strstr(st2, "готов")) break;
        if (mono_seconds() - t1 > 30.0) {
            printf("ПРОВАЛ: повторный проход не завершился (%s)\n", st2);
            kill(child, SIGKILL);
            return 1;
        }
        struct timespec ts { 0, 2 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    check(go_active::PLAYER_TEAM_NAME == 0x2A0, "после reset повторный резолв сошёлся");
    check(go_active::OREHS_MARKER == 0x50 && go_active::TREEHS_SPOT_A == 0x98 &&
          go_active::TREEHS_SPOT_B == 0xB4,
          "после reset поля по типу найдены заново");

    // ---- живые отметки переживают перескан (регрессия 27.09) ----------------
    // Раньше soft_rescan обнулял рабочий s_applied, а неудачный проход (откат
    // «хуже прежнего») его так и не восстанавливал: offset_resolver_is_live
    // молчал до конца сессии и гейтованные записи (быстрая стрельба и
    // перезарядка) умирали навсегда по одной качне резолвера. Стабильный
    // снимок s_committed обязан пережить и обнуление рабочего списка, и
    // откат прохода — пока жив тот же процесс и образ.
    check(offset_resolver_is_live(&go_active::TEAM_OBJECT_STATES),
          "после удачного резолва гейт записи жив (is_live)");
    check(s_committed_count == s_applied_count && s_committed_count > 0,
          "стабильный снимок взят при финализации прохода");
    soft_rescan();   // имитация старта повторного подбора на устройстве
    check(s_applied_count == 0,
          "перескан на старте чистит рабочий список (как и раньше)");
    check(s_committed_count > 0 &&
          offset_resolver_is_live(&go_active::TEAM_OBJECT_STATES),
          "живая отметка пересканом не гаснет — быстрая стрельба не умирает");
    // Итог отката «резов хуже прежнего» выглядит ровно так же: рабочий
    // список пуст, значения слотов — прежние. Отметка живёт именно потому,
    // что была вынесена в s_committed ДО прохода, а откатные ветки его не
    // касаются.
    offset_resolver_reset();
    check(s_committed_count == 0 &&
          !offset_resolver_is_live(&go_active::TEAM_OBJECT_STATES),
          "смена процесса/образа гейты обнуляет честно");

    // ---- кэш первого входа: второй вход без скана ---------------------------
    // Добавляем в конец кэша повреждённые нулевые значения для generic
    // Dictionary. Они не должны затирать ни текущую раскладку, ни baked fallback.
    const std::string cache_path = std::string(OR_CACHE_DIR) + ".offs";
    bool poisoned_dictionary_cache = false;
    if (dict_entries_idx >= 0 && dict_count_idx >= 0) {
        FILE* cache_file = fopen(cache_path.c_str(), "a");
        if (cache_file) {
            const int a = fprintf(cache_file, "res %d 0\n", dict_entries_idx);
            const int b = fprintf(cache_file, "res %d 0\n", dict_count_idx);
            const int close_status = fclose(cache_file);
            poisoned_dictionary_cache = a > 0 && b > 0 && close_status == 0;
        }
    }
    check(poisoned_dictionary_cache, "test cache seeded with invalid zero Dictionary offsets");

    // Сбрасываем состояние (как при перезаходе в игру) и портим слоты: кэш
    // обязан вернуть найденное мгновенно, без единого скана памяти.
    go_active::DICT_ENTRIES = 0x18;
    go_active::DICT_COUNT = 0x20;
    offset_resolver_reset();
    go_active::PLAYER_TEAM_NAME = 0x1234;
    go_active::FPMANAGER_AIM_BLEND = 0x1234;
    go_active::ITEM_DATA = 0x1234;
    go_active::EVENT_HANDLER_AIM_ACTIVITY = 0x1234;
    const double t2 = mono_seconds();
    char st3[128] = {};
    for (;;) {
        g_mem.frame_begin();
        offset_resolver_tick();
        offset_resolver_status(st3, sizeof(st3));
        if (strstr(st3, "готов")) break;
        if (mono_seconds() - t2 > 5.0) {
            printf("ПРОВАЛ: кэш первого входа не применился (%s)\n", st3);
            kill(child, SIGKILL);
            return 1;
        }
        struct timespec ts { 0, 1 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    check(strstr(st3, "кэш") != nullptr, "второй вход взят из кэша, без скана");
    check(mono_seconds() - t2 < 1.0, "из кэша — мгновенно (меньше секунды)");
    check(go_active::PLAYER_TEAM_NAME == 0x2A0, "кэш вернул teamName");
    check(go_active::FPMANAGER_AIM_BLEND == 0xB0, "кэш вернул поле, найденное по типу");
    check(go_active::ITEM_DATA == 0x30, "кэш вернул поле класса-типа");
    check(go_active::EVENT_HANDLER_AIM_ACTIVITY == 0x40,
          "кэш вернул via-поле обработчика прицела");
    check(go_active::DICT_ENTRIES == 0x28 && go_active::DICT_COUNT == 0x38,
          "cache rejects zero instance offsets for Dictionary fields");
    check(offset_resolver_class("PlayerManager", "Oxide") == h.pm_real ||
          offset_resolver_class("PlayerManager", "Oxide") == h.pm_decoy,
          "кэш вернул класс PlayerManager");
    check(offset_resolver_class_nth("PlayerManager", "Oxide", 0) == h.pm_decoy &&
          offset_resolver_class_nth("PlayerManager", "Oxide", 1) == h.pm_real,
          "кэш сохранил обе копии одноимённого класса");
    check(offset_resolver_class("NetworkClient", "Mirror") == h.netclient,
          "кэш вернул класс NetworkClient");

    // ---- чужой ключ: кэш не применяется ------------------------------------
    {
        const std::string cp = std::string(OR_CACHE_DIR) + ".offs";
        FILE* f = fopen(cp.c_str(), "r");
        if (f) {
            std::string txt;
            char chunk[512];
            for (;;) {
                const size_t n = fread(chunk, 1, sizeof(chunk), f);
                if (!n) break;
                txt.append(chunk, n);
            }
            fclose(f);
            const size_t kp = txt.find("key ");
            if (kp != std::string::npos) {
                txt.replace(kp, 4 + 16, "key 0000000000000000");
                f = fopen(cp.c_str(), "w");
                if (f) { fwrite(txt.data(), 1, txt.size(), f); fclose(f); }
            }
        }
        offset_resolver_reset();
        go_active::PLAYER_TEAM_NAME = 0x1234;
        char st4[128] = {};
        bool hit = false;
        const double t3 = mono_seconds();
        while (mono_seconds() - t3 < 20.0) {
            g_mem.frame_begin();
            offset_resolver_tick();
            offset_resolver_status(st4, sizeof(st4));
            if (strstr(st4, "кэш")) { hit = true; break; }   // кэш не должен сработать
            if (strstr(st4, "готов")) break;
            struct timespec ts { 0, 2 * 1000 * 1000 };
            nanosleep(&ts, NULL);
        }
        check(!hit, "кэш с чужим ключом (обновление игры) не применяется");
        check(go_active::PLAYER_TEAM_NAME == 0x2A0, "после чужего ключа резолв сошёлся заново");
    }

    printf(s_fails ? "СТЕНД ПРОВАЛЕН (%d из %d)\n" : "СТЕНД ПРОЙДЕН (%d проверок)\n",
           s_fails ? s_fails : s_checks, s_checks);
    kill(child, SIGKILL);
    waitpid(child, NULL, 0);
    return s_fails ? 1 : 0;
}
