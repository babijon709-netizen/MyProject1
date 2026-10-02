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
// 0xFAB11BAF и заголовок из пар (offset, size) таблиц. Сканируем ВСЕ
// читаемые области карты игры блоками по 64 КБ — анонимные кучи первыми
// (там блоб почти всегда), но и файловые отображения, «(deleted)»/memfd
// (протекторы расшифровывают метадату в memfd), расшаренные и мелкие
// области: фильтр «только anon+.apk» на реальной игре пропуск блоба.
// Магию ищем на КАЖДОМ байте — malloc даёт выравнивание, но протектор
// может специально положить блоб с нечётным смещением. Страница, не
// читающаяся транспортом, не роняет область: чтение дробится до 4 КБ,
// дыра пропускается (протектор закрывает страницу-заголовок, а тело
// оставляет читаемым). Кандидат проверяется по заголовку: версия
// 16..39 (или 104/106 — новые недокументированные), первая таблица
// начинается ровно за заголовком, пары идут по возрастанию, размер =
// max(offset+size) ≥ 1 МБ. ОТВЕРГНУТЫЙ кандидат попадает в лог с вер-
// сией и причиной — «магия есть, но не прошла проверку» видно сразу.
// Если магии нет вовсе (протектор затёр sanity), блоб ищется по стро-
// чной метке: в строковой таблице метадаты всегда есть «Assembly-
// CSharp.dll»/«mscorlib.dll» (ASCII; в управляемой куче строки UTF-16,
// метке не мешают) — от находки идём назад до заголовка, чьи таблицы
// накрывают эту строку. Если игра перезапускалась, в памяти могут
// остаться СТАРЫЕ блобы — сохраняем каждый отдельным файлом, адреса и
// версии в логе.
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

// Приемлемые версии метадаты. Il2CppDumper поддерживает 16..31; свежие
// форки — до 39; 104/106 — недокументированные новые форматы. Значение
// нужно только чтобы отсечь случайные совпадения магии, поэтому берём
// с запасом: отвергнутое всё равно печатается в лог с версией.
static bool version_ok(std::uint32_t v) {
    return (v >= 16 && v <= 39) || v == 104 || v == 106;
}

// Минимальный размер блоба метаданных реальной игры: строковый пул имён
// классов/полей один занимает мегабайты. Отсекает мелкие совпадения магии.
static constexpr std::uint64_t kMinBlobSize = 1ull << 20;

// Блок чтения при скане и выгрузке: крупный, чтобы транспорт реже гнал
// запросы (чанкование по 4К он делает сам), и не большой, чтобы прогресс
// был виден.
static constexpr size_t kChunk = 1u << 16;

// Размер страницы: до него дробится чтение, когда в области попадаются
// нечитаемые страницы (закрытые протектором).
static constexpr size_t kPage = 4096;

// Строковые метки для поиска блоба с затёртой магией: имена образов из
// строковой таблицы метадаты — есть в любой игре (сборка самой игры и
// стандартная mscorlib). ASCII-поиск: управляемая куча хранит строки
// UTF-16 и ложных попаданий почти не даёт.
static const char* const kStringMarkers[] = {
    "Assembly-CSharp.dll",
    "mscorlib.dll",
};
static constexpr size_t kMaxMarkerLen = 24;   // самая длинная метка + запас
static constexpr size_t kMaxMarkerHits = 1024;

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

// Годится ли область под скан: ЛЮБАЯ читаемая область карты. Первая
// версия сканировала только анонимные кучи и .apk — на реальной игре
// (Oxide, 172 области) блоб в них не оказался: протекторы держат
// метадату в memfd («/memfd:... (deleted)» — раньше отбрасывалось
// фильтром), в извлечённых файлах с любым именем (в т.ч. маскирующихся
// под .so), в расшаренных отображениях, а закрытая страница-заголовок
// режет отображение на куски, и тело блоба может лежать в мелкой
// области <1 МБ. Поэтому фильтра по имени/размеру больше нет — только
// «читаемая» и «не меньше страницы»; приоритет задаёт region_score.
static bool region_candidate(const Region& r) {
    if (r.perms[0] != 'r') return false;
    if (r.end - r.start < kPage) return false;
    return true;
}

