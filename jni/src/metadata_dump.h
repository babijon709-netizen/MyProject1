#pragma once
// metadata_dump.h — CLI-команда dump: рантайм-дамп global-metadata.dat из
// памяти ЗАПУЩЕННОЙ игры, из Termux:
//
//     su -c /data/local/tmp/xvcen dump                  // режим из .mode
//     su -c /data/local/tmp/xvcen dump external         // режим аргументом
//     su -c /data/local/tmp/xvcen dump qx 12345         // режим + конкретный pid
//
// Зачем: офсеты чита привязаны к сборке игры, а обфускатор перекатывает их на
// каждом обновлении. Дамп метаданных — первый шаг пересбора офсетов: его
// кладут рядом с libil2cpp.so из APK и прогоняют через Il2CppDumper (или
// свой tools/offsets), получая свежий dump.cs/il2cpp.h. На диске метадата
// лежит внутри APK (и не всегда в пригодном виде) — рабочий вариант взять
// её из памяти процесса, где il2cpp уже держит загруженный/расшифрованный
// образ. Именно это делает команда.
//
// Как ищется метадата: глобальный блоб — это одно непрерывное выделение
// (malloc/mmap на десятки мегабайт), в начале которого стоит магия
// 0xFAB11BAF и заголовок из пар (offset, size) таблиц. Сканируем читаемые
// приватные области карты игры ≥1 МБ (анонимные кучи/пулы scudo, [heap] и
// отображения .apk) блоками по 64 КБ, магию ищем на 4-байтовых выравнива-
// ниях. Кандидат проверяем по заголовку: версия 16..40, первая таблица
// начинается ровно за заголовком (её offset == размеру заголовка), пары
// идут по возрастанию — размер блоба = max(offset+size), зажатый концом
// отображения. Так отсекаются случайные совпадения магии в куче. Если игра
// перезапускалась, в памяти могут остаться СТАРЫЕ блобы — сохраняем каждый
// отдельным файлом, адреса и версии в логе.
//
// Доступ к памяти — ТОЛЬКО через g_mem (выбранный транспорт, KERNEL.md §1):
// чтения идут g_mem.read_quiet мимо кадрового кэша, своего /proc/<pid>/mem
// команда не открывает никогда. Из /proc берутся лишь карта (maps) и
// cmdline — то же, что делает поток привязки. Транспорт назначается ОДИН
// раз (аргумент или сохранённый .mode), никаких фоллбэков и переключений
// (§2): не заработал — команда честно печатает errno и подсказывает
// перезапустить с другим режимом ЯВНЫМ аргументом.
//
// Запуск — как у --check-driver: в main() до stealth-маскировки (нужен
// вывод в терминал, бинарник остаётся на диске), без GUI и потоков, процесс
// завершается сам. Фоновые потоки не создаются — обычные блокирующие
// чтения из единственного потока команды.
//
// Выход: 0 — хотя бы один блоб сохранён целиком; 1 — игра не найдена или
// транспорт не привязался; 2 — привязались, метадата не найдена; 3 — блобы
// найдены, но прочитаны с ошибками (файлы *.partial).

#include <sys/types.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <vector>

#include "config.h"          // CfgDir()
#include "game.h"            // esp_set_mem_backend, esp_mem_last_errno, ESP_MEM_*
#include "game_internal.h"   // g_mem
#include "maps_lookup.h"     // maps::lookup_library_bases
#include "process.h"         // process_find_game_pid, process_find_unity_candidates

namespace metadump {

// Магия начала global-metadata.dat (поле sanity заголовка Il2Cpp).
static constexpr std::uint32_t kMetaMagic = 0xFAB11BAF;

// Минимальный размер блоба метаданных реальной игры: строковый пул имён
// классов/полей один занимает мегабайты. Отсекает мелкие совпадения магии.
static constexpr std::uint64_t kMinBlobSize = 1ull << 20;

// Блок чтения при скане и выгрузке: крупный, чтобы транспорт реже гнал
// запросы (чанкование по 4К он делает сам), и не большой, чтобы прогресс
// был виден.
static constexpr size_t kChunk = 1u << 16;

// ---- журнал: дублируем каждую строку в stdout (Termux) и dump.log ---------

struct Log {
    FILE* f = nullptr;

