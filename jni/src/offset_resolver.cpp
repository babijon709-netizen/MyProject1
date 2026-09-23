// offset_resolver.cpp — автообновление оффсетов по IL2CPP-метаданным живого
// процесса игры (идея «нетвар-менеджера»: оффсеты не только вшиты, но и
// вычитываются из самой игры по именам классов и полей).
//
// Зачем: после апдейта игры оффсеты полей и слоты классов меняются, и раньше
// чит молчал, пока оффсеты не обновят руками по новому дампу. Теперь при
// привязке модуль сам:
//   1) находит все отображения libil2cpp.so (кроме исполняемых) и окно
//      строк метаданных (global-metadata) по /proc/<pid>/maps;
//   2) сканирует эти отображения ПОРЦИЯМИ (бюджет времени на кадр) в поиске
//      слотов-указателей на Il2CppClass: у настоящего класса имя и
//      пространство имён — указатели в окно метаданных, а image — в сам
//      libil2cpp. Так собирается карта «короткое имя -> Il2CppClass*»,
//      которая заменяет *_TYPEINFO_RVA (те больше не обязаны переживать
//      апдейт игры);
//   3) калибрует смещение массива полей в Il2CppClass на живом классе
//      (сначала проверяет табличное 0x80, не сошлось — ищет рядом) и
//      для каждой записи сгенерированной таблицы (offset_resolver_table.inc)
//      читает смещение поля по его имени из FieldInfo;
//   4) записывает найденное в активные слоты go_active. Что не нашлось —
//      остаётся на вшитом значении (резолвер никогда не делает хуже, чем было).
//
// Раскладка самого рантайма il2cpp (Il2CppClass/FieldInfo) от билда игры не
// зависит — см. константы IL2CPP_CLASS_*/IL2CPP_FIELDINFO_* в заголовках
// оффсетов и комментарий там, чем они подтверждены.
//
// Имена классов с ротацией (обфускатор) резолвер не ищет: они помечены в
// карте оффсетов и остаются на ручных значениях.

#include "game.h"
#include "game_internal.h"
#include "game_offsets_active.h"
#include "logfile.h"

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

#include <string>

using namespace go_active;

// ---- Таблица: какие константы каким (класс, поле) заполнять ----------------
// Сгенерирована из tools/offsets/offsets_map.json (gen_resolver_table.py).
namespace {
struct OrField {
    std::uint64_t* slot;
    const char*    cls;     // короткое имя класса в рантайме
    const char*    field;   // имя поля (стабильное, из дампа)
    bool           is_static;
};
#define OR_FIELD(c, k, f, s) { &go_active::c, k, f, s },
static const OrField kOrFields[] = {
#include "offset_resolver_table.inc"
};
#undef OR_FIELD
constexpr int kOrFieldCount = (int)(sizeof(kOrFields) / sizeof(kOrFields[0]));
}  // namespace