// Порядок скана: сначала анонимные изменяемые области (метадата почти всегда
// там), потом [heap], .apk и прочие файловые, read-only анонимные и в конце
// «(deleted)» (мусор старых образов). Найденный блоб выгружается после
// скана — при долгом полном проходе можно прерваться, дождавшись нужной
// находки.
static int region_score(const Region& r) {
    const bool writable = r.perms[1] == 'w';
    static const char kDeleted[] = " (deleted)";
    const size_t dlen = sizeof(kDeleted) - 1;
    const bool deleted = r.name.size() > dlen &&
        r.name.compare(r.name.size() - dlen, dlen, kDeleted) == 0;
    if (deleted) return 5;
    if (r.name.empty() || r.name.rfind("[anon:", 0) == 0) return writable ? 0 : 4;
    if (r.name == "[heap]") return 1;
    return writable ? 2 : 3;
}

// ---- блоб метаданных --------------------------------------------------------

struct Blob {
    std::uint64_t addr = 0;
    std::uint64_t size = 0;
    std::uint32_t version = 0;
    // Заголовок блоба был зашифрован протектором и найден расшифровкой:
    // каждый u32 XOR-ится key, первые три u32 (sanity, version, first_off)
    // переставлены схемой perm (0 = без перестановки). Выгрузка пишет файл
    // УЖЕ расшифрованным — Il2CppDumper берёт его как есть.
    std::uint32_t descramble_key = 0;
    int          descramble_perm = 0;
};

// Проверка заголовка по месту найденной магии. Структурная, без таблицы
// версий: первая пара заголовка указывает на первую таблицу, а та лежит
// сразу за заголовком — значит offset[0] == размеру заголовка (8 + 8*N).
// Дальше пары (offset, size) идут по файлу по возрастанию. Размер блоба =
// max(offset+size). Если структура не сошлась (неизвестная версия), берём
// максимум по правдоподобным парам первых 0x140 байт — с запасом, лишний
// хвост Il2CppDumper не мешает.
// reason_out (если задан) получает короткое имя причины отказа — для лога
// отвергнутых кандидатов: «магия есть, но не прошла проверку» должно быть
// видно в логе, а не молчить. must_cover_rel (если задан) — смещение строки-
// метки от начала блоба: тогда дополнительно требуется, чтобы какая-то
// таблица накрывала её (маркерный путь: заголовок ищем при затёртой магии).
static bool validate_blob_header(const unsigned char* hdr, size_t hdr_len,
                                 std::uint64_t addr, std::uint64_t region_end,
                                 std::uint32_t* version_out,
                                 std::uint64_t* size_out,
                                 const char** reason_out = nullptr,
                                 std::uint64_t must_cover_rel = ~0ull,
                                 bool require_magic = true) {
    if (reason_out) *reason_out = nullptr;
    if (hdr_len < 0x48) { if (reason_out) *reason_out = "заголовок короче 0x48"; return false; }
    std::uint32_t sanity = 0, version = 0;
    memcpy(&sanity, hdr, 4);
    memcpy(&version, hdr + 4, 4);
    // Маркерный путь (require_magic = false) ищет заголовок при затёртой
    // протектором магии — sanity печатается вызывающим для протокола.
    if (require_magic && sanity != kMetaMagic) {
        if (reason_out) *reason_out = "не магия";
        return false;
    }
    if (!version_ok(version)) {
        if (reason_out) *reason_out = "версия вне диапазона";
        return false;
    }

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
            bool covered = false;   // маркер лежит внутри одной из таблиц
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
                // Маркер обязан лежать внутри какой-то таблицы блоба.
                if (must_cover_rel != ~0ull &&
                    must_cover_rel >= off && must_cover_rel < end)
                    covered = true;
            }
            // Строку не накрыла ни одна таблица — заголовок не её.
            if (structural && must_cover_rel != ~0ull && !covered)
                structural = false;
        }
    }
    if (!structural) {
        if (must_cover_rel != ~0ull) {
            // Маркерный путь без СОШЕДШЕЙСЯ структуры не принимаем. Раньше
            // здесь была дыра: «накрыть» маркер успевала первая попавшаяся
            // мусорная пара, и валидатор скатывался в эвристику размера —
            // так в дамп от 14.10 попали три области по 243/190/16 МБ
            // (dalvik-куча и структуры протектора), ни одна не метадата.
            if (reason_out) *reason_out = "структура заголовка не сошлась";
            return false;
        }
        for (size_t p = 8; p + 8 <= hdr_len && p < 0x140; p += 8) {
            std::uint32_t off = 0, sz = 0;
            memcpy(&off, hdr + p, 4);
            memcpy(&sz, hdr + p + 4, 4);
            if (off < 8 || off > 0x40000000u || sz > 0x20000000u) continue;
            const std::uint64_t end = (std::uint64_t)off + sz;
            if (end > max_end) max_end = end;
        }
    }

    if (max_end < kMinBlobSize) {
        if (reason_out) *reason_out = "размер блоба < 1 МБ";
        return false;
    }
    // Блоб — одно выделение и не может выйти за пределы отображения.
    if (max_end > region_end - addr) max_end = region_end - addr;
    if (max_end < kMinBlobSize) {
        if (reason_out) *reason_out = "блоб вылезает за область";
        return false;
    }
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

