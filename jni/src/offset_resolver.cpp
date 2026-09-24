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
#define OR_SELFTEST_BREAK_BAKED 0

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

// Проверка адреса строки (C-string в памяти метаданных не обязана иметь
// 8-байтовое выравнивание, в отличие от указателей на структуры Il2CppClass).
static inline bool valid_str_ptr(uint64_t p) {
    return p >= 0x10000 && p < 0x0001000000000000ULL;
}

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
    PH_HEAP,        // скан кучи вокруг «якоря» (когда в .so классов не нашлось)
    PH_STRS,        // поиск строки «PlayerManager» в анонимной памяти
    PH_BLOB,        // поиск в найденном блобе строк всех табличных имён
    PH_PTR,         // поиск в образе и куче указателей на эти строки (= классы)
};

Phase    s_phase = PH_WAIT;
Segment  s_segments[12];
// Исполняемые (r-x) сегменты libil2cpp.so. Раньше мы их отбрасывали совсем,
// а зря: разбор файла беты показал, что пул метаданных лежит в секции
// «il2cpp» размером 125 МБ, и она попадает ровно в сегмент R+X. Классы по
// ФОРМЕ там искать бессмысленно (кругом код), а вот точные указатели на
// найденные строки имён — самое оно: ложных совпадений у них нет.
Segment  s_xsegments[12];
int      s_xseg_count = 0;
uint64_t s_xseg_bytes = 0;
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
// Раскладка FieldInfo: обычно табличные константы, но на другой сборке игры
// слот родителя/шаг записи могут отличаться — автоподбор может их уточнить.
uint64_t s_fi_size   = IL2CPP_FIELDINFO_SIZE;
uint64_t s_fi_parent = IL2CPP_FIELDINFO_PARENT;
int      s_calib_best = 0;        // лучший счёт формы полей за калибровку (диагностика)
uint64_t s_calib_best_off = 0;    // на каком смещении лучший счёт
int      s_resolved = 0, s_changed = 0;

// Скан кучи вокруг «якоря»: на бете в самих сегментах .so настоящих классов
// не оказалось (кэш забит мусорными «похожими» структурами, табличных имён
// ноль). Рантайм il2cpp выделяет структуры Il2CppClass из общих пулов, поэтому
// рядом с одним живым классом лежат тысячи остальных — их и перебираем.
uint64_t s_heap_pos = 0, s_heap_end = 0;
bool     s_heap_done = false;     // в этом резолве уже сканировали кучу

// Строковый бутстрап (бета после апдейта): вшитые TYPEINFO_RVA устарели и
// указывают в код, зато строки метаданных обычно лежат в анонимной памяти.
// План: найти в ней строку «PlayerManager» -> вокруг неё окно блока строк ->
// в блоке найти адреса ВСЕХ табличных имён -> в куче найти указатели на них
// (слот имени класса на +0x10) -> так находятся живые классы без единого RVA.
struct AnonRegion { uint64_t start = 0, end = 0; bool writable = false; char rperm[5] = {}; };
// Лог №8 показал ровно 96 регионов — это был ПОТОЛОК, и карта процесса
// обрезалась: пул классов, лежащий в 97-м регионе, в поиск не попадал.
// Держим запас с лихвой и считаем, сколько регионов было на самом деле.
AnonRegion s_anon[512];
int      s_anon_count = 0;
int      s_anon_total = 0;        // сколько анонимных регионов было в карте вообще
uint64_t s_anon_noread_bytes = 0; // анонимная память, закрытая от чтения
int      s_anon_noread_count = 0;
uint64_t s_anon_bytes = 0;        // сколько анонимной памяти взяли в работу
uint64_t s_anon_skip_x_bytes = 0; // пропущено исполняемой
int      s_anon_skip_x = 0;
int      s_anon_skip_small = 0;
uint64_t s_anon_biggest = 0;      // самый крупный анонимный регион
AnonRegion s_anon_noread[256];   // закрытые от чтения (для диагностики)
// Память без бита чтения в карте: протектор мог просто снять права у пула
// классов, а /proc/<pid>/mem читает чужие страницы с FOLL_FORCE — то есть
// «закрыто по правам» ещё не значит «не прочитать». Поэтому каждый такой
// регион прокалывается коротким чтением: читается и не пустой — берём в скан
// (и в первые ряды), читается, но сплошные нули — пустая резервация адресов,
// не читается вовсе — тогда уже точно мимо.
AnonRegion s_hidden[128];         // закрытые по правам, но читаемые на деле
int      s_hidden_count = 0;
uint64_t s_hidden_bytes = 0;
int      s_hidden_skipped = 0;    // скрытых регионов не взяли: не хватило бюджета
int      s_probe_ok = 0;          // проба чтения удалась
uint64_t s_probe_bytes = 0;
int      s_probe_zero = 0;        // читается, но внутри одни нули
uint64_t s_probe_zero_bytes = 0;
// Читаемые файловые регионы с именем, но не libil2cpp.so (протектор может
// положить метадату в свой .dat/.bin или в память, отмапленную из apk).
AnonRegion s_filemisc[256];
int      s_filemisc_count = 0;
uint64_t s_str_addr = 0;          // где нашёлся «PlayerManager»
// Блоков со строкой «PlayerManager» в процессе бывает НЕСКОЛЬКО (лог №8:
// в одном нашлось 29 имён из 43, в другом только 5). Собираем все вхождения
// и берём лучший блок по числу найденных табличных имён, а не первый.
uint64_t s_str_cand[8];
int      s_str_cand_region[8];
int      s_str_cand_count = 0;
int      s_str_cand_idx = 0;
int      s_best_found = -1;       // лучший результат по блокам
uint64_t s_best_begin = 0, s_best_end = 0, s_best_addr = 0;
uint64_t s_best_str[64];
// Результаты каждого блока строк: окно и сколько имён в нём нашлось. Имя
// сравнивается по содержимому, поэтому для повторного прохода по указателям
// достаточно переставить окно и прогнать поиск заново.
uint64_t s_blk_begin[8], s_blk_end[8];
int      s_blk_found[8];
bool     s_blk_tried[8];
int      s_ptr_passes = 0;        // сколько блоков уже прогоняли по указателям
uint64_t s_ptr_readfails = 0;     // порций, которые не удалось прочитать
int      s_str_region = 0;
uint64_t s_str_pos = 0;
uint64_t s_str_scanned = 0;
const char* s_wanted_names[64];   // разные имена классов из таблицы
int      s_wanted_name_count = 0;
uint64_t s_wanted_str[64];        // адрес строки каждого имени в блобе (0 = нет)
int      s_wanted_str_found = 0;
uint64_t s_blob_pos = 0;
uint64_t s_blob_fails = 0;        // сколько порций блоба не прочиталось
uint64_t s_ptr_pos = 0;
int      s_ptr_region = 0;
uint64_t s_ptr_scanned = 0;
uint64_t s_ptr_hits = 0;          // совпадений указателя с адресом имени
uint64_t s_ptr_verified = 0;      // из них оказалось живыми классами
uint64_t s_ptr_inwin = 0;         // значений-указателей внутрь окна строк (любых)
uint64_t s_ptr_noverify = 0;      // имя совпало, но структура классом не подтвердилась
uint64_t s_ptr_names = 0;         // указателей на ЛЮБУЮ читаемую строку блока
uint64_t s_ptr_rinwin = 0;        // указателей в окно в текущем диапазоне
uint64_t s_ptr_best_hits = 0;     // максимум в одном диапазоне
int      s_ptr_best_range = -1;
// Три лучших диапазона, а не один: пул классов может быть не самым
// «густонаселённым» местом (лог №12: на первом месте таблица имён шейдеров
// — 3745 указателей, а нужного пула в списке не видно вовсе).
uint64_t s_top_hits[3] = {0, 0, 0};
int      s_top_range[3] = {-1, -1, -1};

// Диапазоны поиска указателей на имена. Порядок важный: СНАЧАЛА читаемые
// сегменты самой libil2cpp.so, ПОТОМ анонимная память. Протектор может вшить
// метадату (массив Il2CppClass вместе с указателями имён) прямо в образ —
// тогда в куче искать нечего, а классы лежат в .data/.rodata библиотеки.
struct PtrRange { uint64_t start = 0, end = 0; };
PtrRange  s_ptr_ranges[1024];
int       s_ptr_range_count = 0;
uint64_t  s_ptr_seg_bytes = 0;    // сколько МБ в сегментах .so
uint64_t  s_ptr_anon_bytes = 0;   // сколько МБ в анонимной памяти
static void build_scan_ranges();   // определение ниже, вызывается из phase_strsearch

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

// Сколько классов в кэше имеют табличные имена.
static int wanted_in_cache() {
    int n = 0;
    for (int i = 0; i < s_class_count; ++i)
        if (name_is_wanted(s_classes[i].name)) ++n;
    return n;
}