namespace {

// Один найденный класс. Имя/пространство имён храним как есть (короткое имя
// — то, что лежит в Il2CppClass.name; вложенные классы зовутся внутренним
// именем, поэтому таблица и сопоставляет по короткому).
struct FoundClass {
    uint64_t klass = 0;
    char     name[48] = {};
    char     ns[40] = {};
};

struct Segment { uint64_t start = 0, end = 0; };

// Фазы. Сканы порционные: на устройство с придушенным процессом нельзя
// вываливать десятки мегабайт чтений одним кадром (см. скан маркеров).
enum Phase {
    PH_WAIT = 0,    // нет привязки
    PH_MAPS,        // разобрать /proc/<pid>/maps
    PH_SCAN,        // поиск слотов Il2CppClass
    PH_FIELDS,      // калибровка массива полей + резолв таблицы
    PH_DONE,
};

Phase    s_phase = PH_WAIT;
Segment  s_segments[12];
int      s_seg_count = 0;
int      s_seg_idx = 0;
uint64_t s_scan_pos = 0;          // текущая позиция в текущем сегменте
uint64_t s_lib_begin = 0, s_lib_end = 0;   // окна libil2cpp (для image-фильтра)
uint64_t s_meta_begin = 0, s_meta_end = 0; // окно строк метаданных (0 = нет)
uint64_t s_total_bytes = 0;       // сколько всего байт предстоит просмотреть
uint64_t s_scanned_bytes = 0;     // сколько уже просмотрено (для статуса)
double   s_scan_deadline = 0;     // общего бюджета скана

FoundClass s_classes[768];
int      s_class_count = 0;
int      s_names_ok = 0, s_names_fail = 0;

uint64_t s_fields_off = 0;        // откалиброванное смещение Il2CppClass.fields
int      s_resolved = 0, s_changed = 0;

struct AppliedRec { std::uint64_t* slot; std::uint64_t value; };
AppliedRec s_applied[256];
int      s_applied_count = 0;
bool     s_logged = false;

constexpr double kTickBudgetSec   = 0.004;  // 4 мс на такт (как у маркеров)
constexpr double kScanTotalSec    = 60.0;   // общий потолок скана (образы по 0.5ГБ+)
constexpr size_t kChunkBytes      = 256 * 1024;
constexpr size_t kMaxReadsPerChunk = 512;   // предел «дорогих» чтений на порцию

static bool all_wanted_found() {
    // Достаточно: все РАЗНЫЕ имена из таблицы найдены хотя бы одним классом.
    for (int i = 0; i < kOrFieldCount; ++i) {
        bool found = false;
        for (int j = 0; j < s_class_count && !found; ++j)
            found = strcmp(s_classes[j].name, kOrFields[i].cls) == 0;
        if (!found) return false;
    }
    return true;
}

static FoundClass* find_class_slot(uint64_t klass) {
    for (int i = 0; i < s_class_count; ++i)
        if (s_classes[i].klass == klass) return &s_classes[i];
    return nullptr;
}

// ---- Фаза MAPS: окна libil2cpp (не исполняемые) и метаданных ---------------
static void phase_maps() {
    s_seg_count = 0;
    s_lib_begin = s_lib_end = 0;
    s_meta_begin = s_meta_end = 0;
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", (int)g_pid);
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { s_phase = PH_DONE; return; }
    std::string text;
    char chunk[8192];
    for (;;) {
        const ssize_t n = read(fd, chunk, sizeof(chunk));
        if (n > 0) { text.append(chunk, (size_t)n); if (text.size() > (8u << 20)) break; continue; }
        if (n < 0) break;
        break;
    }
    close(fd);

    size_t pos = 0;
    while (pos < text.size() && s_seg_count < 12) {
        const size_t eol = text.find('\n', pos);
        const size_t line_end = (eol == std::string::npos) ? text.size() : eol;
        // Строка обязана быть NUL-терминированной: иначе strchr/strstr идут
        // ПО ВСЕМУ остатку файла и «находят» чужие отображения (плавающий
        // провал: окно метаданных раздувалось на полкарты, сегменты терялись).
        char linebuf[1024];
        size_t line_len = line_end - pos;
        if (line_len >= sizeof(linebuf)) line_len = sizeof(linebuf) - 1;
        memcpy(linebuf, text.c_str() + pos, line_len);
        linebuf[line_len] = '\0';
        const char* line = linebuf;
        pos = line_end + 1;

        unsigned long long start = 0, end = 0;
        char perms[5] = {};
        if (sscanf(line, "%llx-%llx %4s", &start, &end, perms) != 3) continue;
        if (end <= start || end - start > (256ull << 20)) continue;
        const char* slash = strchr(line, '/');
        if (!slash) continue;

        if (strstr(slash, "global-metadata") && perms[0] == 'r') {
            // Окно строк метаданных: имя/пространства имён классов лежат тут.
            if (!s_meta_begin || start < s_meta_begin) s_meta_begin = start;
            if (end > s_meta_end) s_meta_end = end;
            continue;
        }
        if (!strstr(slash, "libil2cpp.so")) continue;
        if (strstr(line, "(deleted)")) continue;
        if (perms[2] == 'x') continue;   // текст не сканируем
        if (g_il2cpp_base && (start < g_il2cpp_base || start > g_il2cpp_base + (320ull << 20)))
            continue;  // чужой/мёртвый образ: держимся вокруг выбранной базы
        s_segments[s_seg_count].start = start;
        s_segments[s_seg_count].end = end;
        ++s_seg_count;
        if (!s_lib_begin || start < s_lib_begin) s_lib_begin = start;
        if (end > s_lib_end) s_lib_end = end;
    }
    if (!s_seg_count) { s_phase = PH_DONE; return; }
    // Сканируем с КОНЦА образа: сегменты идут по возрастанию адреса, а слоты
    // классов лежат в .data/.relro ближе к концу; огромный .rodata — в начале.
    // Так классы находятся за первые секунды, а не после минутного прохода.
    for (int i = 0, j = s_seg_count - 1; i < j; ++i, --j) {
        const Segment t = s_segments[i];
        s_segments[i] = s_segments[j];
        s_segments[j] = t;
    }
    s_total_bytes = 0;
    for (int i = 0; i < s_seg_count; ++i)
        s_total_bytes += s_segments[i].end - s_segments[i].start;
    s_scanned_bytes = 0;
    s_seg_idx = 0;
    s_scan_pos = s_segments[0].start;
    s_scan_deadline = mono_seconds() + kScanTotalSec;
    s_phase = PH_SCAN;
}

// Попробовать кандидата как Il2CppClass: image в libil2cpp, имя/нс в
// метаданных; имя читается строкой. Возврат: 1 принят, 0 отклонён.
static int try_class_candidate(uint64_t v) {
    if (find_class_slot(v)) return 0;
    const uint64_t image = rd_ptr(v);
    if (s_lib_begin && (image < s_lib_begin || image >= s_lib_end)) return 0;
    else if (!s_lib_begin && !valid_obj(image)) return 0;
    const uint64_t nm = rd_ptr(v + IL2CPP_CLASS_NAME);
    if (s_meta_begin) {
        if (nm < s_meta_begin || nm >= s_meta_end) return 0;
        const uint64_t ns = rd_ptr(v + IL2CPP_CLASS_NAMESPACE);
        if (ns < s_meta_begin || ns >= s_meta_end) return 0;
    } else if (!valid_obj(nm)) {
        return 0;
    }
    bool ok = false;
    const std::string name = read_remote_string(nm, &ok);
    if (!ok || name.empty() || name.size() > 47) { ++s_names_fail; return 0; }
    ++s_names_ok;
    if (s_class_count >= 768) return 1;
    FoundClass& fc = s_classes[s_class_count];
    fc.klass = v;
    snprintf(fc.name, sizeof(fc.name), "%s", name.c_str());
    bool ns_ok = false;
    const std::string ns = read_remote_string(rd_ptr(v + IL2CPP_CLASS_NAMESPACE), &ns_ok);
    if (ns_ok && ns.size() < sizeof(fc.ns)) snprintf(fc.ns, sizeof(fc.ns), "%s", ns.c_str());
    ++s_class_count;
    return 1;
}

static void phase_scan(double deadline) {
    // Буфер осознанно статический: 256 КБ на стеке кадра на телефоне —
    // риск переполнения стека потока отрисовки. Резолвер однопоточный.
    static uint8_t buf[kChunkBytes];
    while (s_seg_idx < s_seg_count && mono_seconds() < deadline) {
        const Segment& seg = s_segments[s_seg_idx];
        const uint64_t remain = seg.end - s_scan_pos;
        if (remain < 8) {                 // сегмент дочитан
            ++s_seg_idx;
            if (s_seg_idx < s_seg_count) s_scan_pos = s_segments[s_seg_idx].start;
            continue;
        }
        const uint64_t chunk = (kChunkBytes < remain) ? kChunkBytes : remain;
        if (!rd_buf(s_scan_pos, buf, chunk)) {
            s_scan_pos += chunk;          // дыра в отображении — перешагнуть
            s_scanned_bytes += chunk;
        } else {
            // Идём по слотам и сдвигаем s_scan_pos ТОЛЬКО на просмотренное.
            // Если уткнулись в бюджет чтений — выходим, и следующий такт
            // продолжит ровно с этого места, а не выбросит хвост куска
            // (плотные «похожие на указатели» данные не должны голодать
            // настоящие классы, стоящие дальше в том же куске).
            size_t reads_used = 0;
            const size_t slots = chunk / 8;
            size_t examined = 0;
            for (; examined < slots; ++examined) {
                uint64_t v;
                memcpy(&v, buf + examined * 8, 8);
                v = memio::untag(v);
                if (valid_obj(v)) {
                    reads_used += try_class_candidate(v) ? 3 : 1;
                    if (reads_used >= kMaxReadsPerChunk) { ++examined; break; }
                }
            }
            s_scan_pos += examined * 8;
            s_scanned_bytes += examined * 8;
        }
        if (all_wanted_found()) break;
        if (mono_seconds() >= s_scan_deadline) break;
    }
    if (all_wanted_found() || s_seg_idx >= s_seg_count ||
        mono_seconds() >= s_scan_deadline) {
        s_phase = (s_class_count && s_names_ok) ? PH_FIELDS : PH_DONE;
    }
}

// ---- Фаза FIELDS: калибровка и резолв ---------------------------------------
// Поле-счётчик: сколько записей подряд похожи на FieldInfo этого класса.
static int fields_shape_score(uint64_t klass, uint64_t fields_ptr, int probe) {
    if (!valid_obj(fields_ptr)) return 0;
    int score = 0;
    for (int i = 0; i < probe; ++i) {
        const uint64_t e = fields_ptr + (uint64_t)i * IL2CPP_FIELDINFO_SIZE;
        const uint64_t parent = rd_ptr(e + IL2CPP_FIELDINFO_PARENT);
        if (parent != klass) break;
        const uint64_t type = rd_ptr(e + IL2CPP_FIELDINFO_TYPE);
        if (!valid_obj(type)) break;
        const int32_t off = rd<int32_t>(e + IL2CPP_FIELDINFO_OFFSET);
        if (off < -1 || off > 0x20000) break;
        ++score;
        const uint64_t nmp = rd_ptr(e + IL2CPP_FIELDINFO_NAME);
        if (s_meta_begin && (nmp < s_meta_begin || nmp >= s_meta_end)) break;
    }
    return score;
}

static bool calibrate_fields(uint64_t klass) {
    // Сначала табличное значение — почти всегда оно и есть.
    const uint64_t table_off = IL2CPP_CLASS_FIELDS;
    if (fields_shape_score(klass, rd_ptr(klass + table_off), 6) >= 5) {
        s_fields_off = table_off;
        return true;
    }
    for (uint64_t f = 0x70; f <= 0x178; f += 8) {
        if (f == table_off) continue;
        if (fields_shape_score(klass, rd_ptr(klass + f), 6) >= 5) {
            s_fields_off = f;
            return true;
        }
    }
    return false;
}

// Прочитать имя поля; пустой возврат при нечитаемости.
static bool read_field_name(uint64_t entry, char* out, size_t cap) {
    const uint64_t nmp = rd_ptr(entry + IL2CPP_FIELDINFO_NAME);
    if (s_meta_begin && (nmp < s_meta_begin || nmp >= s_meta_end)) return false;
    if (!s_meta_begin && !valid_obj(nmp)) return false;
    bool ok = false;
    const std::string name = read_remote_string(nmp, &ok);
    if (!ok || name.empty() || name.size() >= cap) return false;
    snprintf(out, cap, "%s", name.c_str());
    return true;
}

// Найти поле по имени в классе; вернуть смещение (-1 если нет).
static int32_t find_field_offset(uint64_t klass, const char* field) {
    const uint64_t fields_ptr = rd_ptr(klass + s_fields_off);
    if (!valid_obj(fields_ptr)) return -1;
    int count = rd<uint16_t>(klass + IL2CPP_CLASS_FIELD_COUNT);
    if (count <= 0 || count > 2048) return -1;
    char name[48];
    for (int i = 0; i < count; ++i) {
        const uint64_t e = fields_ptr + (uint64_t)i * IL2CPP_FIELDINFO_SIZE;
        if (rd_ptr(e + IL2CPP_FIELDINFO_PARENT) != klass) break;
        if (!read_field_name(e, name, sizeof(name))) continue;
        if (strcmp(name, field) == 0) return rd<int32_t>(e + IL2CPP_FIELDINFO_OFFSET);
    }
    return -1;
}

static void apply_resolution(const OrField& of, int32_t off) {
    const std::uint64_t value = (std::uint64_t)(uint32_t)off;
    if (of.is_static && (off < 0 || off > 0x4000)) return;
    if (!of.is_static && (off < 0 || off > 0x20000)) return;
    if (*(of.slot) != value) ++s_changed;
    *(of.slot) = value;
    if (s_applied_count < 256) {
        s_applied[s_applied_count].slot = of.slot;
        s_applied[s_applied_count].value = value;
        ++s_applied_count;
    }
    ++s_resolved;
}

static void phase_fields() {
    // Калибровка: первым пробуем самый «толстый» класс (больше полей —
    // надёжнее форма), дальше — любые найденные, пока не сойдётся.
    uint64_t first_choice = 0;
    for (int i = 0; i < s_class_count && !first_choice; ++i)
        if (strcmp(s_classes[i].name, "PlayerManager") == 0) first_choice = s_classes[i].klass;
    if (first_choice) calibrate_fields(first_choice);
    for (int i = 0; i < s_class_count && !s_fields_off && i < 12; ++i)
        calibrate_fields(s_classes[i].klass);
    if (!s_fields_off) { s_phase = PH_DONE; return; }

    for (int i = 0; i < kOrFieldCount; ++i) {
        const OrField& of = kOrFields[i];
        // Классов с одним коротким именем может быть несколько: берём первый,
        // в котором поле реально нашлось (остальные — тёзки).
        for (int j = 0; j < s_class_count; ++j) {
            if (strcmp(s_classes[j].name, of.cls) != 0) continue;
            const int32_t off = find_field_offset(s_classes[j].klass, of.field);
            if (off >= 0) { apply_resolution(of, off); break; }
        }
    }
    s_phase = PH_DONE;
}

static void log_summary() {
    if (s_logged) return;
    s_logged = true;
    LogLine("автооффсеты: классов найдено %d (имён прочитано %d, отказов %d), "
            "fields@0x%llx, разрешено %d/%d констант (%d изменено), метаданные %s",
            s_class_count, s_names_ok, s_names_fail,
            (unsigned long long)s_fields_off, s_resolved, kOrFieldCount, s_changed,
            s_meta_begin ? "найдены" : "НЕ найдены (фильтр слабее)");
}

}  // namespace