// ---- строковые метки (путь при затёртой магии) ------------------------------

struct MarkerHit {
    std::uint64_t addr = 0;
    const char* marker = nullptr;
};

// Быстрый поиск подстроки без memmem (bionic прячет его за _GNU_SOURCE):
// первый редкий байт ловится SIMD-овым memchr, дальше короткий memcmp.
static const unsigned char* find_bytes(const unsigned char* hay, size_t hay_len,
                                       const char* needle, size_t needle_len) {
    if (needle_len == 0 || hay_len < needle_len) return nullptr;
    const unsigned char* p = hay;
    const unsigned char* end = hay + hay_len - needle_len;
    while (p <= end) {
        const unsigned char* q = (const unsigned char*)memchr(p, (unsigned char)needle[0],
                                                              (size_t)(end - p + 1));
        if (!q) return nullptr;
        if (memcmp(q, needle, needle_len) == 0) return q;
        p = q + 1;
    }
    return nullptr;
}

// Поиск меток в буфере; record_below != 0 — записывать только находки
// ДО этого адреса (режим сшивки на границе блоков: метка, начавшаяся в
// хвосте прошлого блока, целиком лежит только в сшивке).
static void find_markers(const unsigned char* buf, size_t len, std::uint64_t addr,
                         std::vector<MarkerHit>* hits,
                         std::uint64_t record_below = 0) {
    for (const char* m : kStringMarkers) {
        const size_t mlen = strlen(m);
        const unsigned char* p = buf;
        for (;;) {
            if (hits->size() >= kMaxMarkerHits) return;
            const unsigned char* q = find_bytes(p, (size_t)(buf + len - p), m, mlen);
            if (!q) break;
            const std::uint64_t at = addr + (std::uint64_t)(q - buf);
            if (!record_below || at < record_below)
                hits->push_back({at, m});
            p = q + 1;
        }
    }
}

// Протектор может шифровать заголовок: каждый u32 XOR-ится ключом, а первые
// три u32 (sanity, version, first_off) — вдобавок переставлены. Ключ не
// перебираем: он выводится из самой магии, key = raw[sanity_slot] ^ 0xFAB11BAF.
// kTriplePerms[p][j] — из какого raw-слота пришёл j-й расшифрованный u32.
static const int kTriplePerms[6][3] = {
    {0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0},
};