    void open(const char* path) { f = fopen(path, "w"); }
    void say(const char* fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        vprintf(fmt, ap);
        printf("\n");
        va_end(ap);
        if (f) {
            va_start(ap, fmt);
            vfprintf(f, fmt, ap);
            fprintf(f, "\n");
            va_end(ap);
            fflush(f);
        }
    }
    ~Log() { if (f) fclose(f); }
};

// ---- карта памяти процесса --------------------------------------------------

struct Region {
    std::uint64_t start = 0, end = 0, file_off = 0;
    char perms[5] = {};
    std::string name;
};

// Текст /proc/<pid>/maps своим fd (карта регионов — разрешено всегда,
// KERNEL.md §1; это не память).
static bool read_proc_maps(pid_t pid, std::string* out) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", (int)pid);
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char buf[8192];
    for (;;) {
        const ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            out->append(buf, (size_t)n);
            if (out->size() > (16u << 20)) break;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        break;
    }
    close(fd);
    return !out->empty();
}

// cmdline процесса — для протокола «что именно дампим» (как у потока
// привязки, не память).
static std::string proc_cmdline(pid_t pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/cmdline", (int)pid);
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return "?";
    char buf[256] = {};
    const ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return "?";
    for (ssize_t i = 0; i < n; ++i)
        if (buf[i] == '\0') buf[i] = ' ';
    return std::string(buf);
}

static void parse_regions(const std::string& text, std::vector<Region>* out) {
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t eol = text.find('\n', pos);
        const size_t line_end = (eol == std::string::npos) ? text.size() : eol;
        const std::string line = text.substr(pos, line_end - pos);
        pos = line_end + 1;

        unsigned long long start = 0, end = 0, foff = 0, inode = 0;
        unsigned devmaj = 0, devmin = 0;
        char perms[5] = {};
        int path_at = -1;
        if (sscanf(line.c_str(), "%llx-%llx %4s %llx %x:%x %llu %n",
                   &start, &end, perms, &foff, &devmaj, &devmin, &inode,
                   &path_at) < 7)
            continue;
        Region r;
        r.start = start;
        r.end = end;
        r.file_off = foff;
        memcpy(r.perms, perms, sizeof(r.perms));
        if (path_at > 0 && (size_t)path_at < line.size())
            r.name = line.substr((size_t)path_at);
        if (r.end > r.start) out->push_back(r);
    }
}

// Годится ли область под скан: читаемая, приватная, размером с метадату.
// Кандидаты — безымянные анонимные области (в т.ч. именованные ядром пулы
// «[anon:...» — туда scudo кладёт крупные выделения как раз размера
// global-metadata), [heap] и файловые отображения .apk (метадата может быть
// отображена прямо из архива). Именованные «(deleted)» пропускаем: это
// остатки старого образа после обновления/перезапуска.
static bool region_candidate(const Region& r) {
    if (r.perms[0] != 'r' || r.perms[3] != 'p') return false;
    if (r.end - r.start < kMinBlobSize) return false;
    if (r.name.empty()) return true;
    static const char kDeleted[] = " (deleted)";
    const size_t dlen = sizeof(kDeleted) - 1;
    if (r.name.size() > dlen &&
        r.name.compare(r.name.size() - dlen, dlen, kDeleted) == 0)
        return false;
    if (r.name == "[heap]") return true;
    if (r.name.rfind("[anon:", 0) == 0) return true;
    if (r.name.size() > 4 && r.name.compare(r.name.size() - 4, 4, ".apk") == 0)
        return true;
    return false;
}

// Порядок скана: сначала анонимные изменяемые области (метадата почти всегда
// там), потом [heap], потом .apk. Найденный блоб выгружается после скана —
// при долгом полном проходе можно прерваться, дождавшись нужной находки.
static int region_score(const Region& r) {
    const bool writable = r.perms[1] == 'w';
    if (r.name.empty() || r.name.rfind("[anon:", 0) == 0) return writable ? 0 : 3;
    if (r.name == "[heap]") return 1;
    return 2;
}

// ---- блоб метаданных --------------------------------------------------------

struct Blob {
    std::uint64_t addr = 0;
    std::uint64_t size = 0;
    std::uint32_t version = 0;
};

