// Съёмщик дампов памяти игры — ОТДЕЛЬНЫЙ бинарь, в сам чит не входит.
//
//   ./benzhack_dump [пакет] [каталог]
//
// пакет   — что снимать (по умолчанию сначала бета, потом релиз);
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
//   benzhack_meta.bin       — окно ±8 МБ вокруг строки «PlayerManager»: таблица
//                             строк метаданных, если она открытым текстом;
//   benzhack_ptrN.bin       — по 256 КБ вокруг первых указателей в это окно из
//                             трёх самых «густых» диапазонов (там либо пул
//                             классов, либо таблица строковых литералов);
//   benzhack_hiddenN.bin    — первые 2 МБ скрытых регионов (без прав чтения,
//                             но отдающихся принудительным чтением);
//   benzhack_imagedata.bin  — пишущиеся сегменты libil2cpp.so (.data.rel.ro,
//                             .data, .bss): в .bss лежат глобальные слоты
//                             Il2CppClass*, из которых виден адрес пула;
//   benzhack_dump_index.txt — опись: все регионы, счётчики указателей, адреса
//                             и размеры каждого снятого файла.
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
    uint64_t inwin = 0;              // сколько указателей в окно строк
    uint64_t first_inwin = 0;        // где первый такой указатель
};

static Reg g_regs[1024];
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
// Список — из таблицы статических оффсетов: по нему
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

// ---- Запись дампов ----------------------------------------------------------

static uint64_t g_written = 0;
static int g_tops[3] = {-1, -1, -1};
static const uint64_t kWriteCap = 96ull << 20;   // побольше 96 МБ не пишем