// Расшифровка буфера: u32 XOR ключом (хвост <4 байт — побайтно младшими
// байтами ключа); perm применяется один раз к первым 12 байтам файла.
static void descramble_buf(unsigned char* buf, size_t len, std::uint32_t key,
                           int perm, bool apply_perm) {
    if (!key) return;
    const size_t n = len / 4;
    for (size_t k = 0; k < n; ++k) {
        std::uint32_t v;
        memcpy(&v, buf + 4 * k, 4);
        v ^= key;
        memcpy(buf + 4 * k, &v, 4);
    }
    for (size_t k = n * 4; k < len; ++k)
        buf[k] ^= (unsigned char)(key >> (8 * (k & 3)));
    if (apply_perm && perm > 0) {
        std::uint32_t t[3];
        for (int j = 0; j < 3; ++j)
            memcpy(&t[j], buf + 4 * kTriplePerms[perm][j], 4);
        memcpy(buf, t, 12);
    }
}

// Сканирование одного прочитанного блока: магию ищем на КАЖДОМ байте —
// malloc/mmap дают выравнивание, но протектор может положить блоб со
// смещением специально, и шаг в 4 байта его пропускал. Отвергнутый
// кандидат печатается в лог с версией и причиной, а не молчит: «магия
// есть, но проверка её режет» должно быть видно в dump.log. Совпадения
// внутри уже найденного блоба — мусор, пропускаем.
static void scan_block(const unsigned char* buf, size_t len, std::uint64_t addr,
                       std::uint64_t region_end, std::vector<Blob>* blobs,
                       Log* log) {
    static int s_rejected = 0;
    for (size_t i = 0; i + 4 <= len; ++i) {
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
        if (!g_mem.read_quiet(at, hdr, (size_t)want)) {
            log->say("   ?? магия по адресу 0x%llx — заголовок не читается",
                     (unsigned long long)at);
            continue;
        }

        std::uint32_t version = 0;
        std::uint64_t size = 0;
        const char* reason = nullptr;
        if (!validate_blob_header(hdr, (size_t)want, at, region_end, &version,
                                  &size, &reason)) {
            if (s_rejected < 30) {
                ++s_rejected;
                std::uint32_t got_ver = 0;
                memcpy(&got_ver, hdr + 4, 4);
                log->say("   ?? магия по адресу 0x%llx: версия %u — %s%s",
                         (unsigned long long)at, (unsigned)got_ver,
                         reason ? reason : "?",
                         s_rejected == 30 ? " (дальше отвергнутые не пишу)" : "");
            }
            continue;
        }
        Blob b;
        b.addr = at;
        b.size = size;
        b.version = version;
        blobs->push_back(b);
        log->say("  ++ global-metadata v%u: адрес 0x%llx, %.1f МБ",
                 (unsigned)version, (unsigned long long)at,
                 (double)size / (1024.0 * 1024.0));
    }

    // Второй проход — зашифрованные заголовки. Строки метадаты в памяти
    // лежат открытым текстом (иначе маркеры бы не находились), значит тело
    // блоба не шифровано и трогать его нельзя — расшифровке подлежит только
    // заголовок 0x210. Отсев дешёвый: версия после XOR должна сойтись,
    // первая пара — попасть в правдоподобный диапазон.
    static int s_descr_rejected = 0;
    for (size_t i = 0; i + 0x48 <= len; i += 4) {
        std::uint32_t t[3];
        memcpy(t, buf + i, 12);
        for (int perm = 0; perm < 6; ++perm) {
            const std::uint32_t key = t[kTriplePerms[perm][0]] ^ kMetaMagic;
            if (key == 0) continue;
            const std::uint32_t ver = t[kTriplePerms[perm][1]] ^ key;
            if (!version_ok(ver)) continue;
            const std::uint32_t fo = t[kTriplePerms[perm][2]] ^ key;
            if (fo < 0x48 || fo > 0x400 || (fo % 8) != 0) continue;

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
            if (want < 0x48) continue;
            unsigned char raw[0x210], dec[0x210];
            if (!g_mem.read_quiet(at, raw, (size_t)want)) continue;
            memcpy(dec, raw, (size_t)want);
            descramble_buf(dec, (size_t)want, key, perm, /*apply_perm=*/true);

            std::uint32_t version = 0;
            std::uint64_t size = 0;
            const char* reason = nullptr;
            if (!validate_blob_header(dec, (size_t)want, at, region_end,
                                      &version, &size, &reason,
                                      /*must_cover_rel=*/~0ull,
                                      /*require_magic=*/true)) {
                if (s_descr_rejected < 10) {
                    ++s_descr_rejected;
                    log->say("   ?? зашифрованный кандидат 0x%llx "
                             "(ключ 0x%08x): %s",
                             (unsigned long long)at, (unsigned)key,
                             reason ? reason : "?");
                }
                continue;
            }
            Blob b;
            b.addr = at;
            b.size = size;
            b.version = version;
            b.descramble_key = key;
            b.descramble_perm = perm;
            blobs->push_back(b);
            log->say("  ++ global-metadata v%u, заголовок зашифрован "
                     "(XOR 0x%08x%s): адрес 0x%llx, %.1f МБ — файл запишу "
                     "с расшифрованным заголовком",
                     (unsigned)version, (unsigned)key,
                     perm ? ", перестановка троек" : "",
                     (unsigned long long)at,
                     (double)size / (1024.0 * 1024.0));
        }
    }
}

