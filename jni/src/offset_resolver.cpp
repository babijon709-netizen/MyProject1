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

// ---- САМОТЕСТ АВТООБНОВЛЕНИЯ (ВРЕМЕННЫЙ, убрать после проверки) -------------
// Сборка с включённым флагом стартует «сломанной»: перед первым сканом все
// вшитые оффсеты из таблицы портятся (+0x40), как будто игра обновилась и все
// смещения устарели. Дальше видно вживую: пока резолвер не сошёлся — ничего
// не работает; как только он находит живые смещения и перезаписывает слоты —
// функционал оживает БЕЗ перезапуска. Что резолвер не разрешил — в конце
// возвращается на исходное вшитое значение, чтобы ничего не осталось битым.
#define OR_SELFTEST_BREAK_BAKED 1

// ---- «Тихие» чтения резолвера ------------------------------------------------
// Резолвер ходит по указателям Вслепую: почти каждый кандидат — мусор, и отказ
// чтения здесь НОРМА, а не «доступ потерян». Обычные обёртки (rd/rd_buf из
// game_internal.h) сообщают об отказе в счётчик отказов memio; спекулятивный
// скан завалил бы его за пару кусков, а это переоткрывает /proc/<pid>/mem и
// роняет проверку живости привязки (пользователь видел «теряет привязку по
// кругу»). Поэтому у резолвера свои чтения — через read_quiet.
static bool qrd_buf(uint64_t addr, void* out, size_t size) {
    return g_mem.read_quiet(addr, out, size);
}
template <typename T>
static T qrd(uint64_t addr) {
    T v{};
    g_mem.read_quiet(addr, &v, sizeof(T));
    return v;
}
static uint64_t qrd_ptr(uint64_t a) { return memio::untag(qrd<uint64_t>(a)); }