// Проверка заголовка по месту найденной магии. Структурная, без таблицы
// версий: первая пара заголовка указывает на первую таблицу, а та лежит
// сразу за заголовком — значит offset[0] == размеру заголовка (8 + 8*N).
// Дальше пары (offset, size) идут по файлу по возрастанию. Размер блоба =
// max(offset+size). Если структура не сошлась (неизвестная версия), берём
// максимум по правдоподобным парам первых 0x140 байт — с запасом, лишний
// хвост Il2CppDumper не мешает.
static bool validate_blob_header(const unsigned char* hdr, size_t hdr_len,
                                 std::uint64_t addr, std::uint64_t region_end,
                                 std::uint32_t* version_out,
                                 std::uint64_t* size_out) {
    if (hdr_len < 0x48) return false;
    std::uint32_t sanity = 0, version = 0;
    memcpy(&sanity, hdr, 4);
    memcpy(&version, hdr + 4, 4);
    if (sanity != kMetaMagic) return false;
    if (version < 16 || version > 40) return false;

    std::uint64_t max_end = 0;
    std::uint32_t first_off = 0;
    memcpy(&first_off, hdr + 8, 4);

    bool structural = false;
    if (first_off >= 0x48 && first_off <= 0x200 && (first_off % 8) == 0) {
        const int pairs = (int)((first_off - 8) / 8);
        if (pairs >= 9 && pairs <= 60 &&
            (size_t)first_off + 8 <= hdr_len) {
            std::uint32_t prev = first_off;
            structural = true;
            for (int k = 0; k < pairs; ++k) {
                std::uint32_t off = 0, sz = 0;
                memcpy(&off, hdr + 8 + 8 * k, 4);
                memcpy(&sz, hdr + 12 + 8 * k, 4);
                if (off == 0 && sz == 0) continue;          // пустая таблица
                if ((off % 4) != 0 || off < first_off || off > 0x40000000u) {
                    structural = false;
                    break;
                }
                if (off < prev) {                            // таблицы по порядку
                    structural = false;
                    break;
                }
                prev = off;
                const std::uint64_t end = (std::uint64_t)off + sz;
                if (end > max_end) max_end = end;
            }
        }
    }
    if (!structural) {
        for (size_t p = 8; p + 8 <= hdr_len && p < 0x140; p += 8) {
            std::uint32_t off = 0, sz = 0;
            memcpy(&off, hdr + p, 4);
            memcpy(&sz, hdr + p + 4, 4);
            if (off < 8 || off > 0x40000000u || sz > 0x20000000u) continue;
            const std::uint64_t end = (std::uint64_t)off + sz;
            if (end > max_end) max_end = end;
        }
    }

    if (max_end < kMinBlobSize) return false;
    // Блоб — одно выделение и не может выйти за пределы отображения.
    if (max_end > region_end - addr) max_end = region_end - addr;
    if (max_end < kMinBlobSize) return false;
    *version_out = version;
    *size_out = max_end;
    return true;
}

static bool inside_any_blob(const std::vector<Blob>& blobs, std::uint64_t addr,
                            std::uint64_t len) {
    for (const Blob& b : blobs)
        if (addr >= b.addr && addr + len <= b.addr + b.size) return true;
    return false;
}

// Сканирование одного прочитанного блока: магия на 4-байтовых выравниваниях
// (блобы выравнены как минимум по 4; начало блока и шаг кратны 4, значит
// магия не разрезается границей блока). Совпадения внутри уже найденного
// блоба — мусор, пропускаем.
static void scan_block(const unsigned char* buf, size_t len, std::uint64_t addr,
                       std::uint64_t region_end, std::vector<Blob>* blobs,
                       Log* log) {
    for (size_t i = 0; i + 4 <= len; i += 4) {
        std::uint32_t v = 0;
        memcpy(&v, buf + i, 4);
        if (v != kMetaMagic) continue;
        const std::uint64_t at = addr + i;
        bool dup = false;
        for (const Blob& b : *blobs)
            if (at >= b.addr && at < b.addr + b.size) {
                dup = true;
                break;
            }
        if (dup) continue;

        std::uint64_t want = 0x210;
        if (want > region_end - at) want = region_end - at;
        unsigned char hdr[0x210];
        if (want < 0x48) continue;
        if (!g_mem.read_quiet(at, hdr, (size_t)want)) continue;

        std::uint32_t version = 0;
        std::uint64_t size = 0;
        if (!validate_blob_header(hdr, (size_t)want, at, region_end, &version,
                                  &size))
            continue;
        Blob b;
        b.addr = at;
        b.size = size;
        b.version = version;
        blobs->push_back(b);
        log->say("  ++ global-metadata v%u: адрес 0x%llx, %.1f МБ",
                 (unsigned)version, (unsigned long long)at,
                 (double)size / (1024.0 * 1024.0));
    }
}

