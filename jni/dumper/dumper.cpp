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
//                             Протектор игры ломает шапку (схемы меняются):
//                             то XOR-ит всю шапку одним ключом, то (ран
//                             pid 19578) затирает мусором слоты магии И
//                             версии, а ТРОЙКИ оставляет чистыми — просто
//                             перемешанными. Поэтому простой поиск
//                             0xFAB11BAF ничего не даёт. Находится блоб
//                             четырьмя путями:
//                               1) магия на месте (старые/чистые сборки) —
//                                  проверка таблиц по версии;
//                               2) скан заголовков БЕЗ магии: версия чистая
//                                  или снимается ключом из слота магии, либо
//                                  ТРУЛЫ ЧИСТЫЕ (среди 93 слов есть offset
//                                  первой секции 0x17C..0x18C -> ключ 0),
//                                  затем приёмка по МОЗАИКЕ: расшифрованные
//                                  тройки отсортированы по offset плотно
//                                  замостили блоб от 0x17C (щели <= 16
//                                  байт), count<=size. Версия, если слот не
//                                  чистый, по ширине первой секции
//                                  (v39 StringLiteral = 4 B, v38 = 8 B);
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
//   benzhack_mreg.bin       — (v2.5) Il2CppMetadataRegistration, 256 Б
//                             сырьём + адрес в шапке;
//   benzhack_creg.bin       — (v2.5) Il2CppCodeRegistration, 256 Б сырьём;
//   benzhack_types_full.bin — (v2.5) ВСЯ таблица types: каждый слот
//                             TypeIndex {u64 dp, u32 bits}: CLASS/VALUETYPE —
//                             dp=TypeDefIndex, GENERICINST — dp=Il2CppGenericClass*,
//                             примитивы — dp=0. Отсюда оффлайн ВСЕ имена
//                             типов (включая generic-инстанции);
//   benzhack_ginst.bin      — (v2.5) по каждой unique generic-инстанции:
//                             TypeDefIndex базового класса + аргументы
//                             (слоты types или инлайновые записи);
//   benzhack_fo.bin         — (v2.5) fieldOffsets: на каждый тип указатель
//                             на массив до 512 int32-смещений полей
//                             (+benzhack_fo_head.bin — 4 КБ сырьём);
//   benzhack_mptrs.bin      — (v2.5) codeGenModules: на модуль запись
//                             {u32 mi, u32 nlen, name, u32 mc, u32 mdone,
//                             mdone×u64 адреса} — RVA = адрес − база .so;
//   benzhack_misc.bin       — (v2.5) genericClasses, genericInsts,
//                             methodSpecs, genericMethodTable, invokers,
//                             typeDefinitionsSizes, genericMethodPointers,
//                             metadataUsages (секции с тегами);
//   benzhack_dump_index.txt — опись: все регионы, счётчики указателей, адреса
//                             и размеры каждого снятого файла, строка «blob»
//                             — адрес/размер/версия полной метадаты.
//
// Искать регистрации (v2.5) не по адресам, а по ФОРМЕ:
//   MetadataRegistration — slot typesCount (5..500k) + указатель на массив
//   указателей на 16-байтовые записи {dp, bits}, где первые 128 записей —
//   валидные Il2CppType (CLASS/VALUETYPE с dp=TypeDefIndex без повторов).
//   CodeRegistration — codeGenModulesCount (50..500) + массив модулей, у
//   каждого читаемое строковое имя и plausible methodPointerCount.
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
    // Порядок скана: ЧИТАЕМЫЕ регионы (перезаписываемые анонимные и файловые)
    // первыми — движок в метаданные читает постоянно, значит регион у него
    // читаемый; скрытые (PROT_NONE) и «(deleted)» — ПОСЛЕ, на остаток
    // бюджета. Старый порядок (скрытые первыми) выедал весь потолок скана
    // на тысячах мусорных скрытых регионов, и читательская часть памяти
    // оставалась не просмотренной.
    // Перестановка ТОЛЬКО через промежуточный массив: in-place
    // «g_regs[i] = g_regs[order[i]]» через индексный массив некорректна —
    // когда order[i] < i, на чтение берётся уже перезаписанный элемент, и
    // список регионов получается с дублями и провалами (часть памяти
    // сканируется дважды, часть — никогда).
    {
        int score[8192];
        for (int i = 0; i < g_reg_count; ++i) {
            const Reg& r = g_regs[i];
            bool deleted = false;
            for (const char* q = r.path; *q;)
                if (!strncmp(q, " (deleted)", 10)) { deleted = true; break; }
                else ++q;
            const bool writable = r.perms[1] == 'w';
            if (deleted) score[i] = 5;
            else if (r.kind == kHidden) score[i] = 4;
            else if (r.path[0] == 0) score[i] = writable ? 0 : 3;  // анонимные
            else score[i] = writable ? 1 : 2;                      // файловые
        }
        int order[8192];
        int n = 0;
        for (int s = 0; s < 6; ++s)
            for (int i = 0; i < g_reg_count; ++i)
                if (score[i] == s) order[n++] = i;
        static Reg tmp[8192];   // 1.25 МБ — не на стек
        for (int i = 0; i < g_reg_count; ++i) tmp[i] = g_regs[order[i]];
        for (int i = 0; i < g_reg_count; ++i) g_regs[i] = tmp[i];
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
static const int kV38MinVer = 29, kV38MaxVer = 110;
// Тройки (offset,size,count), 31 шт., шапка 0x17C — формат версий 29..110
// (до 29 — пары (off,size), их ведёт parse_blob по чистой магии). Unity
// 6000.3 — 33/35/38/39, все четыре — тройки, поэтому мозаику пробуем для
// ЛЮБОЙ версии 29..110: чистая версия 33/35 не проходила старый затвор
// [38..110], и «магия стёрта + чистая версия 33/35 + чистые таблицы» была
// невидима для всех путей (нашёл бы только якорь без затвора — и тот,
// судя по ранам, не сработал).
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

// Новая схема (ран pid 19578): слоты магии И версии залиты мусором, а
// таблица троек ЧИСТА — перемешаны только тройки (ключ 0). Дешёвая
// дофильтрация для сканеров: среди 93 слов таблицы должно быть смещение
// ПЕРВОЙ секции — сразу после шапки, 0x17C..0x17C+16. На мусоре такое
// слово встречается с вероятностью ~4e-7 на позицию, на нашей шапке — да.
static bool v38_table_clean_hint(const uint8_t* hdr) {
    for (int j = 0; j < 93; ++j) {
        uint32_t v;
        memcpy(&v, hdr + 8 + (size_t)j * 4, 4);
        if (v >= kV38HeaderSize && v <= kV38HeaderSize + 16) return true;
    }
    return false;
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
    // Ключ 0 (чистые таблицы): пробуем ВСЕГДА первым — мозаика не зависит
    // от версии и сама по себе почти не даёт ложных совпадений. Протектор
    // этой игры в рантайм-копии метаданных маскирует магическое слово
    // (схемы меняются: то вместе с таблицей под одним ключом, то — ран
    // pid 19578 — магию и версию затирает мусором, оставляя таблицы
    // чистыми, перемешаны только тройки). Версия, если слот не чистый,
    // восстанавливается по ширине ПЕРВОЙ секции: v39 StringLiteral = 4 B,
    // v38 = 8 B (секции пишутся в каноническом порядке, первая —
    // stringLiterals). До этой поправки состояние «магия+версия мусор +
    // таблицы чистые» было невидимым ни для одного пути.
    const bool w1_clean = (w1 >= kV38MinVer && w1 <= kV38MaxVer);
    if (attempt(0, w1_clean ? w1 : 0)) {
        if (!w1_clean) {
            const uint32_t s0 = m.sorted[1], c0 = m.sorted[2];
            const double r = c0 ? (double)s0 / (double)c0 : 0.0;
            m.version = (r >= 7.0 && r <= 9.0) ? 38 : 39;
        }
        *out = m;
        return true;
    }
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
    int raw = 0;                    // 1 = новый протектор: магия цела,
                                    // версия/тройки замаскированы — таблицу
                                    // восстановить нельзя, снимает регион
                                    // сырым (шапка+тело как в памяти)
    const char* via = "";           // "magic"/"scan"/"marker"/"anchor"/"magic-scr"
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
                     const uint32_t* sorted, const char* via, int raw = 0) {
    if (cand_inside(addr) || g_cand_count >= (int)(sizeof(g_cands) / sizeof(g_cands[0])))
        return;
    Cand& c = g_cands[g_cand_count++];
    c.addr = addr; c.extent = extent; c.version = version; c.v38 = is_v38;
    c.key = key; c.string_off = string_off; c.string_size = string_size;
    c.raw = raw;
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

// «Текстовость» образца. Чистые строки метаданных (NUL-терминированный
// UTF-8 / UTF-16, имена классов) дают высокий балл: множество 0x00 и
// печатного ASCII (0x20-0x7E), почти нет управляющих 0x01-0x1F. Если этот
// текст XOR-нут константным ключом, балл заметно падает (NUL превращается
// в байт ключа, печатные рассыпаются по всему диапазону). ВАЖНО: количество
// РАЗЛИЧНЫХ значений байта здесь бесполезно — XOR константой является
// биекцией по 256 значениям, множество значений лишь переставляется, и у
// текста и его XOR-копии оно одинаково.
static double textlikeness(const uint8_t* p, int n) {
    if (n <= 0) return 0;
    double nul = 0, pr = 0, ctrl = 0;
    for (int i = 0; i < n; ++i) {
        if (p[i] == 0) ++nul;
        else if (p[i] >= 0x20 && p[i] <= 0x7e) ++pr;
        else if (p[i] >= 0x01 && p[i] <= 0x1f) ++ctrl;
    }
    return (nul + pr - ctrl) / n;
}

// Дамп полной метадаты. Если шапка была расшифрована (v38+ c XOR-нутыми
// тройками) — сначала пишем КАНОНИЧЕСКУЮ шапку (магия + версия +
// отсортированные тройки): файл сразу берёт Il2CppDumper. Тело — как есть,
// ИСКЛЮЧЕНИЕ: если строковая секция тоже XOR-нута тем же ключом (body_xor) —
// выгружаем тело расшифрованным этим ключом, иначе строки в файле остались
// бы зашифрованными.
// Нечитаемые страницы — нулями (файл не «съезжает»), потери считаются;
// частичная выгрузка переименовывается в *.partial и честно пишется в лог.
static void dump_blob_full(const char* out_dir, uint64_t addr, uint64_t size,
                           int version, bool v38, const uint32_t* sorted,
                           uint32_t key, bool body_xor) {
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
        if (body_xor) {
            // Ключ — 32-битный, применяется к каждому слову тела (как к
            // словам шапки). Фаза паттерна — от начала блоба.
            uint32_t ph = (uint32_t)((body_start + body_pos) & 3);
            for (size_t j = 0; j < want; ++j) {
                wbuf[j] ^= (uint8_t)(key >> (ph << 3));
                ph = (ph + 1) & 3;
            }
        }
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

// ---- Рантайм-таблица типов (v2) --------------------------------------------
//
// Таблица Il2CppMetadataRegistration.types: записи TypeDef =
//   {datapoint = TypeDefIndex (0..N-1), type = CLASS (0x12)};
// рядом (в начале или в конце) — стандартные типы {datapoint = 0,
// type = 1..0x1c: void..object}. Индексы этих записей — те самые
// TypeIndex полей global-metadata.dat. Таблица — в куче процесса.
//
// v1 (dp строго i, i+1, ...) таблицу не нашёл — подпись расслаблена:
//   продел из 300+ СМЕЖНЫХ записей {type == CLASS, dp < 65536},
//   dp — все различны (битмап). Это покрывает таблицу, где TypeDef-ы
//   не в порядке метаданных (dp — перестановка), и различный порядок
//   полей в записи. Плюс: массив у64-указателей, чьи цели — записи
//   Il2CppType в одном из layout'ов (цели могут быть разбросаны).

struct TTLayout {
    const char* name;
    uint32_t stride;    // штрих записи
    uint32_t align;     // выравнивание начала записи (4/8/16)
    uint32_t dp_off;    // смещение dp
    uint32_t dp_len;    // 4 или 8
    int32_t bits_off;   // >=0: смещение u32-слова bits (type = bits>>16)
    uint32_t type_off;  // <0 bits_off: смещение байта type
};
static const TTLayout kLayouts[] = {
    {"S8",    8,  8,  0, 4,  4, 0},
    {"S8r",   8,  8,  4, 4,  0, 0},
    {"S8x",   8,  8,  0, 4, -1, 4},   // {u32 dp, u8 type, ...}
    {"S12",   12, 4,  0, 8,  8, 0},
    {"S12r",  12, 4,  4, 8,  0, 0},
    {"S12t",  12, 4,  4, 8, -1, 0},   // {u8 type, 3b, u64 dp}
    {"S16",   16, 16, 0, 8,  8, 0},
    {"S16r",  16, 16, 8, 8,  0, 0},
    {"S16t",  16, 16, 4, 8, -1, 0},   // {u8 type, 3b, u64 dp, pad}
    {"S16tu", 16, 16, 4, 4, -1, 0},   // {u8 type, 3b, u32 dp, pad}
    {"S16x",  16, 16, 0, 8, -1, 8},   // {u64 dp, u8 type, ...}
    // Il2CppType (runtime, 2020+): {u64 data, u8 type, u8 attrs, u16 numArgs,
    // u64 metadata}. Живёт только по указателям (types = массив ptr на
    // объекты) — inline-гейтом не проверяется, только как цель P-стрима.
    {"S24",   24, 8,  0, 8, -1, 8},
};
static const int kNTL = (int)(sizeof(kLayouts) / sizeof(kLayouts[0]));
static const uint32_t kDPCap = 65536;
static const uint32_t kRunMin = 300;

static bool addr_mapped(uint64_t v) {
    if (v < 0x1000) return false;
    // Регионы в /proc/maps по возрастанию — бинарный поиск по start.
    int lo = 0, hi = g_reg_count - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (v < g_regs[mid].start) hi = mid - 1;
        else if (v >= g_regs[mid].end) lo = mid + 1;
        else return true;
    }
    return false;
}

struct TypesCand {
    uint64_t start = 0;      // голова (после докатки назад)
    int li = 0;              // индекс layout'а
    bool is_ptr = false;     // 1 = массив u64-указателей
    uint64_t run = 0;        // длина CLASS-продела
    uint64_t dpmin = 0, dpmax = 0;
};
static TypesCand g_ttables[8];
static int g_ttable_count = 0;

static uint64_t ttbits64(size_t off, size_t len, const uint8_t* buf) {
    if (off + 8 > len) return ~0ull;
    uint64_t v; memcpy(&v, buf + off, 8); return v;
}

// Затвор по буферу: запись в buf[off..off+stride) выглядит как
// {CLASS, dp < cap}? Возвращает dp.
static bool tt_gate_buf(const TTLayout& L, size_t off, size_t len,
                        const uint8_t* buf, uint64_t* dp) {
    if (off + L.stride > len) return false;
    const uint8_t* p = buf + off;
    uint32_t type;
    if (L.bits_off >= 0) {
        uint32_t b; memcpy(&b, p + L.bits_off, 4);
        type = (b >> 16) & 0xff;
    } else {
        type = p[L.type_off];
    }
    if (type != 0x12) return false;
    uint64_t d = 0;
    if (L.dp_len == 4) { uint32_t v; memcpy(&v, p + L.dp_off, 4); d = v; }
    else memcpy(&d, p + L.dp_off, 8);
    if (d >= kDPCap) return false;
    *dp = d;
    return true;
}

// Чтение записи с логическим индексом i (отрицательный — до головы)
// напрямую из памяти. Возвращает dp и «bits» (для byte-type layout'ов —
// type << 16).
static bool tt_raw(const TypesCand& c, int64_t i, uint64_t* dp, uint32_t* bits) {
    const TTLayout& L = kLayouts[c.li];
    uint8_t rec[16];
    uint64_t a;
    if (c.is_ptr) {
        uint64_t p = 0;
        if (!rd_at(c.start + (uint64_t)i * 8, &p, 8)) return false;
        p = untag(p);
        if (!addr_mapped(p)) return false;
        if (!rd_at(p, rec, L.stride)) return false;
        a = p; (void)a;
    } else {
        a = c.start + (uint64_t)i * L.stride;
        if (!rd_at(a, rec, L.stride)) return false;
    }
    uint32_t type;
    if (L.bits_off >= 0) {
        uint32_t b; memcpy(&b, rec + L.bits_off, 4);
        *bits = b;
        type = (b >> 16) & 0xff;
    } else {
        type = rec[L.type_off];
        *bits = type << 16;
    }
    if (L.dp_len == 4) {
        uint32_t v; memcpy(&v, rec + L.dp_off, 4);
        *dp = v;
    } else {
        memcpy(dp, rec + L.dp_off, 8);
    }
    (void)a;
    return true;
}

// Правдоподобная запись (для пре/постфикса): любой валидный type,
// dp = 0 или < cap.
static bool tt_plausible(const TypesCand& c, int64_t i, uint64_t* dp,
                         uint32_t* type) {
    uint32_t b = 0;
    if (!tt_raw(c, i, dp, &b)) return false;
    *type = (b >> 16) & 0xff;
    const bool type_ok = (*type >= 1 && *type <= 0x1e) || *type == 0x40 ||
                         *type == 0x41 || *type == 0x45 || *type == 0x55;
    return type_ok && (*dp == 0 || *dp < kDPCap);
}

// Длина CLASS-продела от from: все записи {CLASS, dp<cap}, dp без повторов.
static uint64_t tt_run_unique(const TypesCand& c, uint64_t from, uint64_t max,
                              uint8_t* seen, uint64_t* dpmin, uint64_t* dpmax) {
    uint64_t n = 0;
    while (n < max) {
        uint64_t dp = 0; uint32_t t = 0;
        uint32_t b = 0;
        if (!tt_raw(c, (int64_t)(from + n), &dp, &b)) break;
        t = (b >> 16) & 0xff;
        if (t != 0x12 || dp >= kDPCap) break;
        if (seen[dp >> 3] & (1u << (dp & 7))) break;
        seen[dp >> 3] |= (1u << (dp & 7));
        if (dp < *dpmin) *dpmin = dp;
        if (dp > *dpmax) *dpmax = dp;
        ++n;
    }
    return n;
}

// Дамп: префикс + продел + хвост.
// Шапка (64 Б): u32 magic 0x42544154, u32 layout, u32 is_ptr, u32 stride,
//   u64 start, u64 run, u64 dpmin, u64 dpmax, u64 n_prefix, u64 n, u64 spare.
// Тело: n × {u64 dp, u32 bits}. Для CLASS-записей dp — и есть TypeDefIndex;
// стандартные типы — dp == 0 и type 1..14/16/18/19/1c.
// S24-дамп: 20Б-записи {u64 data, u32 bits=(type<<16|attrs), u64 metadata}
// + сайдкар benzhack_types_N_names.txt: "индекс\tтокен\tимя\tпространство"
// (индекс — позиция в массиве указателей от головы CLASS-продела).
static void ttable_dump_s24(const char* out_dir, int slot, const TypesCand& c) {
    char path[256], npath[264];
    snprintf(path, sizeof(path), "%s/benzhack_types_%d.bin", out_dir, slot);
    snprintf(npath, sizeof(npath), "%s/benzhack_types_%d_names.txt", out_dir, slot);
    FILE* f = fopen(path, "wb");
    FILE* nf = fopen(npath, "w");
    if (!f) { say("  %s не открылся", path); return; }

    struct Rec { uint64_t data; uint32_t bits; uint64_t meta;
                 uint32_t token; char name[128]; char ns[64]; };
    auto rd_target = [&](int64_t idx, Rec* r) -> bool {
        if (idx < 0 && (uint64_t)(-idx) * 8 > c.start) return false;
        uint64_t p = 0;
        if (!rd_at(c.start + (int64_t)idx * 8, &p, 8)) return false;
        p = untag(p);
        if (!addr_mapped(p)) return false;
        uint8_t obj[24];
        if (!rd_at(p, obj, 24)) return false;
        memcpy(&r->data, obj, 8);
        r->bits = ((uint32_t)obj[8] << 16) | (uint32_t)obj[9];
        memcpy(&r->meta, obj + 16, 8);
        r->token = 0; r->name[0] = 0; r->ns[0] = 0;
        if (r->meta && addr_mapped(r->meta)) {
            uint8_t md[24];
            if (rd_at(r->meta, md, 24)) {
                uint64_t np = 0, sp = 0;
                memcpy(&np, md, 8); memcpy(&sp, md + 8, 8);
                memcpy(&r->token, md + 16, 4);
                if (np && addr_mapped(np)) {
                    uint8_t sb[128];
                    if (rd_at(np, sb, sizeof(sb) - 1)) {
                        size_t l = strnlen((char*)sb, sizeof(sb) - 1);
                        memcpy(r->name, sb, l); r->name[l] = 0;
                    }
                }
                if (sp && addr_mapped(sp)) {
                    uint8_t sb[64];
                    if (rd_at(sp, sb, sizeof(sb) - 1)) {
                        size_t l = strnlen((char*)sb, sizeof(sb) - 1);
                        memcpy(r->ns, sb, l); r->ns[l] = 0;
                    }
                }
            }
        }
        return true;
    };
    auto type_ok = [](uint32_t t) {
        return (t >= 1 && t <= 0x1e) || t == 0x40 || t == 0x41 ||
               t == 0x45 || t == 0x55;
    };

    static uint8_t tmp[1024 * 1024];
    size_t used = 0;
    auto putrec = [&](const Rec* r) {
        memcpy(tmp + used, &r->data, 8);
        memcpy(tmp + used + 8, &r->bits, 4);
        memcpy(tmp + used + 12, &r->meta, 8);
        used += 20;
        if (used >= sizeof(tmp) - 32) { fwrite(tmp, 1, used, f); used = 0; }
    };
    auto putname = [&](int64_t idx, const Rec* r) {
        fprintf(nf, "%lld\t%08x\t%s\t%s\n", (long long)idx,
                (unsigned)r->token, r->name, r->ns);
    };

    // pre: до 64 валидных записей до головы
    Rec pre[64]; uint64_t npre = 0;
    for (int j = 1; j <= 64 && npre < 64; ++j) {
        Rec r;
        if (!rd_target(-(int64_t)j, &r)) break;
        if (!type_ok((r.bits >> 16) & 0xff)) break;
        pre[npre++] = r;
    }
    for (uint64_t i = 0; i < npre / 2; ++i) {
        Rec t = pre[i];
        pre[i] = pre[npre - 1 - i];
        pre[npre - 1 - i] = t;
    }

    uint32_t magic = 0x42544154, li = 11, isptr = 1, stride = 24;
    uint64_t h_start = c.start, h_run = c.run, h0 = 0;
    uint64_t h_npre = 0, h_n = 0;
    fwrite(&magic, 4, 1, f);  fwrite(&li, 4, 1, f);
    fwrite(&isptr, 4, 1, f);   fwrite(&stride, 4, 1, f);
    fwrite(&h_start, 8, 1, f); fwrite(&h_run, 8, 1, f);
    fwrite(&h0, 8, 1, f);      fwrite(&h0, 8, 1, f);   // dpmin=dpmax=0
    fwrite(&h_npre, 8, 1, f);  fwrite(&h_n, 8, 1, f);
    fwrite(&h0, 8, 1, f);

    for (uint64_t i = 0; i < npre; ++i) {
        putrec(&pre[i]);
        putname(-(int64_t)(npre - i), &pre[i]);
    }

    // run: полный CLASS-продел (до первого не-CLASS)
    uint64_t nrun = 0;
    for (;;) {
        Rec r;
        if (!rd_target((int64_t)nrun, &r)) break;
        if (((r.bits >> 16) & 0xff) != 0x12) break;
        putrec(&r);
        if (nrun < 64 || (nrun & 4095) == 0 || nrun >= 200000)
            say("  types24[%llu]: type=%02x data=%llx meta=%llx",
                (unsigned long long)nrun, (unsigned)((r.bits >> 16) & 0xff),
                (unsigned long long)r.data, (unsigned long long)r.meta);
        putname((int64_t)nrun, &r);
        ++nrun;
        if (nrun >= 200000) break;
    }
    h_run = nrun;

    // tail: до 2048 валидных после продела (первый — записью nrun)
    uint64_t ntail = 0;
    for (uint64_t j = 0; j < 2048; ++j) {
        Rec r;
        if (!rd_target((int64_t)(nrun + j), &r)) break;
        if (!type_ok((r.bits >> 16) & 0xff)) break;
        putrec(&r);
        if (j < 64)
            say("  types24 tail[+%llu]: type=%02x data=%llx",
                (unsigned long long)j,
                (unsigned)((r.bits >> 16) & 0xff), (unsigned long long)r.data);
        putname((int64_t)(nrun + j), &r);
        ++ntail;
    }
    if (used) fwrite(tmp, 1, used, f);

    h_npre = npre;
    h_n = npre + nrun + ntail;
    fseek(f, 24, SEEK_SET);
    fwrite(&h_run, 8, 1, f);
    fseek(f, 48, SEEK_SET);
    fwrite(&h_npre, 8, 1, f);
    fwrite(&h_n, 8, 1, f);
    fclose(f);
    if (nf) fclose(nf);
    say("  %s — S24* массив 0x%llx, CLASS-run %llu, префикс %llu, хвост %llu, "
        "всего %llu (имена: %s)",
        path, (unsigned long long)c.start, (unsigned long long)nrun,
        (unsigned long long)npre, (unsigned long long)ntail,
        (unsigned long long)h_n, npath);
}

static void ttable_dump(const char* out_dir, int slot, const TypesCand& c) {
    if (c.li == 11) { ttable_dump_s24(out_dir, slot, c); return; }
    const TTLayout& L = kLayouts[c.li];
    char path[256];
    snprintf(path, sizeof(path), "%s/benzhack_types_%d.bin", out_dir, slot);
    FILE* f = fopen(path, "wb");
    if (!f) { say("  %s не открылся", path); return; }

    uint32_t magic = 0x42544154, li = (uint32_t)c.li,
             isptr = (uint32_t)(c.is_ptr ? 1 : 0), stride = c.is_ptr ? 8 : L.stride;
    uint64_t h_start = c.start, h_run = c.run, h_dpmin = c.dpmin,
             h_dpmax = c.dpmax, h_npre = 0, h_n = 0, h0 = 0;
    fwrite(&magic, 4, 1, f);  fwrite(&li, 4, 1, f);
    fwrite(&isptr, 4, 1, f);  fwrite(&stride, 4, 1, f);
    fwrite(&h_start, 8, 1, f); fwrite(&h_run, 8, 1, f);
    fwrite(&h_dpmin, 8, 1, f); fwrite(&h_dpmax, 8, 1, f);
    fwrite(&h_npre, 8, 1, f);  fwrite(&h_n, 8, 1, f);
    fwrite(&h0, 8, 1, f);

    static uint8_t tmp[1024 * 1024];
    size_t used = 0;
    auto putrec = [&](uint64_t dp, uint32_t b) {
        memcpy(tmp + used, &dp, 8);
        memcpy(tmp + used + 8, &b, 4);
        used += 12;
        if (used >= sizeof(tmp) - 16) { fwrite(tmp, 1, used, f); used = 0; }
    };

    const int64_t PREF_CAP = 2048, TAIL_CAP = 2048;
    uint8_t pre[2048 * 12];
    size_t npre = 0;
    for (int64_t j = -1; j >= PREF_CAP * -1 && npre < (size_t)PREF_CAP; --j) {
        uint64_t dp = 0; uint32_t t = 0;
        if (!tt_plausible(c, j, &dp, &t)) break;
        const uint32_t tb = t << 16;
        memcpy(pre + npre * 12, &dp, 8);
        memcpy(pre + npre * 12 + 8, &tb, 4);
        ++npre;
        if (npre <= 64)
            say("  types pref[-%llu]: type=%02x dp=%llu",
                (unsigned long long)(-j), (unsigned)t, (unsigned long long)dp);
    }
    for (size_t i = 0; i < npre / 2; ++i)
        for (int k = 0; k < 12; ++k) {
            uint8_t t = pre[i * 12 + k];
            pre[i * 12 + k] = pre[(npre - 1 - i) * 12 + k];
            pre[(npre - 1 - i) * 12 + k] = t;
        }
    if (npre) fwrite(pre, 1, npre * 12, f);
    h_npre = (uint64_t)npre;

    uint64_t nrun = 0;
    for (uint64_t i = 0; i < c.run; ++i) {
        uint64_t dp = 0; uint32_t b = 0;
        if (!tt_raw(c, (int64_t)i, &dp, &b)) break;
        putrec(dp, b);
        ++nrun;
    }

    uint64_t ntail = 0;
    for (uint64_t j = 0; j < (uint64_t)TAIL_CAP; ++j) {
        uint64_t dp = 0; uint32_t t = 0;
        if (!tt_plausible(c, (int64_t)(c.run + j), &dp, &t)) break;
        putrec(dp, t << 16);
        ++ntail;
        if (j < 64)
            say("  types tail[+%llu]: type=%02x dp=%llu",
                (unsigned long long)j, (unsigned)t, (unsigned long long)dp);
    }
    if (used) fwrite(tmp, 1, used, f);

    h_n = h_npre + nrun + ntail;
    fseek(f, 48, SEEK_SET);
    fwrite(&h_npre, 8, 1, f);
    fwrite(&h_n, 8, 1, f);
    fclose(f);

    say("  %s — layout %s%s, массив 0x%llx, run %llu, dp %llu..%llu, "
        "префикс %llu, хвост %llu, всего %llu",
        path, L.name, c.is_ptr ? "*" : "", (unsigned long long)c.start,
        (unsigned long long)c.run, (unsigned long long)c.dpmin,
        (unsigned long long)c.dpmax, (unsigned long long)h_npre,
        (unsigned long long)ntail, (unsigned long long)h_n);
}

// Сканируем всю читаемую память. Чанк 1 МБ (overlap 8 Б); если чанк не
// читается — по 16 КБ кускам (внутри «плохого» региона могут жить
// хорошие страницы).
static void scan_types_table(const char* out_dir) {
    const double t0 = now_s();
    uint64_t scanned = 0;
    static uint8_t buf[1024 * 1024];

    // Диапазон «кучных» адресов для затвора указательных массивов.
    uint64_t heap_lo = ~0ull, heap_hi = 0;
    for (int i = 0; i < g_reg_count; ++i) {
        if (g_regs[i].start < heap_lo) heap_lo = g_regs[i].start;
        if (g_regs[i].end > heap_hi) heap_hi = g_regs[i].end;
    }

    static uint8_t seen[kDPCap / 8];

    // Состояние потокового поиска указательных проделов.
    bool ptr_active = false;
    uint64_t ptr_start = 0, ptr_len = 0;

    auto verify_ptr_run = [&](uint64_t start, uint64_t len) {
        (void)len;
        if (g_ttable_count >= 8) return;
        // S24: стрим указателей на Il2CppType {u64 data, u8 type@8, u8 attrs@9,
        // u16 numArgs, u64 metadata@16}. Голова таблицы = CLASS-продел
        // (TypeDef'ы в порядке образов, mscorlib первым). Стрим может
        // начинаться НЕ с головы (чужие указатели перед таблицей) — ищем
        // длиннейший CLASS-продел среди первых 4000 целей.
        {
            const uint64_t maxj = len < 4000 ? len : 4000;
            uint8_t obj[24];
            int best_h = -1, best_run = 0, cur_h = -1, cur_run = 0;
            for (uint64_t j = 0; j < maxj; ++j) {
                uint64_t p = 0, meta = 0;
                bool cls = rd_at(start + j * 8, &p, 8) &&
                           addr_mapped(untag(p)) &&
                           rd_at(untag(p), obj, 24) && obj[8] == 0x12;
                if (cls) {
                    // metadata обязан быть 0 или картой-указателем: без этого
                    // 12Б-записи {dp, bits=0x120000} читаются как CLASS-объект
                    memcpy(&meta, obj + 16, 8);
                    if (meta && !(untag(meta) % 8 == 0 && addr_mapped(untag(meta))))
                        cls = false;
                }
                if (cls) {
                    if (cur_run == 0) cur_h = (int)j;
                    if (++cur_run > best_run) { best_run = cur_run; best_h = cur_h; }
                } else cur_run = 0;
            }
            if (best_h >= 0 && (uint64_t)best_run >= kRunMin) {
                const uint64_t hs = start + (uint64_t)best_h * 8;
                bool dup = false;
                for (int i = 0; i < g_ttable_count && !dup; ++i) {
                    const TypesCand& k = g_ttables[i];
                    if (!k.is_ptr) continue;
                    if (hs >= k.start && hs < k.start + k.run * 8) dup = true;
                }
                if (!dup) {
                    TypesCand c; c.start = hs; c.li = 11; c.is_ptr = true;
                    c.run = (uint64_t)best_run; c.dpmin = 0; c.dpmax = 0;
                    g_ttables[g_ttable_count++] = c;
                    say("  ТАБЛИЦА ТИПОВ v2: S24* (Il2CppType ptrs), 0x%llx, "
                        "CLASS-run %llu (голова +0x%llx от начала стрима)",
                        (unsigned long long)hs, (unsigned long long)best_run,
                        (unsigned long long)(hs - start));
                    return;
                }
            }
        }
        // Первая цель уже проверялась? — нет: проверяем заново, это 1-2 pread.
        for (int li = 0; li < kNTL; ++li) {
            if (li == 11) continue;   // S24 — отдельный путь ниже
            TypesCand c; c.start = start; c.li = li; c.is_ptr = true;
            uint64_t dp = 0; uint32_t b = 0;
            if (!tt_raw(c, 0, &dp, &b)) continue;
            const uint32_t t = (b >> 16) & 0xff;
            if (t != 0x12 || dp >= kDPCap) continue;
            memset(seen, 0, sizeof(seen));
            uint64_t dpmin = ~0ull, dpmax = 0;
            uint64_t n = tt_run_unique(c, 0, 50000, seen, &dpmin, &dpmax);
            if (n < kRunMin) continue;
            // дедуп
            bool dup = false;
            for (int i = 0; i < g_ttable_count && !dup; ++i) {
                const TypesCand& k = g_ttables[i];
                if (k.li != li || !k.is_ptr) continue;
                if (start >= k.start && start < k.start + k.run * 8) dup = true;
            }
            if (dup) continue;
            // назад
            uint64_t back = 0;
            while (back < 75000) {
                uint64_t d2 = 0; uint32_t b2 = 0;
                if (!tt_raw(c, -(int64_t)(back + 1), &d2, &b2)) break;
                if (((b2 >> 16) & 0xff) != 0x12 || d2 >= kDPCap) break;
                if (seen[d2 >> 3] & (1u << (d2 & 7))) break;
                seen[d2 >> 3] |= (1u << (d2 & 7));
                if (d2 < dpmin) dpmin = d2;
                if (d2 > dpmax) dpmax = d2;
                ++back;
            }
            if (back) c.start -= back * 8;
            c.run = n + back;
            c.dpmin = dpmin; c.dpmax = dpmax;
            g_ttables[g_ttable_count++] = c;
            say("  ТАБЛИЦА ТИПОВ v2: layout %s* (указатели), 0x%llx, run %llu, "
                "dp %llu..%llu (вперёд %llu + назад %llu)",
                kLayouts[li].name, (unsigned long long)c.start,
                (unsigned long long)c.run, (unsigned long long)dpmin,
                (unsigned long long)dpmax, (unsigned long long)n,
                (unsigned long long)back);
            return;
        }
    };

    // Проверки на кикандида: алиас поверх найденной базы + дубль по
    // началу + перекрытие диапазона того же layout.
    auto consider2_checked = [&](uint64_t start, int li) -> bool {
        const int base_li = (li == 1 || li == 4 || li == 7) ? li - 1 : -1;
        if (base_li >= 0) {
            const uint64_t d0 = (li == 1) ? 4 : 8;
            const uint64_t stride = kLayouts[base_li].stride;
            TypesCand cB; cB.start = start; cB.li = li; cB.is_ptr = false;
            for (int i = 0; i < g_ttable_count; ++i) {
                const TypesCand& A = g_ttables[i];
                if (A.li != base_li || A.is_ptr || A.start >= start) continue;
                const uint64_t delta = start - A.start;
                if (delta < d0 || (delta - d0) % stride != 0) continue;
                const uint64_t m = (delta - d0) / stride;
                bool alias = true;
                for (int k = 0; k < 10 && alias; ++k) {
                    uint64_t dA0 = 0, dA1 = 0, dB = 0;
                    uint32_t bA0 = 0, bA1 = 0, bB = 0;
                    if (!tt_raw(A, (uint64_t)(k + m), &dA0, &bA0)) alias = false;
                    else if (!tt_raw(A, (uint64_t)(k + m + 1), &dA1, &bA1)) alias = false;
                    else if (!tt_raw(cB, k, &dB, &bB)) alias = false;
                    else if (bB != bA0 || dB != dA1) alias = false;
                }
                if (alias) {
                    if (m == 0)
                        say("  алиас %s поверх %s@0x%llx — серия отброшена",
                            kLayouts[li].name, kLayouts[base_li].name,
                            (unsigned long long)A.start);
                    return false;
                }
            }
        }
        // Позиция ВНУТРИ уже найденной таблицы (любой layout): это суффикс
        // или чужое прочтение тех же байтов — таблица уже захвачена.
        // Без этого каждый 16Б внутри S16t-таблицы стоил полного прохода
        // ~30К записей (проверка только после прохода — миллиарды чтений).
        for (int i = 0; i < g_ttable_count; ++i) {
            const TypesCand& k = g_ttables[i];
            if (k.is_ptr) continue;
            if (start >= k.start &&
                start < k.start + k.run * kLayouts[k.li].stride) return false;
        }
        return true;
    };
    auto consider2 = [&](uint64_t start, int li) {
        if (g_ttable_count >= 8) return;
        if (!consider2_checked(start, li)) return;
        TypesCand c; c.start = start; c.li = li; c.is_ptr = false;
        memset(seen, 0, sizeof(seen));
        uint64_t dpmin = ~0ull, dpmax = 0;
        uint64_t n = tt_run_unique(c, 0, 50000, seen, &dpmin, &dpmax);
        if (n < kRunMin) return;
        const uint64_t stride = kLayouts[li].stride;
        uint64_t back = 0;
        while (back * stride < 600000) {
            uint64_t d2 = 0; uint32_t b2 = 0;
            if (!tt_raw(c, -(int64_t)(back + 1), &d2, &b2)) break;
            if (((b2 >> 16) & 0xff) != 0x12 || d2 >= kDPCap) break;
            if (seen[d2 >> 3] & (1u << (d2 & 7))) break;
            seen[d2 >> 3] |= (1u << (d2 & 7));
            if (d2 < dpmin) dpmin = d2;
            if (d2 > dpmax) dpmax = d2;
            ++back;
        }
        if (back) c.start -= back * stride;
        c.run = n + back;
        c.dpmin = dpmin; c.dpmax = dpmax;
        // докатка могла подвинуть голову под чужой диапазон — перепроверяем
        if (!consider2_checked(c.start, li)) return;
        g_ttables[g_ttable_count++] = c;
        say("  ТАБЛИЦА ТИПОВ v2: layout %s, 0x%llx, run %llu, dp %llu..%llu "
            "(вперёд %llu + назад %llu)",
            kLayouts[li].name, (unsigned long long)c.start,
            (unsigned long long)c.run, (unsigned long long)dpmin,
            (unsigned long long)dpmax, (unsigned long long)n,
            (unsigned long long)back);
    };

    // Проходы по куску памяти [at, at+len).
    auto scan_piece = [&](uint64_t at, const uint8_t* p, size_t len) {
        // A: 8-выровненные (S8/S8r/S8x + 16-выровн. S16*)
        for (size_t off = 0; off + 16 <= len; off += 8) {
            if (g_ttable_count >= 8) return;
            const uint64_t abs = at + off;
            uint64_t d;
            if (tt_gate_buf(kLayouts[0], off, len, p, &d)) consider2(abs, 0);
            if (tt_gate_buf(kLayouts[1], off, len, p, &d)) consider2(abs, 1);
            if (tt_gate_buf(kLayouts[2], off, len, p, &d)) consider2(abs, 2);
            if ((abs & 15) == 0) {
                if (tt_gate_buf(kLayouts[6], off, len, p, &d)) consider2(abs, 6);
                if (tt_gate_buf(kLayouts[7], off, len, p, &d)) consider2(abs, 7);
                if (tt_gate_buf(kLayouts[8], off, len, p, &d)) consider2(abs, 8);
                if (tt_gate_buf(kLayouts[9], off, len, p, &d)) consider2(abs, 9);
                if (tt_gate_buf(kLayouts[10], off, len, p, &d)) consider2(abs, 10);
            }
            // P: потоковый поиск 300+ смежных «кучных» указателей.
            const uint64_t v = ttbits64(off, len, p);
            const uint64_t u = untag(v);
            const bool pptr = (u >= heap_lo && u < heap_hi) && (u & 7) == 0;
            if (pptr) {
                if (ptr_active && abs == ptr_start + ptr_len * 8) {
                    ++ptr_len;
                } else {
                    if (ptr_active && ptr_len >= kRunMin)
                        verify_ptr_run(ptr_start, ptr_len);
                    ptr_active = true;
                    ptr_start = abs;
                    ptr_len = 1;
                }
            } else if (ptr_active) {
                if (ptr_len >= kRunMin) verify_ptr_run(ptr_start, ptr_len);
                ptr_active = false;
            }
        }
        // B: 4-выровненные (S12/S12r/S12t)
        const size_t first4 = (4 - (at % 4)) % 4;
        for (size_t off = first4; off + 16 <= len; off += 4) {
            if (g_ttable_count >= 8) return;
            const uint64_t abs = at + off;
            uint64_t d;
            if (tt_gate_buf(kLayouts[3], off, len, p, &d)) consider2(abs, 3);
            if (tt_gate_buf(kLayouts[4], off, len, p, &d)) consider2(abs, 4);
            if (tt_gate_buf(kLayouts[5], off, len, p, &d)) consider2(abs, 5);
        }
    };

    for (int i = 0; i < g_reg_count && g_ttable_count < 8; ++i) {
        const Reg& r = g_regs[i];
        int bad = 0;
        for (uint64_t at = r.start; at + 24 < r.end; at += sizeof(buf)) {
            const uint64_t remain = r.end - at;
            const size_t want = (remain > sizeof(buf)) ? sizeof(buf) : (size_t)remain;
            if (want < 64) break;
            if (rd_at(at, buf, want)) {
                scanned += want;
                scan_piece(at, buf, want);
                if (g_ttable_count >= 8) break;
                continue;
            }
            // Чанк не читается целиком — по 16 КБ (хорошие страницы
            // внутри плохого региона всё равно сканируем).
            bool any = false;
            for (uint64_t s = at; s + 16384 <= at + want; s += 16384) {
                uint8_t pbuf[16384];
                if (!rd_at(s, pbuf, 16384)) continue;
                any = true;
                scanned += 16384;
                scan_piece(s, pbuf, 16384);
                if (g_ttable_count >= 8) break;
            }
            if (!any) {
                if (++bad > 4096) {
                    say("  types-scan: регион 0x%llx..0x%llx не читается — мимо",
                        (unsigned long long)r.start, (unsigned long long)r.end);
                    break;
                }
            }
            ptr_active = false;
        }
    }
    if (ptr_active && ptr_len >= kRunMin) verify_ptr_run(ptr_start, ptr_len);

    say("таблица типов v2: просмотрено %llu МБ за %.1f с, найдено %d",
        (unsigned long long)(scanned >> 20), now_s() - t0, g_ttable_count);
    if (!g_ttable_count) {
        say("  таблица типов не найдена (run {CLASS, dp<%u} без повторов "
            ">= %u отсутствует во всех layout'ах)",
            kDPCap, kRunMin);
        return;
    }
    for (int i = 0; i < g_ttable_count; ++i)
        ttable_dump(out_dir, i, g_ttables[i]);
}

// ============================================================================
// v2.5 — регистрации и рантайм-таблицы (для ОФФЛАЙН-сборки полного дампа).
//
// Метадата (global-metadata.dat) уже собрана; для dump.cs/script.json уровня
// Il2CppDumper не хватает то, что живёт ТОЛЬКО в памяти процесса:
//   * Il2CppMetadataRegistration / Il2CppCodeRegistration (статические
//     глобальные в libil2cpp.so — в пишущемся сегменте образа);
//   * массив types (таблица Il2CppType — имена всех слотов TypeIndex,
//     включая generic-инстанции — оффлайн из метадаты не выводятся);
//   * generic-инстанции (Il2CppGenericClass + аргументы);
//   * fieldOffsets (смещения полей — только из рантайма);
//   * указатели методов (codeGenModules) — RVA методов в .so;
//   * invokerPointers / methodSpecs / genericMethodTable / typeDefinitionsSizes.
//
// Ищем РЕГИСТРАЦИИ по форме (без знания адресов):
//   MetadataRegistration (128 Б), поле types в +0x38:
//     +0x00 genericClassesCount / +0x08 genericClasses
//     +0x10 genericInstsCount   / +0x18 genericInsts
//     +0x20 genericMethodTableCount / +0x28 genericMethodTable
//     +0x30 typesCount          / +0x38 types
//     +0x40 methodSpecsCount    / +0x48 methodSpecs
//     +0x50 fieldOffsetsCount   / +0x58 fieldOffsets
//     +0x60 typeDefinitionsSizesCount / +0x68 typeDefinitionsSizes
//     +0x70 metadataUsagesCount / +0x78 metadataUsages
//   Подпись: typesCount в (5000, 500000), types -> массив указателей на
//     записи Il2CppType; первые 64 записи «валидные типы».
//   CodeRegistration (136 Б, v27.1+/v39), codeGenModules в +0x78:
//     +0x00 reversePInvokeWrapperCount / +0x08 reversePInvokeWrappers
//     +0x10 genericMethodPointersCount / +0x18 genericMethodPointers
//     +0x20 genericAdjustorThunks
//     +0x28 invokerPointersCount     / +0x30 invokerPointers
//     +0x38 unresolvedVirtualCallCount / +0x40 unresolvedVirtualCallPointers
//     +0x48 unresolvedInstanceCallPointers / +0x50 unresolvedStaticCallPointers
//     +0x58 interopDataCount / +0x60 interopData
//     +0x68 windowsRuntimeFactoryCount / +0x70 windowsRuntimeFactoryTable
//     +0x78 codeGenModulesCount / +0x80 codeGenModules
//   Подпись: codeGenModulesCount в [50,500], массив модулей, у каждого —
//     читаемое имя (строка) и plausible methodPointerCount.
//
// Записи Il2CppType (S16, как нашёл скан типов): {u64 dp @0, u32 bits @8}:
//   CLASS(0x12)/VALUETYPE(0x11):  dp = TypeDefIndex;
//   GENERICINST(0x15):            dp = Il2CppGenericClass*;
//   VAR(0x13)/MVAR(0x1e):         dp = индекс generic-параметра;
//   ARRAY(0x14)/SZARRAY(0x1d)/PTR(0x0f)/BYREF(0x10): dp = указатель;
//   примитивы (1..0x16,0x18,0x19,0x1b,0x1c): dp = 0.
// ============================================================================

static const uint64_t kMregTypesOff = 0x38;   // поле types в MetadataRegistration
static const uint64_t kCregCgmOff   = 0x78;   // codeGenModulesCount в CodeRegistration

static inline uint64_t le64b(const uint8_t* p) {
    uint64_t v; memcpy(&v, p, 8); return v;
}
static inline uint32_t le32b(const uint8_t* p) {
    uint32_t v; memcpy(&v, p, 4); return v;
}

// Валидна ли запись Il2CppType по {bits, dp} (S16-интерпретация).
static bool type_bits_ok(uint32_t bits, uint64_t dp) {
    const uint32_t t = (bits >> 16) & 0xff;
    const bool kind = (t >= 0x01 && t <= 0x21) || t == 0x40 || t == 0x41 ||
                      t == 0x45 || t == 0x55 || t == 0xff;
    if (!kind) return false;
    if (t == 0x11 || t == 0x12) return dp < kDPCap;                 // klassIndex
    if (t == 0x13 || t == 0x1e) return dp < (1ull << 20);           // var-index
    if (t == 0x15 || t == 0x14 || t == 0x1d || t == 0x0f || t == 0x10)
        return dp > 0x10000 && addr_mapped(dp);                     // ptr-union
    return true;
}

// Проверка кандидата на массив types: head -> массив указателей на Il2CppType.
static bool verify_types_array(uint64_t head, int* ncls_out) {
    // Первые ~50 TypeIndex-слотов — special-типы (void/bool/.../Object/
    // ValueType/generic-параметры), дальше — классы в TypeDef-порядке.
    // Окно 128 закрывает оба блока.
    uint8_t pb[128 * 8];
    if (!rd_at(head, pb, sizeof(pb))) return false;
    int ok = 0, cls = 0, nd = 0;
    uint64_t seen[64];
    for (int j = 0; j < 128; ++j) {
        uint64_t P = untag(le64b(pb + (size_t)j * 8));
        if (!addr_mapped(P)) continue;
        uint8_t rec[16];
        if (!rd_at(P, rec, sizeof(rec))) continue;
        if (!type_bits_ok(le32b(rec + 8), le64b(rec))) continue;
        ++ok;
        const uint32_t t = (le32b(rec + 8) >> 16) & 0xff;
        if (t == 0x11 || t == 0x12) {
            const uint64_t dp = le64b(rec);
            ++cls;
            int dup = 0;
            for (int k = 0; k < nd; ++k) if (seen[k] == dp) { dup = 1; break; }
            if (!dup && nd < 64) seen[nd++] = dp;
        }
    }
    if (ncls_out) *ncls_out = cls;
    return ok >= 80 && cls >= 30 && nd >= 8;
}

// Проверка кандидата на codeGenModules-массив.
static bool verify_cgm_array(uint64_t head) {
    uint8_t pb[8 * 8];
    if (!rd_at(head, pb, sizeof(pb))) return false;
    int ok = 0;
    for (int j = 0; j < 8; ++j) {
        uint64_t M = untag(le64b(pb + (size_t)j * 8));
        if (!addr_mapped(M)) continue;
        // Il2CppCodeGenModule (v27+): +0x00 имя(char*), +0x08 кол-во
        // методов, +0x10 указатель на массив methodPointers.
        uint8_t md[24];
        if (!rd_at(M, md, sizeof(md))) continue;
        uint64_t name_p = untag(le64b(md + 0x00));
        uint64_t mc = le64b(md + 0x08);
        uint64_t mp = untag(le64b(md + 0x10));
        if (name_p < 0x10000 || !addr_mapped(name_p)) continue;
        if (mp && (!addr_mapped(mp) || mc > 300000)) continue;
        char nm[64] = {};
        if (!rd_at(name_p, nm, 63)) continue;
        bool text = true;
        for (int k = 0; k < 63; ++k) {
            unsigned char c = (unsigned char)nm[k];
            if (c == 0) break;
            if (c < 0x20 || c > 0x7e) { text = false; break; }
        }
        if (text && nm[0]) ++ok;
    }
    return ok >= 6;
}

// Сканируем диапазон по 1 МБ ищем обе регистрации. Найдённые базы (или 0).
static void find_registrations(uint64_t lo, uint64_t hi, uint64_t* mreg_out,
                               uint64_t* creg_out, double deadline) {
    static uint8_t buf[1u << 20];
    const double t_scan = now_s();
    uint64_t scanned = 0;
    for (uint64_t at = lo; at < hi && (!mreg_out || !*mreg_out || !*creg_out)
                              && now_s() < deadline;) {
        const size_t want = (size_t)((hi - at < sizeof(buf)) ? (hi - at) : sizeof(buf));
        if (!rd_at(at, buf, want)) { at += 4096; continue; }
        for (size_t i = 0; i + 16 <= want; i += 8) {
            const uint64_t abs = at + i;
            if (abs & 7) continue;
            // MetadataRegistration: types-указатель в abs, typesCount в abs-8.
            if ((!mreg_out || !*mreg_out) && i >= 8) {
                const uint64_t cnt = le64b(buf + i - 8);
                if (cnt < 5000 || cnt > 500000) continue;
                const uint64_t V = untag(le64b(buf + i));
                if (V < 0x10000 || (V & 7) || !addr_mapped(V)) continue;
                if (!verify_types_array(V, nullptr)) continue;
                *mreg_out = abs - kMregTypesOff;
                say("  РЕГИСТРАЦИЯ: MetadataRegistration @0x%llx (typesCount=%llu, "
                    "types=0x%llx)", (unsigned long long)*mreg_out,
                    (unsigned long long)cnt, (unsigned long long)V);
            }
            // CodeRegistration: база 16-выровнена; поля в +0x78/+0x80.
            if (creg_out && !*creg_out && (abs & 15) == 0 && i + 0x88 <= want) {
                const uint64_t c = le64b(buf + i + 0x78);
                if (c < 50 || c > 500) continue;
                const uint64_t M = untag(le64b(buf + i + 0x80));
                if (M < 0x10000 || (M & 7) || !addr_mapped(M)) continue;
                if (!verify_cgm_array(M)) continue;
                *creg_out = abs;
                say("  РЕГИСТРАЦИЯ: CodeRegistration @0x%llx (codeGenModules=%llu)",
                    (unsigned long long)*creg_out, (unsigned long long)c);
            }
        }
        at += want;
        scanned += want;
    }
    say("  регистрации: просмотрено %llu МБ за %.1f с",
        (unsigned long long)(scanned >> 20), now_s() - t_scan);
}

// Читает null-строку (не более cap-1 байт).
static size_t read_cstr(uint64_t addr, char* out, size_t cap) {
    size_t got = 0;
    while (got + 1 < cap) {
        const size_t step = (cap - 1 - got < 64) ? (cap - 1 - got) : 64;
        uint8_t tmp[64];
        if (!rd_at(addr + got, tmp, step)) break;
        size_t z = 0;
        while (z < step && tmp[z]) ++z;
        memcpy(out + got, tmp, z);
        got += z;
        if (z < step) break;
    }
    out[got < cap ? got : cap - 1] = 0;
    return got;
}

// Основной проход v2.5.
static void dump_reg_data(const char* out_dir) {
    const double t0 = now_s();
    say("v2.5: регистрации и рантайм-таблицы");

    uint64_t mreg = 0, creg = 0;
    // Регистрация — статический глобал libil2cpp.so (по данным рана 10513
    // НЕ в видимом rw-сегменте .data — протектор размазывает образ на
    // фрагменты и снимает «r» у части сегментов, т.е. .bss образа живёт в
    // hidden-регионах). Поэтому проходы:
    //   1) ВСЕ сегменты libil2cpp.so (любыми правами, включая hidden);
    //   2) остальные региона: сначала маленькие (<= 64 МБ — там .bss'ы и
    //      мелкие арены), потом большие, потолок 4 ГБ / 150 с.
    // 1-й проход: сегменты образа (путь из /proc/maps содержит libil2cpp).
    {
        uint64_t scanned = 0;
        for (int i = 0; i < g_reg_count && !(mreg && creg)
                            && now_s() < t0 + 90.0; ++i) {
            const Reg& r = g_regs[i];
            if (strstr(r.path, "libil2cpp") == nullptr) continue;
            scanned += r.end - r.start;
            uint64_t mr = mreg, cr = creg;
            find_registrations(r.start, r.end, &mr, &cr, t0 + 90.0);
            if (mr) mreg = mr;
            if (cr) creg = cr;
        }
        say("  1-й проход (сегменты libil2cpp.so): %llu МБ",
            (unsigned long long)(scanned >> 20));
    }
    // 2-й проход: всё остальное читаемое (hidden тоже — FOLL_FORCE).
    if (!mreg || !creg) {
        say("  2-й проход (вся прочая память, мелкие регионы первыми)");
        uint64_t scanned = 0;
        for (int pass2 = 0; pass2 < 2 && (scanned < (4ull << 30))
                                && now_s() < t0 + 150.0; pass2++) {
            for (int i = 0; i < g_reg_count && (scanned < (4ull << 30))
                                && now_s() < t0 + 150.0; ++i) {
                const Reg& r = g_regs[i];
                if (r.kind == kImage) continue;        // 1-й проход
                if (strstr(r.path, "libil2cpp")) continue;
                if (r.perms[0] != 'r' && r.kind != kHidden) continue;
                const uint64_t sz = r.end - r.start;
                if (pass2 == 0) { if (sz > (64ull << 20)) continue; }
                else { if (sz <= (64ull << 20)) continue; }
                const uint64_t remain = (4ull << 30) - scanned;
                const uint64_t take = (sz < remain) ? sz : remain;
                if (take < (1ull << 20)) continue;
                uint64_t mr = mreg, cr = creg;
                find_registrations(r.start, r.start + take, &mr, &cr, t0 + 150.0);
                if (mr != mreg) mreg = mr;
                if (cr != creg) creg = cr;
                scanned += take;
                if (mreg && creg) break;
            }
        }
    }
    if (!mreg) {
        say("v2.5: MetadataRegistration не найдена — таблицы не сняты. "
            "След. шаг: прогнать со вторым запуском (адрес может уехать), "
            "или снять hidden-регионы целиком (benzhack_hiddenN).");
        return;
    }

    // ---- MetadataRegistration: сырьё + раскод ----
    uint8_t mr[256];
    rd_at(mreg, mr, sizeof(mr));
    {
        char path[256];
        snprintf(path, sizeof(path), "%s/benzhack_mreg.bin", out_dir);
        FILE* f = fopen(path, "wb");
        if (f) {
            uint64_t base = mreg;
            fwrite(&base, 8, 1, f);
            fwrite(mr, 1, sizeof(mr), f);
            fclose(f);
            say("  benzhack_mreg.bin — 256 Б @0x%llx", (unsigned long long)mreg);
        }
    }
    const uint64_t gc_cnt = le64b(mr + 0x00), gc_ptr = untag(le64b(mr + 0x08));
    const uint64_t gi_cnt = le64b(mr + 0x10), gi_ptr = untag(le64b(mr + 0x18));
    const uint64_t gmt_cnt = le64b(mr + 0x20), gmt_ptr = untag(le64b(mr + 0x28));
    const uint64_t tc = le64b(mr + 0x30), tp = untag(le64b(mr + 0x38));
    const uint64_t ms_cnt = le64b(mr + 0x40), ms_ptr = untag(le64b(mr + 0x48));
    const uint64_t fo_cnt = le64b(mr + 0x50), fo_ptr = untag(le64b(mr + 0x58));
    const uint64_t tds_cnt = le64b(mr + 0x60), tds_ptr = untag(le64b(mr + 0x68));
    const uint64_t mu_cnt = le64b(mr + 0x70), mu_ptr = untag(le64b(mr + 0x78));
    say("  MREG: types %llu @0x%llx | genericClasses %llu @0x%llx | "
        "genericInsts %llu @0x%llx | gmTable %llu @0x%llx",
        (unsigned long long)tc, (unsigned long long)tp,
        (unsigned long long)gc_cnt, (unsigned long long)gc_ptr,
        (unsigned long long)gi_cnt, (unsigned long long)gi_ptr,
        (unsigned long long)gmt_cnt, (unsigned long long)gmt_ptr);
    say("  MREG: methodSpecs %llu @0x%llx | fieldOffsets %llu @0x%llx | "
        "tdSizes %llu @0x%llx | usages %llu @0x%llx",
        (unsigned long long)ms_cnt, (unsigned long long)ms_ptr,
        (unsigned long long)fo_cnt, (unsigned long long)fo_ptr,
        (unsigned long long)tds_cnt, (unsigned long long)tds_ptr,
        (unsigned long long)mu_cnt, (unsigned long long)mu_ptr);

    // ---- CodeRegistration: сырьё + раскод ----
    uint64_t inv_cnt = 0, inv_ptr = 0, gmp_cnt = 0, gmp_ptr = 0, cgm_cnt = 0, cgm_ptr = 0;
    if (creg) {
        uint8_t cr[256];
        rd_at(creg, cr, sizeof(cr));
        char path[256];
        snprintf(path, sizeof(path), "%s/benzhack_creg.bin", out_dir);
        FILE* f = fopen(path, "wb");
        if (f) {
            uint64_t base = creg;
            fwrite(&base, 8, 1, f);
            fwrite(cr, 1, sizeof(cr), f);
            fclose(f);
            say("  benzhack_creg.bin — 256 Б @0x%llx", (unsigned long long)creg);
        }
        gmp_cnt = le64b(cr + 0x10); gmp_ptr = untag(le64b(cr + 0x18));
        inv_cnt = le64b(cr + 0x28); inv_ptr = untag(le64b(cr + 0x30));
        cgm_cnt = le64b(cr + 0x78); cgm_ptr = untag(le64b(cr + 0x80));
        say("  CREG: codeGenModules %llu @0x%llx | invokers %llu @0x%llx | "
            "genericMethodPtrs %llu @0x%llx",
            (unsigned long long)cgm_cnt, (unsigned long long)cgm_ptr,
            (unsigned long long)inv_cnt, (unsigned long long)inv_ptr,
            (unsigned long long)gmp_cnt, (unsigned long long)gmp_ptr);
    } else {
        say("  CREG не найдена (метод-указатели — на втором прогоне)");
    }

    // ---- Полная таблица types: все слоты {dp, bits} ----
    // Копия слотов в памяти — для резолва аргументов generic-инстанций.
    static uint8_t slotmem[(1u << 20) * 12]; // до 1М слотов × 12 Б
    const uint64_t tc_cap = (tc > (1ull << 20)) ? (1ull << 20) : tc;
    {
        char path[256];
        snprintf(path, sizeof(path), "%s/benzhack_types_full.bin", out_dir);
        FILE* f = fopen(path, "wb");
        if (f) {
            uint64_t hdr[4] = {0x35224245, tc, tp, tc_cap};  // magic BZ25
            fwrite(hdr, 8, 4, f);
            uint64_t i = 0, bad = 0;
            uint8_t pbuf[1u << 20];
            while (i < tc_cap) {
                const uint64_t chunk = (tc_cap - i < (1ull << 20) / 8)
                                         ? (tc_cap - i) : ((1ull << 20) / 8);
                if (!rd_at(tp + i * 8, pbuf, chunk * 8)) {
                    for (uint64_t j = 0; j < chunk; ++j) {
                        uint64_t z = 0xFFFFFFFFFFFFFFFFull, zb = 0;
                        fwrite(&z, 8, 1, f); fwrite(&zb, 4, 1, f);
                        i++;
                    }
                    break;
                }
                for (uint64_t j = 0; j < chunk; ++j, ++i) {
                    uint64_t P = untag(le64b(pbuf + j * 8));
                    uint8_t rec[16] = {};
                    uint64_t dp = 0, bits = 0;
                    if (addr_mapped(P) && rd_at(P, rec, 16)) {
                        dp = le64b(rec);
                        bits = le32b(rec + 8);
                    } else {
                        dp = 0xFFFFFFFFFFFFFFFFull;
                        ++bad;
                    }
                    // в файл 12 Б: dp + bits; в slotmem то же
                    // (i уже инкрементирован — индекс слота = i-1).
                    fwrite(&dp, 8, 1, f);
                    fwrite(&bits, 4, 1, f);
                    memcpy(slotmem + (i - 1) * 12, &dp, 8);
                    memcpy(slotmem + (i - 1) * 12 + 8, &bits, 4);
                }
            }
            fclose(f);
            say("  benzhack_types_full.bin — %llu слотов (не чит. %llu)",
                (unsigned long long)tc_cap, (unsigned long long)bad);
        }
    }

    // ---- Generic-инстанции ----
    {
        char path[256];
        snprintf(path, sizeof(path), "%s/benzhack_ginst.bin", out_dir);
        FILE* f = fopen(path, "wb");
        if (f) {
            // Дедуп по gc-указателю: open addressing, 2^21.
            static uint64_t seen_gc[1u << 21];
            memset(seen_gc, 0, sizeof(seen_gc));
            auto seen = [&](uint64_t gc) -> bool {
                size_t h = (size_t)((gc >> 4) % (1u << 21));
                for (int p = 0; p < 64; ++p) {
                    size_t s = (h + p) & ((1u << 21) - 1);
                    if (seen_gc[s] == 0) { seen_gc[s] = gc; return false; }
                    if (seen_gc[s] == gc) return true;
                }
                return true; // таблицу держим: «уже видели»
            };
            uint64_t n = 0, nerr = 0;
            for (uint64_t i = 0; i < tc_cap && now_s() < t0 + 180.0; ++i) {
                uint64_t dp; memcpy(&dp, slotmem + i * 12, 8);
                uint32_t bits; memcpy(&bits, slotmem + i * 12 + 8, 4);
                if (((bits >> 16) & 0xff) != 0x15) continue;         // GENERICINST
                const uint64_t gc = dp;
                if (gc < 0x10000 || !addr_mapped(gc) || seen(gc)) continue;
                uint8_t g[32];
                if (!rd_at(gc, g, sizeof(g))) { ++nerr; continue; }
                uint64_t type_p = untag(le64b(g));
                uint64_t class_inst = untag(le64b(g + 8));
                uint64_t def_dp = 0; uint32_t def_bits = 0;
                if (addr_mapped(type_p)) {
                    uint8_t tr[16];
                    if (rd_at(type_p, tr, 16)) {
                        def_dp = le64b(tr);
                        def_bits = le32b(tr + 8);
                    }
                }
                // аргументы
                uint32_t nargs = 0;
                uint64_t argsl[64];
                bool inline_a[64];
                uint64_t idp[64]; uint32_t ibits[64];
                if (class_inst && addr_mapped(class_inst)) {
                    uint8_t inst[16];
                    if (rd_at(class_inst, inst, sizeof(inst))) {
                        nargs = le32b(inst);
                        if (nargs > 64) nargs = 64;
                        uint64_t list = untag(le64b(inst + 8));
                        if (nargs && list && addr_mapped(list) &&
                            rd_at(list, argsl, nargs * 8)) {
                            for (uint32_t a = 0; a < nargs; ++a) {
                                uint64_t A = untag(argsl[a]);
                                if (A >= tp && A < tp + tc_cap * 8) {
                                    inline_a[a] = false;
                                    idp[a] = 0; ibits[a] = 0;
                                } else if (addr_mapped(A)) {
                                    uint8_t ar[16];
                                    inline_a[a] = true;
                                    if (rd_at(A, ar, 16)) {
                                        idp[a] = le64b(ar);
                                        ibits[a] = le32b(ar + 8);
                                    }
                                } else {
                                    inline_a[a] = true;
                                    idp[a] = 0xFFFFFFFFFFFFFFFFull; ibits[a] = 0;
                                }
                            }
                        } else nargs = 0;
                    }
                }
                // запись: {u32 slot, u32 defIdx, u32 nargs,
                //          nargs × {i32 slotRefOrNeg, u64 dp, u32 bits}}
                uint32_t slot = (uint32_t)i;
                uint32_t ddef = (uint32_t)(def_dp & 0xFFFFFFFFu);
                fwrite(&slot, 4, 1, f);
                fwrite(&ddef, 4, 1, f);
                fwrite(&nargs, 4, 1, f);
                (void)def_bits;
                for (uint32_t a = 0; a < nargs; ++a) {
                    int32_t ref;
                    uint64_t dp2; uint32_t b2;
                    uint64_t A = untag(argsl[a]);
                    if (!inline_a[a]) {
                        ref = (int32_t)((A - tp) / 8);
                        memcpy(&dp2, slotmem + (size_t)ref * 12, 8);
                        memcpy(&b2, slotmem + (size_t)ref * 12 + 8, 4);
                    } else {
                        ref = -1;
                        dp2 = idp[a]; b2 = ibits[a];
                    }
                    fwrite(&ref, 4, 1, f);
                    fwrite(&dp2, 8, 1, f);
                    fwrite(&b2, 4, 1, f);
                }
                ++n;
            }
            fclose(f);
            say("  benzhack_ginst.bin — %llu инстанций (ошибок %llu)",
                (unsigned long long)n, (unsigned long long)nerr);
        }
    }

    // ---- fieldOffsets ----
    if (fo_ptr && fo_cnt) {
        // Голова сырьём (на случай не--pointer layout'а).
        {
            char path[256];
            snprintf(path, sizeof(path), "%s/benzhack_fo_head.bin", out_dir);
            FILE* f = fopen(path, "wb");
            uint8_t head[4096] = {};
            if (f && rd_at(fo_ptr, head, sizeof(head))) {
                uint64_t p = fo_ptr;
                fwrite(&p, 8, 1, f);
                fwrite(head, 1, sizeof(head), f);
                fclose(f);
            }
        }
        char path[256];
        snprintf(path, sizeof(path), "%s/benzhack_fo.bin", out_dir);
        FILE* f = fopen(path, "wb");
        if (f) {
            uint64_t hdr[3] = {0x35224245, fo_cnt, fo_ptr};
            fwrite(hdr, 8, 3, f);
            const uint64_t n_cap = (fo_cnt > 200000) ? 200000 : fo_cnt;
            uint8_t pbuf[1u << 20];
            uint64_t i = 0;
            while (i < n_cap && now_s() < t0 + 180.0) {
                const uint64_t chunk = (n_cap - i < (1ull << 20) / 8)
                                         ? (n_cap - i) : ((1ull << 20) / 8);
                if (!rd_at(fo_ptr + i * 8, pbuf, chunk * 8)) break;
                for (uint64_t j = 0; j < chunk; ++j, ++i) {
                    uint64_t P = untag(le64b(pbuf + j * 8));
                    uint32_t idx = (uint32_t)i, len = 0;
                    uint8_t data[512 * 4] = {};
                    if (P > 0x10000 && addr_mapped(P)) {
                        // Размер уступающим: массив = int32[fieldCount] и
                        // может уходить в конец маппинга.
                        for (uint32_t tryn = 512; tryn >= 4; tryn /= 2) {
                            if (rd_at(P, data, (size_t)tryn * 4)) {
                                len = tryn;
                                break;
                            }
                        }
                    }
                    fwrite(&idx, 4, 1, f);
                    fwrite(&len, 4, 1, f);
                    if (len)
                        fwrite(data, 1, (size_t)len * 4, f);
                }
            }
            fclose(f);
            say("  benzhack_fo.bin — %llu типов (по 512 u32 на тип)",
                (unsigned long long)n_cap);
        }
    }

    // ---- method pointers (codeGenModules) ----
    if (cgm_ptr && cgm_cnt) {
        char path[256];
        snprintf(path, sizeof(path), "%s/benzhack_mptrs.bin", out_dir);
        FILE* f = fopen(path, "wb");
        if (f) {
            uint64_t hdr[3] = {0x35224245, cgm_cnt, cgm_ptr};
            fwrite(hdr, 8, 3, f);
            uint8_t pbuf[256 * 8];
            uint64_t i = 0;
            while (i < cgm_cnt && now_s() < t0 + 240.0) {
                const uint64_t chunk = (cgm_cnt - i < 256) ? (cgm_cnt - i) : 256;
                if (!rd_at(cgm_ptr + i * 8, pbuf, chunk * 8)) break;
                for (uint64_t j = 0; j < chunk; ++j, ++i) {
                    uint64_t M = untag(le64b(pbuf + j * 8));
                    uint32_t mi = (uint32_t)i, nlen = 0, mc = 0, mdone = 0;
                    if (addr_mapped(M)) {
                        uint8_t md[24];
                        if (rd_at(M, md, sizeof(md))) {
                            // +0 имя, +8 кол-во, +10.. массив (см. verify_cgm_array).
                            uint64_t name_p = untag(le64b(md + 0x00));
                            char nm[128] = {};
                            if (name_p > 0x10000 && addr_mapped(name_p))
                                nlen = (uint32_t)read_cstr(name_p, nm, sizeof(nm));
                            mc = (uint32_t)le64b(md + 0x08);
                            uint64_t mp = untag(le64b(md + 0x10));
                            if (mc && mp > 0x10000 && addr_mapped(mp) &&
                                mc <= 1000000) {
                                uint8_t databuf[1u << 20];
                                while (mdone < mc && now_s() < t0 + 240.0) {
                                    const uint64_t take = (mc - mdone < (1ull << 20) / 8)
                                                            ? (mc - mdone)
                                                            : ((1ull << 20) / 8);
                                    if (!rd_at(mp + mdone * 8, databuf, take * 8))
                                        break;
                                    fwrite(databuf, 1, take * 8, f);
                                    mdone += take;
                                }
                            }
                            fwrite(&mi, 4, 1, f);
                            fwrite(&nlen, 4, 1, f);
                            fwrite(nm, 1, nlen, f);
                            fwrite(&mc, 4, 1, f);
                            fwrite(&mdone, 4, 1, f);
                        }
                    }
                }
            }
            fclose(f);
            say("  benzhack_mptrs.bin — %llu модулей", (unsigned long long)cgm_cnt);
        }
    }

    // ---- misc: genericClasses, genericInsts, methodSpecs, gmTable, invokers,
    //      typeDefinitionsSizes, genericMethodPointers ----
    {
        char path[256];
        snprintf(path, sizeof(path), "%s/benzhack_misc.bin", out_dir);
        FILE* f = fopen(path, "wb");
        auto section = [&](uint32_t tag, uint64_t ptr, uint64_t count,
                           uint64_t estride, uint64_t cap) {
            if (!f || !ptr || count == 0) return;
            if (count > cap) count = cap;
            uint32_t t = tag, c = (uint32_t)count;
            fwrite(&t, 4, 1, f);
            fwrite(&c, 4, 1, f);
            uint8_t databuf[1u << 20];
            uint64_t done = 0;
            while (done < count * estride) {
                const uint64_t take = (count * estride - done < sizeof(databuf))
                                         ? (count * estride - done) : sizeof(databuf);
                if (!rd_at(ptr + done, databuf, (size_t)take)) break;
                fwrite(databuf, 1, (size_t)take, f);
                done += take;
            }
        };
        if (f) {
            // Страйды по roytu/Il2CppDumper (v39):
            //   methodSpecs = 3×int32 = 12Б; gmTable = int32+{int32,int32} = 12Б;
            //   typeDefinitionsSizes = size_t[] = 8Б; остальное — массивы u64.
            section(1, gc_ptr, gc_cnt, 8, 1000000);
            section(2, gi_ptr, gi_cnt, 8, 2000000);
            section(3, ms_ptr, ms_cnt, 12, 2000000);
            section(4, gmt_ptr, gmt_cnt, 12, 2000000);
            section(5, inv_ptr, inv_cnt, 8, 2000000);
            section(6, tds_ptr, tds_cnt, 8, 1000000);
            section(7, gmp_ptr, gmp_cnt, 8, 2000000);
            section(8, mu_ptr, mu_cnt, 8, 2000000);
            fclose(f);
            say("  benzhack_misc.bin — auxiliary tables");
        }
    }

    say("v2.5: готово за %.1f с", now_s() - t0);
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
    // Потолог скана: вся читаемая память процесса игры — ~8-10 ГБ; 16 ГБ —
    // с запасом, чтобы читательские регионы (0..3) прошли целиком, а скрытые
    // (4) и «(deleted)» (5) получили остаток. Дамп — разовая операция,
    // 15-30 секунд скана допустимы.
    // Раньше фиксированные 16 ГБ: протектор аллоцирует собственные 16-МБ
    // регионы (объектные кучи, декой-заливки), и суммарная память
    // перестала в потолок помещаться — часть регионов (и, возможно,
    // настоящий строковый пул метаданных) оставалась НЕ ПРОСКАНИРОВАННОЙ:
    // раны давали «кандидатов 0», а имена из 43 находились в логах.
    // Теперь потолок = min(сумма всех регионов, 64 ГБ): читается всё.
    uint64_t total_reg = 0;
    for (int i = 0; i < g_reg_count; ++i)
        total_reg += g_regs[i].end - g_regs[i].start;
    const uint64_t kScanCap = (total_reg > (64ull << 30)) ? (64ull << 30) : total_reg;
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
    // Wall-clock бюджет скана: на живой игре чтение огромных регионов
    // (page-фолты, конкуренция за память) может занимать минуты без единой
    // строки в логе — выглядит как зависание (ран pid 31751: скан уткнулся
    // в гигантский регион после 2436/4721 и «завис»). Бюджет заканчивает
    // только СКАН, не ран: блоб всё ещё найдут якорный путь (шапка в
    // начале региона) и маркерный (метки уже собраны), затем 5b выбирает.
    const double kScanBudget = 120.0;
    auto scan_overdue = [&]() { return now_s() - t0 > kScanBudget; };
    // версии — в очередь magic_hits (таблицы проверяет parse_blob в 5b).
    auto try_magic = [&](uint64_t mfound, const Reg& r, int ridx) {
        if (cand_inside(mfound)) return;
        uint8_t hdr[0x210];
        const uint64_t rem = r.end - mfound;
        const size_t take = (rem > sizeof(hdr)) ? sizeof(hdr) : (size_t)rem;
        if (take < 0x48) return;
        uint64_t holes = 0;
        read_holes(mfound, hdr, take, &holes);
        uint32_t w0 = 0, ver = 0;
        memcpy(&w0, hdr, 4);
        memcpy(&ver, hdr + 4, 4);
        // Магия НАСТОЯЩАЯ (мы здесь именно по магии) — пробуем мозаику
        // троек ВСЕГДА, независимо от вида версии: протектор может
        // оставить магию чистой, а версию и смещения таблиц XOR-нуть
        // одним ключом (сборка из реального девайса: магия на месте,
        // parse_blob падает на «версия/смещения»). v38_try такой ключ
        // выводит из слота версии (w1 ^ v для v=38..110) — чистой версии
        // для этого не нужно. Старая v16..v37 шапка тройной мозаике не
        // сойдётся (пары 8-байтовые, тройки 12-байтовые) и честно
        // уйдёт в очередь parse_blob ниже.
        if (w0 == kRvmMagic) {
            // В очередь — ВСЕ чистые магии (даже те, что v38_try уже принял):
            // очередь нужна и для оффлайн-дампа файловой маппинга
            // (benzhack_filemapN.bin), и для DIAG-формуляра. Повторный
            // parse_blob в 5b по принятой v38-шапке честно провалится
            // (парный порядок смещений по тройкам не сходится) — дублей
            // кандидатов не будет.
            if (magic_count < 8 && !magic_dup(magic_hits, magic_count, mfound))
                magic_hits[magic_count++] = mfound;
            V38Match m;
            if (v38_try(hdr, rem, ~0ull, &m)) {
                add_cand(mfound, m.size, (int)m.version, true, m.key,
                         m.sorted[6], m.sorted[7], m.sorted, "magic");
                return;
            }
            if (ver >= kV38MinVer && ver <= kV38MaxVer && s_v38_rej < 30) {
                ++s_v38_rej;
                say("   ?? магия @0x%llx: v%u — мозаика троек не сходится "
                    "(ключи из версии перебраны)%s", (unsigned long long)mfound,
                    (unsigned)ver,
                    s_v38_rej == 30 ? " (дальше не пишу)" : "");
            }
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
                  (km && (w1 ^ km) >= kV38MinVer && (w1 ^ km) <= kV38MaxVer) ||
                  v38_table_clean_hint(p)))
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
             scanned < kScanCap && !scan_overdue(); ++i) {
        const Reg& r = g_regs[i];
        int fails = 0;
        for (uint64_t at = r.start; at + kNeed <= r.end && scanned < kScanCap &&
             !scan_overdue();) {
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
    if (scan_overdue())
        say("бюджет времени (%.0f с) исчерпан — скан окончен досрочно, блоб "
            "ищу по якорям/меткам", kScanBudget);
    say("просмотрено %llu МБ за %.1f с, строк: %d, шапок RVM: %d, меток: %d, "
        "кандидатов: %d",
        (unsigned long long)(scanned >> 20), now_s() - t0, hit_count,
        magic_count, marker_count, g_cand_count);

    // 4b. Якорный прогон v38+ в начале крупных регионов: блоб метадаты —
    //     большое mmap-выделение и почти всегда начинается у начала области.
    //     Здесь допустим самый тяжёлый случай (версия XOR-нута вместе с
    //     магией) — полный вывод ключей из контекста на первых килобайтах
    //     дёшев. СВОЙ бюджет времени: общий бюджет скана (t0) к этому
    //     моменту уже исчерпан — bug v2.4 (якорная фаза не запускалась).
    const double t_anchor = now_s();
    auto anchor_overdue = [&]() { return now_s() - t_anchor > 90.0; };
    for (int i = 0; i < g_reg_count && g_cand_count < 16 && !anchor_overdue(); ++i) {
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
    //     СВОЙ бюджет времени (bug v2.4: общий t0 скана уже истёк).
    static int s_giveups = 0;
    const double t_marker = now_s();
    auto marker_overdue = [&]() { return now_s() - t_marker > 120.0; };
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
        // Канонический блоб — отдельный mmap (<= ~30 МБ) с шапкой в начале
        // региона: дальше 32 МБ назад от метки шапки нет. Было 160 МБ — на
        // лже-метках в гигантских аренах это 160 окон по МБ каждая.
        const uint64_t max_back = room > (32ull << 20) ? (32ull << 20) : room;
        uint64_t back = 0;
        bool found = false;
        while (back < max_back && !marker_overdue()) {
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
                    (km && (ver ^ km) >= kV38MinVer && (ver ^ km) <= kV38MaxVer) ||
                    v38_table_clean_hint(hdr)) {
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
    int blob_body_xor = 0;
    uint32_t blob_sorted[93] = {};
    for (int m = 0; m < magic_count && m < 8; ++m) {
        BlobInfo b;
        if (!parse_blob(magic_hits[m], b)) {
            // Новая схема протектора (ран pid 4184): магия 0xFAB11BAF цела,
            // но версия и тройки замаскированы не однозначным XOR — таблицу
            // по шапке восстановить нельзя, а блоб нужен целиком. Снимаем
            // сырой регион: каноническая метадата — отдельный mmap, шапка в
            // начале региона, тело — до его конца (соседи влиты span'ом).
            uint64_t rlo = 0, rhi = 0;
            region_span(magic_hits[m], &rlo, &rhi);
            const uint64_t rel = (rlo && magic_hits[m] >= rlo) ? magic_hits[m] - rlo : ~0ull;
            const uint64_t rsize = (rlo && rhi) ? rhi - rlo : 0;
            if (rel <= 0x1000 && rsize >= kMinBlobSize && rsize <= (256ull << 20)) {
                say("  шапка @0x%llx: магия цела, версия/тройки замаскированы "
                    "(новая схема) — снимаю сырой регион 0x%llx..0x%llx (%llu МБ)",
                    (unsigned long long)magic_hits[m], (unsigned long long)rlo,
                    (unsigned long long)rhi, (unsigned long long)(rsize >> 20));
                const uint64_t ext = rhi - magic_hits[m];
                add_cand(magic_hits[m], ext, 39, false, 0, 0,
                         (ext > (16ull << 20)) ? (16ull << 20) : ext,
                         nullptr, "magic-scr", 1);
            } else {
                say("  шапка @0x%llx не прошла проверку таблиц (версия/смещения) — пропускаю",
                    (unsigned long long)magic_hits[m]);
            }
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
            if (!cd.v38 && !cd.raw && cd.names < 1)
                continue;                            // старый кандидат без имён — мусор
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
            // raw (новая схема маскировки) — каноническую шапку восстанавливать
            // нельзя (тройки неизвестны): выгружаем сырьём с настоящей шапкой.
            blob_v38 = (cd.v38 && !cd.raw) ? 1 : 0;
            blob_key = cd.key;
            if (cd.v38 && !cd.raw) memcpy(blob_sorted, cd.sorted, sizeof(blob_sorted));
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
            // XOR-нута ли СТРОКОВАЯ секция тем же ключом (протектор шифрует
            // и весь блоб, а не только шапку)? Считаем «текстовость» 4-КБ
            // образца сырым и под ключом: чистые строки дают ~1.0, их
            // XOR-копия — заметно меньше. Расшифровываем тело в файле,
            // только если ключ ЯВНО «текстовее» (иначе — снимаем как есть).
            if (blob_v38 && blob_key && cd.string_off) {
                static uint8_t smp[4096];
                uint64_t sh = 0;
                read_holes(blob_at + cd.string_off, smp, sizeof(smp), &sh);
                const double t_raw = textlikeness(smp, (int)sizeof(smp));
                for (size_t j = 0; j < sizeof(smp); ++j)
                    smp[j] ^= (uint8_t)(blob_key >> ((j & 3) << 3));
                const double t_key = textlikeness(smp, (int)sizeof(smp));
                if (t_key > t_raw + 0.15 && t_key > 0.5) {
                    blob_body_xor = 1;
                    say("  строковая секция XOR-нута тем же ключом — тело в файле "
                        "расшифровано (ключ 0x%08x, текстовость: сыро %.2f, "
                        "ключом %.2f)", (unsigned)blob_key, t_raw, t_key);
                } else {
                    say("  строки в памяти чистые (текстовость: сыро %.2f, "
                        "ключом %.2f) — тело снимаю как есть", t_raw, t_key);
                }
            }
        } else {
            say("кандидатов без наших имён — снимаю старое окно");
        }
    } else {
        say("шапки метадаты не найдено ни одним путём (магия, мозаика, метки, "
            "якоря) — метаданные целиком зашифрованы или не загружены; снимаю старое окно");
        // Формуляр: ни один путь не сработал — вываливаем в лог сырые первые
        // 0x17C байта двух наиболее вероятных шапок, чтобы оффлайн по ним
        // снять точную схему маскирования (XOR-построение, замещение,
        // перестановка байт). (a) файловая маппинг по чистой магии,
        // (b) начало региона, в котором нашлась строка-блок (рантайм-копия:
        // её размер в ране = ровно размер блоба, шапка — в начале региона).
        auto hexhdr = [&](const char* tag, uint64_t addr) {
            static uint8_t hb[0x17C];
            if (!rd_at(addr, hb, sizeof(hb))) {
                say("DIAG %s @0x%llx: не читается", tag,
                    (unsigned long long)addr);
                return;
            }
            for (uint64_t off = 0; off < sizeof(hb); off += 16) {
                size_t len = sizeof(hb) - off;
                if (len > 16) len = 16;
                char line[140];
                int n = snprintf(line, sizeof(line), "DIAG %s @0x%llx+0x%03x: ",
                                 tag, (unsigned long long)addr, (unsigned)off);
                for (size_t b = 0; b < len; ++b)
                    n += snprintf(line + n, sizeof(line) - (size_t)n, "%02x ",
                                  hb[off + b]);
                say("%s", line);
            }
        };
        for (int m = 0; m < magic_count; ++m)
            hexhdr("file", magic_hits[m]);
        for (int i = 0; i < g_reg_count; ++i)
            if (win_at >= g_regs[i].start && win_at < g_regs[i].end) {
                hexhdr("runt", g_regs[i].start);
                break;
            }
    }
    if (blob_extent > (2ull << 30)) blob_extent = 2ull << 30;
    if (blob_at) {
        say("ПОЛНАЯ МЕТАДАТА: 0x%llx..0x%llx (%llu МБ, версия %d, v38+=%d%s) -> benzhack_blob.bin",
            (unsigned long long)blob_at, (unsigned long long)(blob_at + blob_extent),
            (unsigned long long)(blob_extent >> 20), blob_version, blob_v38,
            blob_key ? " (шапка XOR-нута — выгружу каноническую)" : "");
    }

    // 5c. Рантайм-таблица типов (Il2CppMetadataRegistration.types).
    //     Нужна оффлайн: по ней читаются индексы стандартных типов
    //     (void/bool/.../object) в шкале TypeIndex метаданных. Таблица —
    //     в куче процесса, в .so её нет; подпись — продел записей
    //     {datapoint = i, type = CLASS} на 300+ штук подряд.
    scan_types_table(out_dir);

    // 5d. v2.5: регистрации (MetadataRegistration/CodeRegistration) и
    //     рантайм-таблицы (types, generic-инстанции, fieldOffsets,
    //     method-указатели) — для ОФФЛАЙН-сборки полного дампа.
    dump_reg_data(out_dir);

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
                       blob_v38 != 0, blob_sorted, blob_key, blob_body_xor != 0);
    }
    // СЛЕПЫЙ съём: в новых ран-билдах (pid 31751/10513) шапка не найдена ни
    // одним путём (якорь, мозаика, метки) — похоже, в памяти зашифрован
    // ЦЕЛИКОМ блоб, а не только шапка. Снимаем сырьём все /dev/zero (и
    // hidden) регионы правдоподобного размера: ключ вскрывается оффлайн
    // (offset первой секции 0x17C..0x18C известен -> мозаика перебором
    // ключа t[j]^(0x17C+g), 465 кандидатов). Фильтр «заполненность»:
    // метадата (чистая или XOR-нутая) почти не содержит нулей, свежая
    // арена — почти вся нули.
    {
        int zeros_done = 0;
        uint64_t zeros_bytes = 0;
        for (int i = 0; i < g_reg_count && zeros_done < 4 &&
             zeros_bytes < (128ull << 20); ++i) {
            const Reg& r = g_regs[i];
            const uint64_t sz = r.end - r.start;
            if (sz < (16ull << 20) || sz > (96ull << 20)) continue;
            const bool is_zero_file = strstr(r.path, "/dev/zero") != nullptr;
            if (!is_zero_file && r.kind != kHidden) continue;
            uint8_t pr[3 * 4096];
            uint64_t holes = 0, nz = 0;
            const uint64_t offs[3] = {0, sz >> 1, sz >> 2};
            for (int t = 0; t < 3; ++t) {
                uint64_t off = offs[t];
                if (off + 4096 > sz) off = sz - 4096;
                if (rd_at(r.start + off, pr + (size_t)t * 4096, 4096)) {
                    for (size_t k = 0; k < 4096; ++k)
                        if (pr[(size_t)t * 4096 + k]) ++nz;
                } else ++holes;
            }
            if (holes >= 3) continue;
            const double frac = (double)nz / (3.0 * 4096.0);
            if (frac < 0.5) {
                say("  %s 0x%llx..0x%llx (%llu МБ) — нулей %.0f%%, пропускаю",
                    is_zero_file ? "/dev/zero" : "hidden",
                    (unsigned long long)r.start, (unsigned long long)r.end,
                    (unsigned long long)(sz >> 20), (100.0 - frac * 100.0));
                continue;
            }
            char name[64];
            snprintf(name, sizeof(name), "benzhack_zero%d.bin", zeros_done + 1);
            say("  %s 0x%llx..0x%llx (%llu МБ, заполнено %.0f%%) -> %s",
                is_zero_file ? "/dev/zero" : "hidden",
                (unsigned long long)r.start, (unsigned long long)r.end,
                (unsigned long long)(sz >> 20), frac * 100.0, name);
            dump_range(out_dir, name, r.start, sz, sz);
            ++zeros_done;
            zeros_bytes += sz;
        }
    }
    if (hit_count) {
        dump_range(out_dir, "benzhack_meta.bin", win_lo, win_hi - win_lo, 16ull << 20);
    }
    // Файловая маппинг (global-metadata.dat): файл для оффлайн-анализа.
    // Здесь шапка с чистой магией, но версия/смещения закрыты (все
    // single-key ключи перебраны) и строк там нет — схема защиты файла
    // определяется по этим байтам (и по самому файлу из приложения).
    for (int m = 0; m < magic_count; ++m) {
        for (int i = 0; i < g_reg_count; ++i)
            if (magic_hits[m] >= g_regs[i].start && magic_hits[m] < g_regs[i].end) {
                char name[64];
                snprintf(name, sizeof(name), "benzhack_filemap%d.bin", m + 1);
                const uint64_t fsz = g_regs[i].end - magic_hits[m];
                dump_range(out_dir, name, magic_hits[m],
                           (fsz > (16ull << 20)) ? (16ull << 20) : fsz, 16ull << 20);
                break;
            }
    }
    if (hit_count) {
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