// Тихий аналог read_remote_string (тот использует сообщающие чтения).
static std::string qrd_string(uint64_t address, bool* readable) {
    if (readable) *readable = false;
    if (!address) return {};
    const size_t kMax = 95;
    char buffer[kMax + 1] = {};
    size_t got = 0;
    while (got < kMax) {
        const size_t want = (kMax - got < sizeof(uint64_t)) ? (kMax - got) : sizeof(uint64_t);
        if (!g_mem.read_quiet(address + got, buffer + got, want)) {
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
// Диагностика «сегментов 0»: что именно увидели в карте процесса.
int      s_maps_lib_lines = 0;    // строк libil2cpp.so вообще
int      s_maps_cut_window = 0;   // отброшено окном вокруг базы
int      s_maps_cut_x = 0;        // исполняемые (не сканируются)
int      s_maps_cut_deleted = 0;  // мёртвый «(deleted)» образ
bool     s_maps_open_failed = false;
double   s_scan_deadline = 0;     // общего бюджета скана

FoundClass s_classes[768 + 256];
int      s_class_count = 0;
// Обычный потолок кэша. Хвост массива — резерв: лог с беты показал, что БЕЗ
// окна метаданных «мягкий» фильтр пропускает сотни тысяч мусорных кандидатов,
// кэш забивается мусором ещё в начале скана, и настоящие классы (вставшие в
// образе дальше) в него не попадают. Классы с именами ИЗ ТАБЛИЦЫ сохраняем
// даже сверх потолка (их всего ~44 имени, резерв с запасом).
constexpr int kClassCap = 768;
int      s_names_ok = 0, s_names_fail = 0;
// Сколько значений прошли проверки «указатель + image внутри libil2cpp», ДО
// фильтров имени. По нему в логе видно, на каком именно этапе умерли
// кандидаты: 0 — в базе вообще нет ничего похожего на классы (мёртвый образ
// или другая раскладка), >0 при нуле имён — кандидаты были, но имена не
// прошли фильтр (окно метаданных / нечитаемость).
int      s_shape_pass = 0;

uint64_t s_fields_off = 0;        // откалиброванное смещение Il2CppClass.fields
int      s_calib_best = 0;        // лучший счёт формы полей за калибровку (диагностика)
int      s_resolved = 0, s_changed = 0;

// Знания о базах в рамках ЖИВОГО процесса (переживают сброс скана при
// перепривязке; очищаются только при смене pid). Лог с устройства показал:
// после самообновления игры в карте процесса живут ДВА образа libil2cpp.so —
// живой с классами и «пустой» (свежезагруженная копия .so, классов в ней
// нет). Привязка по одному лишь ELF-заголовку выбирает между ними вслепую и
// могла встать на пустой — скан 221 МБ закончился нулём. Поэтому:
//   кандидаты  — все базы образа из карты (перебираем по кругу);
//   мёртвые    — базы, где полный скан не дал ни одного класса;
//   живая      — база, где классы нашлись (её и держим).
uint64_t s_base_candidates[8];
int      s_candidate_count = 0;
uint64_t s_dead_bases[8];
int      s_dead_count = 0;
uint64_t s_good_base = 0;          // база, где нашлись живые классы
pid_t    s_known_pid = -1;         // pid, для которого знания выше собраны
uint64_t s_scan_base = 0;          // база, к которой относится текущий скан
bool     s_good_logged = false;    // «вернулся на живую базу» не спамить

// Самтест: исходные вшитые значения испорченных слотов — то, что резолвер не
// разрешит, в конце вернётся на них (ничего не останется битым навсегда).
struct SavedSlot { std::uint64_t* slot; std::uint64_t value; };
SavedSlot s_saved[160];
int      s_saved_count = 0;

struct AppliedRec { std::uint64_t* slot; std::uint64_t value; };
AppliedRec s_applied[256];
int      s_applied_count = 0;
bool     s_logged = false;
pid_t    s_bound_pid = -1;        // к какому процессу/базе собрано состояние
uint64_t s_bound_base = 0;
double   s_last_progress_log = 0; // темп строк прогресса в лог

// Бюджет такта 4 мс (как у других сканов): потеря привязки на устройстве, как
// выяснилось по логу, была не от чтений сканера, а от смотрителя кадра.
// Крупный кусок — статический буфер, стеку не стоит.
constexpr double kTickBudgetSec   = 0.004;  // 4 мс на такт
constexpr double kScanTotalSec    = 90.0;   // общий потолок скана (образы по 0.5ГБ+)
constexpr size_t kChunkBytes      = 256 * 1024;
constexpr size_t kMaxReadsPerChunk = 256;   // предел «дорогих» чтений на порцию
// Сколько байт достаточно просмотреть, чтобы при НУЛЕ классов признать базу
// пустой: в живом образе слоты классов лежат в хвостовых сегментах
// (.data/.relro), а скан идёт с конца образа — живые классы нашлись на
// устройстве уже в первых 7 МБ. Пустая база (например, свежезагруженная копия
// .so после самообновления игры) не даст ничего и за все 221 МБ; не ждём.
constexpr uint64_t kEmptyBaseBytes = 96ull << 20;

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

// Имя входит в таблицу резолвера? Такие классы сохраняем в кэш даже сверх
// обычного потолка (см. kClassCap) и калибруем поля в первую очередь на них:
// совпадение имени с именем из дампа — признак НАСТОЯЩЕГО класса.
static bool name_is_wanted(const char* name) {
    for (int i = 0; i < kOrFieldCount; ++i)
        if (strcmp(kOrFields[i].cls, name) == 0) return true;
    return false;
}

static FoundClass* find_class_slot(uint64_t klass) {
    for (int i = 0; i < s_class_count; ++i)
        if (s_classes[i].klass == klass) return &s_classes[i];
    return nullptr;
}

static bool base_is_dead(uint64_t b) {
    for (int i = 0; i < s_dead_count; ++i) if (s_dead_bases[i] == b) return true;
    return false;
}

// Текущая база не дала классов: пометить пустой и перейти на следующего
// кандидата из карты. true — перешли (новый список сегментов соберёт
// PH_MAPS), false — пробовать больше нечего.
static bool try_next_base() {
    if (s_dead_count < 8 && !base_is_dead(g_il2cpp_base))
        s_dead_bases[s_dead_count++] = g_il2cpp_base;
    for (int i = 0; i < s_candidate_count; ++i) {
        const uint64_t b = s_base_candidates[i];
        if (b == g_il2cpp_base || base_is_dead(b)) continue;
        LogLine("автооффсеты: на базе 0x%llx классов нет — пробую базу 0x%llx "
                "(кандидат %d из %d)",
                (unsigned long long)g_il2cpp_base, (unsigned long long)b,
                i + 1, s_candidate_count);
        g_il2cpp_base = b;
        s_scan_base = b;
        s_bound_base = b;
        s_names_ok = 0; s_names_fail = 0; s_shape_pass = 0;
        s_calib_best = 0;
        s_phase = PH_MAPS;
        return true;
    }
    if (s_dead_count > 1)
        LogLine("автооффсеты: все базы-кандидаты (%d) пусты — остаюсь на вшитых оффсетах",
                s_dead_count);
    return false;
}

#if OR_SELFTEST_BREAK_BAKED
// Испортить все вшитые оффсеты из таблицы (запомнив исходные значения).
static void selftest_break_baked() {
    // Если прошлый цикл не дошёл до DONE (игра перезапустилась посреди скана),
    // сначала вернём те слоты на истинные вшитые значения, чтобы не потерять
    // настоящий базовый уровень.
    for (int i = 0; i < s_saved_count; ++i) *(s_saved[i].slot) = s_saved[i].value;
    s_saved_count = 0;
    for (int i = 0; i < kOrFieldCount; ++i) {
        std::uint64_t* slot = kOrFields[i].slot;
        bool seen = false;
        for (int j = 0; j < s_saved_count; ++j) if (s_saved[j].slot == slot) seen = true;
        if (seen) continue;
        if (s_saved_count < 160) {
            s_saved[s_saved_count].slot = slot;
            s_saved[s_saved_count].value = *slot;
            ++s_saved_count;
        }
        *slot += 0x40;
    }
    LogLine("автооффсеты: САМОТЕСТ — вшитые оффсеты испорчены (%d слотов), "
            "дальше всё должно починиться живым резолвом", s_saved_count);
}

// Вернуть на вшитое всё, что резолвер так и не разрешил.
static void selftest_restore_unresolved() {
    int restored = 0;
    for (int i = 0; i < s_saved_count; ++i) {
        bool resolved = false;
        for (int j = 0; j < s_applied_count; ++j)
            if (s_applied[j].slot == s_saved[i].slot) { resolved = true; break; }
        if (!resolved) { *(s_saved[i].slot) = s_saved[i].value; ++restored; }
    }
    LogLine("автооффсеты: САМОТЕСТ — резолвер починил %d из %d слотов, "
            "возвращено на вшитые %d",
            s_saved_count - restored, s_saved_count, restored);
    s_saved_count = 0;
}
#endif  // OR_SELFTEST_BREAK_BAKED

// ---- Фаза MAPS: окна libil2cpp (не исполняемые) и метаданных ---------------
static void phase_maps() {
    s_seg_count = 0;
    s_lib_begin = s_lib_end = 0;
    s_meta_begin = s_meta_end = 0;
    s_maps_lib_lines = 0; s_maps_cut_window = 0; s_maps_cut_x = 0;
    s_maps_cut_deleted = 0; s_maps_open_failed = false;
    s_candidate_count = 0;
    if (!s_scan_base) s_scan_base = g_il2cpp_base;
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", (int)g_pid);
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { s_maps_open_failed = true; s_phase = PH_DONE; return; }
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

        unsigned long long start = 0, end = 0, foff = 0;
        char perms[5] = {};
        if (sscanf(line, "%llx-%llx %4s %llx", &start, &end, perms, &foff) < 3) continue;
        // Потолок сегмента — 1 ГБ: у больших игр .rodata бывает огромным,
        // а выкинув его целиком мы бы могли выкинуть и нужный сегмент рядом.
        if (end <= start || end - start > (1ull << 30)) continue;
        const char* slash = strchr(line, '/');
        if (!slash) continue;

        if (strstr(slash, "global-metadata") && perms[0] == 'r') {
            // Окно строк метаданных: имя/пространства имён классов лежат тут.
            if (!s_meta_begin || start < s_meta_begin) s_meta_begin = start;
            if (end > s_meta_end) s_meta_end = end;
            continue;
        }
        if (!strstr(slash, "libil2cpp.so")) continue;
        ++s_maps_lib_lines;
        // Начало образа (файловое смещение 0) — кандидат базы для скана. Лог с
        // устройства показал, что после самообновления игры в карте живут ДВА
        // образа libil2cpp.so, и по одной лишь базе, выбранной привязкой, классы
        // могут не находиться — тогда скан пойдёт по следующему кандидату.
        if (foff == 0 && s_candidate_count < 8) {
            bool dup = false;
            for (int i = 0; i < s_candidate_count; ++i) dup |= s_base_candidates[i] == start;
            if (!dup) s_base_candidates[s_candidate_count++] = start;
        }
        if (strstr(line, "(deleted)")) { ++s_maps_cut_deleted; continue; }
        if (perms[2] == 'x') { ++s_maps_cut_x; continue; }   // текст не сканируем
        // Окно вокруг сканируемой базы широкое: у больших игр образ занимает
        // сотни мегабайт, и сегмент с классами (relro/data) может лежать
        // далеко от начала. Фильтр лишь отсекает ЧУЖИЕ образы.
        if (s_scan_base && (start < s_scan_base || start > s_scan_base + (2ull << 30))) {
            ++s_maps_cut_window;
            continue;
        }
        s_segments[s_seg_count].start = start;
        s_segments[s_seg_count].end = end;
        ++s_seg_count;
        if (!s_lib_begin || start < s_lib_begin) s_lib_begin = start;
        if (end > s_lib_end) s_lib_end = end;
    }
    // Ловушка беты: в /proc/pid/maps нашёлся маппинг с именем
    // «global-metadata» размером всего ОДНА СТРАНИЦА (4 КБ) — заглушка или
    // заголовок, а настоящий блок строк лежит где-то в другом месте. Строгий
    // фильтр имён по такому «окну» отклонил ВСЕ классы (указатели имён в 4 КБ
    // не помещаются) — скан на бете дал ноль классов при нуле прочитанных
    // имён. Настоящие метаданные всегда многомегабайтные; окно меньше 1 МБ
    // игнорируем и переходим на «мягкий» фильтр (имя — просто читаемая
    // строка): релизная первая привязка без метаданных так находила классы.
    if (s_meta_begin && (s_meta_end - s_meta_begin) < (uint64_t)(1u << 20)) {
        LogLine("автооффсеты: окно метаданных подозрительно мало (%llu КБ) — игнорирую, фильтр имён ослаблен",
                (unsigned long long)((s_meta_end - s_meta_begin) >> 10));
        s_meta_begin = s_meta_end = 0;
    }
    if (!s_seg_count) {
        LogLine("автооффсеты: база 0x%llx — сегментов 0 (строк .so %d: окно %d, иск %d, дел %d, карта %s)",
                (unsigned long long)s_scan_base,
                s_maps_lib_lines, s_maps_cut_window, s_maps_cut_x, s_maps_cut_deleted,
                s_maps_open_failed ? "НЕ открылась" : "прочитана");
        if (try_next_base()) return;
        s_phase = PH_DONE;
        return;
    }
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
    s_last_progress_log = 0;
    LogLine("автооффсеты: база 0x%llx — сегментов %d (%llu МБ), образ 0x%llx..0x%llx, "
            "метаданные %s 0x%llx..0x%llx, кандидатов баз %d",
            (unsigned long long)s_scan_base, s_seg_count,
            (unsigned long long)(s_total_bytes >> 20),
            (unsigned long long)s_lib_begin, (unsigned long long)s_lib_end,
            s_meta_begin ? "найдены" : "НЕ найдены",
            (unsigned long long)s_meta_begin, (unsigned long long)s_meta_end,
            s_candidate_count);
    s_seg_idx = 0;
    s_scan_pos = s_segments[0].start;
    s_scan_deadline = mono_seconds() + kScanTotalSec;
    s_phase = PH_SCAN;
}

// Попробовать кандидата как Il2CppClass: image в libil2cpp, имя/нс в
// метаданных; имя читается строкой. Возврат: 1 принят, 0 отклонён.
static int try_class_candidate(uint64_t v) {
    if (find_class_slot(v)) return 0;
    const uint64_t image = qrd_ptr(v);
    if (s_lib_begin && (image < s_lib_begin || image >= s_lib_end)) return 0;
    else if (!s_lib_begin && !valid_obj(image)) return 0;
    ++s_shape_pass;   // «похож на класс» по форме — до фильтров имени
    const uint64_t nm = qrd_ptr(v + IL2CPP_CLASS_NAME);
    if (s_meta_begin) {
        if (nm < s_meta_begin || nm >= s_meta_end) return 0;
        const uint64_t ns = qrd_ptr(v + IL2CPP_CLASS_NAMESPACE);
        if (ns < s_meta_begin || ns >= s_meta_end) return 0;
    } else {
        if (!valid_obj(nm)) return 0;
        // «Мягкий» режим без окна метаданных (бета): мусорных указателей на
        // читаемые строки в сотни раз больше настоящих классов — один лишь
        // «имя читается» забил кэш мусором. Настоящий класс ДОПОЛНИТЕЛЬНО
        // имеет валидный указатель пространства имён (он всегда есть, хотя бы
        // пустая строка); это дешёвый дополнительный фильтр.
        if (!valid_obj(qrd_ptr(v + IL2CPP_CLASS_NAMESPACE))) return 0;
    }
    bool ok = false;
    const std::string name = qrd_string(nm, &ok);
    if (!ok || name.empty() || name.size() > 47) { ++s_names_fail; return 0; }
    ++s_names_ok;
    const int cap_total = (int)(sizeof(s_classes) / sizeof(s_classes[0]));
    if (s_class_count >= kClassCap && !name_is_wanted(name.c_str())) return 1;
    if (s_class_count >= cap_total) return 1;
    FoundClass& fc = s_classes[s_class_count];
    fc.klass = v;
    snprintf(fc.name, sizeof(fc.name), "%s", name.c_str());
    bool ns_ok = false;
    const std::string ns = qrd_string(qrd_ptr(v + IL2CPP_CLASS_NAMESPACE), &ns_ok);
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
        if (!qrd_buf(s_scan_pos, buf, chunk)) {
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
        // База, не давшая ни одного класса после стольких прочитанных байт,
        // считается пустой: не ждём конца её 221 МБ, идём к следующему кандидату.
        if (!s_class_count && s_scanned_bytes >= kEmptyBaseBytes) break;
        // Прогресс в лог раз в 8 секунд — видно, что скан жив и куда дошёл.
        const double now_s = mono_seconds();
        if (now_s - s_last_progress_log >= 8.0) {
            s_last_progress_log = now_s;
            LogLine("автооффсеты: скан %llu/%llu МБ, классов %d (форма %d, имён %d/%d)",
                    (unsigned long long)(s_scanned_bytes >> 20),
                    (unsigned long long)(s_total_bytes >> 20),
                    s_class_count, s_shape_pass, s_names_ok, s_names_fail);
        }
    }
    const bool empty_base = !s_class_count && s_scanned_bytes >= kEmptyBaseBytes;
    if (all_wanted_found() || empty_base || s_seg_idx >= s_seg_count ||
        mono_seconds() >= s_scan_deadline) {
        if (s_class_count && s_names_ok) {
            if (!s_good_base)
                LogLine("автооффсеты: живые классы нашлись на базе 0x%llx",
                        (unsigned long long)g_il2cpp_base);
            s_good_base = g_il2cpp_base;   // живой образ — запомнить для привязки
            s_phase = PH_FIELDS;
        } else if (!s_class_count && try_next_base()) {
            // Классов нет: база помечена пустой, скан пойдёт по следующему
            // кандидату — новый список сегментов соберёт PH_MAPS.
        } else {
            s_phase = PH_DONE;
        }
    }
}

// ---- Фаза FIELDS: калибровка и резолв ---------------------------------------
// Поле-счётчик: сколько записей подряд похожи на FieldInfo этого класса.
static int fields_shape_score(uint64_t klass, uint64_t fields_ptr, int probe) {
    if (!valid_obj(fields_ptr)) return 0;
    int score = 0;
    for (int i = 0; i < probe; ++i) {
        const uint64_t e = fields_ptr + (uint64_t)i * IL2CPP_FIELDINFO_SIZE;
        const uint64_t parent = qrd_ptr(e + IL2CPP_FIELDINFO_PARENT);
        if (parent != klass) break;
        const uint64_t type = qrd_ptr(e + IL2CPP_FIELDINFO_TYPE);
        if (!valid_obj(type)) break;
        const int32_t off = qrd<int32_t>(e + IL2CPP_FIELDINFO_OFFSET);
        if (off < -1 || off > 0x20000) break;
        ++score;
        const uint64_t nmp = qrd_ptr(e + IL2CPP_FIELDINFO_NAME);
        if (s_meta_begin && (nmp < s_meta_begin || nmp >= s_meta_end)) break;
    }
    return score;
}

static int probe_fields_shape(uint64_t klass, uint64_t off) {
    const int score = fields_shape_score(klass, qrd_ptr(klass + off), 6);
    if (score > s_calib_best) s_calib_best = score;   // диагностика: насколько
    return score;                                     // близко вообще сходились
}

static bool calibrate_fields(uint64_t klass) {
    // Сначала табличное значение — почти всегда оно и есть.
    const uint64_t table_off = IL2CPP_CLASS_FIELDS;
    if (probe_fields_shape(klass, table_off) >= 5) {
        s_fields_off = table_off;
        return true;
    }
    for (uint64_t f = 0x70; f <= 0x178; f += 8) {
        if (f == table_off) continue;
        if (probe_fields_shape(klass, f) >= 5) {
            s_fields_off = f;
            return true;
        }
    }
    return false;
}

// Прочитать имя поля; пустой возврат при нечитаемости.
static bool read_field_name(uint64_t entry, char* out, size_t cap) {
    const uint64_t nmp = qrd_ptr(entry + IL2CPP_FIELDINFO_NAME);
    if (s_meta_begin && (nmp < s_meta_begin || nmp >= s_meta_end)) return false;
    if (!s_meta_begin && !valid_obj(nmp)) return false;
    bool ok = false;
    const std::string name = qrd_string(nmp, &ok);
    if (!ok || name.empty() || name.size() >= cap) return false;
    snprintf(out, cap, "%s", name.c_str());
    return true;
}

// Найти поле по имени в классе; вернуть смещение (-1 если нет).
static int32_t find_field_offset(uint64_t klass, const char* field) {
    const uint64_t fields_ptr = qrd_ptr(klass + s_fields_off);
    if (!valid_obj(fields_ptr)) return -1;
    int count = qrd<uint16_t>(klass + IL2CPP_CLASS_FIELD_COUNT);
    if (count <= 0 || count > 2048) return -1;
    char name[48];
    for (int i = 0; i < count; ++i) {
        const uint64_t e = fields_ptr + (uint64_t)i * IL2CPP_FIELDINFO_SIZE;
        if (qrd_ptr(e + IL2CPP_FIELDINFO_PARENT) != klass) break;
        if (!read_field_name(e, name, sizeof(name))) continue;
        if (strcmp(name, field) == 0) return qrd<int32_t>(e + IL2CPP_FIELDINFO_OFFSET);
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
    // надёжнее форма), потом ВСЕ классы с именами из таблицы (совпадение
    // имени с дампом — признак настоящего класса; на бете кэш без окна
    // метаданных полон мусора, и «первые 12» могли оказаться мусорными),
    // дальше — любые найденные, пока не сойдётся.
    int wanted_in_cache = 0;
    for (int i = 0; i < s_class_count; ++i)
        if (name_is_wanted(s_classes[i].name)) ++wanted_in_cache;
    uint64_t first_choice = 0;
    for (int i = 0; i < s_class_count && !first_choice; ++i)
        if (strcmp(s_classes[i].name, "PlayerManager") == 0) first_choice = s_classes[i].klass;
    if (first_choice) calibrate_fields(first_choice);
    for (int i = 0; i < s_class_count && !s_fields_off; ++i)
        if (name_is_wanted(s_classes[i].name)) calibrate_fields(s_classes[i].klass);
    for (int i = 0; i < s_class_count && !s_fields_off && i < 64; ++i)
        calibrate_fields(s_classes[i].klass);
    if (!s_fields_off) {
        LogLine("автооффсеты: калибровка полей не сошлась — кэш %d классов, "
                "табличных имён в кэше %d, лучший счёт формы полей %d",
                s_class_count, wanted_in_cache, s_calib_best);
        s_phase = PH_DONE;
        return;
    }

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
#if OR_SELFTEST_BREAK_BAKED
    selftest_restore_unresolved();
#endif
    int wanted = 0;
    for (int i = 0; i < s_class_count; ++i)
        if (name_is_wanted(s_classes[i].name)) ++wanted;
    LogLine("автооффсеты: классов найдено %d (по форме %d, имён прочитано %d, отказов %d, табличных %d), "
            "fields@0x%llx, разрешено %d/%d констант (%d изменено), метаданные %s, "
            "база 0x%llx, пустых баз %d",
            s_class_count, s_shape_pass, s_names_ok, s_names_fail, wanted,
            (unsigned long long)s_fields_off, s_resolved, kOrFieldCount, s_changed,
            s_meta_begin ? "найдены" : "НЕ найдены (фильтр слабее)",
            (unsigned long long)g_il2cpp_base, s_dead_count);
}

}  // namespace

// ---- Публичный слой ----------------------------------------------------------

void offset_resolver_tick() {
    if (g_pid <= 0 || !g_il2cpp_base) return;
    // Знания о базах (пустые/живая) привязаны к процессу: новый процесс —
    // новая карта памяти, всё недействительно.
    if (g_pid != s_known_pid) {
        s_known_pid = g_pid;
        s_good_base = 0;
        s_dead_count = 0;
        s_good_logged = false;
    }
    // Перепривязка может снова встать на пустой образ: после самообновления
    // игры в карте два образа libil2cpp.so, и ELF-проверка их не различает.
    // База с живыми классами уже известна — возвращаемся на неё (это же
    // чинит все чтения чита, которые считаются от g_il2cpp_base).
    if (s_good_base && g_il2cpp_base != s_good_base && !base_is_dead(s_good_base)) {
        if (!s_good_logged) {
            s_good_logged = true;
            LogLine("автооффсеты: привязка на базе 0x%llx — возвращаюсь на живую базу 0x%llx",
                    (unsigned long long)g_il2cpp_base, (unsigned long long)s_good_base);
        }
        g_il2cpp_base = s_good_base;
    } else if (s_good_base && g_il2cpp_base == s_good_base) {
        s_good_logged = false;
    }
    // Сброс — только когда СМЕНИЛСЯ процесс/база (игра перезапустилась, либо
    // привязка встала на базу, которую мы ещё не сканировали). Перепривязка
    // смотрителем кадра к той же игре и базе состояние не трогает: найденные
    // классы и смещения полей верны, пока жив тот же процесс — иначе скан
    // стартовал бы заново на каждом отвале и не успевал бы сойтись.
    if (g_pid != s_bound_pid || g_il2cpp_base != s_bound_base) {
        if (s_bound_pid > 0)
            LogLine("автооффсеты: процесс/база сменились (%d/0x%llx -> %d/0x%llx), резолв заново",
                    (int)s_bound_pid, (unsigned long long)s_bound_base,
                    (int)g_pid, (unsigned long long)g_il2cpp_base);
        offset_resolver_reset();
        s_bound_pid = g_pid;
        s_bound_base = g_il2cpp_base;
        s_scan_base = g_il2cpp_base;
    }
    const double deadline = mono_seconds() + kTickBudgetSec;
    switch (s_phase) {
        case PH_WAIT:
#if OR_SELFTEST_BREAK_BAKED
            selftest_break_baked();
#endif
            s_phase = PH_MAPS;
            break;
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
    s_maps_lib_lines = 0; s_maps_cut_window = 0; s_maps_cut_x = 0;
    s_maps_cut_deleted = 0; s_maps_open_failed = false;
    s_class_count = 0; s_names_ok = 0; s_names_fail = 0; s_shape_pass = 0;
    s_fields_off = 0; s_calib_best = 0; s_resolved = 0; s_changed = 0;
    s_applied_count = 0; s_logged = false;
    // Кандидаты баз и сканируемая база собираются заново из карты. Знания
    // «какие базы пусты, а где классы живые» НЕ сбрасываются: они привязаны
    // к процессу (s_known_pid) и переживают перепривязки к той же игре.
    s_candidate_count = 0;
    s_scan_base = 0;
}

// После переключения релиз/бета слоты перезаписываются табличными значениями;
// найденные живьём — поверх них (они верны для работающего процесса).
void offset_resolver_reapply() {
    for (int i = 0; i < s_applied_count; ++i)
        *(s_applied[i].slot) = s_applied[i].value;
}

// База, на которой для ЭТОГО процесса нашлись живые классы (0 — неизвестна).
// Привязка спрашивает её, чтобы после самообновления игры не вставать снова на
// пустую копию libil2cpp.so: ELF-заголовок есть у обеих, а классы — только в
// той, по которой реально работает рантайм.
uint64_t offset_resolver_good_base(pid_t pid) {
    if (pid <= 0 || pid != s_known_pid) return 0;
    return s_good_base;
}

// Идёт ли ещё подбор оффсетов (скан/калибровка). Пока идёт — перепривязка
// смотрителю кадра не поможет: слепоту после апдейта игры лечат только новые
// оффсеты, а не новое подключение к тому же процессу и базе.
bool offset_resolver_busy() {
    return s_phase == PH_MAPS || s_phase == PH_SCAN || s_phase == PH_FIELDS;
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
            "meta[0x%llx..0x%llx), классов %d (форма %d), имён ок/отказ %d/%d, fields@0x%llx, "
            "дедлайн %s\n",
            (int)s_phase, s_seg_count, s_seg_idx,
            (unsigned long long)s_scan_pos,
            (unsigned long long)s_lib_begin, (unsigned long long)s_lib_end,
            (unsigned long long)s_meta_begin, (unsigned long long)s_meta_end,
            s_class_count, s_shape_pass, s_names_ok, s_names_fail,
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
        // Почему сегментов ноль — прямо в строке, чтобы видеть на устройстве.
        if (s_maps_open_failed) {
            snprintf(out, cap, "карта процесса не открылась — вшитые оффсеты");
        } else if (s_maps_lib_lines == 0) {
            snprintf(out, cap, "в карте нет отображений .so — вшитые оффсеты");
        } else {
            snprintf(out, cap, "сегменты .so отброшены (%d): окно %d, иск %d, дел %d",
                     s_maps_lib_lines, s_maps_cut_window, s_maps_cut_x, s_maps_cut_deleted);
        }
        return;
    }
    if (s_names_ok == 0 && s_names_fail > 0) {
        snprintf(out, cap, "имена классов не читаются (%d отказов) — вшитые оффсеты",
                 s_names_fail);
        return;
    }
    if (s_names_ok == 0 && s_shape_pass > 0) {
        // Кандидаты «по форме» были, но ни одно имя не прошло: их отклонил
        // фильтр окна метаданных или строки не прочитались.
        snprintf(out, cap, "кандидаты были (%d), имена отсеяны — вшитые оффсеты",
                 s_shape_pass);
        return;
    }
    if (s_names_ok == 0) {
        snprintf(out, cap, "кандидатов в классы не нашлось — вшитые оффсеты");
        return;
    }
    snprintf(out, cap, "классы не сложились — вшитые оффсеты");
}
