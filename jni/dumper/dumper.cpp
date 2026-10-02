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
//                             Находится по шапке RVM (магия 0xFAB11BAF,
//                             таблицы версий 16–43; наш билд — v39). Чтобы
//                             не принять случайные 4 байта за шапку, каждая
//                             кандидат-шапка проверяется: таблицы должны идти
//                             по порядку, а в её секции строк должны лежать
//                             наши табличные имена (PlayerManager, TeamMember…).
//                             Блок снимается от шапки до конца всех секций по
//                             её таблицам. Если регион, в котором живёт блок,
//                             лишь немногим больше, чем говорят таблицы
//                             шапки, — снимается до конца региона (страховка
//                             от неизвестных секций). В этом файле ВСЕ
//                             таблицы метаданных: строки, типы, методы, поля,
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

    // 4. Один проход по всей читаемой памяти с двумя иголками: строкой
    //    «PlayerManager» (для старого окна ±8 МБ) и магией шапки RVM — по ней
    //    находится начало ПОЛНОГО блока метаданных. Чанки пересекаются на
    //    12 байт, поэтому 4-байтная магия на стыке чанков не потеряется.
    static const char kNeedle[] = "PlayerManager";
    const size_t kNeed = sizeof(kNeedle);
    static const uint8_t kMagic[4] = {0xAF, 0x1B, 0xB1, 0xFA};   // LE 0xFAB11BAF
    static uint8_t buf[1024 * 1024];
    const uint64_t kScanCap = 4ull << 30;
    uint64_t scanned = 0;
    uint64_t hits[16];
    int hit_count = 0;
    uint64_t magic_hits[8];
    int magic_count = 0;
    const double t0 = now_s();
    for (int i = 0; i < g_reg_count && (hit_count < 16 || magic_count < 8) &&
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
            if (hit) {
                const uint64_t found = at + (uint64_t)(hit - buf);
                // Перед тем как перепрыгнуть через строку, проверяем магию в
                // пропущеной части чанка — шапка метадаты могла быть раньше.
                if (magic_count < 8) {
                    uint8_t* search = buf;
                    const uint8_t* stop = hit;
                    while (search + 4 <= stop && magic_count < 8) {
                        uint8_t* mh = (uint8_t*)memmem(search, (size_t)(stop - search),
                                                       kMagic, 4);
                        if (!mh) break;
                        const uint64_t mfound = at + (uint64_t)(mh - buf);
                        if (!magic_dup(magic_hits, magic_count, mfound)) {
                            magic_hits[magic_count] = mfound;
                            ++magic_count;
                            say("  шапка метадаты (0xFAB11BAF) найдена @0x%llx (регион %d, %s)",
                                (unsigned long long)mfound, i, r.path);
                        }
                        search = mh + 4;
                    }
                }
                if (hit_count < 16) {
                    hits[hit_count] = found;
                    ++hit_count;
                    say("  строка найдена @0x%llx (регион %d, %s)", (unsigned long long)found, i, r.path);
                }
                at = found + kNeed;
                continue;
            }
            if (magic_count < 8) {
                uint8_t* search = buf;
                uint8_t* end = buf + want;
                while (search + 4 <= end && magic_count < 8) {
                    uint8_t* mh = (uint8_t*)memmem(search, (size_t)(end - search), kMagic, 4);
                    if (!mh) break;
                    const uint64_t mfound = at + (uint64_t)(mh - buf);
                    if (!magic_dup(magic_hits, magic_count, mfound)) {
                        magic_hits[magic_count] = mfound;
                        ++magic_count;
                        say("  шапка метадаты (0xFAB11BAF) найдена @0x%llx (регион %d, %s)",
                            (unsigned long long)mfound, i, r.path);
                    }
                    search = mh + 4;
                }
            }
            // Чанки пересекаются на 12 байт — магия из 4 байт на стыке
            // найдётся целиком в следующем.
            at += want - (kNeed - 1);
        }
    }
    say("просмотрено %llu МБ за %.1f с, вхождений строки: %d, шапок RVM: %d",
        (unsigned long long)(scanned >> 20), now_s() - t0, hit_count, magic_count);

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

    // 5b. Полная метадата. Из найденных шапок RVM выбираем настоящую:
    //     таблицы шапки должны пройти проверку (версия, порядок секций),
    //     а в её секции строк должны лежать наши табличные имена — ложное
    //     совпадение четырёх байт этого не выдержит. Снимаем весь блок от
    //     шапки до конца секций по таблицам.
    uint64_t blob_at = 0, blob_extent = 0, blob_rlo = 0, blob_rhi = 0;
    int blob_version = 0, blob_names = 0;
    if (magic_count) {
        int best_names = -1, best_covers = -1, best_fit = -1;
        for (int m = 0; m < magic_count && m < 6; ++m) {
            BlobInfo b;
            if (!parse_blob(magic_hits[m], b)) {
                say("  шапка @0x%llx не прошла проверку таблиц (версия/смещения) — пропускаю",
                    (unsigned long long)magic_hits[m]);
                continue;
            }
            int covers = 0;
            for (int h = 0; h < hit_count; ++h)
                if (hits[h] >= magic_hits[m] && hits[h] < magic_hits[m] + b.extent) ++covers;
            const int names = count_names_in(magic_hits[m] + b.string_off, b.string_size);
            // Регион, в котором живёт блок: mmap метаданных — это отдельный
            // (или пара соседних, если mprotect расколол) регион.
            uint64_t rlo = 0, rhi = 0;
            int ri = -1;
            for (int i = 0; i < g_reg_count; ++i)
                if (magic_hits[m] >= g_regs[i].start && magic_hits[m] < g_regs[i].end) { ri = i; break; }
            if (ri >= 0) {
                rlo = g_regs[ri].start;
                rhi = g_regs[ri].end;
                for (int k = 1; k < 4 && ri + k < g_reg_count; ++k)
                    if (g_regs[ri + k].start == rhi) rhi = g_regs[ri + k].end;
                    else break;
            }
            say("  шапка @0x%llx: версия %d, таблиц %d, по шапке %llu МБ; регион 0x%llx..0x%llx (%llu МБ); наших имён в строках %d, вхождений «PlayerManager» %d",
                (unsigned long long)magic_hits[m], b.version, b.pairs,
                (unsigned long long)(b.extent >> 20),
                (unsigned long long)rlo, (unsigned long long)rhi,
                (unsigned long long)((rhi - rlo) >> 20), names, covers);
            // Канонический вид — отдельный mmap файла метаданных: шапка на
            // границе региона, размер региона = размер блока. Копии блока в
            // куче (если вдруг есть) живут внутри большого арены-региона —
            // при прочих равных доверяем каноническому.
            int fit = 0;
            if (rlo && magic_hits[m] == rlo) {
                fit = 1;
                if (rhi - rlo == b.extent) fit = 2;
            }
            // Строго «лучше» — иначе первый кандидат при полном равенстве
            // остаётся (детерминированный выбор).
            if (names < best_names ||
                (names == best_names && covers < best_covers) ||
                (names == best_names && covers == best_covers && fit < best_fit)) continue;
            if (names == best_names && covers == best_covers && fit == best_fit) continue;
            best_names = names;
            best_covers = covers;
            best_fit = fit;
            blob_at = magic_hits[m];
            blob_extent = b.extent;
            blob_version = b.version;
            blob_names = names;
            blob_rlo = rlo;
            blob_rhi = rhi;
        }
        if (!blob_at || blob_names < 1) {
            // Ни одна кандидат-шапка не содержит наших имён — это ложное
            // совпадение байтов, не метадата.
            say("настоящая шапка метадаты не опознана (наших имён в строках: %d) — "
                "снимаю старое окно", blob_names < 0 ? 0 : blob_names);
            blob_at = 0;
            blob_extent = 0;
        } else if (blob_names < 3) {
            say("в строках только %d наших имени — имена классов могли повернуть, "
                "но это метадатная шапка", blob_names);
        }
        if (blob_at) {
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
        }
    } else {
        say("шапки RVM (0xFAB11BAF) в памяти нет — метаданные зашифрованы или "
            "не загружены; снимаю старое окно");
    }
    if (blob_extent > (2ull << 30)) blob_extent = 2ull << 30;
    if (blob_at) {
        say("ПОЛНАЯ МЕТАДАТА: 0x%llx..0x%llx (%llu МБ, версия %d) -> benzhack_blob.bin",
            (unsigned long long)blob_at, (unsigned long long)(blob_at + blob_extent),
            (unsigned long long)(blob_extent >> 20), blob_version);
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
        // Главное — весь блок метаданных.
        dump_range(out_dir, "benzhack_blob.bin", blob_at, blob_extent, 2ull << 30);
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
                fprintf(f, "blob 0x%llx..0x%llx version %d file benzhack_blob.bin\n",
                        (unsigned long long)blob_at, (unsigned long long)(blob_at + blob_extent),
                        blob_version);
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