// ---- Публичный слой ----------------------------------------------------------

void offset_resolver_tick() {
    if (g_pid <= 0 || !g_il2cpp_base) return;
    const double deadline = mono_seconds() + kTickBudgetSec;
    switch (s_phase) {
        case PH_WAIT:   s_phase = PH_MAPS; break;
        case PH_MAPS:   phase_maps(); break;
        case PH_SCAN:   phase_scan(deadline); break;
        case PH_FIELDS: phase_fields(); break;
        case PH_DONE:   break;
    }
    if (s_phase == PH_DONE) log_summary();
}

void offset_resolver_reset() {
    s_phase = PH_WAIT;
    s_seg_count = 0; s_seg_idx = 0; s_scan_pos = 0;
    s_lib_begin = s_lib_end = 0; s_meta_begin = s_meta_end = 0;
    s_total_bytes = 0; s_scanned_bytes = 0;
    s_class_count = 0; s_names_ok = 0; s_names_fail = 0;
    s_fields_off = 0; s_resolved = 0; s_changed = 0;
    s_applied_count = 0; s_logged = false;
}

// После переключения релиз/бета слоты перезаписываются табличными значениями;
// найденные живьём — поверх них (они верны для работающего процесса).
void offset_resolver_reapply() {
    for (int i = 0; i < s_applied_count; ++i)
        *(s_applied[i].slot) = s_applied[i].value;
}

