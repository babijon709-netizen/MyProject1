// Съёмщик дампов памяти игры — ОТДЕЛЬНЫЙ бинарь, в сам чит не входит.
//
//   ./benzhack_dump [пакет|pid] [каталог]
//
// пакет   — что снимать (по умолчанию сначала бета, потом релиз);
//           числом — pid процесса, снимать его без поиска по имени;
// каталог — куда писать (по умолчанию /storage/emulated/0/Download).
//
// Зачем отдельным бинарём. Чит крутится в кадре игры: у него на каждый проход
// памяти — десятки миллисекунд, иначе картинка встаёт. Разбор же того, как в
// бете хранятся имена классов, требует полных проходов по гигабайтам и записи
// десятков мегабайт на диск — в кадре это не сделать, а в лог не влезет. Здесь
// ограничений нет: бинарь один раз работает минуту-две и складывает в «Загрузки»
// сырые куски памяти, которые потом разбираются на компьютере.
//
// Что снимается:
//   benzhack_blob.bin       — ПОЛНАЯ метадата процесса (global-metadata).
//                             Наш билд (6000.3.18f1) — v39: шапка 0x17C из
//                             ТРОЕК (offset, size, count) — 31 регион.
//                             Протектор игры XOR-ит всю шапку одним ключом
//                             и ПЕРЕМЕШИВАЕТ тройки, магию при этом затирает
//                             — поэтому простой поиск 0xFAB11BAF ничего не
//                             даёт (так было до этой переделки). Находится
//                             блоб четырьмя путями:
//                               1) магия на месте (старые/чистые сборки) —
//                                  проверка таблиц по версии;
//                               2) скан заголовков БЕЗ магии: версия чистая
//                                  или снимается ключом из слота магии, затем
//                                  приёмка по МОЗАИКЕ: расшифрованные тройки
//                                  отсортированы по offset плотно замостили
//                                  блоб от 0x17C (щели <= 16 байт), count<=size;
//                               3) маркерный путь: в строковой таблице всегда
//                                  есть «Assembly-CSharp.dll»/«mscorlib.dll» —
//                                  от метки идём назад до заголовка, чьи
//                                  таблицы НАКРЫВАЮТ её (работает при любой
//                                  порче шапки);
//                               4) якорный прогон у начала крупных регионов
//                                  (блоб — большое mmap-выделение).
//                             Ключ не перебираем: он выводится из контекста
//                             (магия, версия, первый offset). Выбранный блоб
//                             снимается целиком; если шапка была
//                             расшифрована — файл пишем с канонической шапкой
//                             (магия + версия + отсортированные тройки),
//                             готовой для Il2CppDumper. Случайные 4 байта
//                             этого не проходят: мозаика из 31 тройки на
//                             мусоре не сходится, а в секции строк кандидата
//                             должны лежать наши табличные имена
//                             (PlayerManager, TeamMember…). Если регион, в
//                             котором живёт блок, лишь немногим больше
//                             блока — снимаем до конца региона. В этом
//                             файле ВСЕ таблицы: строки, типы, методы, поля,
//                             параметры, AOT-секции — оффлайнеру достаточно
//                             его и libil2cpp.so;
//   benzhack_meta.bin       — (старый режим, для совместимости со старым
//                             оффлайнером) окно ±8 МБ вокруг строки
//                             «PlayerManager» — только фрагмент строк;
//   benzhack_ptrN.bin       — пул классов: ВЕСЬ размах указателей, ведущих в
//                             метадату, в трёх самых «густых» регионах (до
//                             64 МБ на регион; в старом режиме без шапки —
//                             256 КБ вокруг первого указателя);
//   benzhack_hiddenN.bin    — первые 2 МБ скрытых регионов (без прав чтения,
//                             но отдающихся принудительным чтением);
//   benzhack_imagedata.bin  — пишущиеся сегменты libil2cpp.so (.data.rel.ro,
//                             .data, .bss): в .bss лежат глобальные слоты
//                             Il2CppClass*, из которых виден адрес пула;
//   benzhack_dump_index.txt — опись: все регионы, счётчики указателей, адреса
//                             и размеры каждого снятого файла, строка «blob»
//                             — адрес/размер/версия полной метадаты.
//
// Собирается тем же NDK, что и чит (второй модуль в jni/Android.mk).

#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

// ---- Мелочи ----------------------------------------------------------------

static const char* kOutDir = "/storage/emulated/0/Download";
static const char* kPkgRelease = "com.catsbit.oxidesurvivalisland";
static const char* kPkgBeta = "com.catsbit.oxidesurvivalisland.beta";