// Чтение с терпимостью к дырам: нечитаемая страница не должна ронять всю
// область — протектор закрывает страницу-заголовок, а тело блоба оставляет
// читаемым, и за дырой метадата продолжается. Чтение дробится до 4 КБ;
// дыры заполняются нулями и считаются (в лог области — сколько вылетело).
static void read_span_holes(std::uint64_t at, unsigned char* buf, size_t want,
                            std::uint64_t* holes) {
    if (g_mem.read_quiet(at, buf, want)) return;
    if (want <= kPage) {
        memset(buf, 0, want);
        *holes += want;
        return;
    }
    const size_t half = want / 2;
    read_span_holes(at, buf, half, holes);
    read_span_holes(at + half, buf + half, want - half, holes);
}

static void scan_regions(const std::vector<Region>& regions,
                         std::vector<Blob>* blobs,
                         std::vector<MarkerHit>* markers,
                         std::uint64_t* scanned_total, Log* log) {
    static unsigned char buf[kChunk];
    static unsigned char stitch[kMaxMarkerLen * 2];
    std::uint64_t scanned = 0;
    if (scanned_total) *scanned_total = 0;
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
        std::uint64_t holes = 0;
        size_t carry = 0;                       // хвост прошлого блока для меток
        while (done < size) {
            const std::uint64_t left = size - done;
            const size_t want =
                (size_t)(left < kChunk ? left : kChunk);
            const std::uint64_t at = r.start + done;
            // Хвост уже найденного блоба перечитывать незачем.
            if (!inside_any_blob(*blobs, at, want)) {
                read_span_holes(at, buf, want, &holes);
                scan_block(buf, want, at, r.end, blobs, log);
                if (markers) {
                    find_markers(buf, want, at, markers);
                    // Сшивка границы блоков: метка могла начаться в хвосте
                    // прошлого блока — в нём самом она не поместилась.
                    if (carry > 0) {
                        const size_t head = want < kMaxMarkerLen ? want : kMaxMarkerLen;
                        memcpy(stitch + carry, buf, head);
                        find_markers(stitch, carry + head, at - carry, markers, at);
                    }
                }
                if (markers) {
                    carry = want < kMaxMarkerLen ? want : kMaxMarkerLen;
                    memcpy(stitch, buf + want - carry, carry);
                }
            } else {
                carry = 0;   // внутри найденного блоба сшивку меток не ведём
            }
            done += want;
            scanned += want;
            if ((scanned / (64u << 20)) != ((scanned - want) / (64u << 20)))
                log->say("   ... прочитано %.0f МБ",
                         (double)scanned / (1024.0 * 1024.0));
        }
        if (holes)
            log->say("   !! нечитаемо %.1f МБ из %.1f МБ (закрытые страницы)",
                     (double)holes / (1024.0 * 1024.0),
                     (double)size / (1024.0 * 1024.0));
    }
    if (scanned_total) *scanned_total = scanned;
}