// «Якорь» — живой настоящий класс, читаемый по вшитому TYPEINFO_RVA. Привязка
// проверяет базу ровно этой проверкой («база верна по вшитому RVA ...»), так
// что если она прошла — по RVA объект настоящего класса этого процесса.
// Возвращает адрес класса и (опционально) ожидаемое имя для диагностики.
static uint64_t anchor_class(const char** expected_name) {
    if (expected_name) *expected_name = "";
    if (!g_il2cpp_base) return 0;
    if (PLAYER_MANAGER_TYPEINFO_RVA) {
        const uint64_t k = qrd_ptr(g_il2cpp_base + PLAYER_MANAGER_TYPEINFO_RVA);
        if (valid_obj(k)) { if (expected_name) *expected_name = "PlayerManager"; return k; }
    }
    if (GAME_CONTROLLER_TYPEINFO_RVA) {
        const uint64_t k = qrd_ptr(g_il2cpp_base + GAME_CONTROLLER_TYPEINFO_RVA);
        if (valid_obj(k)) { if (expected_name) *expected_name = "GameControllerBase"; return k; }
    }
    return 0;
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
        s_calib_best = 0; s_calib_best_off = 0;
        s_fi_size = IL2CPP_FIELDINFO_SIZE; s_fi_parent = IL2CPP_FIELDINFO_PARENT;
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
    s_xseg_count = 0; s_xseg_bytes = 0;
    s_maps_lib_lines = 0; s_maps_cut_window = 0; s_maps_cut_x = 0;
    s_maps_cut_deleted = 0; s_maps_open_failed = false;
    s_candidate_count = 0;
    s_anon_count = 0; s_anon_total = 0; s_filemisc_count = 0;
    s_anon_noread_bytes = 0; s_anon_noread_count = 0;
    s_anon_bytes = 0; s_anon_skip_x = 0; s_anon_skip_x_bytes = 0;
    s_anon_skip_small = 0; s_anon_biggest = 0;
    s_hidden_count = 0; s_hidden_bytes = 0; s_hidden_skipped = 0;
    s_probe_ok = 0; s_probe_bytes = 0; s_probe_zero = 0; s_probe_zero_bytes = 0;
    if (!s_scan_base) s_scan_base = g_il2cpp_base;
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", (int)g_pid);
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { s_maps_open_failed = true; s_phase = PH_DONE; return; }
    std::string text;
    char chunk[8192];
    for (;;) {
        const ssize_t n = read(fd, chunk, sizeof(chunk));
        if (n > 0) { text.append(chunk, (size_t)n); if (text.size() > (16u << 20)) break; continue; }
        if (n < 0) break;
        break;
    }
    close(fd);

    size_t pos = 0;
    while (pos < text.size()) {
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
        if (end <= start) continue;
        const char* slash = strchr(line, '/');
        // Потолок 1 ГБ — только для ФАЙЛОВЫХ сегментов: у больших игр .rodata
        // бывает огромным. Для анонимной памяти потолка нет: куча игры часто
        // выделяется ОДНИМ регионом больше гигабайта, и, выкинув его, мы
        // теряем как раз тот пул классов, который ищем.
        if (slash && end - start > (1ull << 30)) continue;
        if (!slash) {
            // Анонимный читаемый регион — кандидат на блок строк метаданных
            // (бета маппит его без имени «global-metadata») и на кучу, где
            // лежат структуры классов. Собираем для строкового бутстрапа.
            if (perms[0] != 'r') {
                // Бит чтения снят, но страницы могут отдаваться: /proc/<pid>/mem
                // читает с FOLL_FORCE. Проверяем несколько точек в регионе:
                // если хотя бы одна читается — регион доступен для чтения.
                const uint64_t rsize = end - start;
                uint8_t probe[128] = {};
                bool can_read = false, non_zero = false;
                const uint64_t offs[] = {
                    0, 64 * 1024, 256 * 1024, 1024 * 1024,
                    2 * 1024 * 1024, 4 * 1024 * 1024, 8 * 1024 * 1024,
                    16 * 1024 * 1024, rsize >> 2, rsize >> 1
                };
                for (size_t t = 0; t < sizeof(offs)/sizeof(offs[0]); ++t) {
                    if (offs[t] >= rsize) continue;
                    if (!qrd_buf(start + offs[t], probe, sizeof(probe))) continue;
                    can_read = true;
                    for (size_t k = 0; k < sizeof(probe); ++k) {
                        if (probe[k]) { non_zero = true; break; }
                    }
                    if (non_zero) break;
                }
                if (!can_read) {
                    s_anon_noread_bytes += rsize;   // и правда не прочитать
                    if (s_anon_noread_count < 256) {
                        s_anon_noread[s_anon_noread_count].start = start;
                        s_anon_noread[s_anon_noread_count].end = end;
                        snprintf(s_anon_noread[s_anon_noread_count].rperm,
                                 sizeof(s_anon_noread[0].rperm), "%s", perms);
                    }
                    ++s_anon_noread_count;
                } else if (!non_zero && rsize > (64ull << 20)) {
                    // Читается, но пусто и гигантский размер (>64 МБ): скорее всего
                    // резервация виртуального адресного пространства.
                    ++s_probe_zero;
                    s_probe_zero_bytes += rsize;
                } else {
                    ++s_probe_ok;
                    s_probe_bytes += rsize;
                    if (s_hidden_count < 128) {
                        s_hidden[s_hidden_count].start = start;
                        s_hidden[s_hidden_count].end = end;
                        s_hidden[s_hidden_count].writable = (perms[1] == 'w');
                        snprintf(s_hidden[s_hidden_count].rperm,
                                 sizeof(s_hidden[0].rperm), "%s", perms);
                        ++s_hidden_count;
                        s_hidden_bytes += rsize;
                    }
                }
            } else if (perms[2] == 'x') {
                ++s_anon_skip_x;
                s_anon_skip_x_bytes += end - start;
            } else if (end - start < (256ull << 10)) {
                ++s_anon_skip_small;
            }
            if (end - start > s_anon_biggest) s_anon_biggest = end - start;
            if (perms[0] == 'r' && perms[2] != 'x' && end - start >= (256ull << 10)) {
                ++s_anon_total;
                if (s_anon_count < 512) {
                    s_anon[s_anon_count].start = start;
                    s_anon[s_anon_count].end = end;
                    s_anon[s_anon_count].writable = (perms[1] == 'w');
                    snprintf(s_anon[s_anon_count].rperm, sizeof(s_anon[0].rperm), "%s", perms);
                    ++s_anon_count;
                    s_anon_bytes += end - start;
                }
            }
            continue;
        }

        // Прочий файловый маппинг (не libil2cpp.so и не метаданные): метадата
        // протектора может лежать в своём .dat/.bin. Явный мусор (.dex/.oat/
        // .vdex/.art/.apk — там байт-код, а не структуры) не собираем.
        if (perms[0] == 'r' && perms[2] != 'x' && end - start >= (256ull << 10) &&
            s_filemisc_count < 256 &&
            !strstr(slash, "libil2cpp.so") && !strstr(slash, "global-metadata") &&
            !strstr(slash, ".oat") && !strstr(slash, ".dex") &&
            !strstr(slash, ".vdex") && !strstr(slash, ".art") &&
            !strstr(slash, ".apk")) {
            s_filemisc[s_filemisc_count].start = start;
            s_filemisc[s_filemisc_count].end = end;
            s_filemisc[s_filemisc_count].writable = (perms[1] == 'w');
            ++s_filemisc_count;
            continue;
        }

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
        if (perms[2] == 'x') {
            ++s_maps_cut_x;
            // Текст не сканируем формой, но в список поиска указателей берём:
            // в бете весь пул классов лежит в исполняемом сегменте образа.
            if (s_scan_base && start >= s_scan_base &&
                start <= s_scan_base + (2ull << 30) && s_xseg_count < 12) {
                s_xsegments[s_xseg_count].start = start;
                s_xsegments[s_xseg_count].end = end;
                ++s_xseg_count;
                s_xseg_bytes += end - start;
            }
            continue;
        }
        // Окно вокруг сканируемой базы широкое: у больших игр образ занимает
        // сотни мегабайт, и сегмент с классами (relro/data) может лежать
        // далеко от начала. Фильтр лишь отсекает ЧУЖИЕ образы.
        if (s_scan_base && (start < s_scan_base || start > s_scan_base + (2ull << 30))) {
            ++s_maps_cut_window;
            continue;
        }
        if (s_seg_count < 12) {
            s_segments[s_seg_count].start = start;
            s_segments[s_seg_count].end = end;
            ++s_seg_count;
            if (!s_lib_begin || start < s_lib_begin) s_lib_begin = start;
            if (end > s_lib_end) s_lib_end = end;
        }
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
    // Сводка по карте: видно, какую память мы реально сканируем, а какую нет.
    {
        LogLine("автооффсеты: карта процесса — анонимной памяти %llu МБ в %d регионах, "
                "самый крупный %llu МБ; пропущено: исполняемых %d (%llu МБ), мелких %d, закрытых %d (%llu МБ)",
                (unsigned long long)(s_anon_bytes >> 20), s_anon_count,
                (unsigned long long)(s_anon_biggest >> 20),
                s_anon_skip_x, (unsigned long long)(s_anon_skip_x_bytes >> 20),
                s_anon_skip_small, s_anon_noread_count,
                (unsigned long long)(s_anon_noread_bytes >> 20));
        int top[3] = {-1, -1, -1};
        for (int i = 0; i < s_anon_count; ++i) {
            const uint64_t sz = s_anon[i].end - s_anon[i].start;
            if (top[0] < 0 || sz > s_anon[top[0]].end - s_anon[top[0]].start) {
                top[2] = top[1]; top[1] = top[0]; top[0] = i;
            } else if (top[1] < 0 || sz > s_anon[top[1]].end - s_anon[top[1]].start) {
                top[2] = top[1]; top[1] = i;
            } else if (top[2] < 0 || sz > s_anon[top[2]].end - s_anon[top[2]].start) {
                top[2] = i;
            }
        }
        for (int k = 0; k < 3; ++k) {
            const int i = top[k];
            if (i < 0) continue;
            LogLine("автооффсеты: крупный регион №%d — 0x%llx..0x%llx (%llu МБ, %s)",
                    k + 1, (unsigned long long)s_anon[i].start,
                    (unsigned long long)s_anon[i].end,
                    (unsigned long long)((s_anon[i].end - s_anon[i].start) >> 20),
                    s_anon[i].rperm);
        }
    }
    // Итог пробы памяти без прав чтения: сколько её вообще, сколько отдаётся
    // принудительным чтением и сколько из этого не пусто. Если пул классов
    // спрятан снятием бита чтения — он виден здесь, а не в сводке выше.
    if (s_anon_noread_count || s_probe_ok || s_probe_zero) {
        LogLine("автооффсеты: память без прав чтения — совсем не читается %d (%llu МБ), "
                "принудительно прочиталось: с данными %d (%llu МБ), пустых резерваций %d (%llu МБ)",
                s_anon_noread_count, (unsigned long long)(s_anon_noread_bytes >> 20),
                s_probe_ok, (unsigned long long)(s_probe_bytes >> 20),
                s_probe_zero, (unsigned long long)(s_probe_zero_bytes >> 20));
        for (int i = 0; i < s_hidden_count && i < 3; ++i)
            LogLine("автооффсеты: скрытый регион №%d — 0x%llx..0x%llx (%llu МБ, %s)",
                    i + 1, (unsigned long long)s_hidden[i].start,
                    (unsigned long long)s_hidden[i].end,
                    (unsigned long long)((s_hidden[i].end - s_hidden[i].start) >> 20),
                    s_hidden[i].rperm);
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
    // Урок беты: после апдейта игры вшитый TYPEINFO_RVA указывает в КОД, и
    // скан .so находит только мусор. Проверка свежести: имя по якорю должно
    // читаться и совпасть с ожидаемым. Не совпало — сразу строковый бутстрап.
    const char* ank_expected = nullptr;
    const uint64_t ank = anchor_class(&ank_expected);
    if (ank && ank_expected && ank_expected[0]) {
        bool aok = false;
        const std::string an = qrd_string(qrd_ptr(ank + IL2CPP_CLASS_NAME), &aok);
        if (!aok || an != ank_expected) {
            LogLine("автооффсеты: вшитый RVA, судя по всему, устарел (имя %s) — начинаю поиск строк в памяти",
                    aok ? "не совпало" : "не читается");
            s_str_region = 0; s_str_pos = 0; s_str_scanned = 0;
            s_phase = PH_STRS;
        }
    }
}

// Попробовать кандидата как Il2CppClass: image в libil2cpp, имя/нс в
// метаданных; имя читается строкой. Возврат: 1 принят, 0 отклонён.
static int try_class_candidate(uint64_t v) {
    if (find_class_slot(v)) return 0;
    const uint64_t image = qrd_ptr(v);
    if (!valid_obj(image)) return 0;
    if (!s_meta_begin && s_lib_begin && (image < s_lib_begin || image >= s_lib_end)) return 0;
    ++s_shape_pass;   // «похож на класс» по форме — до фильтров имени
    const uint64_t nm = qrd_ptr(v + IL2CPP_CLASS_NAME);
    if (s_meta_begin) {
        if (nm < s_meta_begin || nm >= s_meta_end) return 0;
        const uint64_t ns = qrd_ptr(v + IL2CPP_CLASS_NAMESPACE);
        if (ns < s_meta_begin || ns >= s_meta_end) return 0;
    } else {
        if (!valid_str_ptr(nm)) return 0;
        // «Мягкий» режим без окна метаданных (бета): мусорных указателей на
        // читаемые строки в сотни раз больше настоящих классов — один лишь
        // «имя читается» забил кэш мусором. Настоящий класс ДОПОЛНИТЕЛЬНО
        // имеет валидный указатель пространства имён (он всегда есть, хотя бы
        // пустая строка); это дешёвый дополнительный фильтр.
        if (!valid_str_ptr(qrd_ptr(v + IL2CPP_CLASS_NAMESPACE))) return 0;
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

// ---- Скан кучи вокруг «якоря» ----------------------------------------------
// Запускается, когда в сегментах .so не нашлось ни одного табличного класса.
// Идём окном ±8 МБ вокруг адреса живого класса (якоря) и применяем тот же
// кандидатный фильтр: структуры Il2CppClass лежат в общих пулах рантайма.
static void start_heap_scan(uint64_t anchor) {
    constexpr uint64_t kSpan = 8ull << 20;   // ±8 МБ
    s_heap_pos = (anchor >= kSpan) ? (anchor - kSpan) : 0x1000;
    s_heap_pos &= ~7ull;                     // по 8-байтовым слотам
    s_heap_end = anchor + kSpan;
    s_heap_done = true;
    s_total_bytes = s_heap_end - s_heap_pos;
    s_scanned_bytes = 0;
    s_last_progress_log = 0;
    s_scan_deadline = mono_seconds() + 60.0;   // свой бюджет, не общий с .so
    LogLine("автооффсеты: табличных классов в .so нет — сканирую кучу вокруг якоря 0x%llx (±8 МБ)",
            (unsigned long long)anchor);
    s_phase = PH_HEAP;
}

static void phase_heap(double deadline) {
    static uint8_t buf[kChunkBytes];
    while (s_heap_pos < s_heap_end && mono_seconds() < deadline) {
        const uint64_t remain = s_heap_end - s_heap_pos;
        const uint64_t chunk = (kChunkBytes < remain) ? kChunkBytes : remain;
        if (!qrd_buf(s_heap_pos, buf, chunk)) {
            s_heap_pos += chunk;
            s_scanned_bytes += chunk;
        } else {
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
            s_heap_pos += examined * 8;
            s_scanned_bytes += examined * 8;
        }
        if (all_wanted_found()) break;
        if (mono_seconds() >= s_scan_deadline) break;
        const double now_s = mono_seconds();
        if (now_s - s_last_progress_log >= 8.0) {
            s_last_progress_log = now_s;
            LogLine("автооффсеты: куча %llu/%llu МБ, классов %d (табличных %d, форма %d)",
                    (unsigned long long)(s_scanned_bytes >> 20),
                    (unsigned long long)(s_total_bytes >> 20),
                    s_class_count, wanted_in_cache(), s_shape_pass);
        }
    }
    if (all_wanted_found() || s_heap_pos >= s_heap_end ||
        mono_seconds() >= s_scan_deadline) {
        if (!all_wanted_found() && wanted_in_cache() == 0) {
            LogLine("автооффсеты: в куче вокруг якоря табличных классов нет — начинаю поиск строк в памяти");
            s_str_region = 0; s_str_pos = 0; s_str_scanned = 0;
            s_phase = PH_STRS;
        } else {
            s_phase = PH_FIELDS;
        }
    }
}

// ---- Строковый бутстрап ------------------------------------------------------
// Когда вшитые TYPEINFO_RVA устарели (игра обновилась) и скан .so не даёт ни
// одного табличного класса, идём через строки: имена классов живут в блоке
// строк метаданных (на бете — в анонимной памяти). Находим «PlayerManager»,
// вокруг него окно блока, в нём — адреса всех табличных имён, а затем в куче
// ищем указатели на эти адреса: слот имени класса лежит на +0x10.
static bool s_first_char[256];

static void build_wanted_names() {
    if (s_wanted_name_count) return;
    memset(s_first_char, 0, sizeof(s_first_char));
    for (int i = 0; i < kOrFieldCount && s_wanted_name_count < 64; ++i) {
        bool dup = false;
        for (int j = 0; j < s_wanted_name_count; ++j)
            if (strcmp(s_wanted_names[j], kOrFields[i].cls) == 0) dup = true;
        if (dup) continue;
        s_wanted_names[s_wanted_name_count] = kOrFields[i].cls;
        s_first_char[(uint8_t)kOrFields[i].cls[0]] = true;
        ++s_wanted_name_count;
    }
}

static bool calibrate_fields(uint64_t klass);   // определена ниже, в фазе FIELDS

static void store_verified_class(uint64_t klass, const char* name) {
    if (find_class_slot(klass)) return;
    const int cap_total = (int)(sizeof(s_classes) / sizeof(s_classes[0]));
    if (s_class_count >= cap_total) return;
    FoundClass& fc = s_classes[s_class_count++];
    fc.klass = klass;
    snprintf(fc.name, sizeof(fc.name), "%s", name);
    fc.ns[0] = 0;
}

static void apply_str_window(int idx);   // определена ниже, после phase_strsearch

static void phase_strsearch(double deadline) {
    static uint8_t buf[kChunkBytes];
    static const char kNeedle[] = "PlayerManager";   // вместе с концевым NUL
    constexpr size_t kNeed = sizeof(kNeedle);
    // Имя ищем по ВСЕЙ читаемой памяти, включая исполняемые сегменты образа.
    // Файл беты показал: метадата вшита в секцию «il2cpp» (125 МБ), которая
    // лежит в сегменте R+X. Если протектор расшифровал её на месте, имена
    // лежат открытым текстом внутри образа — раньше мы их там не искали.
    if (s_ptr_range_count == 0) build_scan_ranges();
    while (s_str_region < s_ptr_range_count && mono_seconds() < deadline) {
        const PtrRange& r = s_ptr_ranges[s_str_region];
        // Курсор один на весь список, а диапазоны идут НЕ по возрастанию
        // адреса: скрытые регионы (без прав чтения) ставятся вперёд. Лог №13
        // показал провал: дойдя до конца скрытого региона 0x7d7a..., курсор
        // оказался ВЫШЕ всей анонимной кучи, и 317 диапазонов были объявлены
        // пройденными без единого чтения — ищи строку по всей памяти, а
        // прочитал 281 МБ из 3,5 ГБ. Поэтому вышедший за диапазон курсор
        // возвращается на его начало, а не «проходит» диапазон мимо.
        if (s_str_pos == r.end) { ++s_str_region; continue; }
        if (s_str_pos < r.start || s_str_pos > r.end) s_str_pos = r.start;
        if (s_str_pos >= r.end) { ++s_str_region; continue; }
        const uint64_t remain = r.end - s_str_pos;
        const uint64_t chunk = (kChunkBytes < remain) ? kChunkBytes : remain;
        if (chunk < kNeed) { s_str_pos = r.end; ++s_str_region; continue; }
        if (!qrd_buf(s_str_pos, buf, chunk)) {
            s_str_pos += chunk; s_str_scanned += chunk; continue;
        }
        const uint8_t* hit = (const uint8_t*)memmem(buf, chunk, kNeedle, kNeed);
        if (hit) {
            if (s_str_cand_count < 8) {
                s_str_cand[s_str_cand_count] = s_str_pos + (uint64_t)(hit - buf);
                s_str_cand_region[s_str_cand_count] = s_str_region;
                ++s_str_cand_count;
            }
            s_str_pos += (uint64_t)(hit - buf) + kNeed;   // ищем следующее
            if (s_str_cand_count >= 8) break;
            continue;
        }
        s_str_scanned += chunk;
        s_str_pos += chunk - (kNeed - 1);   // нахлёст, чтобы не потерять стык
        if (s_str_scanned >= (6144ull << 20)) break;
    }
    if (s_str_region >= s_ptr_range_count || s_str_scanned >= (6144ull << 20) ||
        s_str_cand_count >= 8) {
        if (s_str_cand_count > 0) {
            s_str_cand_idx = 0;
            s_best_found = -1;
            apply_str_window(0);
            return;
        }
        LogLine("автооффсеты: строка «PlayerManager» нигде НЕ найдена (просмотрено %llu МБ, %d диапазонов из %d) — имена зашифрованы, автообновление по именам невозможно",
                (unsigned long long)(s_str_scanned >> 20), s_str_region, s_ptr_range_count);
        s_phase = PH_DONE;
    }
}

// Окно блока строк вокруг кандидата: строковый раздел метаданных может лежать
// в нескольких СМЕЖНЫХ анонимных областях (протектор режет маппинг кусками).
// Собираем непрерывную цепочку соседей (до 64 МБ), иначе имена за границей
// первой области теряются. Если цепочка гигантская — окно ±32 МБ вокруг хита.
static void apply_str_window(int idx) {
    const int reg = s_str_cand_region[idx];
    s_str_addr = s_str_cand[idx];
    uint64_t wb = s_ptr_ranges[reg].start, we = s_ptr_ranges[reg].end;
    for (int j = reg + 1; j < s_ptr_range_count &&
             s_ptr_ranges[j].start == we && (we - wb) < (64ull << 20); ++j)
        we = s_ptr_ranges[j].end;
    for (int j = reg - 1; j >= 0 &&
             s_ptr_ranges[j].end == wb && (we - wb) < (64ull << 20); --j)
        wb = s_ptr_ranges[j].start;
    if (we - wb > (64ull << 20)) {
        const uint64_t kPad = 32ull << 20;
        wb = (s_str_addr > kPad) ? (s_str_addr - kPad) : 0;
        we = s_str_addr + kPad;
        if (wb < s_ptr_ranges[reg].start) wb = s_ptr_ranges[reg].start;
        if (we > s_ptr_ranges[reg].end) we = s_ptr_ranges[reg].end;
    }
    s_meta_begin = wb;
    s_meta_end = we;
    build_wanted_names();
    for (int i = 0; i < s_wanted_name_count; ++i) s_wanted_str[i] = 0;
    s_wanted_str_found = 0;
    s_blob_pos = s_meta_begin;
    s_last_progress_log = 0;
    // Блок строк — холодное файлово маппинг: страницы подчитываются с
    // диска, чтение медленное. Дедлайн скана .so тут не годится.
    s_scan_deadline = mono_seconds() + 180.0;
    LogLine("автооффсеты: блок строк №%d из %d — «PlayerManager» @0x%llx, окно 0x%llx..0x%llx (%llu МБ), %s",
            idx + 1, s_str_cand_count, (unsigned long long)s_str_addr,
            (unsigned long long)s_meta_begin, (unsigned long long)s_meta_end,
            (unsigned long long)((s_meta_end - s_meta_begin) >> 20),
            (s_lib_begin && s_str_addr >= s_lib_begin && s_str_addr < s_lib_end)
                ? "ВНУТРИ ОБРАЗА libil2cpp.so" : "вне образа");
    // Что вокруг имени: если это таблица строк метаданных, рядом лежат другие
    // имена впритык, разделённые нулями. Если мусор или зашифрованный блок —
    // по дампу это видно сразу, и тогда искать указатели на строки бессмысленно.
    {
        uint8_t raw[48] = {};
        const uint64_t from = (s_str_addr > 24) ? (s_str_addr - 24) : 0;
        if (qrd_buf(from, raw, sizeof(raw))) {
            char hex[3 * sizeof(raw) + 8] = {};
            char txt[sizeof(raw) + 8] = {};
            size_t hn = 0, tn = 0;
            for (size_t i = 0; i < sizeof(raw); ++i) {
                hn += (size_t)snprintf(hex + hn, sizeof(hex) - hn, "%02x ", raw[i]);
                const unsigned char c = raw[i];
                txt[tn++] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
            }
            txt[tn] = 0;
            LogLine("автооффсеты: вокруг имени @0x%llx — байты: %s", (unsigned long long)from, hex);
            LogLine("автооффсеты: вокруг имени @0x%llx — текст:  %s", (unsigned long long)from, txt);
        }
    }
    s_phase = PH_BLOB;
}

static void phase_blob(double deadline) {
    static uint8_t buf[kChunkBytes];
    while (s_blob_pos < s_meta_end && mono_seconds() < deadline &&
           mono_seconds() < s_scan_deadline) {
        const uint64_t remain = s_meta_end - s_blob_pos;
        const uint64_t chunk = (kChunkBytes < remain) ? kChunkBytes : remain;
        if (chunk < 64) { s_blob_pos = s_meta_end; break; }   // хвост дочитан
        if (!qrd_buf(s_blob_pos, buf, chunk)) {
            ++s_blob_fails;
            s_blob_pos += chunk;
            continue;
        }
        for (size_t p = 0; p + 1 < chunk; ++p) {
            // Строки блока разделены NUL: кандидат начинается сразу после NUL.
            if (p && buf[p - 1] != '\0') continue;
            const uint8_t c = buf[p];
            if (c == '\0' || !s_first_char[c]) continue;
            for (int n = 0; n < s_wanted_name_count; ++n) {
                if (s_wanted_str[n]) continue;                 // уже найдено
                const char* nm = s_wanted_names[n];
                if (c != (uint8_t)nm[0]) continue;
                const size_t len = strlen(nm);
                if (p + len + 1 > chunk) continue;             // стык — докроет нахлёст
                if (memcmp(buf + p, nm, len) == 0 && buf[p + len] == '\0') {
                    s_wanted_str[n] = s_blob_pos + p;
                    ++s_wanted_str_found;
                }
            }
        }
        // Хвостовой ДЕДЛОК: при остатке [64..128) шаг «chunk - 64» обнулялся,
        // и скан крутился на месте до дедлайна. Хвост меньше куска — дочитали.
        if (chunk < kChunkBytes) s_blob_pos = s_meta_end;
        else s_blob_pos += chunk - 64;   // нахлёст на имена, лёгшие на стык
        const double now_s = mono_seconds();
        if (now_s - s_last_progress_log >= 8.0) {
            s_last_progress_log = now_s;
            LogLine("автооффсеты: блок строк %llu/%llu МБ, табличных имён %d, отказов чтения %llu",
                    (unsigned long long)((s_blob_pos - s_meta_begin) >> 20),
                    (unsigned long long)((s_meta_end - s_meta_begin) >> 20),
                    s_wanted_str_found, (unsigned long long)s_blob_fails);
        }
    }
    if (s_blob_pos >= s_meta_end || mono_seconds() >= s_scan_deadline) {
        LogLine("автооффсеты: блок строк №%d из %d (0x%llx..0x%llx) — табличных имён %d из %d",
                s_str_cand_idx + 1, s_str_cand_count,
                (unsigned long long)s_meta_begin, (unsigned long long)s_meta_end,
                s_wanted_str_found, s_wanted_name_count);
        // Запоминаем лучший блок: настоящие имена метаданных лежат только в
        // одном из них, остальные — обрезки или чужие копии строк.
        s_blk_begin[s_str_cand_idx] = s_meta_begin;
        s_blk_end[s_str_cand_idx] = s_meta_end;
        s_blk_found[s_str_cand_idx] = s_wanted_str_found;
        s_blk_tried[s_str_cand_idx] = false;
        if (s_wanted_str_found > s_best_found) {
            s_best_found = s_wanted_str_found;
            s_best_begin = s_meta_begin; s_best_end = s_meta_end;
            s_best_addr = s_str_addr;
            for (int i = 0; i < s_wanted_name_count; ++i) s_best_str[i] = s_wanted_str[i];
        }
        ++s_str_cand_idx;
        if (s_str_cand_idx < s_str_cand_count && s_best_found < s_wanted_name_count) {
            apply_str_window(s_str_cand_idx);   // пробуем следующий блок
            return;
        }
        // Все блоки проверены — работаем дальше с лучшим.
        s_meta_begin = s_best_begin; s_meta_end = s_best_end; s_str_addr = s_best_addr;
        for (int i = 0; i < s_str_cand_count; ++i)
            if (s_blk_begin[i] == s_best_begin) s_blk_tried[i] = true;
        for (int i = 0; i < s_wanted_name_count; ++i) s_wanted_str[i] = s_best_str[i];
        s_wanted_str_found = (s_best_found > 0) ? s_best_found : 0;
        LogLine("автооффсеты: лучший блок строк 0x%llx..0x%llx — табличных имён %d из %d",
                (unsigned long long)s_meta_begin, (unsigned long long)s_meta_end,
                s_wanted_str_found, s_wanted_name_count);
        // Диагностика ротации: какие табличные имена в блоке отсутствуют.
        if (s_wanted_str_found > 0 && s_wanted_str_found < s_wanted_name_count) {
            char miss[512];
            int mp = 0, shown = 0;
            const int missing = s_wanted_name_count - s_wanted_str_found;
            for (int i = 0; i < s_wanted_name_count && mp < (int)sizeof(miss) - 48; ++i) {
                if (s_wanted_str[i]) continue;
                mp += snprintf(miss + mp, sizeof(miss) - mp, "%s%s",
                               shown ? ", " : "", s_wanted_names[i]);
                ++shown;
                if (shown >= 10) {
                    mp += snprintf(miss + mp, sizeof(miss) - mp,
                                   " и ещё %d", missing - shown);
                    break;
                }
            }
            LogLine("автооффсеты: не найдены имена: %s", miss);
        }
        if (s_wanted_str_found > 0) {
            if (s_ptr_range_count == 0) build_scan_ranges();
            s_ptr_passes = 1;
            s_ptr_region = 0; s_ptr_pos = 0; s_ptr_scanned = 0;
            s_ptr_hits = 0; s_ptr_verified = 0; s_ptr_inwin = 0; s_ptr_noverify = 0;
            s_ptr_names = 0; s_ptr_rinwin = 0; s_ptr_best_hits = 0; s_ptr_best_range = -1;
            for (int k = 0; k < 3; ++k) { s_top_hits[k] = 0; s_top_range[k] = -1; }
            s_ptr_readfails = 0;
            s_last_progress_log = 0;
            s_scan_deadline = mono_seconds() + 120.0;
            s_phase = PH_PTR;
        } else {
            LogLine("автооффсеты: блок строк есть, но табличных имён в нём нет — имена, видимо, ротированы обфускатором");
            s_phase = PH_DONE;
        }
    }
}

// Собираем карту поиска: читаемые (не исполняемые) сегменты libil2cpp.so +
// все анонимные регионы. Метадата может быть вшита в образ протектором, и
// тогда пул классов лежит в .data/.rodata библиотеке, а не в куче процесса.
static void build_scan_ranges() {
    s_ptr_range_count = 0;
    s_ptr_seg_bytes = 0;
    s_ptr_anon_bytes = 0;
    for (int i = 0; i < s_seg_count && s_ptr_range_count < 128; ++i) {
        if (s_segments[i].end <= s_segments[i].start) continue;
        s_ptr_ranges[s_ptr_range_count].start = s_segments[i].start;
        s_ptr_ranges[s_ptr_range_count].end = s_segments[i].end;
        s_ptr_seg_bytes += s_segments[i].end - s_segments[i].start;
        ++s_ptr_range_count;
    }
    for (int i = 0; i < s_xseg_count && s_ptr_range_count < 1024; ++i) {
        if (s_xsegments[i].end <= s_xsegments[i].start) continue;
        s_ptr_ranges[s_ptr_range_count].start = s_xsegments[i].start;
        s_ptr_ranges[s_ptr_range_count].end = s_xsegments[i].end;
        s_ptr_seg_bytes += s_xsegments[i].end - s_xsegments[i].start;
        ++s_ptr_range_count;
    }
    // Скрытые регионы (без бита чтения, но отдающиеся принудительным чтением).
    // Регионы размером от 16 до 64 МБ — главные кандидаты на глобальные
    // метаданные global-metadata.dat (в Unity размер файла почти всегда 20–50 МБ).
    // Помещаем их ПЕРВЫМИ, чтобы строковый поиск сразу попал в метаданные.
    s_hidden_skipped = 0;
    uint64_t taken_hidden = 0;
    // 1-й проход: кандидаты в метаданные (16..64 МБ)
    for (int i = 0; i < s_hidden_count && s_ptr_range_count < 1024; ++i) {
        if (s_hidden[i].end <= s_hidden[i].start) continue;
        const uint64_t sz = s_hidden[i].end - s_hidden[i].start;
        if (sz >= (16ull << 20) && sz <= (64ull << 20)) {
            s_ptr_ranges[s_ptr_range_count].start = s_hidden[i].start;
            s_ptr_ranges[s_ptr_range_count].end = s_hidden[i].end;
            s_ptr_anon_bytes += sz;
            taken_hidden += sz;
            ++s_ptr_range_count;
        }
    }
    // 2-й проход: остальные скрытые регионы (до бюджета 2048 МБ)
    for (int i = 0; i < s_hidden_count && s_ptr_range_count < 1024; ++i) {
        if (s_hidden[i].end <= s_hidden[i].start) continue;
        const uint64_t sz = s_hidden[i].end - s_hidden[i].start;
        if (sz >= (16ull << 20) && sz <= (64ull << 20)) continue; // уже добавлены
        if (taken_hidden && taken_hidden + sz > (2048ull << 20)) {
            ++s_hidden_skipped;
            continue;
        }
        s_ptr_ranges[s_ptr_range_count].start = s_hidden[i].start;
        s_ptr_ranges[s_ptr_range_count].end = s_hidden[i].end;
        s_ptr_anon_bytes += sz;
        taken_hidden += sz;
        ++s_ptr_range_count;
    }
    for (int i = 0; i < s_anon_count && s_ptr_range_count < 1024; ++i) {
        if (s_anon[i].end <= s_anon[i].start) continue;
        s_ptr_ranges[s_ptr_range_count].start = s_anon[i].start;
        s_ptr_ranges[s_ptr_range_count].end = s_anon[i].end;
        s_ptr_anon_bytes += s_anon[i].end - s_anon[i].start;
        ++s_ptr_range_count;
    }
    for (int i = 0; i < s_filemisc_count && s_ptr_range_count < 1024; ++i) {
        if (s_filemisc[i].end <= s_filemisc[i].start) continue;
        s_ptr_ranges[s_ptr_range_count].start = s_filemisc[i].start;
        s_ptr_ranges[s_ptr_range_count].end = s_filemisc[i].end;
        s_ptr_anon_bytes += s_filemisc[i].end - s_filemisc[i].start;
        ++s_ptr_range_count;
    }
    LogLine("автооффсеты: ищу указатели на имена в %d диапазонах — "
            "сегменты libil2cpp.so %llu МБ (из них исполняемых %llu МБ) + "
            "анонимная память %llu МБ (анонимных регионов %d из %d, файловых прочих %d)",
            s_ptr_range_count,
            (unsigned long long)(s_ptr_seg_bytes >> 20),
            (unsigned long long)(s_xseg_bytes >> 20),
            (unsigned long long)(s_ptr_anon_bytes >> 20),
            s_anon_count, s_anon_total, s_filemisc_count);
    if (s_anon_noread_count)
        LogLine("автооффсеты: не читается даже принудительно %d регионов (%llu МБ)",
                s_anon_noread_count, (unsigned long long)(s_anon_noread_bytes >> 20));
    if (s_hidden_count)
        LogLine("автооффсеты: в поиск добавлено скрытых регионов %d (%llu МБ) — без прав чтения, но читаемых%s",
                s_hidden_count, (unsigned long long)(s_hidden_bytes >> 20),
                s_hidden_skipped ? " (остальное не влезло в бюджет)" : "");
    if (s_anon_noread_count) {
        uint64_t top[2] = {0, 0};
        uint64_t topb[3] = {0, 0, 0};
        for (int i = 0; i < s_anon_noread_count; ++i) {
            const uint64_t sz = s_anon_noread[i].end - s_anon_noread[i].start;
            if (sz > topb[0]) { topb[2]=topb[1]; topb[1]=topb[0]; topb[0]=sz; top[1]=top[0]; top[0]=i; }
            else if (sz > topb[1]) { topb[2]=topb[1]; topb[1]=sz; top[1]=i; }
            else if (sz > topb[2]) { topb[2]=sz; }
        }
        LogLine("автооффсеты: крупнейшие НЕчитаемые регионы %llu МБ и %llu МБ — если там пул классов, взять его нечем",
                (unsigned long long)(topb[0] >> 20), (unsigned long long)(topb[1] >> 20));
    }
}

// Закрываем диапазон поиска: запоминаем, в каком из них больше всего
// указателей на строки — там и лежит пул классов (или его нет нигде).
static void ptr_range_done() {
    if (s_ptr_rinwin > s_ptr_best_hits) {
        s_ptr_best_hits = s_ptr_rinwin;
        s_ptr_best_range = s_ptr_region;
    }
    if (s_ptr_rinwin > 0) {
        for (int k = 0; k < 3; ++k) {
            if (s_ptr_rinwin <= s_top_hits[k]) continue;
            for (int j = 2; j > k; --j) { s_top_hits[j] = s_top_hits[j - 1]; s_top_range[j] = s_top_range[j - 1]; }
            s_top_hits[k] = s_ptr_rinwin;
            s_top_range[k] = s_ptr_region;
            break;
        }
    }
    s_ptr_rinwin = 0;
}

// Похоже ли содержимое на текстовое имя: печатных символов до NUL и хотя бы
// три знака. Отличает настоящие указатели на строки от случайного мусора.
static bool looks_like_name(const char* s, size_t cap) {
    size_t n = 0;
    while (n < cap && s[n]) {
        const unsigned char c = (unsigned char)s[n];
        if (c < 0x20 || c >= 0x7f) return false;
        ++n;
    }
    if (n == cap || n < 3) return false;         // не завершено NUL или пусто
    const unsigned char c0 = (unsigned char)s[0];
    return (c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z') || c0 == '_';
}

static void phase_ptr(double deadline) {
    static uint8_t buf[kChunkBytes];
    while (s_ptr_region < s_ptr_range_count && mono_seconds() < deadline) {
        const PtrRange& r = s_ptr_ranges[s_ptr_region];
        // Тот же курсор и та же ловушка, что в поиске строк (см. там):
        // диапазоны не отсортированы по адресу, поэтому вышедший за пределы
        // диапазона курсор возвращается на его начало.
        // Сканируем и образ, и ВСЕ анонимные регионы, не только writable:
        // протектор может закрыть пул классов на r-- после инициализации.
        // Конец диапазона и «курсор за пределами» — разные вещи, и путать их
        // нельзя. Лог №14 показал вторую ошибку подряд: курсор шёл ровно по
        // диапазону, в конце стал ему равен, а проверка возвращала его в начало
        // — диапазон перечитывался бесконечно, пока не выбран весь бюджет
        // («просмотрено 3072 МБ, 0 диапазонов из 322», ни одного указателя).
        // Теперь РАВЕНСТВО концу — это «диапазон пройден, берём следующий»,
        // а курсор ВНЕ диапазона (список не отсортирован по адресу) — повод
        // вернуть его в начало, иначе диапазон будет пропущен (лог №13).
        if (s_ptr_pos == r.end) { ptr_range_done(); ++s_ptr_region; continue; }
        if (s_ptr_pos < r.start || s_ptr_pos > r.end) s_ptr_pos = r.start;
        if (s_ptr_pos >= r.end) { ptr_range_done(); ++s_ptr_region; continue; }
        const uint64_t remain = r.end - s_ptr_pos;
        const uint64_t chunk = (kChunkBytes < remain) ? kChunkBytes : remain;
        if (chunk < 8) { ptr_range_done(); s_ptr_pos = r.end; ++s_ptr_region; continue; }
        if (!qrd_buf(s_ptr_pos, buf, chunk)) {
            ++s_ptr_readfails;
            if (s_ptr_pos == r.start && s_ptr_readfails <= 8)
                LogLine("автооффсеты: диапазон %d (0x%llx..0x%llx) недоступен для чтения",
                        s_ptr_region, (unsigned long long)r.start, (unsigned long long)r.end);
            s_ptr_pos += chunk; s_ptr_scanned += chunk; continue;
        }
        // Шаг 4 байта, а не 8: слот имени в структуре класса может быть не
        // выровнен по 8 (тогда указатель на 8-байтовой сетке не виден).
        for (size_t i = 0; i + 8 <= chunk; i += 4) {
            uint64_t v;
            memcpy(&v, buf + i, 8);
            v = memio::untag(v);
            // Имя может лежать абсолютным указателем, а может — RVA от начала
            // образа: вшитая в .so метадата часто хранит именно смещения.
            uint64_t cand[2];
            int ncand = 0;
            if (v >= s_meta_begin && v < s_meta_end) cand[ncand++] = v;
            else if (s_lib_begin && v && v < (2ull << 30) &&
                     s_lib_begin + v >= s_meta_begin && s_lib_begin + v < s_meta_end)
                cand[ncand++] = s_lib_begin + v;
            if (!ncand) continue;
            ++s_ptr_inwin;   // значение указывает внутрь окна строк (любое)
            ++s_ptr_rinwin;
            if (s_ptr_inwin <= 4)
                LogLine("автооффсеты: указатель в окно строк @0x%llx -> 0x%llx (диапазон %d)",
                        (unsigned long long)(s_ptr_pos + (uint64_t)i),
                        (unsigned long long)cand[0], s_ptr_region);
            // Сверяем НЕ адрес, а САМУ СТРОКУ по этому адресу. Имя лежит в
            // блоке по нескольку раз (лог №9: в одном окне две записи
            // «PlayerManager», обе дают 30 имён из 43), и адрес, запомненный
            // при считывании блока, почти наверняка не тот вход, на который
            // указывает класс. Читаем строку и сравниваем по содержимому.
            const uint64_t slot = s_ptr_pos + (uint64_t)i;
            char sname[64];
            int nm = -1;
            size_t rl = 0;
            for (int c = 0; c < ncand && nm < 0; ++c) {
                // Читаем не дальше конца блока: у самого края страницы лишние
                // байты могут быть не отображены, и чтение вернёт отказ.
                rl = 48;
                if (cand[c] + rl > s_meta_end) rl = (size_t)(s_meta_end - cand[c]);
                if (rl < 8) { rl = 0; continue; }
                if (!qrd_buf(cand[c], sname, rl)) { rl = 0; continue; }
                sname[rl] = 0;
                for (int n = 0; n < s_wanted_name_count; ++n) {
                    const char* w = s_wanted_names[n];
                    if (sname[0] != w[0]) continue;
                    const size_t wl = strlen(w);
                    if (wl + 1 > rl) continue;         // имя длиннее прочитанного
                    if (memcmp(sname, w, wl) != 0 || sname[wl] != '\0') continue;
                    nm = n;
                    break;
                }
            }
            if (nm < 0) {
                // Диагностика: что вообще лежит по этим указателям? Если это
                // имена классов — пул найден, и дело в ротации имён. Если
                // мусор — перед нами не указатели на строки вовсе.
                if (rl >= 8 && looks_like_name(sname, rl)) {
                    ++s_ptr_names;
                    if (s_ptr_names <= 8)
                        LogLine("автооффсеты: указатель @0x%llx ведёт на строку «%s» (цель 0x%llx)",
                                (unsigned long long)slot, sname, (unsigned long long)cand[0]);
                }
                continue;
            }
            ++s_ptr_hits;
            // Смещение слота имени в Il2CppClass: у беты раскладка может
            // отличаться от релизной. Перебираем plausible-варианты.
            static const uint64_t kNameOffs[] = {
                IL2CPP_CLASS_NAME, 0x18, 0x08, 0x20, 0x28, 0x30, 0x38, 0x40, 0x48
            };
            bool ok = false;
            for (int o = 0; o < (int)(sizeof(kNameOffs) / sizeof(kNameOffs[0])) && !ok; ++o) {
                const uint64_t klass = slot - kNameOffs[o];
                const uint64_t image = qrd_ptr(klass);
                if (!valid_obj(image)) continue;
                store_verified_class(klass, s_wanted_names[nm]);
                ++s_ptr_verified;
                if (kNameOffs[o] != IL2CPP_CLASS_NAME && s_ptr_verified <= 3)
                    LogLine("автооффсеты: слот имени класса не на +0x%llx, а на +0x%llx («%s» @0x%llx)",
                            (unsigned long long)IL2CPP_CLASS_NAME,
                            (unsigned long long)kNameOffs[o],
                            s_wanted_names[nm], (unsigned long long)klass);
                if (!s_good_base) {
                    s_good_base = g_il2cpp_base;
                    LogLine("автооффсеты: строковый бутстрап дал живой класс «%s» @0x%llx",
                            s_wanted_names[nm], (unsigned long long)klass);
                }
                if (!s_fields_off) calibrate_fields(klass);
                ok = true;
            }
            if (!ok) {
                ++s_ptr_noverify;
                if (s_ptr_noverify <= 3)
                    LogLine("автооффсеты: имя «%s» найдено @0x%llx, но ни одно смещение не дало живого класса (слот @0x%llx)",
                            s_wanted_names[nm], (unsigned long long)cand[0],
                            (unsigned long long)slot);
            }
        }
        s_ptr_pos += chunk;
        s_ptr_scanned += chunk;
        if (all_wanted_found()) break;
        if (s_ptr_scanned >= (6144ull << 20)) break;
        if (mono_seconds() >= s_scan_deadline) break;
        const double now_s = mono_seconds();
        if (now_s - s_last_progress_log >= 8.0) {
            s_last_progress_log = now_s;
            LogLine("автооффсеты: поиск указателей %llu МБ (диапазон %d/%d), в окне строк %llu, похожих на имена %llu, по строке совпало %llu, живых классов %llu, неподтверждённых %llu",
                    (unsigned long long)(s_ptr_scanned >> 20), s_ptr_region, s_ptr_range_count,
                    (unsigned long long)s_ptr_inwin,
                    (unsigned long long)s_ptr_names,
                    (unsigned long long)s_ptr_hits,
                    (unsigned long long)s_ptr_verified,
                    (unsigned long long)s_ptr_noverify);
        }
    }
    if (all_wanted_found() || s_ptr_region >= s_ptr_range_count ||
        s_ptr_scanned >= (6144ull << 20) || mono_seconds() >= s_scan_deadline) {
        ptr_range_done();
        LogLine("автооффсеты: бутстрап завершён — просмотрено %llu МБ (%d диапазонов из %d), "
                "отказов чтения %llu, в окне строк %llu значений, похожих на имена %llu, "
                "по строке совпало %llu, живых классов %llu, неподтверждённых %llu (в кэше %d)",
                (unsigned long long)(s_ptr_scanned >> 20), s_ptr_region, s_ptr_range_count,
                (unsigned long long)s_ptr_readfails,
                (unsigned long long)s_ptr_inwin,
                (unsigned long long)s_ptr_names,
                (unsigned long long)s_ptr_hits,
                (unsigned long long)s_ptr_verified,
                (unsigned long long)s_ptr_noverify, s_class_count);
        for (int k = 0; k < 3; ++k) {
            const int i = s_top_range[k];
            if (i < 0 || i >= s_ptr_range_count) continue;
            LogLine("автооффсеты: указателей в диапазоне №%d — %d (0x%llx..0x%llx) — %llu штук%s",
                    k + 1, i,
                    (unsigned long long)s_ptr_ranges[i].start,
                    (unsigned long long)s_ptr_ranges[i].end,
                    (unsigned long long)s_top_hits[k],
                    i == s_ptr_best_range ? " (лучший)" : "");
        }
        // Ни одного совпадения — возможно, классы ссылаются на ДРУГОЙ блок
        // строк (в процессе их бывает несколько). Прогоняем поиск заново по
        // следующему необработанному блоку: это те же диапазоны, несколько
        // секунд на проход.
        int next = -1, next_found = -1;
        for (int i = 0; i < s_str_cand_count; ++i) {
            if (s_blk_tried[i] || s_blk_found[i] <= 0) continue;
            if (s_blk_found[i] > next_found) { next_found = s_blk_found[i]; next = i; }
        }
        if (s_ptr_hits == 0 && next >= 0 && s_ptr_passes < 3) {
            ++s_ptr_passes;
            s_meta_begin = s_blk_begin[next];
            s_meta_end = s_blk_end[next];
            s_blk_tried[next] = true;
            s_ptr_region = 0; s_ptr_pos = 0; s_ptr_scanned = 0;
            s_ptr_hits = 0; s_ptr_verified = 0; s_ptr_inwin = 0;
            s_ptr_noverify = 0; s_ptr_names = 0; s_ptr_readfails = 0;
            s_ptr_rinwin = 0; s_ptr_best_hits = 0; s_ptr_best_range = -1;
            for (int k = 0; k < 3; ++k) { s_top_hits[k] = 0; s_top_range[k] = -1; }
            s_last_progress_log = 0;
            s_scan_deadline = mono_seconds() + 120.0;
            LogLine("автооффсеты: совпадений нет — пробую следующий блок строк №%d (0x%llx..0x%llx, имён %d)",
                    next + 1, (unsigned long long)s_meta_begin,
                    (unsigned long long)s_meta_end, s_blk_found[next]);
            return;
        }
        s_phase = PH_FIELDS;
    }
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
            // Ловушка беты: кэш может целиком состоять из мусорных «похожих»
            // структур — ни одного класса с табличным именем. Живой класс при
            // этом читается по вшитому TYPEINFO_RVA (привязка им проверяет
            // базу). Если он лежит В КУЧЕ (вне окна .so) — сканируем кучу
            // вокруг него: рантайм выделяет Il2CppClass из общих пулов, и
            // остальные классы лежат рядом. Если якорь внутри окна .so — там
            // уже отсканировано, куча не поможет.
            const char* anchor_name = nullptr;
            uint64_t anchor = (!s_heap_done && wanted_in_cache() == 0)
                                  ? anchor_class(&anchor_name) : 0;
            if (anchor && anchor_name && anchor_name[0]) {
                bool aok = false;
                const std::string an = qrd_string(qrd_ptr(anchor + IL2CPP_CLASS_NAME), &aok);
                if (aok && an != anchor_name) {
                    LogLine("автооффсеты: якорь 0x%llx («%s») не совпал с ожидаемым «%s» — начинаю поиск строк в памяти",
                            (unsigned long long)anchor, an.c_str(), anchor_name);
                    anchor = 0;
                    s_str_region = 0; s_str_pos = 0; s_str_scanned = 0;
                    s_phase = PH_STRS;
                }
            }
            if (anchor && s_lib_begin && anchor >= s_lib_begin && anchor < s_lib_end)
                anchor = 0;
            if (anchor) start_heap_scan(anchor);
            else if (s_phase != PH_STRS) {
                if (wanted_in_cache() == 0) {
                    LogLine("автооффсеты: табличных классов в .so нет — начинаю поиск строк в памяти");
                    s_str_region = 0; s_str_pos = 0; s_str_scanned = 0;
                    s_phase = PH_STRS;
                } else {
                    s_phase = PH_FIELDS;
                }
            }
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
        const uint64_t e = fields_ptr + (uint64_t)i * s_fi_size;
        const uint64_t parent = qrd_ptr(e + s_fi_parent);
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
    if (score > s_calib_best) { s_calib_best = score; s_calib_best_off = off; }
    return score;
}

static bool calibrate_fields(uint64_t klass) {
    // Сначала табличное значение — почти всегда оно и есть.
    const uint64_t table_off = IL2CPP_CLASS_FIELDS;
    if (probe_fields_shape(klass, table_off) >= 5) {
        s_fields_off = table_off;
        return true;
    }
    // Шире привычного 0x70–0x178: на другой сборке игры массив полей может
    // съехать дальше обычного (проба дешёвая: мусор отваливается за 1 чтение).
    for (uint64_t f = 0x30; f <= 0x1F8; f += 8) {
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
    if (!s_meta_begin && !valid_str_ptr(nmp)) return false;
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
        const uint64_t e = fields_ptr + (uint64_t)i * s_fi_size;
        if (qrd_ptr(e + s_fi_parent) != klass) break;
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

// Дамп первых байт якоря: даже если имена классов затёрты обфускатором, по
// структуре видно, настоящий ли это Il2CppClass (image в окне .so на +0x00,
// правдоподобные указатели в табличных слотах).
static void log_anchor_dump(uint64_t anchor) {
    uint8_t raw[48] = {};
    if (!qrd_buf(anchor, raw, sizeof(raw))) {
        LogLine("автооффсеты: якорь — дамп структуры не читается");
        return;
    }
    char hex[3 * sizeof(raw) + 16] = {};
    size_t hn = 0;
    for (size_t i = 0; i < sizeof(raw); ++i) {
        if (i && i % 8 == 0 && hn + 3 < sizeof(hex)) { hex[hn++] = ' '; hex[hn++] = '|'; hex[hn++] = ' '; }
        hn += (size_t)snprintf(hex + hn, sizeof(hex) - hn, "%02x ", raw[i]);
    }
    LogLine("автооффсеты: якорь — первые 48 байт: %s", hex);
    const uint64_t image = qrd_ptr(anchor);
    const uint64_t nm = qrd_ptr(anchor + IL2CPP_CLASS_NAME);
    const uint64_t ns = qrd_ptr(anchor + IL2CPP_CLASS_NAMESPACE);
    const uint64_t fp = qrd_ptr(anchor + IL2CPP_CLASS_FIELDS);
    LogLine("автооффсеты: якорь — image 0x%llx %s, имя 0x%llx %s, нс 0x%llx %s, fields@0x80 0x%llx %s",
            (unsigned long long)image,
            (s_lib_begin && image >= s_lib_begin && image < s_lib_end) ? "в окне .so" : "ВНЕ окна",
            (unsigned long long)nm, valid_str_ptr(nm) ? "указатель валиден" : "указатель НЕвалиден",
            (unsigned long long)ns, valid_str_ptr(ns) ? "валиден" : "НЕвалиден",
            (unsigned long long)fp, valid_obj(fp) ? "валиден" : "НЕвалиден");
}

// Автоподбор раскладки: если табличные смещения не сошлись, ищем массив, где
// подряд несколько записей ссылаются обратно на класс (родитель), перебирая
// слот родителя и шаг записи. Возврат: найдено ли; результат в out.
struct FiLayout { uint64_t fields_off; uint64_t parent_off; uint64_t size; };
static bool probe_layout(uint64_t klass, FiLayout& out) {
    static const uint64_t kParents[] = {0x08, 0x10, 0x18, 0x20};
    static const uint64_t kSizes[]   = {0x18, 0x20, 0x28};
    for (uint64_t f = 0x30; f <= 0x1F8; f += 8) {
        const uint64_t fp = qrd_ptr(klass + f);
        if (!valid_obj(fp)) continue;
        for (int p = 0; p < 4; ++p) {
            for (int s = 0; s < 3; ++s) {
                int ok = 0;
                for (int i = 0; i < 4; ++i) {
                    const uint64_t e = fp + (uint64_t)i * kSizes[s];
                    if (qrd_ptr(e + kParents[p]) == klass) ++ok;
                    else break;
                }
                if (ok >= 4) {
                    out.fields_off = f;
                    out.parent_off = kParents[p];
                    out.size = kSizes[s];
                    return true;
                }
            }
        }
    }
    return false;
}

static void phase_fields() {
    // Якорь: по вшитому TYPEINFO_RVA читается ЖИВОЙ класс (привязка сверяет
    // базу ровно этой проверкой). Логуем его имя — сразу видно, ротированы ли
    // имена в этой сборке игры, — и калибруем поля прямо на нём (надёжнее,
    // чем гадать по кэшу, который на бете может быть весь мусорный).
    // Якорь по вшитому RVA нужен только если строковый бутстрап ничего не дал:
    // после апдейта игры RVA указывает в код, и доверять ему нельзя.
    const char* anchor_expected = nullptr;
    const uint64_t anchor = (s_ptr_verified == 0) ? anchor_class(&anchor_expected) : 0;
    if (anchor) {
        bool aok = false;
        const std::string aname = qrd_string(qrd_ptr(anchor + IL2CPP_CLASS_NAME), &aok);
        char shown[40] = {};
        if (aok && aname.size() < sizeof(shown)) {
            for (size_t i = 0; i < aname.size(); ++i) {
                const char c = aname[i];
                shown[i] = (c >= 0x20 && c < 0x7f) ? c : '.';
            }
        }
        LogLine("автооффсеты: якорь 0x%llx (%s окна .so), ждали «%s», имя %s «%s»",
                (unsigned long long)anchor,
                (s_lib_begin && anchor >= s_lib_begin && anchor < s_lib_end)
                    ? "внутри" : "ВНЕ",
                anchor_expected, aok ? "читается" : "НЕ читается",
                aok ? shown : "-");
        if (aok && anchor_expected && aname != anchor_expected) {
            // Имя не совпало с ожидаемым: печатаем первые байты — по ним
            // видно, ротация это/ксор или что-то ещё.
            char hex[64] = {};
            size_t hn = 0;
            for (size_t i = 0; i < aname.size() && i < 16 && hn + 4 < sizeof(hex); ++i)
                hn += (size_t)snprintf(hex + hn, sizeof(hex) - hn, "%02x ",
                                       (unsigned)(uint8_t)aname[i]);
            LogLine("автооффсеты: якорь — байты имени: %s", hex);
        }
        log_anchor_dump(anchor);
        if (!s_fields_off) calibrate_fields(anchor);
    }
    // Калибровка: первым пробуем самый «толстый» класс (больше полей —
    // надёжнее форма), потом ВСЕ классы с именами из таблицы (совпадение
    // имени с дампом — признак настоящего класса; на бете кэш без окна
    // метаданных полон мусора, и «первые 12» могли оказаться мусорными),
    // дальше — любые найденные, пока не сойдётся.
    const int wanted = wanted_in_cache();
    uint64_t first_choice = 0;
    for (int i = 0; i < s_class_count && !first_choice; ++i)
        if (strcmp(s_classes[i].name, "PlayerManager") == 0) first_choice = s_classes[i].klass;
    if (first_choice) calibrate_fields(first_choice);
    for (int i = 0; i < s_class_count && !s_fields_off; ++i)
        if (name_is_wanted(s_classes[i].name)) calibrate_fields(s_classes[i].klass);
    for (int i = 0; i < s_class_count && !s_fields_off && i < 64; ++i)
        calibrate_fields(s_classes[i].klass);
    if (!s_fields_off && anchor) {
        // Табличная раскладка не сошлась: ищем массив полей с другим шагом
        // записи/слотом родителя (другая сборка рантайма).
        FiLayout lay{};
        if (probe_layout(anchor, lay)) {
            s_fields_off = lay.fields_off;
            s_fi_parent = lay.parent_off;
            s_fi_size = lay.size;
            LogLine("автооффсеты: подобрана ДРУГАЯ раскладка полей — fields@0x%llx, "
                    "parent@0x%llx, шаг 0x%llx",
                    (unsigned long long)lay.fields_off,
                    (unsigned long long)lay.parent_off,
                    (unsigned long long)lay.size);
        }
    }
    if (!s_fields_off) {
        LogLine("автооффсеты: калибровка полей не сошлась — кэш %d классов, "
                "табличных имён в кэше %d, лучший счёт формы полей %d@0x%llx",
                s_class_count, wanted, s_calib_best,
                (unsigned long long)s_calib_best_off);
        s_phase = PH_DONE;
        return;
    }
    if (anchor) {
        // Читаются ли имена ПОЛЕЙ: даже найдя массив, поля мы ищем по имени.
        bool fok = false;
        char shown[40] = {};
        const uint64_t fp = qrd_ptr(anchor + s_fields_off);
        if (valid_obj(fp)) {
            const uint64_t nmp = qrd_ptr(fp + IL2CPP_FIELDINFO_NAME);
            const std::string fname = qrd_string(nmp, &fok);
            if (fok && fname.size() < sizeof(shown)) {
                for (size_t i = 0; i < fname.size(); ++i) {
                    const char c = fname[i];
                    shown[i] = (c >= 0x20 && c < 0x7f) ? c : '.';
                }
            }
        }
        LogLine("автооффсеты: якорь — имя первого поля %s «%s»",
                fok ? "читается" : "НЕ читается", fok ? shown : "-");
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
    const int wanted = wanted_in_cache();
    LogLine("автооффсеты: классов найдено %d (по форме %d, имён прочитано %d, отказов %d, табличных %d), "
            "fields@0x%llx, разрешено %d/%d констант (%d изменено), метаданные %s, "
            "база 0x%llx, пустых баз %d, бутстрап: строк %d/%d, по указателям классов %llu",
            s_class_count, s_shape_pass, s_names_ok, s_names_fail, wanted,
            (unsigned long long)s_fields_off, s_resolved, kOrFieldCount, s_changed,
            s_meta_begin ? "найдены" : "НЕ найдены (фильтр слабее)",
            (unsigned long long)g_il2cpp_base, s_dead_count,
            s_wanted_str_found, s_wanted_name_count,
            (unsigned long long)s_ptr_verified);
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
        case PH_HEAP:   phase_heap(deadline); break;
        case PH_STRS:   phase_strsearch(deadline); break;
        case PH_BLOB:   phase_blob(deadline); break;
        case PH_PTR:    phase_ptr(deadline); break;
        case PH_FIELDS: phase_fields(); break;
        case PH_DONE:   break;
    }
    if (s_phase == PH_DONE) log_summary();
}

void offset_resolver_reset() {
    s_phase = PH_WAIT;
    s_seg_count = 0; s_seg_idx = 0; s_scan_pos = 0;
    s_xseg_count = 0; s_xseg_bytes = 0;
    s_lib_begin = s_lib_end = 0; s_meta_begin = s_meta_end = 0;
    s_total_bytes = 0; s_scanned_bytes = 0;
    s_maps_lib_lines = 0; s_maps_cut_window = 0; s_maps_cut_x = 0;
    s_maps_cut_deleted = 0; s_maps_open_failed = false;
    s_class_count = 0; s_names_ok = 0; s_names_fail = 0; s_shape_pass = 0;
    s_fields_off = 0; s_calib_best = 0; s_calib_best_off = 0;
    s_fi_size = IL2CPP_FIELDINFO_SIZE; s_fi_parent = IL2CPP_FIELDINFO_PARENT;
    s_resolved = 0; s_changed = 0;
    s_heap_pos = s_heap_end = 0; s_heap_done = false;
    s_anon_count = 0; s_anon_total = 0; s_filemisc_count = 0;
    s_anon_noread_bytes = 0; s_anon_noread_count = 0;
    s_anon_bytes = 0; s_anon_skip_x = 0; s_anon_skip_x_bytes = 0;
    s_anon_skip_small = 0; s_anon_biggest = 0;
    s_hidden_count = 0; s_hidden_bytes = 0; s_hidden_skipped = 0;
    s_probe_ok = 0; s_probe_bytes = 0; s_probe_zero = 0; s_probe_zero_bytes = 0;
    s_str_addr = 0; s_str_region = 0; s_str_pos = 0; s_str_scanned = 0;
    s_str_cand_count = 0; s_str_cand_idx = 0;
    s_best_found = -1; s_best_begin = 0; s_best_end = 0; s_best_addr = 0;
    for (int i = 0; i < 8; ++i) {
        s_blk_begin[i] = 0; s_blk_end[i] = 0; s_blk_found[i] = 0; s_blk_tried[i] = false;
    }
    for (int i = 0; i < 8; ++i) { s_str_cand[i] = 0; s_str_cand_region[i] = 0; }
    for (int i = 0; i < 64; ++i) s_best_str[i] = 0;
    s_wanted_name_count = 0; s_wanted_str_found = 0;
    s_blob_pos = 0; s_blob_fails = 0;
    for (int i = 0; i < 64; ++i) s_wanted_str[i] = 0;
    s_ptr_pos = 0; s_ptr_region = 0; s_ptr_scanned = 0;
    s_ptr_hits = 0; s_ptr_verified = 0; s_ptr_inwin = 0; s_ptr_noverify = 0;
    s_ptr_names = 0; s_ptr_rinwin = 0; s_ptr_best_hits = 0; s_ptr_best_range = -1;
            for (int k = 0; k < 3; ++k) { s_top_hits[k] = 0; s_top_range[k] = -1; }
    s_ptr_passes = 0; s_ptr_readfails = 0;
    s_ptr_range_count = 0; s_ptr_seg_bytes = 0; s_ptr_anon_bytes = 0;
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
    return s_phase == PH_MAPS || s_phase == PH_SCAN ||
           s_phase == PH_HEAP || s_phase == PH_STRS ||
           s_phase == PH_BLOB || s_phase == PH_PTR ||
           s_phase == PH_FIELDS;
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
    if (s_phase == PH_HEAP) {
        snprintf(out, cap, "ищет классы в куче у якоря: %llu/%llu МБ, табличных %d",
                 (unsigned long long)(s_scanned_bytes >> 20),
                 (unsigned long long)(s_total_bytes >> 20),
                 wanted_in_cache());
        return;
    }
    if (s_phase == PH_STRS || s_phase == PH_BLOB || s_phase == PH_PTR) {
        snprintf(out, cap, "поиск по строкам: просмотрено %llu МБ, имён %d, классов %d",
                 (unsigned long long)((s_str_scanned + s_ptr_scanned) >> 20),
                 s_wanted_str_found, s_class_count);
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