static double now_s() {
    struct timespec ts = {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static FILE* g_log = nullptr;

static void say(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char* fmt, ...) {
    char body[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    fprintf(stdout, "%s\n", body);
    fflush(stdout);
    if (g_log) { fprintf(g_log, "%s\n", body); fflush(g_log); }
}

// Метка сверху указателя (TBI/PAC) нам не нужна.
static inline uint64_t untag(uint64_t v) { return v & 0x00FFFFFFFFFFFFFFull; }

// ---- Чтение памяти процесса -------------------------------------------------

static int g_mem_fd = -1;

static bool rd_at(uint64_t addr, void* out, size_t n) {
    if (g_mem_fd < 0) return false;
    uint8_t* p = (uint8_t*)out;
    size_t got = 0;
    while (got < n) {
        const ssize_t r = pread(g_mem_fd, p + got, n - got, (off_t)(addr + got));
        if (r <= 0) return false;
        got += (size_t)r;
    }
    return true;
}

// ---- Поиск процесса ---------------------------------------------------------

static int cmdline_rank(pid_t pid, const char* pkg) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    char command[256] = {};
    const ssize_t n = read(fd, command, sizeof(command) - 1);
    close(fd);
    if (n <= 0) return -1;
    if (strcmp(command, pkg) == 0) return 2;               // сам процесс пакета
    const size_t len = strlen(pkg);
    if (strncmp(command, pkg, len) == 0 && command[len] == ':') return 1;  // его сервис
    return -1;
}

static pid_t find_pid(const char* pkg, int min_rank) {
    DIR* d = opendir("/proc");
    if (!d) return -1;
    pid_t best = -1;
    int best_rank = 0;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        const pid_t pid = (pid_t)atoi(e->d_name);
        const int rank = cmdline_rank(pid, pkg);
        if (rank < min_rank) continue;
        if (rank > best_rank) { best_rank = rank; best = pid; }
    }
    closedir(d);
    return best;
}

// ---- Карта памяти -----------------------------------------------------------

enum RegKind { kImage = 0, kAnon = 1, kHidden = 2, kFile = 3 };

struct Reg {
    uint64_t start = 0, end = 0;
    char perms[8] = {};
    char path[96] = {};
    int kind = kAnon;
    uint64_t inwin = 0;              // сколько указателей в окно метадаты
    uint64_t first_inwin = 0;        // где первый такой указатель
    uint64_t last_inwin = 0;         // где последний — размах между ними и есть
                                     // плотная часть пула классов
};

// Процессы Unity в 2023+ могут давать карту длиннее 1024 строк — не роняем
// хвост (в нём как раз и живёт куча с метаданными).
static Reg g_regs[8192];
static int g_reg_count = 0;
static uint64_t g_lib_begin = 0, g_lib_end = 0;

// Читается ли регион, у которого снят бит чтения: /proc/<pid>/mem отдаёт чужие
// страницы с FOLL_FORCE, поэтому «прав нет» не значит «не прочитать». Три
// короткие пробы — начало, середина и четверть.
static bool probe_readable(uint64_t start, uint64_t end) {
    const uint64_t size = end - start;
    const uint64_t offs[3] = {0, size >> 1, size >> 2};
    uint8_t probe[128] = {};
    for (int t = 0; t < 3; ++t)
        if (rd_at(start + offs[t], probe, sizeof(probe))) {
            for (size_t k = 0; k < sizeof(probe); ++k)
                if (probe[k]) return true;      // не пусто — там данные
            return true;                        // читается, но нули — тоже читаемо
        }
    return false;
}

static void parse_maps(pid_t pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    FILE* f = fopen(path, "re");
    if (!f) { say("карта %s не открылась", path); return; }
    char line[1024];
    while (fgets(line, sizeof(line), f) && g_reg_count < (int)(sizeof(g_regs) / sizeof(g_regs[0]))) {
        unsigned long long start = 0, end = 0, foff = 0;
        char perms[8] = {};
        if (sscanf(line, "%llx-%llx %7s %llx", &start, &end, perms, &foff) < 3) continue;
        if (end <= start) continue;
        Reg& r = g_regs[g_reg_count];
        r.start = start;
        r.end = end;
        snprintf(r.perms, sizeof(r.perms), "%s", perms);
        const char* slash = strchr(line, '/');
        if (slash) {
            const size_t n = strlen(slash);
            snprintf(r.path, sizeof(r.path), "%s", slash + (n > sizeof(r.path) - 1 ? n - (sizeof(r.path) - 1) : 0));
        }
        const bool is_lib = (slash != nullptr) && (strstr(slash, "libil2cpp.so") != nullptr);
        if (is_lib) {
            if (strstr(line, "(deleted)")) continue;
            r.kind = kImage;
            if (start < g_lib_begin || !g_lib_begin) g_lib_begin = start;
            if (end > g_lib_end) g_lib_end = end;
        } else if (!slash) {
            if (perms[0] == 'r') {
                r.kind = (perms[2] == 'x') ? kFile : kAnon;
            } else if (probe_readable(start, end)) {
                r.kind = kHidden;                    // без прав чтения, но читается
            } else {
                continue;                            // резервация адресов, не считаем
            }
        } else {
            if (perms[0] != 'r') continue;
            if (strstr(slash, ".oat") || strstr(slash, ".dex") ||
                strstr(slash, ".vdex") || strstr(slash, ".art") ||
                strstr(slash, ".apk")) continue;     // байт-код, не структуры
            r.kind = kFile;
        }
        ++g_reg_count;
    }
    fclose(f);
    // Порядок скана: сначала скрытые и ПЕРЕЗАПИСЫВАЕМЫЕ анонимные области
    // (метадата почти всегда там), потом файловые, в конце read-only
    // анонимные и «(deleted)». Даже упираясь в потолок скана, к блобу
    // попадаем быстро.
    {
        int score[8192];
        for (int i = 0; i < g_reg_count; ++i) {
            const Reg& r = g_regs[i];
            bool deleted = false;
            for (const char* q = r.path; *q;)
                if (!strncmp(q, " (deleted)", 10)) { deleted = true; break; }
                else ++q;
            const bool writable = r.perms[1] == 'w';
            if (r.kind == kHidden) score[i] = 0;
            else if (deleted) score[i] = 5;
            else if (r.path[0] == 0) score[i] = writable ? 1 : 4;  // анонимные
            else score[i] = writable ? 2 : 3;                      // файловые
        }
        int order[8192];
        int n = 0;
        for (int s = 0; s < 6; ++s)
            for (int i = 0; i < g_reg_count; ++i)
                if (score[i] == s) order[n++] = i;
        for (int i = 0; i < g_reg_count; ++i) g_regs[i] = g_regs[order[i]];
    }
}

// ---- Имена классов, которые ищем --------------------------------------------
// Список — из таблицы резолвера (jni/src/offset_resolver_table.inc): по нему
// выбирается блок строк (тот, где таких имён больше) и оценивается дамп.

static const char* const kNames[] = {
    "BodyPart", "CameraManager", "CharacterAnimation", "CycleParameters",
    "DelayIgnoreTimeScalePromise", "DelayPromise", "Delegate", "FPManager",
    "FPMelee", "FPMotion", "FPObject", "FPTool", "FPWeaponBase",
    "GameControllerBase", "HitBox", "HitMarkerItem", "Item", "ItemData",
    "ItemPickup", "KCC", "LootItem", "LootObject", "MineableObject",
    "MulticastDelegate", "NetworkClient", "NetworkIdentity", "OreHitstreaks",
    "OreHitstreaksMarker", "PlayerInventory", "PlayerLoopHelper",
    "PlayerLoopRunner", "PlayerManager", "PlayerModelInfo", "PlayerWeapon",
    "Ragdoll", "RaycastHit", "RaycastManager", "TeamMember", "Text",
    "TransformOffset", "TreeHitstreaks", "VoicePlayerState", "WeaponPiece",
};
static const int kNameCount = (int)(sizeof(kNames) / sizeof(kNames[0]));

// ---- Шапка глобальной метадаты (RVM) ----------------------------------------
// global-metadata в памяти процесса начинается с шапки:
//   u32 магия 0xFAB11BAF, i32 версия, затем пары (u32 смещение, u32 размер)
//   — по одной на каждую секцию (строки, типы, методы, поля, AOT-таблицы…).
// Порядок и набор пар зависит от версии (раскладка по Il2CppDumper,
// поддерживаются 16–43; наш билд 6000.3.18f1 — v39, там 31 пара / 256 байт).
// Отсюда полный размер блока: max(смещение + размер) по всем парам.

static const uint32_t kRvmMagic = 0xFAB11BAF;

static uint32_t rd32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

// Сколько пар (смещение, размер) несёт шапка версии v. Порядок в файле:
// базовые 22 (stringLiteral…assemblies), затем — по условиям версии:
// rgctxEntries (<=24), metadataUsageLists+Pairs (19..24), fieldRefs (>=19),
// referencedAssemblies (>=20), attributesInfo+Types (21..27),
// attributeData+Range (>=29), unresolvedVirtualCall* (>=22),
// windowsRuntimeTypeNames (>=23), windowsRuntimeStrings (>=27),
// exportedTypeDefinitions (>=24). Для v39 получается 31 пара (256 байт).
static int rvm_pairs(int v) {
    int n = 22;
    if (v <= 24) ++n;
    if (v >= 19 && v <= 24) n += 2;
    if (v >= 19) ++n;
    if (v >= 20) ++n;
    if (v >= 21 && v <= 27) n += 2;
    if (v >= 29) n += 2;
    if (v >= 22) n += 2;
    if (v >= 23) ++n;
    if (v >= 27) ++n;
    if (v >= 24) ++n;
    return n;
}

struct BlobInfo {
    bool valid = false;
    int version = 0;
    int pairs = 0;
    uint64_t extent = 0;      // до какого смещения живут все секции
    uint64_t string_off = 0;  // секция строк метаданных (пара №3)
    uint64_t string_size = 0;
};

// Читаем и проверяем шапку по адресу at. Проверка строгая: таблицы должны
// идти по возрастанию смещений и накрывать саму шапку — случайные четыре
// байта 0xFAB11BAF это не пройдут.
static bool parse_blob(uint64_t at, BlobInfo& b) {
    uint8_t head[8];
    if (!rd_at(at, head, 8)) return false;
    if (rd32(head) != kRvmMagic) return false;
    const int version = (int)rd32(head + 4);
    if (version < 16 || version > 43) return false;
    const int n = rvm_pairs(version);
    const size_t need = 8 + (size_t)n * 8;
    static uint8_t tbl[8 + 34 * 8];
    if (need > sizeof(tbl) || !rd_at(at, tbl, need)) return false;
    uint64_t extent = 0;
    uint32_t prev_off = 0;
    for (int i = 0; i < n; ++i) {
        const uint32_t off = rd32(tbl + 8 + (size_t)i * 8);
        const uint32_t sz = rd32(tbl + 8 + (size_t)i * 8 + 4);
        if (!sz) continue;                            // пустая секция — не проверяем
        if (off < prev_off) return false;             // таблицы идут по порядку
        prev_off = off;
        if ((uint64_t)off + sz > extent) extent = (uint64_t)off + sz;
    }
    if (extent < need || extent >= (4ull << 30)) return false;
    b.valid = true;
    b.version = version;
    b.pairs = n;
    b.extent = extent;
    b.string_off = rd32(tbl + 8 + 2 * 8);
    b.string_size = rd32(tbl + 8 + 2 * 8 + 4);
    return true;
}

// Сколько наших табличных имён (kNames) лежит в секции строк кандидата.
// У настоящей метадаты — десяток и больше (пересчитали 16 МБ секции, этого
// достаточно, чтобы отличить пул строк от мусора); у ложной шапки — ноль.
static int count_names_in(uint64_t addr, uint64_t size) {
    const uint64_t cap = 16ull << 20;
    if (size > cap) size = cap;
    static uint8_t cbuf[1024 * 1024];
    bool found[kNameCount] = {};
    int total = 0;
    uint64_t at = 0;
    while (at < size) {
        const uint64_t remain = size - at;
        const size_t want = (remain > sizeof(cbuf)) ? sizeof(cbuf) : (size_t)remain;
        if (!rd_at(addr + at, cbuf, want)) break;
        for (int n = 0; n < kNameCount; ++n) {
            if (found[n]) continue;
            if (memmem(cbuf, want, kNames[n], strlen(kNames[n]) + 1) != nullptr) {
                found[n] = true;
                ++total;
            }
        }
        if (total == kNameCount) break;
        at += want;
    }
    return total;
}

// ---- v38+: тройки (offset, size, count) и схема протектора ------------------
// С v38 (Unity 2022+/6000.x) шапка метадаты — не пары, а ТРОЙКИ: 31 регион =
// 93 u32, шапка 0x17C. Протектор этой игры XOR-ит всю шапку ОДНИМ ключом
// (магию и версию вместе с таблицей) и ПЕРЕМЕШИВАЕТ тройки. Проверка —
// «мозаикой»: расшифрованные тройки, отсортированные по offset, обязаны
// плотно замостить блоб от 0x17C (щели выравнивания <= 16 байт), у каждой
// непустой тройки count>0 и count<=size. Такое совпадение на мусоре
// практически невозможно. Ключ не перебираем: выводится из контекста.

static const uint64_t kMinBlobSize = 1ull << 20;   // реальная метадата >= 1 МБ
static const int kV38MinVer = 38, kV38MaxVer = 110;
static const int kV38Triples = 31;
static const uint64_t kV38HeaderSize = 8 + 93 * 4; // 0x17C

struct V38Match {
    uint32_t version = 0;
    uint32_t key = 0;           // 0 = таблица чистая
    uint64_t size = 0;          // ТОЧНЫЙ размер блоба = конец мозаики
    uint32_t sorted[93] = {};   // тройки по возрастанию offset
};

// Мозаика: f — 93 расшифрованных u32; avail — доступные байты от кандидата
// до конца региона (в памяти размер файла неизвестен — он и есть результат).
static bool v38_tile(const uint32_t* f, uint64_t avail, uint64_t* size_out,
                     uint32_t* sorted_out) {
    struct T { uint32_t o, s, c; } g[kV38Triples];
    for (int i = 0; i < kV38Triples; ++i) {
        g[i].o = f[3 * i]; g[i].s = f[3 * i + 1]; g[i].c = f[3 * i + 2];
        if (g[i].s) {
            if (g[i].c == 0 || g[i].c > g[i].s) return false;
            if ((uint64_t)g[i].o + g[i].s > avail) return false;
        }
    }
    for (int i = 1; i < kV38Triples; ++i) {   // вставки, 31 элемент
        const T t = g[i];
        int j = i - 1;
        while (j >= 0 && g[j].o > t.o) { g[j + 1] = g[j]; --j; }
        g[j + 1] = t;
    }
    uint64_t pos = kV38HeaderSize;
    for (int i = 0; i < kV38Triples; ++i) {
        if (g[i].o < pos) return false;               // регион до шапки
        if ((uint64_t)g[i].o - pos > 16) return false; // дыра в мозаике
        pos = ((uint64_t)g[i].o + g[i].s + 3) & ~3ull;
    }
    if (pos < kMinBlobSize || pos > avail) return false;
    *size_out = pos;
    if (sorted_out)
        for (int i = 0; i < kV38Triples; ++i) {
            sorted_out[3 * i] = g[i].o;
            sorted_out[3 * i + 1] = g[i].s;
            sorted_out[3 * i + 2] = g[i].c;
        }
    return true;
}

// Кандидаты ключа для позиции шапки. w0/w1 — первые два u32 (слоты магии и
// версии), t — 93 u32 таблицы.
static int v38_key_candidates(uint32_t w0, uint32_t w1, const uint32_t* t,
                              uint32_t* out, int cap) {
    int n = 0;
    {   // вся шапка XOR одним ключом: ключ = магия на месте XOR настоящая
        const uint32_t k = w0 ^ kRvmMagic;
        if (k && n < cap) out[n++] = k;
    }
    for (int v = kV38MinVer; v <= kV38MaxVer; ++v) {  // версия XOR-нута тем же
        const uint32_t k = w1 ^ (uint32_t)v;
        if (!k) continue;
        bool seen = false;
        for (int i = 0; i < n; ++i) if (out[i] == k) seen = true;
        if (!seen && n < cap) out[n++] = k;
    }
    if (w1 >= kV38MinVer && w1 <= kV38MaxVer) {
        // версия чистая, магия затёрта, таблица XOR-нута: первый регион
        // начинается на 0x17C -> t[j] ^ 0x17C = ключ
        for (int j = 0; j < 93; ++j) {
            const uint32_t k = t[j] ^ (uint32_t)kV38HeaderSize;
            if (!k) continue;
            bool seen = false;
            for (int i = 0; i < n; ++i) if (out[i] == k) seen = true;
            if (!seen && n < cap) out[n++] = k;
        }
    }
    return n;
}

// Полная проверка v38+ шапки по буферу hdr (нужно >= 0x17C байт).
// must_cover_rel — смещение строки-метки от начала кандидата: метка обязана
// попасть внутрь одного из регионов мозаики (~0 — не проверяем).
static bool v38_try(const uint8_t* hdr, uint64_t avail, uint64_t must_cover_rel,
                    V38Match* out) {
    if (avail < kV38HeaderSize) return false;
    uint32_t w0 = 0, w1 = 0;
    memcpy(&w0, hdr, 4);
    memcpy(&w1, hdr + 4, 4);
    uint32_t t[93];
    memcpy(t, hdr + 8, sizeof(t));
    uint32_t f[93];
    V38Match m;
    auto attempt = [&](uint32_t k, uint32_t ver) -> bool {
        for (int j = 0; j < 93; ++j) f[j] = t[j] ^ k;
        if (!v38_tile(f, avail, &m.size, m.sorted)) return false;
        if (must_cover_rel != ~0ull) {
            bool covered = false;
            for (int i = 0; i < kV38Triples; ++i)
                if (must_cover_rel >= m.sorted[3 * i] &&
                    must_cover_rel < (uint64_t)m.sorted[3 * i] + m.sorted[3 * i + 1]) {
                    covered = true;
                    break;
                }
            if (!covered) return false;
        }
        m.version = ver;
        m.key = k;
        return true;
    };
    if (w0 == kRvmMagic && w1 >= kV38MinVer && w1 <= kV38MaxVer)
        if (attempt(0, w1)) { *out = m; return true; }
    uint32_t keys[128];
    const int nk = v38_key_candidates(w0, w1, t, keys, 128);
    for (int ki = 0; ki < nk; ++ki) {
        const uint32_t k = keys[ki];
        const uint32_t ve = w1 ^ k;             // версия расшифрованная
        if (ve >= kV38MinVer && ve <= kV38MaxVer && attempt(k, ve)) {
            *out = m;
            return true;
        }
        if (w1 >= kV38MinVer && w1 <= kV38MaxVer && attempt(k, w1)) {
            *out = m;                           // версия была чистой
            return true;
        }
    }
    return false;
}

// Каноническая шапка для выгрузки: магия, версия, тройки по возрастанию.
static void v38_canonical(const V38Match& m, uint8_t* out) {
    memset(out, 0, kV38HeaderSize);
    const uint32_t magic = kRvmMagic;
    memcpy(out, &magic, 4);
    memcpy(out + 4, &m.version, 4);
    memcpy(out + 8, m.sorted, 93 * 4);
}

// Чтение с терпимостью к дырам: нечитаемая страница не роняет чтение —
// протектор закрывает страницу-шапку, а тело оставляет читаемым. Чтение
// дробится до 4 КБ, дыры нулями, счётчик потерь.
static void read_holes(uint64_t at, uint8_t* buf, size_t want, uint64_t* holes) {
    if (rd_at(at, buf, want)) return;
    if (want <= 4096) { memset(buf, 0, want); *holes += want; return; }
    const size_t half = want / 2;
    read_holes(at, buf, half, holes);
    read_holes(at + half, buf + half, want - half, holes);
}

// Строковые метки для поиска блоба с порванной шапкой: имена образов из
// строковой таблицы метадаты — есть в любой игре. ASCII: управляемая куча
// хранит строки UTF-16, ложных попаданий почти не даёт.
static const char* const kMarkers[] = { "Assembly-CSharp.dll", "mscorlib.dll" };
static const int kMarkerCount = 2;

// ---- Кандидаты блоба (все пути) ---------------------------------------------

struct Cand {
    uint64_t addr = 0, extent = 0;
    int version = 0;
    bool v38 = false;
    uint32_t key = 0;               // XOR-ключ (0 = шапка чистая)
    uint64_t string_off = 0, string_size = 0;
    uint32_t sorted[93] = {};       // v38: тройки по возрастанию
    int names = -1;
    int covers = 0;
    int fit = 0;
    const char* via = "";           // "magic"/"scan"/"marker"/"anchor"
};
static Cand g_cands[16];
static int g_cand_count = 0;

static bool cand_inside(uint64_t addr) {
    for (int i = 0; i < g_cand_count; ++i)
        if (addr >= g_cands[i].addr && addr < g_cands[i].addr + g_cands[i].extent)
            return true;
    return false;
}

static void add_cand(uint64_t addr, uint64_t extent, int version, bool is_v38,
                     uint32_t key, uint64_t string_off, uint64_t string_size,
                     const uint32_t* sorted, const char* via) {
    if (cand_inside(addr) || g_cand_count >= (int)(sizeof(g_cands) / sizeof(g_cands[0])))
        return;
    Cand& c = g_cands[g_cand_count++];
    c.addr = addr; c.extent = extent; c.version = version; c.v38 = is_v38;
    c.key = key; c.string_off = string_off; c.string_size = string_size;
    c.via = via;
    if (is_v38 && sorted) memcpy(c.sorted, sorted, sizeof(c.sorted));
    say("  ++ кандидат v%d %s: 0x%llx, %.1f МБ%s", version, via,
        (unsigned long long)addr, (double)extent / (1024.0 * 1024.0),
        key ? ", шапка XOR-нута — восстановлю" : "");
}

// ---- Запись дампов ----------------------------------------------------------

static uint64_t g_written = 0;
static int g_tops[3] = {-1, -1, -1};
// Полная метадата игры — это сотни мегабайт (у нас — под гигабайт таблицами),
// поэтому общий потолок большой; per_cap каждого файла — ниже.
static const uint64_t kWriteCap = 8ull << 30;    // не более 8 ГБ на весь дамп

static void dump_range(const char* out_dir, const char* name, uint64_t addr,
                       uint64_t size, uint64_t per_cap) {
    if (!size || g_written >= kWriteCap) return;
    if (size > per_cap) size = per_cap;
    if (g_written + size > kWriteCap) size = kWriteCap - g_written;
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", out_dir, name);
    FILE* out = fopen(path, "wb");
    if (!out) { say("  не открылся %s", path); return; }
    static uint8_t buf[256 * 1024];
    uint64_t left = size, at = addr;
    while (left > 0) {
        const size_t want = (left > sizeof(buf)) ? sizeof(buf) : (size_t)left;
        if (!rd_at(at, buf, want)) break;
        if (fwrite(buf, 1, want, out) != want) break;
        at += want;
        left -= want;
    }
    fclose(out);
    g_written += size - left;
    say("  %s — 0x%llx..0x%llx (%llu МБ)", name, (unsigned long long)addr,
        (unsigned long long)(addr + size - left), (unsigned long long)((size - left) >> 20));
}

// Магия (4 байта) может найтись дважды из-за 12-байтного пересечения чанков:
// адрес уже учтён?
static bool magic_dup(const uint64_t* hits, int count, uint64_t at) {
    for (int i = 0; i < count; ++i)
        if (hits[i] == at) return true;
    return false;
}

// Регион, содержащий addr, и его непрерывное соседство (mmap метаданных мог
// расколоть mprotect на 2-3 региона). Список регионов — в порядке ПОИСКА
// (по приоритету), не по адресам, поэтому соседей ищем по адресам.
static void region_span(uint64_t addr, uint64_t* rlo, uint64_t* rhi) {
    *rlo = 0;
    *rhi = 0;
    int ri = -1;
    for (int i = 0; i < g_reg_count; ++i)
        if (addr >= g_regs[i].start && addr < g_regs[i].end) { ri = i; break; }
    if (ri < 0) return;
    *rlo = g_regs[ri].start;
    *rhi = g_regs[ri].end;
    for (int it = 0; it < 4; ++it) {
        bool grew = false;
        for (int i = 0; i < g_reg_count; ++i)
            if (g_regs[i].start == *rhi) { *rhi = g_regs[i].end; grew = true; break; }
        if (!grew) break;
    }
}

// Дамп полной метадаты. Если шапка была расшифрована (v38+ c XOR-нутыми
// тройками) — сначала пишем КАНОНИЧЕСКУЮ шапку (магия + версия +
// отсортированные тройки): файл сразу берёт Il2CppDumper. Тело — как есть
// (шифрована только шапка). Нечитаемые страницы — нулями (файл не
// «съезжает»), потери считаются; частичная выгрузка переименовывается в
// *.partial и честно пишется в лог.
static void dump_blob_full(const char* out_dir, uint64_t addr, uint64_t size,
                           int version, bool v38, const uint32_t* sorted) {
    if (!size || g_written >= kWriteCap) return;
    if (size > (2ull << 30)) size = 2ull << 30;
    if (g_written + size > kWriteCap) size = kWriteCap - g_written;
    char path[256];
    snprintf(path, sizeof(path), "%s/benzhack_blob.bin", out_dir);
    FILE* f = fopen(path, "wb");
    if (!f) {
        say("  benzhack_blob.bin: не открыть на запись: %s", strerror(errno));
        return;
    }
    static uint8_t wbuf[1024 * 1024];
    uint64_t done = 0, failed = 0, body_pos = 0;
    // Тело в памяти начинается ПОСЛЕ сырой шапки: в v38+ её мы заменили
    // канонической, поэтому читаем от addr + 0x17C (иначе в файл попадёт
    // и сырая, и каноническая шапка, и всё тело съедет на 0x17C).
    const uint64_t body_start = v38 ? kV38HeaderSize : 0;
    if (v38) {
        uint8_t canon[0x210];
        V38Match m = {};
        m.version = (uint32_t)version;
        memcpy(m.sorted, sorted, 93 * 4);
        v38_canonical(m, canon);
        if (fwrite(canon, 1, (size_t)kV38HeaderSize, f) != kV38HeaderSize) {
            fclose(f);
            say("  benzhack_blob.bin: не дописал каноническую шапку");
            return;
        }
        done = kV38HeaderSize;
        say("  benzhack_blob.bin: шапка восстановлена (магия/версия выписаны, "
            "тройки отсортированы) — файл канонический v%d", version);
    }
    while (done < size) {
        const uint64_t left = size - done;
        const size_t want = (left > sizeof(wbuf)) ? sizeof(wbuf) : (size_t)left;
        uint64_t holes = 0;
        read_holes(addr + body_start + body_pos, wbuf, want, &holes);
        failed += holes;
        if (fwrite(wbuf, 1, want, f) != want) break;
        done += want;
        body_pos += want;
        g_written += want;
    }
    fclose(f);
    if (failed >= size || body_pos < size - body_start) {
        remove(path);
        say("  benzhack_blob.bin: страницы метаданных не читаются — файл не сохранён");
        g_written -= body_pos;
        return;
    }
    say("  benzhack_blob.bin — 0x%llx..0x%llx (%llu МБ, версия %d)",
        (unsigned long long)addr, (unsigned long long)(addr + size),
        (unsigned long long)(size >> 20), version);
    if (failed) {
        char partial[280];
        snprintf(partial, sizeof(partial), "%s.partial", path);
        rename(path, partial);
        say("  benzhack_blob.bin: не прочитано %.1f МБ (нули) — файл сохранён "
            "как benzhack_blob.bin.partial", (double)failed / (1024.0 * 1024.0));
    }
}

// ---- main -------------------------------------------------------------------

int main(int argc, char** argv) {
    const char* out_dir = (argc > 2) ? argv[2] : kOutDir;
    const char* pkg = (argc > 1) ? argv[1] : nullptr;

    char log_path[256];
    snprintf(log_path, sizeof(log_path), "%s/benzhack_dump.log", out_dir);
    g_log = fopen(log_path, "w");

    say("дамп памяти: выход %s", out_dir);

    // 1. Процесс игры. Числовой аргумент — pid напрямую (и для теста на
    //    хосте, и если процесс игры запущен под непривычным именем).
    pid_t pid = -1;
    if (pkg && pkg[0] >= '0' && pkg[0] <= '9') {
        bool digits = true;
        for (const char* c = pkg; *c; ++c)
            if (*c < '0' || *c > '9') { digits = false; break; }
        if (digits) {
            pid = (pid_t)atoi(pkg);
            say("pid по аргументу: %d", (int)pid);
        }
    }
    if (pid < 0) {
        const char* pkgs[2] = {pkg ? pkg : kPkgBeta, pkg ? pkg : kPkgRelease};
        for (int i = 0; i < 2 && pid < 0; ++i) {
            if (!pkgs[i]) continue;
            pid = find_pid(pkgs[i], 1);
            if (pid > 0) say("процесс %s — pid %d", pkgs[i], (int)pid);
        }
    }
    if (pid < 0) { say("процесс игры не найден"); return 1; }

    // 2. Память процесса.
    char mem_path[64];
    snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem", (int)pid);
    g_mem_fd = open(mem_path, O_RDONLY | O_CLOEXEC);
    if (g_mem_fd < 0) { say("%s не открылась (errno %d) — нет доступа", mem_path, errno); return 1; }

    // 3. Карта.
    parse_maps(pid);
    say("регионов в работе: %d, образ libil2cpp.so 0x%llx..0x%llx", g_reg_count,
        (unsigned long long)g_lib_begin, (unsigned long long)g_lib_end);
    int n_hidden = 0;
    for (int i = 0; i < g_reg_count; ++i) if (g_regs[i].kind == kHidden) ++n_hidden;
    say("из них скрытых (без прав чтения, но читаемых): %d", n_hidden);

    // 4. Один проход по всей читаемой памяти с несколькими иголками:
    //    - строка «PlayerManager» — для старого окна ±8 МБ;
    //    - магия 0xFAB11BAF — чистые шапки (старые сборки);
    //    - v38+ шапки БЕЗ магии (протектор её затирает): дешёвый затвор по
    //      версии + приёмка мозаикой троек;
    //    - строковые метки «Assembly-CSharp.dll»/«mscorlib.dll» — маркерный
    //      путь при любой порче шапки.
    //    Чанки пересекаются на 12 байт — ни одна иголка на стыке не потеряется.
    static const char kNeedle[] = "PlayerManager";
    const size_t kNeed = sizeof(kNeedle);
    static const uint8_t kMagic[4] = {0xAF, 0x1B, 0xB1, 0xFA};   // LE 0xFAB11BAF
    static uint8_t buf[1024 * 1024];
    const uint64_t kScanCap = 8ull << 30;
    uint64_t scanned = 0;
    uint64_t hits[16];
    int hit_count = 0;
    uint64_t magic_hits[8];
    int magic_count = 0;
    struct MarkerHit { uint64_t addr; int idx; };
    MarkerHit marker_hits[64];
    int marker_count = 0;
    uint64_t last_marker_addr = ~0ull;
    static int s_v38_rej = 0, s_scan_rej = 0;
    const double t0 = now_s();

    // Магия найдена по адресу mfound: v38+ — тройки/мозаика сразу, старые
    // версии — в очередь magic_hits (таблицы проверяет parse_blob в 5b).
    auto try_magic = [&](uint64_t mfound, const Reg& r, int ridx) {
        if (cand_inside(mfound)) return;
        uint8_t hdr[0x210];
        const uint64_t rem = r.end - mfound;
        const size_t take = (rem > sizeof(hdr)) ? sizeof(hdr) : (size_t)rem;
        if (take < 0x48) return;
        uint64_t holes = 0;
        read_holes(mfound, hdr, take, &holes);
        uint32_t ver = 0;
        memcpy(&ver, hdr + 4, 4);
        if (ver >= kV38MinVer && ver <= kV38MaxVer) {
            V38Match m;
            if (v38_try(hdr, rem, ~0ull, &m))
                add_cand(mfound, m.size, (int)m.version, true, m.key,
                         m.sorted[6], m.sorted[7], m.sorted, "magic");
            else if (s_v38_rej < 30) {
                ++s_v38_rej;
                say("   ?? магия @0x%llx: v%u — мозаика троек не сходится%s",
                    (unsigned long long)mfound, (unsigned)ver,
                    s_v38_rej == 30 ? " (дальше отвергнутые не пишу)" : "");
            }
            return;
        }
        if (magic_count < 8 && !magic_dup(magic_hits, magic_count, mfound)) {
            magic_hits[magic_count] = mfound;
            ++magic_count;
            say("  шапка метадаты (0xFAB11BAF) найдена @0x%llx (регион %d, %s)",
                (unsigned long long)mfound, ridx, r.path);
        }
    };

    // v38+ заголовки БЕЗ магии: затвор дешёвый (версия чистая либо снимается
    // ключом из слота магии), тяжёлая мозаика — только у прошедших.
    auto scan_v38 = [&](const uint8_t* lo, const uint8_t* hi, uint64_t base,
                        const Reg& r) {
        const uint8_t* b = buf;
        for (const uint8_t* p = lo; p + kV38HeaderSize <= hi && g_cand_count < 16; p += 4) {
            uint32_t w0 = 0, w1 = 0;
            memcpy(&w0, p, 4);
            memcpy(&w1, p + 4, 4);
            const uint32_t km = w0 ^ kRvmMagic;
            if (!((w1 >= kV38MinVer && w1 <= kV38MaxVer) ||
                  (km && (w1 ^ km) >= kV38MinVer && (w1 ^ km) <= kV38MaxVer)))
                continue;
            const uint64_t abs = base + (uint64_t)(p - b);
            if (cand_inside(abs)) continue;
            V38Match m;
            if (!v38_try(p, r.end - abs, ~0ull, &m)) {
                if (s_scan_rej < 10) {
                    ++s_scan_rej;
                    say("   ?? заголовок без магии 0x%llx — мозаика v38+ не сошлась%s",
                        (unsigned long long)abs,
                        s_scan_rej == 10 ? " (дальше не пишу)" : "");
                }
                continue;
            }
            add_cand(abs, m.size, (int)m.version, true, m.key,
                     m.sorted[6], m.sorted[7], m.sorted, "scan");
        }
    };

    // Строковые метки: адрес + номер. Повтор на стыке чанков отсекается
    // последним адресом (адреса в скане монотонны).
    auto scan_markers = [&](const uint8_t* lo, const uint8_t* hi, uint64_t base,
                            const Reg& r, int ridx) {
        const uint8_t* b = buf;
        for (int mi = 0; mi < kMarkerCount && marker_count < 64; ++mi) {
            const size_t ml = strlen(kMarkers[mi]);
            const uint8_t* search = lo;
            while (search + ml <= hi && marker_count < 64) {
                const uint8_t* mh = (const uint8_t*)memmem(search, (size_t)(hi - search),
                                                           kMarkers[mi], ml);
                if (!mh) break;
                const uint64_t abs = base + (uint64_t)(mh - b);
                if (abs != last_marker_addr) {
                    marker_hits[marker_count].addr = abs;
                    marker_hits[marker_count].idx = mi;
                    ++marker_count;
                    last_marker_addr = abs;
                    say("  метка «%s» @0x%llx (регион %d, %s)", kMarkers[mi],
                        (unsigned long long)abs, ridx, r.path);
                }
                search = mh + 1;
            }
        }
    };

    for (int i = 0; i < g_reg_count &&
             (hit_count < 16 || marker_count < 64 || g_cand_count < 16) &&
             scanned < kScanCap; ++i) {
        const Reg& r = g_regs[i];
        int fails = 0;
        for (uint64_t at = r.start; at + kNeed <= r.end && scanned < kScanCap;) {
            const uint64_t remain = r.end - at;
            size_t want = (remain > sizeof(buf)) ? sizeof(buf) : (size_t)remain;
            if (!rd_at(at, buf, want)) {
                // 16 МБ подряд не читается — дальше в этом регионе и не будет:
                // без лимита на «провалы» такой регион способен крутить цикл
                // минутами (scanned считает только успешные чтения).
                if (++fails > 4096) {
                    say("  регион %d (0x%llx..0x%llx) не читается — пропускаю остаток",
                        i, (unsigned long long)r.start, (unsigned long long)r.end);
                    break;
                }
                at += 4096;
                continue;
            }
            fails = 0;
            scanned += want;
            const uint8_t* hit = (const uint8_t*)memmem(buf, want, kNeedle, kNeed);
            const uint8_t* pre_stop = hit ? hit : (buf + want);
            // Всё, что до строки (или весь чанк, если строки нет): магия,
            // v38+ без магии, метки — иначе строка «съест» эту часть чанка.
            if (g_cand_count < 16) {
                uint8_t* search = buf;
                while (search + 4 <= pre_stop) {
                    uint8_t* mh = (uint8_t*)memmem(search, (size_t)(pre_stop - search),
                                                   kMagic, 4);
                    if (!mh) break;
                    try_magic(at + (uint64_t)(mh - buf), r, i);
                    search = mh + 4;
                }
                scan_v38(buf, pre_stop, at, r);
                scan_markers(buf, pre_stop, at, r, i);
            }
            if (hit) {
                const uint64_t found = at + (uint64_t)(hit - buf);
                if (hit_count < 16) {
                    hits[hit_count] = found;
                    ++hit_count;
                    say("  строка найдена @0x%llx (регион %d, %s)",
                        (unsigned long long)found, i, r.path);
                }
                at = found + kNeed;
                continue;
            }
            // Чанки пересекаются на 12 байт — иголки на стыке найдутся
            // целиком в следующем; повторы отсекаются cand_inside/адресом.
            at += want - (kNeed - 1);
        }
    }
    say("просмотрено %llu МБ за %.1f с, строк: %d, шапок RVM: %d, меток: %d, "
        "кандидатов: %d",
        (unsigned long long)(scanned >> 20), now_s() - t0, hit_count,
        magic_count, marker_count, g_cand_count);

    // 4b. Якорный прогон v38+ в начале крупных регионов: блоб метадаты —
    //     большое mmap-выделение и почти всегда начинается у начала области.
    //     Здесь допустим самый тяжёлый случай (версия XOR-нута вместе с
    //     магией) — полный вывод ключей из контекста на первых килобайтах
    //     дёшев.
    for (int i = 0; i < g_reg_count && g_cand_count < 16; ++i) {
        const Reg& r = g_regs[i];
        const uint64_t size = r.end - r.start;
        if (size < (8ull << 20)) continue;
        static uint8_t abuf[0x1000 + kV38HeaderSize];
        const size_t take = (size < sizeof(abuf)) ? (size_t)size : sizeof(abuf);
        uint64_t holes = 0;
        read_holes(r.start, abuf, take, &holes);
        for (size_t o = 0; o + kV38HeaderSize <= take && g_cand_count < 16; o += 4) {
            const uint64_t at = r.start + o;
            if (cand_inside(at)) continue;
            V38Match m;
            if (!v38_try(abuf + o, r.end - at, ~0ull, &m)) continue;
            add_cand(at, m.size, (int)m.version, true, m.key,
                     m.sorted[6], m.sorted[7], m.sorted, "anchor");
        }
    }

    // 4c. Маркерный путь: от строки-метки («Assembly-CSharp.dll» из
    //     строковой таблицы метадаты) идём назад в поисках заголовка, чьи
    //     таблицы НАКРЫВАЮТ эту строку. Работает даже когда протектор
    //     затёр или зашифровал магию — реальная шапка печатается в лог.
    static int s_giveups = 0;
    for (int mh_i = 0; mh_i < marker_count && s_giveups < 64; ++mh_i) {
        const MarkerHit& hit = marker_hits[mh_i];
        bool inside = false;
        for (int c = 0; c < g_cand_count; ++c)
            if (hit.addr >= g_cands[c].addr && hit.addr < g_cands[c].addr + g_cands[c].extent)
                inside = true;
        if (inside) continue;                       // метка внутри найденного блоба
        // Регион, в котором лежит метка.
        int ri = -1;
        for (int i = 0; i < g_reg_count; ++i)
            if (hit.addr >= g_regs[i].start && hit.addr < g_regs[i].end) { ri = i; break; }
        if (ri < 0) { ++s_giveups; continue; }
        const Reg& region = g_regs[ri];
        // Окно 1 МБ с запасом 0x210: кандидаты у верхней границы читают
        // шапку, выходящую за окно (win_hi..win_hi+0x210).
        static uint8_t win[(1u << 20) + 0x210];
        const uint64_t kWin = 1u << 20;
        const uint64_t room = hit.addr - region.start;
        const uint64_t max_back = room > (160ull << 20) ? (160ull << 20) : room;
        uint64_t back = 0;
        bool found = false;
        while (back < max_back) {
            const uint64_t win_hi = hit.addr - back;   // кандидаты строго ниже
            uint64_t win_lo = win_hi > kWin ? win_hi - kWin : region.start;
            if (win_lo < region.start) win_lo = region.start;
            const uint64_t read_hi = (win_hi + 0x210 < region.end) ? win_hi + 0x210
                                                                   : region.end;
            if (read_hi <= win_lo || read_hi - win_lo < 0x48) break;
            uint64_t holes = 0;
            read_holes(win_lo, win, (size_t)(read_hi - win_lo), &holes);
            for (uint64_t h = win_lo; h < win_hi && h + 0x210 <= read_hi; h += 4) {
                const uint8_t* hdr = win + (size_t)(h - win_lo);
                uint32_t w0 = 0, ver = 0;
                memcpy(&w0, hdr, 4);
                memcpy(&ver, hdr + 4, 4);
                // v38+: тройки + мозаика + накрытие метки.
                const uint32_t km = w0 ^ kRvmMagic;
                if ((ver >= kV38MinVer && ver <= kV38MaxVer) ||
                    (km && (ver ^ km) >= kV38MinVer && (ver ^ km) <= kV38MaxVer)) {
                    V38Match m;
                    if (v38_try(hdr, region.end - h, hit.addr - h, &m)) {
                        if (w0 != kRvmMagic)
                            say("   !! магия заголовка изменена протектором: "
                                "0x%08x вместо 0x%08x", (unsigned)w0, kRvmMagic);
                        add_cand(h, m.size, (int)m.version, true, m.key,
                                 m.sorted[6], m.sorted[7], m.sorted, "marker");
                        found = true;
                        break;
                    }
                    continue;      // v38+ без сошедшейся мозаики не принимаем
                }
                // Старые версии (v<38): пары + структура + накрытие метки.
                // Магия обязана быть на месте (иначе это мусор под v38+).
                if (ver >= kV38MinVer || rd32(hdr) != kRvmMagic) continue;
                BlobInfo b;
                if (!parse_blob(h, b)) continue;
                if (b.extent > (128ull << 20)) continue;
                if (hit.addr - h < b.string_off || hit.addr - h >= b.string_off + b.string_size)
                    continue;
                // Вторая РАЗЛИЧНАЯ метка внутри блоба: в строковой таблице
                // их всегда несколько (mscorlib + Assembly-CSharp).
                int distinct = 0;
                for (int j = 0; j < marker_count; ++j)
                    if (marker_hits[j].idx != hit.idx &&
                        marker_hits[j].addr >= h && marker_hits[j].addr < h + b.extent) {
                        ++distinct;
                        break;
                    }
                if (distinct < 1) continue;
                add_cand(h, b.extent, (int)b.version, false, 0,
                         b.string_off, b.string_size, nullptr, "marker");
                found = true;
                break;
            }
            if (found) break;
            back += win_hi - win_lo;
        }
        if (!found) {
            ++s_giveups;
            if (s_giveups == 64)
                say("   !! маркерный путь: 64 неудачи подряд — остальные метки "
                    "не гоняю (похоже на декой-заливку)");
        }
    }

    // 5. Выбираем блок строк: тот, где больше всего наших имён в окне ±8 МБ.
    uint64_t win_lo = 0, win_hi = 0, win_at = 0;
    int best_names = -1;
    for (int h = 0; h < hit_count; ++h) {
        const uint64_t pad = 8ull << 20;
        uint64_t lo = (hits[h] > pad) ? (hits[h] - pad) : 0;
        uint64_t hi = hits[h] + pad;
        // Окно не должно вылезать за регион, в котором лежит строка: за его
        // краем /proc/pid/mem просто отдаёт ноль и весь блок не прочитается.
        for (int i = 0; i < g_reg_count; ++i)
            if (hits[h] >= g_regs[i].start && hits[h] < g_regs[i].end) {
                if (lo < g_regs[i].start) lo = g_regs[i].start;
                if (hi > g_regs[i].end) hi = g_regs[i].end;
                break;
            }
        const uint64_t size = (hi - lo > (16ull << 20)) ? (16ull << 20) : (hi - lo);
        uint8_t* check = (uint8_t*)malloc((size_t)size);
        if (!check) break;
        size_t got = 0;
        while (got < size) {
            const size_t want = (size - got > (1u << 20)) ? (1u << 20) : (size_t)(size - got);
            if (!rd_at(lo + got, check + got, want)) break;
            got += want;
        }
        int found = 0;
        for (int n = 0; n < kNameCount && got > 0; ++n)
            if (memmem(check, got, kNames[n], strlen(kNames[n]) + 1) != nullptr) ++found;
        free(check);
        say("  блок @0x%llx — табличных имён %d из %d", (unsigned long long)hits[h], found, kNameCount);
        if (found > best_names) { best_names = found; win_lo = lo; win_hi = lo + size; win_at = hits[h]; }
    }
    if (!hit_count) {
        say("строка «PlayerManager» не найдена — снимем хотя бы скрытые регионы и данные образа");
    } else {
        say("выбран блок @0x%llx — окно 0x%llx..0x%llx (%llu МБ), имён %d из %d (старый режим)",
            (unsigned long long)win_at, (unsigned long long)win_lo, (unsigned long long)win_hi,
            (unsigned long long)((win_hi - win_lo) >> 20), best_names, kNameCount);
    }

    // 5b. Полная метадата. Кандидаты собрали четыре пути: чистая магия
    //     (старые версии — пары, parse_blob; v38+ — тройки), шапки БЕЗ
    //     магии по мозаике, маркерный путь и якорный прогон. Для каждого:
    //     наши имена в секции строк, вхождения «PlayerManager» в блоке,
    //     «прилегание» к границе региона. Мозаика v38+ — структурное
    //     подтверждение (31 тройка случайно не замостится), поэтому такой
    //     кандидат принимаем и при нуле имён (строковый пул больше 16 МБ
    //     может не дать имён в окне счётчика); старые версии — только с
    //     именами. Лучший выигрывает; полное равенство — первый в порядке
    //     обнаружения (детерминированно).
    uint64_t blob_at = 0, blob_extent = 0, blob_rlo = 0, blob_rhi = 0;
    int blob_version = 0, blob_names = 0, blob_v38 = 0;
    uint32_t blob_key = 0;
    uint32_t blob_sorted[93] = {};
    for (int m = 0; m < magic_count && m < 8; ++m) {
        BlobInfo b;
        if (!parse_blob(magic_hits[m], b)) {
            say("  шапка @0x%llx не прошла проверку таблиц (версия/смещения) — пропускаю",
                (unsigned long long)magic_hits[m]);
            continue;
        }
        add_cand(magic_hits[m], b.extent, b.version, false, 0,
                 b.string_off, b.string_size, nullptr, "magic");
    }
    if (g_cand_count) {
        int best_i = -1;
        for (int c = 0; c < g_cand_count; ++c) {
            Cand& cd = g_cands[c];
            if (cd.names < 0)
                cd.names = count_names_in(cd.addr + cd.string_off, cd.string_size);
            int covers = 0;
            for (int h = 0; h < hit_count; ++h)
                if (hits[h] >= cd.addr && hits[h] < cd.addr + cd.extent) ++covers;
            cd.covers = covers;
            // Регион, в котором живёт блок (соседние вливаются — mmap мог
            // расколоться mprotect'ом).
            uint64_t rlo = 0, rhi = 0;
            region_span(cd.addr, &rlo, &rhi);
            // Канонический вид — отдельный mmap файла метаданных: шапка на
            // границе региона, размер региона = размер блока. Копия блока в
            // куче живёт внутри большой арены — при прочих равных доверяем
            // каноническому.
            int fit = 0;
            if (rlo && cd.addr == rlo) {
                fit = 1;
                if (rhi - rlo == cd.extent) fit = 2;
            }
            cd.fit = fit;
            say("  кандидат @0x%llx (по %s): v%d, %llu МБ; регион 0x%llx..0x%llx (%llu МБ); "
                "наших имён в строках %d, вхождений «PlayerManager» %d",
                (unsigned long long)cd.addr, cd.via, cd.version,
                (unsigned long long)(cd.extent >> 20),
                (unsigned long long)rlo, (unsigned long long)rhi,
                (unsigned long long)((rhi - rlo) >> 20), cd.names, covers);
            if (!cd.v38 && cd.names < 1) continue;   // старый кандидат без имён — мусор
            if (best_i >= 0) {
                const Cand& bd = g_cands[best_i];
                if (cd.names < bd.names ||
                    (cd.names == bd.names && cd.covers < bd.covers) ||
                    (cd.names == bd.names && cd.covers == bd.covers && cd.fit < bd.fit))
                    continue;
                if (cd.names == bd.names && cd.covers == bd.covers && cd.fit == bd.fit)
                    continue;                        // полное равенство — первый
            }
            best_i = c;
        }
        if (best_i >= 0) {
            const Cand& cd = g_cands[best_i];
            blob_at = cd.addr;
            blob_extent = cd.extent;
            blob_version = cd.version;
            blob_names = cd.names;
            blob_v38 = cd.v38 ? 1 : 0;
            blob_key = cd.key;
            if (cd.v38) memcpy(blob_sorted, cd.sorted, sizeof(blob_sorted));
            region_span(cd.addr, &blob_rlo, &blob_rhi);
            if (blob_names < 3)
                say("в строках только %d наших имени — имена классов могли повернуть, "
                    "но блок — метадата", blob_names);
            // Регион лишь немногим больше, чем говорят таблицы шапки:
            // выравнивание в конце файла или секция, которую мы не знаем.
            // Берём до конца региона — метадата не обрежется. Больше 16 МБ
            // «хвоста» — это уже не метаданные (куча/арена вокруг копии
            // блока), а снимаем ровно по шапке.
            if (blob_rhi > blob_rlo && blob_rhi > blob_at + blob_extent &&
                (blob_rhi - blob_rlo) <= blob_extent + (16ull << 20)) {
                say("  регион крупнее таблиц шапки — снимаю до конца региона (%llu МБ)",
                    (unsigned long long)((blob_rhi - blob_at) >> 20));
                blob_extent = blob_rhi - blob_at;
            }
        } else {
            say("кандидатов без наших имён — снимаю старое окно");
        }
    } else {
        say("шапки метадаты не найдено ни одним путём (магия, мозаика, метки, "
            "якоря) — метаданные целиком зашифрованы или не загружены; снимаю старое окно");
    }
    if (blob_extent > (2ull << 30)) blob_extent = 2ull << 30;
    if (blob_at) {
        say("ПОЛНАЯ МЕТАДАТА: 0x%llx..0x%llx (%llu МБ, версия %d, v38+=%d%s) -> benzhack_blob.bin",
            (unsigned long long)blob_at, (unsigned long long)(blob_at + blob_extent),
            (unsigned long long)(blob_extent >> 20), blob_version, blob_v38,
            blob_key ? " (шапка XOR-нута — выгружу каноническую)" : "");
    }

    // 6. Считаем, сколько указателей из каждого региона ведёт в метадату
    //    (всю, не только в окно строк): самый «густой» регион — это пул
    //    классов и таблицы, ссылающиеся на строки метаданных.
    const uint64_t plo = blob_at ? blob_at : win_lo;
    const uint64_t phi = blob_at ? (blob_at + blob_extent) : win_hi;
    if (hit_count || blob_at) {
        const double t1 = now_s();
        uint64_t ptr_scanned = 0;
        for (int i = 0; i < g_reg_count && ptr_scanned < (4ull << 30); ++i) {
            const Reg& r = g_regs[i];
            int pfails = 0;
            for (uint64_t at = r.start; at + 8 <= r.end && ptr_scanned < (4ull << 30);) {
                const uint64_t remain = r.end - at;
                size_t want = (remain > sizeof(buf)) ? sizeof(buf) : (size_t)remain;
                if (!rd_at(at, buf, want)) {
                    if (++pfails > 4096) break;   // см. комментарий в шаге 4
                    at += 4096;
                    continue;
                }
                pfails = 0;
                for (size_t k = 0; k + 8 <= want; k += 4) {
                    uint64_t v;
                    memcpy(&v, buf + k, 8);
                    v = untag(v);
                    if (v < plo || v >= phi) continue;
                    ++g_regs[i].inwin;
                    if (!g_regs[i].first_inwin) g_regs[i].first_inwin = at + k;
                    g_regs[i].last_inwin = at + k;
                }
                at += want;
                ptr_scanned += want;
            }
        }
        say("указателей сочтено: %llu МБ за %.1f с", (unsigned long long)(ptr_scanned >> 20), now_s() - t1);
    }

    // 7. Пишем файлы.
    say("пишу дампы:");
    if (blob_at) {
        // Главное — весь блок метаданных (с канонической шапкой, если та
        // была расшифрована).
        dump_blob_full(out_dir, blob_at, blob_extent, blob_version,
                       blob_v38 != 0, blob_sorted);
    }
    if (hit_count) {
        dump_range(out_dir, "benzhack_meta.bin", win_lo, win_hi - win_lo, 16ull << 20);
        // Три самых густых по указателям региона. Если метадата найдена —
        // снимаем весь размах между первым и последним указателем (это и есть
        // пул классов целиком, не 256 КБ вокруг первого, как раньше).
        for (int top = 0; top < 3; ++top) {
            int best = -1;
            for (int i = 0; i < g_reg_count; ++i) {
                if (g_regs[i].inwin == 0) continue;
                bool used = false;
                for (int t = 0; t < top; ++t)
                    if (g_regs[g_tops[t]].start == g_regs[i].start) used = true;
                if (used) continue;
                if (best < 0 || g_regs[i].inwin > g_regs[best].inwin) best = i;
            }
            if (best < 0) break;
            g_tops[top] = best;
            const uint64_t f = g_regs[best].first_inwin;
            // Не вылезаем за начало региона: раньше по адресам — не наша память.
            const uint64_t lo = (f > (128ull << 10) && f - (128ull << 10) >= g_regs[best].start)
                ? (f - (128ull << 10)) : g_regs[best].start;
            char name[64];
            snprintf(name, sizeof(name), "benzhack_ptr%d.bin", top + 1);
            if (blob_at) {
                uint64_t hi = g_regs[best].last_inwin + (128ull << 10);
                if (hi > g_regs[best].end) hi = g_regs[best].end;
                const uint64_t span = hi - lo;
                if (span > (64ull << 20)) say("  ptr%d: размах %llu МБ — снимаю первые 64 МБ",
                    top + 1, (unsigned long long)(span >> 20));
                dump_range(out_dir, name, lo, span, 64ull << 20);
            } else {
                dump_range(out_dir, name, lo, 256ull << 10, 256ull << 10);
            }
        }
    }
    {
        int n = 0;
        for (int i = 0; i < g_reg_count && n < 3; ++i) {
            if (g_regs[i].kind != kHidden) continue;
            ++n;
            char name[64];
            snprintf(name, sizeof(name), "benzhack_hidden%d.bin", n);
            dump_range(out_dir, name, g_regs[i].start, 2ull << 20, 2ull << 20);
        }
        if (!n) say("  скрытых регионов с данными нет");
    }
    {
        // Пишущиеся сегменты образа: там глобальные слоты Il2CppClass*.
        uint64_t lo = 0, hi = 0;
        for (int i = 0; i < g_reg_count; ++i) {
            const Reg& r = g_regs[i];
            if (r.kind != kImage || r.perms[1] != 'w') continue;
            if (!lo) lo = r.start;
            if (r.end > hi) hi = r.end;
        }
        if (lo && hi > lo) {
            say("  пишущиеся сегменты образа 0x%llx..0x%llx (%llu МБ)",
                (unsigned long long)lo, (unsigned long long)hi, (unsigned long long)((hi - lo) >> 20));
            dump_range(out_dir, "benzhack_imagedata.bin", lo, hi - lo, 2ull << 30);
        }
    }
    say("всего записано %llu МБ", (unsigned long long)(g_written >> 20));

    // 8. Опись: все регионы, счётчики, адреса файлов.
    {
        char path[256];
        snprintf(path, sizeof(path), "%s/benzhack_dump_index.txt", out_dir);
        FILE* f = fopen(path, "w");
        if (f) {
            fprintf(f, "pid %d\n", (int)pid);
            fprintf(f, "libil2cpp 0x%llx..0x%llx\n",
                    (unsigned long long)g_lib_begin, (unsigned long long)g_lib_end);
            if (blob_at)
                fprintf(f, "blob 0x%llx..0x%llx version %d v38plus %d key 0x%02x "
                        "file benzhack_blob.bin\n",
                        (unsigned long long)blob_at,
                        (unsigned long long)(blob_at + blob_extent),
                        blob_version, blob_v38, (unsigned)blob_key);
            if (hit_count)
                fprintf(f, "window 0x%llx..0x%llx hit 0x%llx names %d of %d\n",
                        (unsigned long long)win_lo, (unsigned long long)win_hi,
                        (unsigned long long)win_at, best_names, kNameCount);
            fprintf(f, "regions %d\n", g_reg_count);
            for (int i = 0; i < g_reg_count; ++i) {
                const Reg& r = g_regs[i];
                static const char* kKind[4] = {"image", "anon", "hidden", "file"};
                fprintf(f, "reg %d %s 0x%llx 0x%llx %s inwin=%llu first=0x%llx last=0x%llx %s\n",
                        i, kKind[r.kind], (unsigned long long)r.start, (unsigned long long)r.end,
                        r.perms, (unsigned long long)r.inwin,
                        (unsigned long long)r.first_inwin, (unsigned long long)r.last_inwin,
                        r.path);
            }
            fclose(f);
            say("опись: %s", path);
        }
    }
    say("готово");
    if (g_log) fclose(g_log);
    return 0;
}