// Найденный класс: короткое имя (+ пространство имён, если задано).
// 0 — не найден (вызывающий код падает на вшитый TYPEINFO_RVA).
uint64_t offset_resolver_class(const char* name, const char* ns) {
    if (s_phase != PH_DONE && s_phase != PH_FIELDS) return 0;
    if (!name || !name[0]) return 0;
    for (int i = 0; i < s_class_count; ++i) {
        if (strcmp(s_classes[i].name, name) != 0) continue;
        if (ns && ns[0] && s_classes[i].ns[0] && strcmp(s_classes[i].ns, ns) != 0) continue;
        return s_classes[i].klass;
    }
    return 0;
}

// Отладочный дамп состояния — только для хост-стенда (не вызывается из чита,
// в устройство не собирается).
#ifndef __ANDROID__
void offset_resolver_debug_dump() {
    fprintf(stderr,
            "[ор] фаза %d, сегментов %d (idx %d), скан@0x%llx, lib[0x%llx..0x%llx), "
            "meta[0x%llx..0x%llx), классов %d, имён ок/отказ %d/%d, fields@0x%llx, "
            "дедлайн %s\n",
            (int)s_phase, s_seg_count, s_seg_idx,
            (unsigned long long)s_scan_pos,
            (unsigned long long)s_lib_begin, (unsigned long long)s_lib_end,
            (unsigned long long)s_meta_begin, (unsigned long long)s_meta_end,
            s_class_count, s_names_ok, s_names_fail,
            (unsigned long long)s_fields_off,
            (s_scan_deadline && mono_seconds() < s_scan_deadline) ? "жив" : "вышел");
}
#endif  // __ANDROID__