static void scan_regions(const std::vector<Region>& regions,
                         std::vector<Blob>* blobs, Log* log) {
    static unsigned char buf[kChunk];
    std::uint64_t scanned = 0;
    for (const Region& r : regions) {
        const std::uint64_t size = r.end - r.start;
        if (r.name.empty())
            log->say("скан 0x%llx-0x%llx (%.0f МБ)",
                     (unsigned long long)r.start, (unsigned long long)r.end,
                     (double)size / (1024.0 * 1024.0));
        else
            log->say("скан 0x%llx-0x%llx (%.0f МБ) — %s",
                     (unsigned long long)r.start, (unsigned long long)r.end,
                     (double)size / (1024.0 * 1024.0), r.name.c_str());
        std::uint64_t done = 0;
        while (done < size) {
            const std::uint64_t left = size - done;
            const size_t want =
                (size_t)(left < kChunk ? left : kChunk);
            const std::uint64_t at = r.start + done;
            // Хвост уже найденного блоба перечитывать незачем.
            if (!inside_any_blob(*blobs, at, want)) {
                if (g_mem.read_quiet(at, buf, want))
                    scan_block(buf, want, at, r.end, blobs, log);
                else {
                    // Область не читается транспортом целиком: именно так
                    // выглядит защищённая память метаданных на части
                    // устройств (EIO у страниц метаданных, см. class_identity
                    // в game.cpp) — говорим об этом прямо, а не «не найдено».
                    log->say("   !! область не читается транспортом "
                             "(errno %d) — если таких много, страницы "
                             "метаданных закрыты; попробуй другой режим, "
                             "например: dump external",
                             esp_mem_last_errno());
                    break;
                }
            }
            done += want;
            scanned += want;
            if ((scanned / (64u << 20)) != ((scanned - want) / (64u << 20)))
                log->say("   ... прочитано %.0f МБ",
                         (double)scanned / (1024.0 * 1024.0));
        }
    }
}

// Выгрузка блоба в файл. Кусок, не прочитавшийся транспортом, — нули (файл
// не «съезжает»), счетчик потерь; если не прочитался самый первый кусок —
// блоб целиком в закрытых страницах, файл не пишем. При потерях файл
// переименовываем в *.partial — в лог и итог честно.
static bool dump_blob(const Blob& b, const char* path, Log* log) {
    FILE* f = fopen(path, "wb");
    if (!f) {
        log->say("!! не открыть %s на запись: %s", path, strerror(errno));
        return false;
    }
    static unsigned char buf[kChunk];
    std::uint64_t done = 0, failed = 0;
    while (done < b.size) {
        const std::uint64_t left = b.size - done;
        const size_t want = (size_t)(left < kChunk ? left : kChunk);
        if (g_mem.read_quiet(b.addr + done, buf, want)) {
            fwrite(buf, 1, want, f);
        } else if (done == 0) {
            fclose(f);
            remove(path);
            log->say("!! 0x%llx: страницы метаданных не читаются транспортом "
                     "(errno %d) — попробуй другой режим, например: "
                     "dump external",
                     (unsigned long long)b.addr, esp_mem_last_errno());
            return false;
        } else {
            memset(buf, 0, want);
            fwrite(buf, 1, want, f);
            failed += want;
        }
        done += want;
        if ((done / (8u << 20)) != ((done - want) / (8u << 20)))
            log->say("   ... %.0f / %.0f МБ", (double)done / (1024.0 * 1024.0),
                     (double)b.size / (1024.0 * 1024.0));
    }
    fclose(f);
    if (failed) {
        const std::string partial = std::string(path) + ".partial";
        rename(path, partial.c_str());
        log->say("!! %s: не прочитано %.1f МБ (нули), файл сохранён как %s",
                 path, (double)failed / (1024.0 * 1024.0), partial.c_str());
        return false;
    }
    return true;
}

// ---- выбор режима -----------------------------------------------------------