static void dump_range(const char* out_dir, const char* name, uint64_t addr, uint64_t size) {
    if (!size || g_written >= kWriteCap) return;
    if (size > (16ull << 20)) size = 16ull << 20;
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

// ---- main -------------------------------------------------------------------

int main(int argc, char** argv) {
    const char* out_dir = (argc > 2) ? argv[2] : kOutDir;
    const char* pkg = (argc > 1) ? argv[1] : nullptr;

    char log_path[256];
    snprintf(log_path, sizeof(log_path), "%s/benzhack_dump.log", out_dir);
    g_log = fopen(log_path, "w");

    say("дамп памяти: выход %s", out_dir);

    // 1. Процесс игры.
    pid_t pid = -1;
    const char* pkgs[2] = {pkg ? pkg : kPkgBeta, pkg ? pkg : kPkgRelease};
    for (int i = 0; i < 2 && pid < 0; ++i) {
        if (!pkgs[i]) continue;
        pid = find_pid(pkgs[i], 1);
        if (pid > 0) say("процесс %s — pid %d", pkgs[i], (int)pid);
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

    // 4. Ищем строку «PlayerManager» по всей читаемой памяти.
    static const char kNeedle[] = "PlayerManager";
    const size_t kNeed = sizeof(kNeedle);
    static uint8_t buf[1024 * 1024];
    const uint64_t kScanCap = 4ull << 30;
    uint64_t scanned = 0;
    uint64_t hits[16];
    int hit_count = 0;
    const double t0 = now_s();
    for (int i = 0; i < g_reg_count && hit_count < 16 && scanned < kScanCap; ++i) {
        const Reg& r = g_regs[i];
        for (uint64_t at = r.start; at + kNeed <= r.end && scanned < kScanCap;) {
            const uint64_t remain = r.end - at;
            size_t want = (remain > sizeof(buf)) ? sizeof(buf) : (size_t)remain;
            if (!rd_at(at, buf, want)) { at += 4096; continue; }
            scanned += want;
            const uint8_t* hit = (const uint8_t*)memmem(buf, want, kNeedle, kNeed);
            if (hit) {
                const uint64_t found = at + (uint64_t)(hit - buf);
                hits[hit_count] = found;
                ++hit_count;
                say("  строка найдена @0x%llx (регион %d, %s)", (unsigned long long)found, i, r.path);
                at = found + kNeed;
                if (hit_count >= 16) break;
                continue;
            }
            at += want - (kNeed - 1);
        }
    }
    say("просмотрено %llu МБ за %.1f с, найдено вхождений: %d",
        (unsigned long long)(scanned >> 20), now_s() - t0, hit_count);

    // 5. Выбираем блок строк: тот, где больше всего наших имён в окне ±8 МБ.
    uint64_t win_lo = 0, win_hi = 0, win_at = 0;
    int best_names = -1;
    for (int h = 0; h < hit_count; ++h) {
        const uint64_t pad = 8ull << 20;
        const uint64_t lo = (hits[h] > pad) ? (hits[h] - pad) : 0;
        const uint64_t hi = hits[h] + pad;
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
        say("выбран блок @0x%llx — окно 0x%llx..0x%llx (%llu МБ), имён %d из %d",
            (unsigned long long)win_at, (unsigned long long)win_lo, (unsigned long long)win_hi,
            (unsigned long long)((win_hi - win_lo) >> 20), best_names, kNameCount);
    }

    // 6. Считаем, сколько указателей из каждого региона ведёт в окно строк:
    //    самый «густой» регион — это либо пул классов, либо таблица литералов.
    if (hit_count) {
        const double t1 = now_s();
        uint64_t ptr_scanned = 0;
        for (int i = 0; i < g_reg_count && ptr_scanned < (4ull << 30); ++i) {
            const Reg& r = g_regs[i];
            for (uint64_t at = r.start; at + 8 <= r.end && ptr_scanned < (4ull << 30);) {
                const uint64_t remain = r.end - at;
                size_t want = (remain > sizeof(buf)) ? sizeof(buf) : (size_t)remain;
                if (!rd_at(at, buf, want)) { at += 4096; continue; }
                for (size_t k = 0; k + 8 <= want; k += 4) {
                    uint64_t v;
                    memcpy(&v, buf + k, 8);
                    v = untag(v);
                    if (v < win_lo || v >= win_hi) continue;
                    ++g_regs[i].inwin;
                    if (!g_regs[i].first_inwin) g_regs[i].first_inwin = at + k;
                }
                at += want;
                ptr_scanned += want;
            }
        }
        say("указателей сочтено: %llu МБ за %.1f с", (unsigned long long)(ptr_scanned >> 20), now_s() - t1);
    }

    // 7. Пишем файлы.
    say("пишу дампы:");
    if (hit_count) {
        dump_range(out_dir, "benzhack_meta.bin", win_lo, win_hi - win_lo);
        // Три самых густых по указателям региона — по 256 КБ вокруг первого.
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
            const uint64_t at = g_regs[best].first_inwin;
            const uint64_t lo = (at > (128ull << 10)) ? (at - (128ull << 10)) : g_regs[best].start;
            char name[64];
            snprintf(name, sizeof(name), "benzhack_ptr%d.bin", top + 1);
            dump_range(out_dir, name, lo, 256ull << 10);
        }
    }
    {
        int n = 0;
        for (int i = 0; i < g_reg_count && n < 3; ++i) {
            if (g_regs[i].kind != kHidden) continue;
            ++n;
            char name[64];
            snprintf(name, sizeof(name), "benzhack_hidden%d.bin", n);
            dump_range(out_dir, name, g_regs[i].start, 2ull << 20);
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
            dump_range(out_dir, "benzhack_imagedata.bin", lo, hi - lo);
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
            if (hit_count)
                fprintf(f, "window 0x%llx..0x%llx hit 0x%llx names %d of %d\n",
                        (unsigned long long)win_lo, (unsigned long long)win_hi,
                        (unsigned long long)win_at, best_names, kNameCount);
            fprintf(f, "regions %d\n", g_reg_count);
            for (int i = 0; i < g_reg_count; ++i) {
                const Reg& r = g_regs[i];
                static const char* kKind[4] = {"image", "anon", "hidden", "file"};
                fprintf(f, "reg %d %s 0x%llx 0x%llx %s inwin=%llu first=0x%llx %s\n",
                        i, kKind[r.kind], (unsigned long long)r.start, (unsigned long long)r.end,
                        r.perms, (unsigned long long)r.inwin,
                        (unsigned long long)r.first_inwin, r.path);
            }
            fclose(f);
            say("опись: %s", path);
        }
    }
    say("готово");
    if (g_log) fclose(g_log);
    return 0;
}