// Маркерный путь: от строки-метки («Assembly-CSharp.dll» из строковой
// таблицы метадаты) идём назад в поисках заголовка, чьи таблицы НАКРЫВАЮТ
// эту строку. Так блоб находится, даже если протектор затёр или заменил
// магию — реальный sanity печатается для протокола.
static bool blob_from_marker(const MarkerHit& hit, const Region& region,
                             std::vector<Blob>* blobs, Log* log) {
    for (const Blob& b : *blobs)
        if (hit.addr >= b.addr && hit.addr < b.addr + b.size)
            return false;                       // метка внутри найденного блоба
    // Окно с запасом 0x210: кандидаты у верхней границы окна читают заголовок,
    // выходящий ЗА окно (win_hi..win_hi+0x210). Без запаса чтение писало мимо
    // буфера (переполнение 0x210 байт) и портило соседние статики — валидатор
    // принимал «заголовки» из повреждённых байт.
    static unsigned char win[(1u << 20) + 0x210];
    const std::uint64_t kWin = 1u << 20;
    const std::uint64_t room = hit.addr - region.start;
    const std::uint64_t max_back = room > (160ull << 20) ? (160ull << 20) : room;
    std::uint64_t back = 0;
    while (back < max_back) {
        const std::uint64_t win_hi = hit.addr - back;   // кандидаты строго ниже
        std::uint64_t win_lo = win_hi > kWin ? win_hi - kWin : region.start;
        if (win_lo < region.start) win_lo = region.start;
        const std::uint64_t read_hi =
            (win_hi + 0x210 < region.end) ? win_hi + 0x210 : region.end;
        if (read_hi <= win_lo || read_hi - win_lo < 0x48) break;
        std::uint64_t holes = 0;
        read_span_holes(win_lo, win, (size_t)(read_hi - win_lo), &holes);
        // Кандидат — начало заголовка ниже метки; окно читается с запасом
        // 0x210, чтобы заголовок, начинающийся вплотную к метке, вошёл в
        // буфер целиком (условие «h + 0x210 <= win_hi» такое начало теряло).
        for (std::uint64_t h = win_lo; h < win_hi && h + 0x210 <= read_hi; h += 4) {
            const unsigned char* hdr = win + (size_t)(h - win_lo);
            std::uint32_t ver = 0;
            memcpy(&ver, hdr + 4, 4);
            if (!version_ok(ver)) continue;     // дешёвый отсев мусора
            std::uint32_t version = 0;
            std::uint64_t size = 0;
            if (validate_blob_header(hdr, (size_t)(read_hi - h), h, region.end,
                                     &version, &size, nullptr, hit.addr - h,
                                     /*require_magic=*/false)) {
                std::uint32_t sanity = 0;
                memcpy(&sanity, hdr, 4);
                if (sanity != kMetaMagic)
                    log->say("   !! магия заголовка изменена протектором: "
                             "0x%08x вместо 0x%08x",
                             (unsigned)sanity, (unsigned)kMetaMagic);
                Blob b;
                b.addr = h;
                b.size = size;
                b.version = version;
                blobs->push_back(b);
                log->say("  ++ global-metadata v%u по строковой метке («%s»): "
                         "адрес 0x%llx, %.1f МБ",
                         (unsigned)version, hit.marker,
                         (unsigned long long)h,
                         (double)size / (1024.0 * 1024.0));
                return true;
            }
        }
        back += win_hi - win_lo;
    }
    return false;
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
    if (b.descramble_key)
        log->say("   заголовок блоба был зашифрован (XOR 0x%08x) — первые "
                 "0x210 байт файла записываю расшифрованными",
                 (unsigned)b.descramble_key);
    std::uint64_t done = 0, failed = 0;
    while (done < b.size) {
        const std::uint64_t left = b.size - done;
        const size_t want = (size_t)(left < kChunk ? left : kChunk);
        // Дыры (закрытые страницы) — нулями, чтобы файл не съезжал; счётчик
        // потерь скажет, сколько вылетело. Весь блоб в дырах — файл не пишем.
        read_span_holes(b.addr + done, buf, want, &failed);
        // Зашифрован только заголовок (таблицы в памяти открыты — иначе
        // строковые маркеры не находились бы), тело пишем как есть.
        if (b.descramble_key && done < 0x210) {
            const size_t head =
                (size_t)((0x210 - done) < want ? (0x210 - done) : want);
            descramble_buf(buf, head, b.descramble_key, b.descramble_perm,
                           /*apply_perm=*/done == 0);
        }
        fwrite(buf, 1, want, f);
        done += want;
        if ((done / (8u << 20)) != ((done - want) / (8u << 20)))
            log->say("   ... %.0f / %.0f МБ", (double)done / (1024.0 * 1024.0),
                     (double)b.size / (1024.0 * 1024.0));
    }
    fclose(f);
    if (failed >= b.size) {
        remove(path);
        log->say("!! 0x%llx: страницы метаданных не читаются транспортом — "
                 "попробуй другой режим (external/ft/qx/rt)",
                 (unsigned long long)b.addr);
        return false;
    }
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
        log.say("!! в карте нет читаемых областей — игра ещё грузится?");
        return 2;
    }
    log.say("к скану   : %d областей, %.0f МБ (все читаемые области карты)",
            (int)cand.size(), (double)cand_bytes / (1024.0 * 1024.0));

    std::vector<Blob> blobs;
    std::vector<MarkerHit> markers;
    std::uint64_t scanned = 0;
    scan_regions(cand, &blobs, &markers, &scanned, &log);

    // Дедупликация меток: сшивка границ блоков может записать находку дважды.
    {
        std::sort(markers.begin(), markers.end(),
                  [](const MarkerHit& a, const MarkerHit& b) { return a.addr < b.addr; });
        markers.erase(std::unique(markers.begin(), markers.end(),
                                  [](const MarkerHit& a, const MarkerHit& b) {
                                      return a.addr == b.addr;
                                  }),
                      markers.end());
    }

    // Путь при затёртой магии: заголовок ищем по строковой метке. Запускаем
    // и после удачного скана по магии — копий блоба может быть несколько, и
    // активная как раз с изменённым заголовком.
    {
        int by_marker = 0;
        for (const MarkerHit& hit : markers) {
            const Region* owner = nullptr;
            for (const Region& r : cand)
                if (hit.addr >= r.start && hit.addr < r.end) {
                    owner = &r;
                    break;
                }
            if (owner && blob_from_marker(hit, *owner, &blobs, &log))
                ++by_marker;
        }
        if (by_marker)
            log.say("по меткам : %d блоб(ов) найдено без магии", by_marker);
        else if (!markers.empty() && blobs.empty())
            log.say("строковые метки метадаты видны (%d шт.), но заголовок "
                    "к ним не подобрался — вероятно, закрыт страницами",
                    (int)markers.size());
    }

    if (blobs.empty()) {
        log.say("!! метадата не найдена: магии 0x%08x не встретилось за %.0f МБ "
                "скана.", (unsigned)kMetaMagic,
                (double)scanned / (1024.0 * 1024.0));
        log.say("   Значит, протектор держит её нестандартно. Что попробовать:");
        if (backend != ESP_MEM_EXTERNAL)
            log.say("   1) другой режим чтения: xvcen dump external");
        else
            log.say("   1) режимы драйвера видят страницы, закрытые для "
                    "прямого чтения: xvcen dump ft | qx | rt");
        log.say("   2) подожди пару минут на сервере (метадата могла ещё не "
                "расшифроваться) и перезапусти дамп");
        log.say("   3) пришли %s/dump.log и maps-%d.txt — по ним видно, "
                "куда протектор спрятал блоб", dump_dir.c_str(), (int)used_pid);
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