// Строка для меню: что резолвер смог в этой сессии. При нулевом результате —
// с ПРИЧИНОЙ, чтобы на устройстве было видно, что именно не сложилось.
void offset_resolver_status(char* out, size_t cap) {
    if (!out || !cap) return;
    if (s_phase == PH_WAIT) {
        snprintf(out, cap, "ждёт привязку к игре");
        return;
    }
    if (s_phase == PH_MAPS || s_phase == PH_SCAN || s_phase == PH_FIELDS) {
        // PH_FIELDS сюда же: «готов» показываем только по факту (PH_DONE),
        // иначе строка врёт про фазу, где поля ещё не разрешены.
        snprintf(out, cap, "сканирует: сегментов %d, %llu/%llu МБ, классов %d",
                 s_seg_count,
                 (unsigned long long)(s_scanned_bytes >> 20),
                 (unsigned long long)(s_total_bytes >> 20),
                 s_class_count);
        return;
    }
    // PH_DONE
    if (s_class_count > 0) {
        snprintf(out, cap, "готов: классов %d, полей %d/%d (изменено %d)",
                 s_class_count, s_resolved, kOrFieldCount, s_changed);
        return;
    }
    if (!s_seg_count) {
        snprintf(out, cap, "не нашёл сегменты .so — вшитые оффсеты");
        return;
    }
    if (s_names_ok == 0 && s_names_fail > 0) {
        snprintf(out, cap, "имена классов не читаются (%d отказов) — вшитые оффсеты",
                 s_names_fail);
        return;
    }
    if (s_names_ok == 0) {
        snprintf(out, cap, "кандидатов в классы не нашлось — вшитые оффсеты");
        return;
    }
    snprintf(out, cap, "классы не сложились — вшитые оффсеты");
}