// Сохранённый выбор пользователя (.mode рядом с .lang/.accent) — тот же
// формат, что пишет RememberMode в mode_select.h.
static int saved_backend() {
    FILE* f = fopen((std::string(CfgDir()) + "/.mode").c_str(), "rb");
    if (!f) return ESP_MEM_EXTERNAL;
    char buf[32] = {};
    const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    std::string s(buf, n);
    while (!s.empty() &&
           (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();
    if (s == "kernel") return ESP_MEM_FT;
    if (s == "qx") return ESP_MEM_QX;
    if (s == "rt") return ESP_MEM_RT;
    return ESP_MEM_EXTERNAL;
}

static const char* backend_name(int backend) {
    switch (backend) {
        case ESP_MEM_FT: return "FT-Драйвер";
        case ESP_MEM_QX: return "QX-Драйвер";
        case ESP_MEM_RT: return "RT-Драйвер";
        default: return "Прямое чтение";
    }
}

// ---- точка входа ------------------------------------------------------------

static int Run(int argc, char* argv[]) {
    // argv[0] — само слово «dump»; дальше опционально режим и/или pid.
    const char* mode_arg = nullptr;
    pid_t want_pid = 0;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (!a || !a[0]) continue;
        if (a[0] >= '0' && a[0] <= '9') {
            want_pid = (pid_t)atoi(a);
        } else if (!mode_arg) {
            mode_arg = a;
        } else {
            printf("не понимаю аргумент: %s\n", a);
            printf("использование: xvcen dump [external|ft|qx|rt] [pid]\n");
            return 1;
        }
    }
    int backend = -1;
    if (mode_arg) {
        if (!strcmp(mode_arg, "external") || !strcmp(mode_arg, "ext"))
            backend = ESP_MEM_EXTERNAL;
        else if (!strcmp(mode_arg, "ft") || !strcmp(mode_arg, "kernel"))
            backend = ESP_MEM_FT;
        else if (!strcmp(mode_arg, "qx"))
            backend = ESP_MEM_QX;
        else if (!strcmp(mode_arg, "rt"))
            backend = ESP_MEM_RT;
        if (backend < 0) {
            printf("неизвестный режим: %s "
                   "(external | ft | qx | rt)\n", mode_arg);
            return 1;
        }
    } else {
        backend = saved_backend();
    }

    const std::string dump_dir = std::string(CfgDir()) + "/dump";
    mkdir(CfgDir(), 0777);
    mkdir(dump_dir.c_str(), 0777);

    Log log;
    log.open((dump_dir + "/dump.log").c_str());
    log.say("== xvcen dump: рантайм-дамп global-metadata.dat ==");
    log.say("uid/euid : %d / %d%s", (int)getuid(), (int)geteuid(),
            geteuid() == 0 ? " (root)" : "  <-- НЕ ROOT");
    if (geteuid() != 0)
        log.say("!! нужен root: su -c <путь>/xvcen dump");
    log.say("режим    : %s%s", backend_name(backend),
            mode_arg ? " (аргументом)" : " (из .mode)");
    log.say("каталог  : %s", dump_dir.c_str());

    // Единственное назначение транспорта за жизнь этого процесса (§2):
    // до первого bind, без переключений и фоллбэков.
    esp_set_mem_backend(backend);

    // ---- процесс игры: тот же поиск, что у потока привязки ----------------
    std::vector<pid_t> pids;
    if (want_pid > 0) {
        pids.push_back(want_pid);
    } else {
        const pid_t official = process_find_game_pid();
        if (official > 0) {
            pids.push_back(official);
        } else {
            pid_t cand[16] = {};
            const int n = process_find_unity_candidates(cand, 16);
            for (int i = 0; i < n; ++i) pids.push_back(cand[i]);
        }
    }
    if (pids.empty()) {
        log.say("!! игра не найдена — запусти её и дождись логотипа Unity "
                "(метадата грузится на старте il2cpp)");
        return 1;
    }

    // ---- привязка: bind по базе libil2cpp.so, без проверки класса --------
    // ВАЖНО: в отличие от esp_init, класс игры здесь НЕ сверяется — команда
    // нужна как раз после обновления игры, когда старые офсеты уже не
    // резолвятся. Для дампа метадаты достаточно, чтобы транспорт читал
    // память процесса.
    bool bound = false;
    pid_t used_pid = 0;
    std::string maps_text;
    std::vector<Region> regions;
    for (const pid_t pid : pids) {
        std::string text;
        if (!read_proc_maps(pid, &text)) continue;
        log.say("процесс  : pid %d (%s)", (int)pid,
                proc_cmdline(pid).c_str());

        // Кандидаты пробы bind: сначала базы libil2cpp.so (обычный путь —
        // bind читает по пробе ELF-заголовок образа), затем, если не взял,
        // начала файловых отображений карты (в начале файла тоже ELF) — так
        // привязывается и процесс без libil2cpp (явный pid). Пробы — обычные
        // чтения выбранным транспортом, как у потока привязки; локальный
        // разбор карты, чтобы регионы непривязавшегося pid не путались с
        // картой выбранного.
        std::vector<std::uint64_t> probes;
        {
            std::uint64_t bases[8] = {};
            const int nb =
                maps::lookup_library_bases(text, "libil2cpp.so", bases, 8);
            for (int i = 0; i < nb; ++i) probes.push_back(bases[i]);
        }
        {
            std::vector<Region> all_regions;
            parse_regions(text, &all_regions);
            for (const Region& r : all_regions) {
                if (r.file_off != 0) continue;   // не начало файла
                bool dup = false;
                for (const std::uint64_t p : probes)
                    if (p == r.start) {
                        dup = true;
                        break;
                    }
                if (!dup) probes.push_back(r.start);
                if (probes.size() >= 32) break;
            }
        }
        for (const std::uint64_t probe : probes)
            if (g_mem.bind(pid, probe)) {
                bound = true;
                break;
            }
        if (bound) {
            used_pid = pid;
            maps_text = text;
            break;
        }
        log.say("   не привязалось (errno %d)", esp_mem_last_errno());
    }
    if (!bound) {
        log.say("!! транспорт не смог читать память процесса: errno %d.",
                esp_mem_last_errno());
        log.say("   Проверь драйвер:  xvcen --check-driver");
        log.say("   Или перезапусти с другим режимом ЯВНЫМ аргументом: "
                "xvcen dump external");
        return 1;
    }

    // Карта процесса — тоже артефакт дампа (пригодится для офлайн-разбора).
    {
        char path[128];
        snprintf(path, sizeof(path), "%s/maps-%d.txt", dump_dir.c_str(),
                 (int)used_pid);
        FILE* f = fopen(path, "wb");
        if (f) {
            fwrite(maps_text.data(), 1, maps_text.size(), f);
            fclose(f);
            log.say("карта    : %s", path);
        }
    }

    // ---- скан --------------------------------------------------------------
    if (regions.empty()) parse_regions(maps_text, &regions);
    std::vector<Region> cand;
    std::uint64_t cand_bytes = 0;
    for (const Region& r : regions)
        if (region_candidate(r)) {
            cand.push_back(r);
            cand_bytes += r.end - r.start;
        }
    std::stable_sort(cand.begin(), cand.end(),
                     [](const Region& a, const Region& b) {
                         return region_score(a) < region_score(b);
                     });
    if (cand.empty()) {
        log.say("!! в карте нет областей, где может лежать метадата — "
                "игра ещё грузится?");
        return 2;
    }
    log.say("к скану   : %d областей, %.0f МБ", (int)cand.size(),
            (double)cand_bytes / (1024.0 * 1024.0));

    std::vector<Blob> blobs;
    scan_regions(cand, &blobs, &log);
    if (blobs.empty()) {
        log.say("!! магия global-metadata не найдена.");
        log.say("   Возможные причины: игра не догрузилась; метадата в "
                "страницах, закрытых для транспорта (см. ошибки EIO выше) — "
                "тогда перезапусти другим режимом: xvcen dump external");
        return 2;
    }
    if (blobs.size() > 1)
        log.say("найдено %d копий — игра перезапускалась/обновлялась; "
                "свежая обычно последняя в логе, сверяй версию и размер",
                (int)blobs.size());

    // ---- выгрузка ----------------------------------------------------------
    int ok_count = 0;
    for (size_t i = 0; i < blobs.size(); ++i) {
        char path[160];
        if (i == 0)
            snprintf(path, sizeof(path), "%s/global-metadata.dat",
                     dump_dir.c_str());
        else
            snprintf(path, sizeof(path), "%s/global-metadata-%d.dat",
                     dump_dir.c_str(), (int)i + 1);
        log.say("пишу %s ...", path);
        if (dump_blob(blobs[i], path, &log)) ++ok_count;
    }

    log.say("итог     : %d из %d блобов выгружено целиком в %s",
            ok_count, (int)blobs.size(), dump_dir.c_str());
    if (ok_count > 0)
        log.say("дальше   : libil2cpp.so из APK + global-metadata.dat -> "
                "Il2CppDumper -> свежий dump.cs для tools/offsets");
    return ok_count > 0 ? 0 : 3;
}

}  // namespace metadump
