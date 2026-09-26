// offset_resolver.cpp — автообновление оффсетов по IL2CPP-метаданным живого
// процесса игры (идея «нетвар-менеджера»: оффсеты не только вшиты, но и
// вычитываются из самой игры по именам классов и полей).
//
// Зачем: после апдейта игры оффсеты полей и слоты классов меняются, и раньше
// чит молчал, пока оффсеты не обновят руками по новому дампу. Теперь при
// привязке модуль сам:
//   1) находит все отображения libil2cpp.so (кроме исполняемых) и окно
//      строк метаданных по /proc/<pid>/maps. Если имени «global-metadata» в
//      карте нет (после апдейта 26.09 игра мапит метадату прямо из .apk) и
//      вшитый TYPEINFO_RVA устарел, блок метаданных ищется по СИГНАТУРЕ
//      0xFAB11BAF (Il2CppGlobalMetadataHeader.sanity), а окно имён берётся
//      из его заголовка — поля stringOffset/stringSize (фаза PH_META);
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
//
// Порядок попыток, когда живого класса-якоря нет (игра обновилась):
//   PH_META  — блок метаданных по сигнатуре (начала отображений, затем
//              страницы; декой-блоки отбрасываются по отсутствию имён);
//   PH_SCAN  — слоты классов в сегментах .so (уже с точным окном имён);
//   PH_BLOB  — адреса всех табличных имён внутри окна;
//   PH_PTR   — указатели на эти адреса = живые Il2CppClass;
//   разведка окрестностей имени — если указателей на имя нет вовсе (имя лежит
//              инлайном в структуре), класс ищется среди соседних указателей
//              с полной проверкой формы;
//   PH_STRS  — последний рубеж: поиск имени по всей памяти.

#include "game.h"
#include "game_internal.h"
#include "game_offsets_active.h"
#include "logfile.h"
#include "str.h"

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

#include <string>

using namespace go_active;

// ---- Таблица: какие константы каким (класс, поле) заполнять ----------------
// Сгенерирована из tools/offsets/offsets_map.json (gen_resolver_table.py).
// Пятый столбец — сигнатура типа поля: запасной способ найти поле, когда его
// ИМЯ ротировано обфускатором (см. resolve_type_group ниже).
namespace {
struct OrField {
    std::uint64_t* slot;
    const char*    cls;     // имя класса в рантайме (у некоторых — с подчёркиваниями)
    const char*    field;   // имя поля (стабильное, из дампа)
    bool           is_static;
    const char*    type;    // "" | f4 | b1 | u4 | i4 | str | obj | obj1 | c:ИмяКласса
};
#define OR_VIA(...)   // в первом массиве цепочки не нужны
#define OR_FIELD(c, k, f, s, t) { &go_active::c, k, f, s, t },
static const OrField kOrFields[] = {
#include "offset_resolver_table.inc"
};
#undef OR_FIELD
#undef OR_VIA
constexpr int kOrFieldCount = (int)(sizeof(kOrFields) / sizeof(kOrFields[0]));

// Классы с РОТИРУЮЩИМСЯ именами (PlayerEventHandler, nicklabel, VoicePlayer...):
// обфускатор переименовывает их в каждом билде, поэтому по имени они не
// находятся. Но их ТИП не меняется: поле PlayerManager.playerEventHandler
// ссылается именно на такой класс, а FieldInfo.type того поля несёт индекс
// определения типа (klassIndex). По индексу класс находится в памяти — имя не
// нужно (см. phase_kid). Таблица описывает цепочку: из какого класса и через
// какое поле прийти (hops), какое поле забрать (field) и его тип (type).
struct ViaField {
    std::uint64_t* slot;
    const char*    via_cls;    // стартовый класс — ищется по имени в кэше
    const char*    hops[2];    // hops[0] — поле в via_cls, hops[1] — в классе после него
    int            nhops;
    const char*    field;      // искомое поле в целевом классе
    bool           is_static;
    const char*    type;       // сигнатура типа (как у OR_FIELD)
    const char*    rot;        // ротированное имя целевого класса (из карты)
};
#define OR_FIELD(...)  // во втором массиве обычные записи не нужны
#define OR_VIA(c, k, h1, h2, f, s, t, r) \
    { &go_active::c, k, {h1, h2}, h2[0] ? 2 : 1, f, s, t, r },
static const ViaField kViaFields[] = {
#include "offset_resolver_table.inc"
};
#undef OR_VIA
#undef OR_FIELD
constexpr int kViaFieldCount = (int)(sizeof(kViaFields) / sizeof(kViaFields[0]));
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
    p = memio::untag(p);
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
    // Подтверждена ли форма (массив полей на месте, записи похожи на FieldInfo).
    // Лог 26.09 показал, зачем: один проход по указателям дал 4135 «классов»,
    // массив упёрся в потолок из одних тёзок, и резолв полей пошёл по мусору
    // (5/160 констант вместо 153/160).
    bool     shape_ok = false;
    // 64: имена вида «MineableObjectExtension_OreHitstreaksMarker» — 43 знака,
    // и запас на такие же «составные» имена.
    char     name[64] = {};
    char     ns[40] = {};
};

struct Segment { uint64_t start = 0, end = 0; };

// Фазы. Сканы порционные: на устройство с придушенным процессом нельзя
// вываливать десятки мегабайт чтений одним кадром (см. скан маркеров).
enum Phase {
    PH_WAIT = 0,    // нет привязки
    PH_MAPS,        // разобрать /proc/<pid>/maps
    PH_SCAN,        // поиск слотов Il2CppClass
    PH_KID,         // охота за классом по индексу типа (цепочки via)
    PH_FIELDS,      // калибровка массива полей + резолв таблицы
    PH_DONE,
    PH_HEAP,        // скан кучи вокруг «якоря» (когда в .so классов не нашлось)
    PH_META,        // поиск блока global-metadata по сигнатуре 0xFAB11BAF
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
// Отпечаток образа игры для кэша оффсетов и признак «взято из кэша».
uint64_t s_image_key = 0;
bool     s_from_cache = false;
bool     s_cache_checked = false;
bool     s_cache_saved = false;
bool     s_cache_retryable = false;    // ключ совпал, но класс-якорь ещё не загрузился
// Кэш без живого якоря: смещения применены, классы ищутся сканом. Повторно
// такой кэш применять не нужно (и ждать якорь — тоже).
bool     s_cache_offsets_used = false;
double   s_cache_wait_since = 0;
double   s_cache_retry_at = 0;
double   s_cache_save_retry_at = 0;
double   s_anchor_wait_since = 0;
int      s_anchor_retries = 0;
// Обычный потолок кэша. Хвост массива — резерв: лог с беты показал, что БЕЗ
// окна метаданных «мягкий» фильтр пропускает сотни тысяч мусорных кандидатов,
// кэш забивается мусором ещё в начале скана, и настоящие классы (вставшие в
// образе дальше) в него не попадают. Классы с именами ИЗ ТАБЛИЦЫ сохраняем
// даже сверх потолка (их всего ~44 имени, резерв с запасом).
constexpr int kClassCap = 768;
// Сколько копий одного имени держим: тёзки в игре реальны (пустой
// PlayerManager без полей рядом с настоящим), но их единицы, а не тысячи.
constexpr int kSameNameCap = 6;
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
struct HeapScanRange { uint64_t start = 0, end = 0; };
static HeapScanRange s_heap_ranges[64];
static int           s_heap_range_count = 0;
static int           s_heap_range_idx = 0;
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
AnonRegion s_hidden[768];         // закрытые по правам, но читаемые на деле
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
int      s_str_needle_idx = 0;
static const char* const kStrNeedles[] = {
    "PlayerManager",
    "GameControllerBase",
    "NetworkClient"
};
static void start_strsearch(const char* reason);
// Кандидат на блок строк: «плотный» (строки впритык, как в таблице строк
// метаданных) или «разреженный» (имя лежит в структуре, вокруг нули). Раньше
// разреженные ОТБРАСЫВАЛИСЬ — и лог 26.09 показал цену: в новой сборке игры
// имена нашлись только в таком виде, а резолвер написал «строка не найдена».
// Теперь берём оба вида, плотные пробуем первыми.
bool     s_str_cand_packed[8] = {};
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
// ---- Разведка диапазонов -----------------------------------------------------
// Классы лежат кучно: в логах 26.09 из 450–700 диапазонов указатели на имена
// давали два-три («0x7710000000..0x7717000000 — 346370 штук», «0x76c0…000 —
// 100186»), остальные — единицы. Гнать полный проход по 5 ГБ в адресном
// порядке значит найти нужное в лучшем случае к середине. Поэтому сначала
// дешёвая разведка: по 8 проб на диапазон, и порядок обхода — по плотности
// попаданий. Ни один диапазон не выбрасывается, меняется только очередь.
int      s_ptr_order[2048];        // порядок обхода (индексы в s_ptr_ranges)
int      s_ptr_score[2048];        // попаданий в окно строк на пробах
int      s_scout_idx = 0;          // прогресс разведки
bool     s_scout_done = false;
uint64_t s_scout_bytes = 0;
constexpr int    kScoutProbes = 8;            // проб на диапазон
constexpr size_t kScoutProbeBytes = 16 * 1024; // по 16 КБ на пробу
// «Горячие» диапазоны прошлого прохода: при повторе их проверяем первыми.
uint64_t s_hot_begin[4] = {}, s_hot_end[4] = {};
int      s_hot_count = 0;
// Ранняя остановка: как только ключевые классы найдены, дальше сканируем
// совсем немного (соседи в том же пуле) и заканчиваем.
uint64_t s_ptr_enough_at = 0;
constexpr uint64_t kPtrGraceBytes = 192ull << 20;
// Три лучших диапазона, а не один: пул классов может быть не самым
// «густонаселённым» местом (лог №12: на первом месте таблица имён шейдеров
// — 3745 указателей, а нужного пула в списке не видно вовсе).
uint64_t s_top_hits[3] = {0, 0, 0};
int      s_top_range[3] = {-1, -1, -1};

// ---- Блок global-metadata по СИГНАТУРЕ ---------------------------------------
// Главный урок лога 26.09 (игра обновилась): раньше окно строк метаданных
// искалось ровно двумя способами — по имени отображения «global-metadata» в
// /proc/<pid>/maps и по вшитому TYPEINFO_RVA (якорю). После апдейта не
// сработало ни то, ни другое: имени в карте больше нет (Unity на Android
// мапит метадату прямо из .apk, а такие строки карты у нас вообще
// вычёркивались), а RVA уехал. Резолвер остался без окна имён, принял 768
// мусорных структур за классы и честно доложил «имена зашифрованы».
//
// На деле у файла метаданных есть опознавательный знак, который не зависит ни
// от имени файла, ни от версии игры: первые четыре байта заголовка
// Il2CppGlobalMetadataHeader — 0xFAB11BAF («sanity»), следом int32 версии
// (Unity 2018…6 — это 24…31). Так его ищут и Il2CppDumper, и Il2CppInspector:
// «если файла нет на обычном месте — просканируй всё на эти байты».
// Мы делаем то же самое по ЖИВОЙ памяти процесса и берём из заголовка точное
// окно таблицы строк (stringOffset/stringSize) — это лучше прежних «±32 МБ
// вокруг имени», потому что окно получается ровно тем, чем должно быть.
constexpr uint32_t kMetaSanity  = 0xFAB11BAFu;   // Il2CppGlobalMetadataHeader.sanity
constexpr uint64_t kMetaMaxSize = 768ull << 20;  // потолок здравого смысла для файла метаданных
constexpr int      kMetaHeaderPairs = 30;        // сколько пар (offset,size) проверяем/перебираем

struct MetaRange { uint64_t start = 0, end = 0; };
MetaRange s_meta_ranges[1024];
int       s_meta_range_count = 0;
int       s_meta_range_idx = 0;
uint64_t  s_meta_pos = 0;
uint64_t  s_meta_scanned = 0;
int       s_meta_pass = 0;        // 0 — только начала отображений, 1 — все страницы
bool      s_meta_sig_done = false;  // сигнатурный поиск в этом резолве уже отработал
bool      s_meta_scan_first = false;  // после сигнатуры вернуться к скану .so, а не к блобу
bool      s_meta_quick_only = false;  // первый заход — только начала отображений (доли секунды)
// Блоки, которые оказались пустышкой: сигнатура и заголовок в порядке, а
// табличных имён в таблице строк нет. У протекторов встречается декой-файл
// метаданных рядом с настоящим, поэтому найденный блок — ещё не финал: если
// имён в нём нет, поиск продолжается со следующего.
uint64_t  s_meta_bad[4] = {};
int       s_meta_bad_count = 0;
int       s_meta_tries = 0;
bool      s_meta_from_sig = false;  // окно строк получено из заголовка метаданных
uint64_t  s_meta_blob = 0;          // адрес самого заголовка
int32_t   s_meta_version = 0;       // версия формата метаданных (24…31)
const char* s_meta_reason = nullptr; // зачем затеяли бутстрап (для лога)

// Файловые отображения ЛЮБЫХ имён (включая .apk) — только для сигнатурного
// поиска. В поиск указателей они по-прежнему не идут: пул классов там не
// лежит, а гигабайтный .apk сжёг бы весь бюджет.
struct NamedRange { uint64_t start = 0, end = 0; char path[48] = {}; };
NamedRange s_filesig[512];
int        s_filesig_count = 0;
uint64_t   s_filesig_bytes = 0;

// Диапазоны поиска указателей на имена. Порядок важный: СНАЧАЛА читаемые
// сегменты самой libil2cpp.so, ПОТОМ анонимная память. Протектор может вшить
// метадату (массив Il2CppClass вместе с указателями имён) прямо в образ —
// тогда в куче искать нечего, а классы лежат в .data/.rodata библиотеки.
struct PtrRange { uint64_t start = 0, end = 0; };
PtrRange  s_ptr_ranges[2048];
int       s_ptr_range_count = 0;
uint64_t  s_ptr_seg_bytes = 0;    // сколько МБ в сегментах .so
uint64_t  s_ptr_anon_bytes = 0;   // сколько МБ в анонимной памяти
static void build_scan_ranges();   // определение ниже, вызывается из phase_strsearch
static void start_heap_scan(uint64_t anchor);
// Бутстрап без якоря: сначала блок метаданных по сигнатуре, потом разбор его
// таблицы строк, и только если и это не вышло — старый поиск имён по памяти.
static void start_metadata_bootstrap(const char* reason);
static void start_meta_scan(const char* reason);
static void start_blob_scan(const char* reason);
static bool meta_header_ok(uint64_t addr, uint64_t* out_begin, uint64_t* out_end,
                           int32_t* out_version);
// Проверка «структура похожа на живой Il2CppClass» (определена ниже).
static bool class_shape_ok(uint64_t klass);

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
// Значения слотов ДО этого резолва и качество предыдущего: новый проход не
// имеет права затереть хороший результат худшим (лог 26.09: второй резолв
// разрешил 5 констант вместо 153 и испортил PlayerManager.team).
AppliedRec s_rollback[256];
int        s_rollback_count = 0;
// Сколько записей в s_applied пришло из кэша (их откатывать нельзя) и сколько
// значений применил ЖИВОЙ резолв этого прохода.
int        s_applied_base = 0;
int        s_live_resolved = 0;
int        s_prev_resolved = 0;   // сколько разрешил последний удачный резолв (на этом pid)
// Класс каждой применённой записи: по нему второй проход понимает, какие
// смещения этого класса уже заняты (поле, найденное по имени, повторно не
// отдаётся полю с ротированным именем).
const char* s_applied_cls[256];
// ---- Охота за классами с ротированными именами (цепочки via) ---------------
// Найденные по индексу типа классы: kidx -> живой Il2CppClass.
struct ViaClass { int32_t kidx = 0; std::uint64_t klass = 0; };
ViaClass  s_via_classes[16];
int       s_via_class_count = 0;
// Что ищем прямо сейчас: индекс определения типа -> чья это цепочка.
int32_t        s_want_kidx[8];
const ViaField* s_want_owner[8];
int            s_want_kidx_count = 0;
bool           s_via_done = false;   // охоту больше не повторять
int            s_via_found = 0;      // сколько цепочек замкнулось
// Курсор охоты по диапазонам (порционно, как и остальные сканы).
int            s_kid_region = 0;
std::uint64_t  s_kid_pos = 0;
std::uint64_t  s_kid_scanned = 0;
// Ротированное имя класса -> его kidx. Тип поля бывает ссылкой на ДРУГОЙ
// ротированный класс («c:eDw»), и в кэше классов такого имени нет — сверять
// не с чем. Зато kidx у таких классов известен из цепочек: eDw — это Aim
// второй цепочки, а eDg — playerEventHandler первой. Ищем по имени из карты.
struct RotKidx { const char* name; int32_t kidx; };
RotKidx  s_rot_kidx[16];
int      s_rot_kidx_count = 0;
// Результат цепочки: kidx каждого шага и класс, который по нему нашёлся.
int32_t       s_via_kidx[kViaFieldCount][2];
std::uint64_t s_via_klass[kViaFieldCount][2];
// Метка целевого класса для списка занятых смещений (см. третий проход).
char          s_via_mark[kViaFieldCount][40];int      s_applied_count = 0;
bool     s_logged = false;
// Повторный подбор. Игра поднимается не мгновенно: пока грузится мир, классов
// Assembly-CSharp в памяти просто НЕТ (рантайм создаёт Il2CppClass по мере
// обращения). Лог 26.09 17:56 — ровно этот случай: метадата нашлась, все 46
// имён в таблице строк есть, а по указателям нашлись только системные классы
// (Delegate, Dictionary), PlayerManager не существовал. Резолвер уходил в
// PH_DONE навсегда и больше не пробовал — чит так и оставался пустым.
constexpr int kRescanLimit = 20;   // 5 попыток по 25 с + 15 по 60 с ≈ 17 минут
int      s_rescan_count = 0;
double   s_rescan_at = 0;
bool     s_so_scan_failed = false;   // скан .so на этой базе уже не дал табличных классов
bool     s_so_scan_done = false;     // скан .so в этом проходе уже отработал
bool     s_bootstrap_done = false;   // строковый бутстрап в этом проходе уже отработал
pid_t    s_bound_pid = -1;        // к какому процессу/базе собрано состояние
uint64_t s_bound_base = 0;
double   s_last_progress_log = 0; // темп строк прогресса в лог

// Бюджет такта 4 мс (как у других сканов): потеря привязки на устройстве, как
// выяснилось по логу, была не от чтений сканера, а от смотрителя кадра.
// Крупный кусок — статический буфер, стеку не стоит.
//
// Общий потолок скана ОСОЗНАННО щедрый (300 с): лог 24.09 показал, что при
// 90 с скан кучи обрывался на 153/248 МБ и часть табличных классов так и не
// находилась — их поля оставались на вшитых значениях («не все оффсеты
// автообновляются»). Скан порционный (4 мс/кадр), поэтому длинный потолок не
// грузит игру, а после первого входа результат берётся из кэша мгновенно.
constexpr double kTickBudgetSec   = 0.004;  // 4 мс на такт, когда чит уже рисует кадр
// Пока ключевых классов нет, чит СЛЕП: ESP нечего рисовать, аиму не за кем
// следить. Тратить в этот момент 4 мс из 16 — значит растягивать подбор на
// минуты ради плавности картинки, которой всё равно нет. Втрое больший квант
// сокращает первый подбор во столько же раз и не стоит ничего.
// 6 мс, а не 12: на устройстве больший квант выливался в лаги САМОЙ ИГРЫ —
// чтение чужой памяти отъедает ту же пропускную способность, что и рендер
// игры (лог 26.09 19:46: сигнатурный проход шёл на 727 МБ/с, и всё подвисало).
constexpr double kTickBudgetBlind = 0.006;
// Потолок чтения: не больше 192 МБ в секунду, иначе подбор мешает игре
// сильнее, чем помогает чит.
constexpr uint64_t kScanBytesPerSec = 192ull << 20;
constexpr double kScanTotalSec    = 300.0;  // общий потолок скана (образы по 0.5ГБ+)
// Куски по 512 КБ вместо 256 КБ: вдвое меньше syscall'ов, но такт не залипает
// на разборе гигантского куска (при 1 МБ на устройстве лагала сама игра).
// Буфер один на все фазы — они никогда не работают одновременно.
constexpr size_t kChunkBytes      = 512 * 1024;
// Пределы «дорогих» чтений на порцию выросли вчетверо вместе с размером
// порции: иначе кусок в 1 МБ обрабатывался бы по четверти и перечитывался.
constexpr size_t kMaxReadsPerChunk = 512;   // предел «дорогих» чтений на порцию
// В куче «похожих на указатели» значений на порядки больше, чем в .data/.relro
// образа, и каждая порция упиралась в прежний предел 1024 — скан полз
// ~1.7 МБ/с. Удвоенный предел вдвое ускоряет первый (и единственный) проход.
constexpr size_t kHeapReadsPerChunk = 4096;
// Сколько байт достаточно просмотреть, чтобы при НУЛЕ классов признать базу
// пустой: в живом образе слоты классов лежат в хвостовых сегментах
// (.data/.relro), а скан идёт с конца образа — живые классы нашлись на
// устройстве уже в первых 7 МБ. Пустая база (например, свежезагруженная копия
// .so после самообновления игры) не даст ничего и за все 221 МБ; не ждём.
constexpr uint64_t kEmptyBaseBytes = 96ull << 20;

// Общий буфер чтения для всех сканирующих фаз (последовательны по времени).
uint8_t  s_scan_buf[kChunkBytes];

static bool s_first_char[256];
const char* s_wanted_names[64];   // разные имена классов из таблицы
int         s_wanted_name_count = 0;

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

static bool all_wanted_found() {
    build_wanted_names();
    for (int i = 0; i < s_wanted_name_count; ++i) {
        bool found = false;
        for (int j = 0; j < s_class_count && !found; ++j)
            found = strcmp(s_classes[j].name, s_wanted_names[i]) == 0;
        if (!found) return false;
    }
    return true;
}

// Классы из СИГНАТУР ТИПОВ («c:Имя», пятый столбец таблицы). Их тоже надо
// сохранять в кэш: второй проход ищет поля с ротированными именами по ТИПУ
// (resolve_type_group → cached_class_type_index), и если такого класса
// в кэше нет, сверять не с чем — поле так и останется на вшитом значении.
// Имена в сигнатуре короткие, а в кэше может лежать полное («c:HitMarkerItem»
// ↔ «MineableObjectExtension_HitMarkerItem») — сверяем и хвост за последним
// подчёркиванием, ровно как cached_class_type_index при поиске.
const char* s_wanted_type_names[64];
int         s_wanted_type_count = 0;
static bool s_first_type_char[256];

static void build_wanted_type_names() {
    if (s_wanted_type_count) return;
    memset(s_first_type_char, 0, sizeof(s_first_type_char));
    for (int i = 0; i < kOrFieldCount && s_wanted_type_count < 64; ++i) {
        const char* t = kOrFields[i].type;
        if (!t || t[0] != 'c' || t[1] != ':' || !t[2]) continue;
        const char* nm = t + 2;
        bool dup = false;
        for (int j = 0; j < s_wanted_type_count; ++j)
            if (strcmp(s_wanted_type_names[j], nm) == 0) dup = true;
        if (dup) continue;
        s_wanted_type_names[s_wanted_type_count++] = nm;
        s_first_type_char[(uint8_t)nm[0]] = true;
    }
}

static bool type_name_is_wanted(const char* name) {
    build_wanted_type_names();
    if (!name || !name[0]) return false;
    if (s_first_type_char[(uint8_t)name[0]]) {
        for (int i = 0; i < s_wanted_type_count; ++i)
            if (strcmp(s_wanted_type_names[i], name) == 0) return true;
    }
    // Хвост за подчёркиванием: «MineableObjectExtension_HitMarkerItem» для «HitMarkerItem».
    const char* under = strrchr(name, '_');
    if (under && under[1] && s_first_type_char[(uint8_t)under[1]]) {
        for (int i = 0; i < s_wanted_type_count; ++i)
            if (strcmp(s_wanted_type_names[i], under + 1) == 0) return true;
    }
    return false;
}

// Имя входит в таблицу резолвера? Такие классы сохраняем в кэш даже сверх
// обычного потолка (см. kClassCap) и калибруем поля в первую очередь на них:
// совпадение имени с именем из дампа — признак НАСТОЯЩЕГО класса.
static bool name_is_wanted(const char* name) {
    build_wanted_names();
    if (!name || !name[0]) return false;
    if (s_first_char[(uint8_t)name[0]]) {
        for (int i = 0; i < s_wanted_name_count; ++i)
            if (strcmp(s_wanted_names[i], name) == 0) return true;
    }
    // Классы из сигнатур типов: без них не работает поиск полей по типу.
    return type_name_is_wanted(name);
}

// Сколько классов в кэше имеют табличные имена.
// Ключевые классы: без них не работает ни одна функция чита (локальный игрок,
// контроллер, реестр объектов мира). Лог 26.09 18:41 показал, зачем это нужно
// отдельно от «табличных классов»: скан .so насобирал 256 табличных имён
// (Delegate, Dictionary, Text...), резолвер счёл работу сделанной и не пошёл
// в строковый бутстрап — а PlayerManager так и не нашёлся.
// Ключевые имена: только по ним допустимо строить окно имён «на глаз».
static bool name_is_key(const char* name) {
    return name && (strcmp(name, "PlayerManager") == 0 ||
                    strcmp(name, "GameControllerBase") == 0 ||
                    strcmp(name, "NetworkClient") == 0 ||
                    strcmp(name, "PlayerInventory") == 0 ||
                    strcmp(name, "CharacterAnimation") == 0);
}

static bool class_in_cache(const char* name) {
    for (int i = 0; i < s_class_count; ++i)
        if (strcmp(s_classes[i].name, name) == 0) return true;
    return false;
}

static bool key_classes_missing() {
    return !class_in_cache("PlayerManager") ||
           !class_in_cache("GameControllerBase") ||
           !class_in_cache("NetworkClient");
}

// НИ ОДНОГО ключевого класса — верный признак, что «классы» набраны из мусора
// (лог 26.09 19:46: 769 структур, 50 из 50 табличных имён не найдено, а
// «смещения» всё равно посчитались). Частичный результат (нашёлся, скажем,
// только PlayerManager) — это нормальная недогрузка мира, его не выбрасываем.
static bool no_key_class_found() {
    return !class_in_cache("PlayerManager") &&
           !class_in_cache("GameControllerBase") &&
           !class_in_cache("NetworkClient");
}

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
    static const struct { uint64_t rva; const char* name; } kCandRvas[] = {
        { PLAYER_MANAGER_TYPEINFO_RVA, "PlayerManager" },
        { 0xDADA550ULL, "PlayerManager" },
        { 0xDADBE58ULL, "PlayerManager" },
        { 0xDAE42B8ULL, "PlayerManager" },
        { 0xD9C5310ULL, "PlayerManager" },
        { GAME_CONTROLLER_TYPEINFO_RVA, "GameControllerBase" },
        { 0xDADA348ULL, "GameControllerBase" },
        { 0xDAE0978ULL, "GameControllerBase" },
        { 0xD9C0690ULL, "GameControllerBase" },
        { NETWORK_CLIENT_TYPEINFO_RVA, "NetworkClient" },
        { 0xDADAD88ULL, "NetworkClient" },
        { 0xDAE1D90ULL, "NetworkClient" },
        { 0xD9C4570ULL, "NetworkClient" },
        { PLAYERLOOPHELPER_TYPEINFO_RVA, "PlayerLoopHelper" },
        { 0xDAE2AC0ULL, "PlayerLoopHelper" },
        { 0xD9C52E0ULL, "PlayerLoopHelper" },
    };
    for (const auto& c : kCandRvas) {
        if (!c.rva) continue;
        const uint64_t k = memio::untag(qrd_ptr(g_il2cpp_base + c.rva));
        if (valid_obj(k)) {
            const uint64_t img = memio::untag(qrd_ptr(k));
            const uint64_t nm  = memio::untag(qrd_ptr(k + IL2CPP_CLASS_NAME));
            if (valid_obj(img) && valid_str_ptr(nm)) {
                if (expected_name) *expected_name = c.name;
                return k;
            }
        }
    }
    return 0;
}

// Проверяет, держит ли хотя бы один из слотов TypeInfo ожидающий токен
// метаданных IL2CPP (нечётный бит 0, значение < 4 ГБ). В рантайме IL2CPP
// слоты TypeInfo инициализируются лениво — до создания первого объекта
// или первого обращения там лежит токен метаданных.
static bool anchor_has_pending_token() {
    if (!g_il2cpp_base) return false;
    static const uint64_t kTokenRvas[] = {
        PLAYER_MANAGER_TYPEINFO_RVA,
        0xDADA550ULL,
        0xDADBE58ULL,
        0xDAE42B8ULL,
        0xD9C5310ULL,
        GAME_CONTROLLER_TYPEINFO_RVA,
        0xDADA348ULL,
        0xDAE0978ULL,
        0xD9C0690ULL,
        NETWORK_CLIENT_TYPEINFO_RVA,
        0xDADAD88ULL,
        0xDAE1D90ULL,
        0xD9C4570ULL,
        PLAYERLOOPHELPER_TYPEINFO_RVA,
        0xDAE2AC0ULL,
        0xD9C52E0ULL,
    };
    for (uint64_t rva : kTokenRvas) {
        if (!rva) continue;
        const uint64_t val = memio::untag(qrd<uint64_t>(g_il2cpp_base + rva));
        if ((val & 1) && val < 0x100000000ULL && val >= 0x1000ULL) return true;
    }
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
    s_filesig_count = 0; s_filesig_bytes = 0;
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
        // Файловые отображения ЛЮБЫХ имён — в список сигнатурного поиска, и
        // делается это ДО всех отсевов ниже. Именно здесь ломалось
        // автообновление после апдейта игры: global-metadata.dat на Android
        // лежит внутри .apk, Unity мапит его оттуда, а строки карты с «.apk»
        // отбрасывались целиком — блок метаданных оказывался невидим.
        if (slash && perms[0] == 'r' && (end - start) >= (64ull << 10) &&
            !strstr(slash, "/dev/") && s_filesig_count < 512) {
            uint64_t sig_end = end;
            if (sig_end - start > (512ull << 20)) sig_end = start + (512ull << 20);
            s_filesig[s_filesig_count].start = start;
            s_filesig[s_filesig_count].end = sig_end;
            const char* base = strrchr(slash, '/');
            snprintf(s_filesig[s_filesig_count].path, sizeof(s_filesig[0].path), "%s",
                     base ? base + 1 : slash);
            s_filesig_bytes += sig_end - start;
            ++s_filesig_count;
        }
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
                // Проб меньше и с общим потолком: на устройстве регионов без
                // бита чтения бывает под тысячу, и десяток проб на каждый — это
                // сотни миллисекунд в ОДНОМ кадре (лог 26.09: ESP=448 мс на
                // старте карты). Четырёх точек хватает, чтобы понять, отдаёт
                // ли регион данные.
                static int probe_budget = 0;
                if (s_anon_noread_count == 0 && s_probe_ok == 0) probe_budget = 512;
                const uint64_t offs[] = {
                    0, 1024 * 1024, rsize >> 2, rsize >> 1
                };
                if (probe_budget <= 0) {
                    ++s_anon_noread_count;
                    s_anon_noread_bytes += rsize;
                    continue;
                }
                probe_budget -= (int)(sizeof(offs) / sizeof(offs[0]));
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
                    if (s_hidden_count < 768) {
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
    // Окно из карты — это ВЕСЬ файл метаданных. Если в его начале лежит
    // настоящий заголовок Il2CppGlobalMetadataHeader, сужаем окно до таблицы
    // строк: фильтр имён становится точным (раньше в окно попадали и таблицы
    // типов, и литералы, и любой указатель туда считался «именем»).
    if (s_meta_begin) {
        uint64_t sb = 0, se = 0;
        int32_t ver = 0;
        if (meta_header_ok(s_meta_begin, &sb, &se, &ver)) {
            s_meta_blob = s_meta_begin;
            s_meta_version = ver;
            s_meta_from_sig = true;
            s_meta_sig_done = true;
            LogLine("автооффсеты: заголовок метаданных подтверждён @0x%llx (версия %d) — "
                    "окно имён сужено до таблицы строк 0x%llx..0x%llx (%llu МБ)",
                    (unsigned long long)s_meta_begin, (int)ver,
                    (unsigned long long)sb, (unsigned long long)se,
                    (unsigned long long)((se - sb) >> 20));
            s_meta_begin = sb;
            s_meta_end = se;
        }
    }
    // Крупнейшие файловые отображения — в лог: по ним видно, откуда игра
    // берёт метадату (отдельный файл, .apk, memfd протектора).
    if (s_filesig_count) {
        int top[3] = {-1, -1, -1};
        for (int i = 0; i < s_filesig_count; ++i) {
            const uint64_t sz = s_filesig[i].end - s_filesig[i].start;
            for (int k = 0; k < 3; ++k) {
                if (top[k] >= 0 && sz <= s_filesig[top[k]].end - s_filesig[top[k]].start) continue;
                for (int j = 2; j > k; --j) top[j] = top[j - 1];
                top[k] = i;
                break;
            }
        }
        for (int k = 0; k < 3; ++k) {
            const int i = top[k];
            if (i < 0) continue;
            LogLine("автооффсеты: файловое отображение №%d — %s (%llu МБ, 0x%llx)",
                    k + 1, s_filesig[i].path,
                    (unsigned long long)((s_filesig[i].end - s_filesig[i].start) >> 20),
                    (unsigned long long)s_filesig[i].start);
        }
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
    // Урок беты: после апдейта игры вшитый TYPEINFO_RVA может указывать на
    // другой класс (например, класс «U» вместо «PlayerManager») или в код.
    // Если структура валидна как Il2CppClass — её указатель имени указывает
    // прямо в таблицу строк метаданных! Используем его для задания s_meta_begin/end
    // и сразу запускаем скан кучи вокруг якоря, где лежат остальные классы.
    const char* ank_expected = nullptr;
    const uint64_t ank = anchor_class(&ank_expected);
    if (ank) {
        bool aok = false;
        const uint64_t nm = memio::untag(qrd_ptr(ank + IL2CPP_CLASS_NAME));
        const std::string an = valid_str_ptr(nm) ? qrd_string(nm, &aok) : std::string();
        const uint64_t ns = memio::untag(qrd_ptr(ank + IL2CPP_CLASS_NAMESPACE));
        const uint64_t img = memio::untag(qrd_ptr(ank));
        const bool is_real_class = valid_obj(img) && aok && !an.empty() && (ns == 0 || valid_str_ptr(ns));
        if (is_real_class) {
            LogLine("автооффсеты: якорь 0x%llx («%s») подтверждён как живой класс (имя @0x%llx)",
                    (unsigned long long)ank, an.c_str(), (unsigned long long)nm);
            if (!s_meta_begin) {
                const uint64_t kSpan = 32ull << 20;
                s_meta_begin = (nm > kSpan) ? ((nm - kSpan) & ~0xFFFFFFull) : 0x1000;
                s_meta_end = s_meta_begin + (64ull << 20);
                LogLine("автооффсеты: окно метаданных задано по якорю: 0x%llx..0x%llx (64 МБ)",
                        (unsigned long long)s_meta_begin, (unsigned long long)s_meta_end);
            }
            if (s_lib_begin && ank >= s_lib_begin && ank < s_lib_end) {
                // Якорь внутри .so — продолжаем скан .so
            } else {
                start_heap_scan(ank);
            }
        } else {
            start_metadata_bootstrap(aok ? "вшитый RVA не совпал по имени" : "вшитый RVA не читается");
        }
    } else {
        LogLine("автооффсеты: якорь 0 (слот TypeInfo ещё не инициализирован или RVA сменился) — продолжаю сканирование");
        // Якоря нет — значит, вшитые RVA устарели (игра обновилась). Пока окно
        // имён неизвестно, скан .so ловит десятки тысяч мусорных структур и
        // заканчивается ничем (лог 26.09: 768 «классов», 0 табличных). Перед
        // сканом дёшево проверяем начала всех отображений на сигнатуру
        // метаданных: доли секунды, зато дальше фильтр имён работает точно.
        if (s_meta_begin) {
            // Окно имён известно — поиск по указателям на имена строго лучше
            // скана .so по форме: он сверяет имя точно, покрывает и сегменты
            // образа, и кучу, и не забивает массив мусором. Скан .so остаётся
            // запасным путём (см. конец фазы PTR) и тратит свои две минуты
            // только если бутстрап ничего не дал.
            start_blob_scan("якоря нет, зато известно окно имён");
        } else if (!s_meta_begin) {
            s_meta_scan_first = true;
            s_meta_quick_only = true;
            start_meta_scan("якоря нет, окно имён неизвестно");
        }
    }
}

static void start_strsearch(const char* reason) {
    LogLine("автооффсеты: %s — начинаю поиск строк в памяти", reason ? reason : "");
    s_str_needle_idx = 0;
    s_str_region = 0;
    s_str_pos = 0;
    s_str_scanned = 0;
    s_str_cand_count = 0;
    s_phase = PH_STRS;
}

// Попробовать кандидата как Il2CppClass: image в libil2cpp или куче, имя/нс в
// метаданных; имя читается строкой. Возврат: 1 принят, 0 отклонён.
static int try_class_candidate(uint64_t v) {
    v = memio::untag(v);
    if (find_class_slot(v)) return 0;
    const uint64_t image = memio::untag(qrd_ptr(v));
    if (!valid_obj(image)) return 0;
    ++s_shape_pass;   // «похож на класс» по форме — до фильтров имени
    const uint64_t nm = memio::untag(qrd_ptr(v + IL2CPP_CLASS_NAME));
    const uint64_t ns = memio::untag(qrd_ptr(v + IL2CPP_CLASS_NAMESPACE));
    if (s_meta_begin) {
        if (nm < s_meta_begin || nm >= s_meta_end) {
            if (!valid_str_ptr(nm)) return 0;
        }
        if (ns != 0 && (ns < s_meta_begin || ns >= s_meta_end) && !valid_str_ptr(ns)) return 0;
    } else {
        if (!valid_str_ptr(nm)) return 0;
        if (ns != 0 && !valid_str_ptr(ns)) return 0;
    }
    bool ok = false;
    const std::string name = qrd_string(nm, &ok);
    if (!ok || name.empty() || name.size() > 63) { ++s_names_fail; return 0; }
    ++s_names_ok;
    // Окно имён по найденному классу — только по ДЛИННОМУ ключевому имени и
    // только если структура проходит проверку формы. Лог 26.09 19:46 показал
    // цену прежней вольности: окно задалось по классу «gL» (двухбуквенное
    // ротированное имя из таблицы), после чего 769 мусорных структур сошли за
    // классы, из них резолвер вычислил 254 «смещения» и записал их в кэш —
    // боксы ESP поехали, игра залагала, чит упал.
    if (!s_meta_begin && strlen(name.c_str()) >= 12 && name_is_key(name.c_str()) &&
        class_shape_ok(v)) {
        const uint64_t kSpan = 32ull << 20;
        s_meta_begin = (nm > kSpan) ? ((nm - kSpan) & ~0xFFFFFFull) : 0x1000;
        s_meta_end = s_meta_begin + (64ull << 20);
        LogLine("автооффсеты: окно метаданных задано по ключевому классу «%s»: 0x%llx..0x%llx (64 МБ)",
                name.c_str(), (unsigned long long)s_meta_begin, (unsigned long long)s_meta_end);
    }
    const int cap_total = (int)(sizeof(s_classes) / sizeof(s_classes[0]));
    // Без окна имён «класс» — это любая структура, у которой на +0x10 лежит
    // читаемая строка. Лог 26.09: 768 таких «классов» при 239 604 совпадениях
    // по форме, и ни одного настоящего. Копить их незачем: они забивают
    // массив и врут в статусе, что база живая. Берём только табличные имена.
    const bool wanted = name_is_wanted(name.c_str());
    if (!s_meta_begin && !wanted) return 1;
    if (s_class_count >= kClassCap && !wanted) return 1;
    // Тёзок держим не больше шести: иначе одно популярное имя выедает массив.
    if (wanted) {
        int same = 0;
        for (int i = 0; i < s_class_count; ++i)
            if (strcmp(s_classes[i].name, name.c_str()) == 0) ++same;
        if (same >= kSameNameCap) return 1;
    }
    int slot = -1;
    if (s_class_count < cap_total) {
        slot = s_class_count++;
    } else if (wanted) {
        // Массив полон, но пришло ТАБЛИЧНОЕ имя: вытесняем нетабличный класс.
        // Лог 26.09 18:41 — ровно эта потеря: 1024 из 1024 заняты, и
        // PlayerManager записать уже некуда.
        for (int i = cap_total - 1; i >= 0 && slot < 0; --i)
            if (!name_is_wanted(s_classes[i].name)) slot = i;
    }
    if (slot < 0) return 1;
    FoundClass& fc = s_classes[slot];
    fc.klass = v;
    fc.shape_ok = false;
    snprintf(fc.name, sizeof(fc.name), "%s", name.c_str());
    bool ns_ok = false;
    const std::string ns_str = (ns != 0) ? qrd_string(ns, &ns_ok) : std::string();
    if (ns_ok && ns_str.size() < sizeof(fc.ns)) snprintf(fc.ns, sizeof(fc.ns), "%s", ns_str.c_str());
    else fc.ns[0] = '\0';
    return 1;
}

// ---- Скан кучи вокруг «якоря» ----------------------------------------------
// Запускается, когда в сегментах .so не нашлось ни одного табличного класса.
// Сканируем регионы анонимной кучи: сначала регион самого якоря, затем остальные
// близкие регионы (до 64 МБ каждый), пока не найдутся все табличные классы.
static void start_heap_scan(uint64_t anchor) {
    build_wanted_names();
    if (wanted_in_cache() == 0) {
        s_class_count = 0;
        s_shape_pass = 0;
        s_names_ok = 0;
        s_names_fail = 0;
    }
    s_heap_range_count = 0;
    s_heap_range_idx = 0;

    // 1. Регион кучи, в котором живёт сам якорь (приоритет №1)
    int anchor_reg = -1;
    for (int i = 0; i < s_anon_count; ++i) {
        if (anchor >= s_anon[i].start && anchor < s_anon[i].end) {
            anchor_reg = i;
            break;
        }
    }

    if (anchor_reg >= 0) {
        s_heap_ranges[s_heap_range_count].start = s_anon[anchor_reg].start;
        s_heap_ranges[s_heap_range_count].end   = s_anon[anchor_reg].end;
        ++s_heap_range_count;
    } else {
        constexpr uint64_t kSpan = 32ull << 20;
        s_heap_ranges[0].start = (anchor >= kSpan) ? ((anchor - kSpan) & ~7ull) : 0x1000;
        s_heap_ranges[0].end   = anchor + kSpan;
        ++s_heap_range_count;
    }

    // 2. Остальные анонимные регионы кучи (от 256 КБ до 64 МБ), отдавая приоритет близким к якорю (в пределах 512 МБ)
    for (int pass = 0; pass < 2 && s_heap_range_count < 64; ++pass) {
        for (int i = 0; i < s_anon_count && s_heap_range_count < 64; ++i) {
            if (i == anchor_reg) continue;
            if (s_anon[i].end <= s_anon[i].start) continue;
            const uint64_t sz = s_anon[i].end - s_anon[i].start;
            if (sz < (256ull << 10) || sz > (64ull << 20)) continue;
            const uint64_t dist = (s_anon[i].start > anchor) ? (s_anon[i].start - anchor) : (anchor - s_anon[i].start);
            const bool is_near = (dist <= (512ull << 20));
            if (pass == 0 && !is_near) continue;
            if (pass == 1 && is_near) continue;

            bool dup = false;
            for (int k = 0; k < s_heap_range_count; ++k) {
                if (s_heap_ranges[k].start == s_anon[i].start) { dup = true; break; }
            }
            if (dup) continue;

            s_heap_ranges[s_heap_range_count].start = s_anon[i].start;
            s_heap_ranges[s_heap_range_count].end   = s_anon[i].end;
            ++s_heap_range_count;
        }
    }

    s_heap_done = true;
    s_total_bytes = 0;
    for (int k = 0; k < s_heap_range_count; ++k)
        s_total_bytes += s_heap_ranges[k].end - s_heap_ranges[k].start;
    s_scanned_bytes = 0;
    s_last_progress_log = 0;
    // 300 с, а не 90: скан порционный, и на устройстве куча в 250+ МБ за 90 с
    // не проходится — обрыв на середине оставлял часть табличных классов
    // ненайденной (см. комментарий про «не все оффсеты» в шапке констант).
    s_scan_deadline = mono_seconds() + kScanTotalSec;
    s_heap_pos = s_heap_ranges[0].start;
    s_heap_end = s_heap_ranges[0].end;

    LogLine("автооффсеты: табличных классов в .so нет — сканирую кучу вокруг якоря 0x%llx (%d диапазонов, %llu МБ)",
            (unsigned long long)anchor, s_heap_range_count,
            (unsigned long long)(s_total_bytes >> 20));
    s_phase = PH_HEAP;
}

static void phase_heap(double deadline) {
    uint8_t* const buf = s_scan_buf;
    while (s_heap_range_idx < s_heap_range_count && mono_seconds() < deadline) {
        const HeapScanRange& r = s_heap_ranges[s_heap_range_idx];
        if (s_heap_pos < r.start || s_heap_pos >= r.end) s_heap_pos = r.start;
        const uint64_t remain = r.end - s_heap_pos;
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
                    if (reads_used >= kHeapReadsPerChunk) { ++examined; break; }
                }
            }
            s_heap_pos += examined * 8;
            s_scanned_bytes += examined * 8;
        }
        if (all_wanted_found()) break;
        if (s_heap_pos >= r.end) {
            ++s_heap_range_idx;
            if (s_heap_range_idx < s_heap_range_count) {
                s_heap_pos = s_heap_ranges[s_heap_range_idx].start;
            }
        }
        if (mono_seconds() >= s_scan_deadline) break;
        const double now_s = mono_seconds();
        if (now_s - s_last_progress_log >= 8.0) {
            s_last_progress_log = now_s;
            LogLine("автооффсеты: куча рег %d/%d, %llu/%llu МБ, классов %d (табличных %d, форма %d)",
                    s_heap_range_idx + 1, s_heap_range_count,
                    (unsigned long long)(s_scanned_bytes >> 20),
                    (unsigned long long)(s_total_bytes >> 20),
                    s_class_count, wanted_in_cache(), s_shape_pass);
        }
    }
    if (all_wanted_found() || s_heap_range_idx >= s_heap_range_count ||
        mono_seconds() >= s_scan_deadline) {
        if (all_wanted_found()) {
            LogLine("автооффсеты: ВСЕ табличные классы найдены в куче (%d табличных)", wanted_in_cache());
        }
        if (!all_wanted_found() && wanted_in_cache() == 0) {
            start_metadata_bootstrap("в куче вокруг якоря табличных классов нет");
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

static bool calibrate_fields(uint64_t klass);   // определена ниже, в фазе FIELDS
static int  fields_shape_score(uint64_t klass, uint64_t fields_ptr, int probe);
static int  probe_names_neighbourhood();        // запасной путь, определён ниже

// Похоже ли на настоящий Il2CppClass по ФОРМЕ: у класса с полями обязан быть
// читаемый массив FieldInfo (parent смотрит на сам класс, тип — валидный
// указатель, смещение в разумных границах), у класса без полей — нулевой
// указатель массива. Проверка нужна ровно потому, что «указатель на имя»
// встречается не только в классах: в логе 26.09 по одному имени набралось
// больше тысячи «классов», и настоящий в потолок массива уже не влез.
static bool class_shape_ok(uint64_t klass) {
    klass = memio::untag(klass);
    if (!valid_obj(klass)) return false;
    const uint64_t foff = s_fields_off ? s_fields_off : IL2CPP_CLASS_FIELDS;
    const uint64_t fields = memio::untag(qrd_ptr(klass + foff));
    const uint16_t count = qrd<uint16_t>(klass + IL2CPP_CLASS_FIELD_COUNT);
    if (count == 0) return fields == 0;
    if (count > 1024 || !valid_obj(fields)) return false;
    const int probe = (count < 4) ? (int)count : 4;
    const int need = (probe < 2) ? probe : 2;
    return fields_shape_score(klass, fields, probe) >= need;
}

static void store_verified_class(uint64_t klass, const char* name) {
    if (find_class_slot(klass)) return;
    const int cap_total = (int)(sizeof(s_classes) / sizeof(s_classes[0]));
    const bool ok = class_shape_ok(klass);
    int same = 0, weak = -1, first_same = -1;
    for (int i = 0; i < s_class_count; ++i) {
        if (strcmp(s_classes[i].name, name) != 0) continue;
        ++same;
        if (first_same < 0) first_same = i;
        if (!s_classes[i].shape_ok && weak < 0) weak = i;
    }
    if (same >= kSameNameCap) {
        // Потолок по имени выбран: хороший кандидат вытесняет пустышку,
        // иначе просто проходим мимо (массив классов не должен забиваться).
        if (!ok || weak < 0) return;
        s_classes[weak].klass = klass;
        s_classes[weak].shape_ok = true;
        return;
    }
    if (s_class_count >= cap_total) return;
    FoundClass& fc = s_classes[s_class_count++];
    fc.klass = klass;
    fc.shape_ok = ok;
    snprintf(fc.name, sizeof(fc.name), "%s", name);
    fc.ns[0] = 0;
    // Подтверждённый формой класс должен идти раньше пустышки-тёзки: резолв
    // полей перебирает одноимённые классы по порядку.
    if (ok && first_same >= 0 && !s_classes[first_same].shape_ok) {
        const FoundClass tmp = s_classes[first_same];
        s_classes[first_same] = s_classes[s_class_count - 1];
        s_classes[s_class_count - 1] = tmp;
    }
}

static void apply_str_window(int idx);   // определена ниже, после phase_strsearch

// ---- Фаза META: блок global-metadata по сигнатуре ----------------------------
// Похоже ли содержимое на таблицу строк метаданных: сплошные ASCII-имена,
// разделённые нулями. Проверка дешёвая (одно чтение) и отсекает случайные
// совпадения сигнатуры в мусоре: у настоящего блока по stringOffset лежат
// именно имена.
static bool looks_like_string_table(uint64_t addr) {
    char buf[192];
    if (!qrd_buf(addr, buf, sizeof(buf))) return false;
    int printable = 0, nuls = 0, tokens = 0, token_len = 0, bad = 0;
    for (size_t i = 0; i < sizeof(buf); ++i) {
        const unsigned char c = (unsigned char)buf[i];
        if (c == '\0') {
            ++nuls;
            if (token_len >= 2) ++tokens;
            token_len = 0;
            continue;
        }
        if (c >= 0x20 && c < 0x7f) { ++printable; ++token_len; continue; }
        ++bad;
        token_len = 0;
    }
    if (bad * 20 > (int)sizeof(buf)) return false;   // >5% небуквенного мусора
    return tokens >= 3 && nuls >= 3 && printable >= (int)sizeof(buf) / 3;
}

// Разбор заголовка Il2CppGlobalMetadataHeader по адресу addr. Возвращает окно
// ТАБЛИЦЫ СТРОК (не всего блока): именно на него смотрят Il2CppClass.name.
static bool meta_header_ok(uint64_t addr, uint64_t* out_begin, uint64_t* out_end,
                           int32_t* out_version) {
    uint32_t hdr[2 + 2 * kMetaHeaderPairs];
    if (!qrd_buf(addr, hdr, sizeof(hdr))) return false;
    if (hdr[0] != kMetaSanity) return false;
    const int32_t ver = (int32_t)hdr[1];
    // 24 — Unity 2018/2019, 27 — 2020/2021, 29 — 2022, 31 — Unity 6. Запас с
    // обеих сторон: версию протектор иногда подменяет, но не до абсурда.
    if (ver < 16 || ver > 40) return false;
    // Все пары (смещение, размер) обязаны укладываться в разумный файл. Одно
    // это отсекает почти все случайные 0xFAB11BAF в куче.
    int sane = 0;
    for (int i = 0; i < kMetaHeaderPairs; ++i) {
        const uint64_t off = hdr[2 + 2 * i];
        const int32_t  sz  = (int32_t)hdr[3 + 2 * i];
        if (sz < 0) return false;
        if (off > kMetaMaxSize || (uint64_t)sz > kMetaMaxSize) return false;
        if (off + (uint64_t)sz > kMetaMaxSize) return false;
        if (off >= 0x40 && sz > 0) ++sane;
    }
    if (sane < 8) return false;
    // Штатный путь: stringOffset/stringSize лежат третьей парой (0x18/0x1C) —
    // это место одинаково во всех версиях от 19 до 31.
    struct Cand { uint64_t off; uint64_t size; };
    Cand best = {0, 0};
    const uint64_t str_off = hdr[6], str_size = hdr[7];
    if (str_off >= 0x40 && str_size >= (16u << 10) && looks_like_string_table(addr + str_off)) {
        best.off = str_off; best.size = str_size;
    } else {
        // Запасной путь: заголовок мог быть перетасован протектором (такое
        // встречается) — тогда таблицу строк ищем перебором пар заголовка.
        for (int i = 0; i < kMetaHeaderPairs; ++i) {
            const uint64_t off = hdr[2 + 2 * i];
            const uint64_t sz  = (uint64_t)(int32_t)hdr[3 + 2 * i];
            if (off < 0x40 || sz < (64u << 10) || sz > (256ull << 20)) continue;
            if (!looks_like_string_table(addr + off)) continue;
            if (sz > best.size) { best.off = off; best.size = sz; }
        }
    }
    if (!best.size) return false;
    if (out_begin) *out_begin = addr + best.off;
    if (out_end)   *out_end   = addr + best.off + best.size;
    if (out_version) *out_version = ver;
    return true;
}

// Диапазоны сигнатурного поиска: сначала файловые отображения (метадата почти
// всегда именно там — отдельным файлом или страницей внутри .apk), затем
// скрытые регионы подходящего размера, затем анонимная память и сегменты .so.
static void build_meta_ranges() {
    s_meta_range_count = 0;
    auto add = [&](uint64_t s, uint64_t e) {
        if (s_meta_range_count >= (int)(sizeof(s_meta_ranges) / sizeof(s_meta_ranges[0]))) return;
        if (e <= s) return;
        s_meta_ranges[s_meta_range_count].start = s;
        s_meta_ranges[s_meta_range_count].end = e;
        ++s_meta_range_count;
    };
    for (int i = 0; i < s_filesig_count; ++i) add(s_filesig[i].start, s_filesig[i].end);
    for (int i = 0; i < s_hidden_count; ++i) {
        const uint64_t sz = s_hidden[i].end - s_hidden[i].start;
        if (sz < (1ull << 20) || sz > (256ull << 20)) continue;
        add(s_hidden[i].start, s_hidden[i].end);
    }
    for (int i = 0; i < s_anon_count; ++i) add(s_anon[i].start, s_anon[i].end);
    for (int i = 0; i < s_seg_count; ++i) add(s_segments[i].start, s_segments[i].end);
    for (int i = 0; i < s_xseg_count; ++i) add(s_xsegments[i].start, s_xsegments[i].end);
    for (int i = 0; i < s_hidden_count; ++i) {
        const uint64_t sz = s_hidden[i].end - s_hidden[i].start;
        if (sz >= (1ull << 20) && sz <= (256ull << 20)) continue;   // уже добавлены
        add(s_hidden[i].start, s_hidden[i].end);
    }
}

static void start_meta_scan(const char* reason) {
    s_meta_reason = reason;
    build_meta_ranges();
    s_meta_range_idx = 0;
    s_meta_pos = 0;
    s_meta_scanned = 0;
    s_meta_pass = 0;
    s_meta_bad_count = 0;
    s_last_progress_log = 0;
    s_scan_deadline = mono_seconds() + 120.0;
    uint64_t bytes = 0;
    for (int i = 0; i < s_meta_range_count; ++i)
        bytes += s_meta_ranges[i].end - s_meta_ranges[i].start;
    LogLine("автооффсеты: %s — ищу блок метаданных по сигнатуре 0xFAB11BAF "
            "(%d диапазонов, %llu МБ, файловых %d / %llu МБ)",
            reason ? reason : "", s_meta_range_count,
            (unsigned long long)(bytes >> 20), s_filesig_count,
            (unsigned long long)(s_filesig_bytes >> 20));
    s_phase = PH_META;
}

// Разбор таблицы строк: адреса всех табличных имён внутри известного окна.
// Отдельная точка входа (раньше в PH_BLOB попадали только через поиск имени
// по всей памяти) — окно у нас уже точное, искать его заново незачем.
static void start_blob_scan(const char* reason) {
    build_wanted_names();
    for (int i = 0; i < s_wanted_name_count; ++i) s_wanted_str[i] = 0;
    s_wanted_str_found = 0;
    s_str_cand_count = 1;
    s_str_cand_idx = 0;
    s_str_cand[0] = s_meta_begin;
    s_str_cand_region[0] = -1;
    s_str_cand_packed[0] = true;
    s_best_found = -1;
    s_blob_pos = s_meta_begin;
    s_blob_fails = 0;
    s_last_progress_log = 0;
    s_scan_deadline = mono_seconds() + 180.0;
    LogLine("автооффсеты: %s — разбираю таблицу строк 0x%llx..0x%llx (%llu МБ)",
            reason ? reason : "", (unsigned long long)s_meta_begin,
            (unsigned long long)s_meta_end,
            (unsigned long long)((s_meta_end - s_meta_begin) >> 20));
    s_phase = PH_BLOB;
}

// Перечитать /proc/<pid>/maps в поисках окна метаданных. Нужно потому, что при
// первой привязке игра ещё грузится: в логе 26.09 в 17:27:28 окна не было
// («метаданные НЕ найдены»), а через 16 секунд оно появилось
// (0x76ab407000..0x76ad000000, 27 МБ). Проверка стоит миллисекунды и экономит
// полторы минуты постраничного скана.
static bool recheck_metadata_mapping() {
    if (s_meta_begin) return true;
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", (int)g_pid);
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    std::string text;
    char chunk[8192];
    for (;;) {
        const ssize_t n = read(fd, chunk, sizeof(chunk));
        if (n > 0) { text.append(chunk, (size_t)n); if (text.size() > (16u << 20)) break; continue; }
        break;
    }
    close(fd);
    uint64_t begin = 0, end = 0;
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t eol = text.find('\n', pos);
        const size_t line_end = (eol == std::string::npos) ? text.size() : eol;
        char linebuf[1024];
        size_t line_len = line_end - pos;
        if (line_len >= sizeof(linebuf)) line_len = sizeof(linebuf) - 1;
        memcpy(linebuf, text.c_str() + pos, line_len);
        linebuf[line_len] = '\0';
        pos = line_end + 1;
        unsigned long long a = 0, b = 0;
        char perms[5] = {};
        if (sscanf(linebuf, "%llx-%llx %4s", &a, &b, perms) < 3) continue;
        if (perms[0] != 'r' || b <= a) continue;
        if (!strstr(linebuf, "global-metadata")) continue;
        if (!begin || a < begin) begin = a;
        if (b > end) end = b;
    }
    if (!begin || (end - begin) < (1ull << 20)) return false;
    uint64_t sb = 0, se = 0;
    int32_t ver = 0;
    if (meta_header_ok(begin, &sb, &se, &ver)) {
        s_meta_blob = begin; s_meta_version = ver;
        s_meta_from_sig = true;
        s_meta_begin = sb; s_meta_end = se;
        LogLine("автооффсеты: окно метаданных появилось в карте 0x%llx (версия %d) — "
                "таблица строк 0x%llx..0x%llx", (unsigned long long)begin, (int)ver,
                (unsigned long long)sb, (unsigned long long)se);
    } else {
        s_meta_begin = begin; s_meta_end = end;
        LogLine("автооффсеты: окно метаданных появилось в карте 0x%llx..0x%llx (%llu МБ) — "
                "заголовок не опознан, беру окно целиком",
                (unsigned long long)begin, (unsigned long long)end,
                (unsigned long long)((end - begin) >> 20));
    }
    return true;
}

// Единая точка входа «якоря нет, ищем классы сами». Порядок попыток:
//   1) окно строк уже известно (из карты или из заголовка) — сразу в блоб:
//      сигнатурный скан на несколько гигабайт тут не нужен;
//   2) карта могла обновиться, пока шёл скан — перечитываем её;
//   3) сигнатурный поиск блока метаданных;
//   4) старый поиск имён по памяти — последний рубеж.
static void start_metadata_bootstrap(const char* reason) {
    if (s_meta_begin) { start_blob_scan(reason); return; }
    if (recheck_metadata_mapping()) { start_blob_scan(reason); return; }
    if (!s_meta_sig_done) { start_meta_scan(reason); return; }
    start_strsearch(reason);
}

// Чем заняться после сигнатурного поиска. Если его запустили ПЕРЕД сканом .so
// (штатный случай «якоря нет»), возвращаемся к скану — теперь уже с точным
// окном имён. Если после того, как скан ничего не дал, — идём разбирать
// таблицу строк (или к последнему рубежу, поиску имён по памяти).
static void meta_scan_continue(bool found) {
    if (s_meta_scan_first) {
        s_meta_scan_first = false;
        s_seg_idx = 0;
        s_scan_pos = s_seg_count ? s_segments[0].start : 0;
        s_scanned_bytes = 0;
        s_last_progress_log = 0;
        s_scan_deadline = mono_seconds() + kScanTotalSec;
        if (s_seg_count) {
            LogLine("автооффсеты: возвращаюсь к скану .so (%llu МБ), окно имён %s",
                    (unsigned long long)(s_total_bytes >> 20),
                    found ? "известно" : "по-прежнему неизвестно");
            s_phase = PH_SCAN;
        } else if (found) {
            start_blob_scan("сегментов .so нет");
        } else {
            start_strsearch("сегментов .so нет");
        }
        return;
    }
    if (found) { start_blob_scan("блок метаданных найден"); return; }
    if (s_meta_begin) { start_blob_scan(s_meta_reason); return; }
    start_strsearch(s_meta_reason ? s_meta_reason : "сигнатуры метаданных нет");
}

static void phase_meta(double deadline) {
    uint8_t* const buf = s_scan_buf;
    while (s_meta_range_idx < s_meta_range_count && mono_seconds() < deadline) {
        const MetaRange& r = s_meta_ranges[s_meta_range_idx];
        if (s_meta_pass == 0) {
            // Проход №1 — только НАЧАЛА отображений: файл метаданных мапится
            // целиком, и заголовок лежит ровно в первых байтах. Несколько
            // сотен коротких чтений, доли секунды.
            uint64_t hit = 0;
            uint32_t head[2] = {};
            if (qrd_buf(r.start, head, sizeof(head)) && head[0] == kMetaSanity) hit = r.start;
            for (int b = 0; b < s_meta_bad_count; ++b) if (s_meta_bad[b] == hit) hit = 0;
            if (hit) {
                uint64_t b = 0, e = 0;
                int32_t ver = 0;
                if (meta_header_ok(hit, &b, &e, &ver)) {
                    s_meta_blob = hit; s_meta_version = ver;
                    s_meta_begin = b; s_meta_end = e;
                    s_meta_from_sig = true;
                    s_meta_sig_done = true;
                    LogLine("автооффсеты: блок метаданных найден по сигнатуре @0x%llx "
                            "(версия %d), таблица строк 0x%llx..0x%llx (%llu МБ)",
                            (unsigned long long)hit, (int)ver,
                            (unsigned long long)b, (unsigned long long)e,
                            (unsigned long long)((e - b) >> 20));
                    meta_scan_continue(true);
                    return;
                }
            }
            ++s_meta_range_idx;
            if (s_meta_range_idx >= s_meta_range_count) {
                if (s_meta_quick_only) {
                    // Первый заход: постраничный проход по нескольким гигабайтам
                    // стоит полторы минуты слепоты (лог 26.09: смотритель кадра
                    // трижды просил перепривязку, пока шёл скан). Откладываем
                    // его: сначала дешёвые пути, а игра тем временем домапит
                    // метадату — на следующем заходе её найдёт быстрый проход.
                    s_meta_quick_only = false;
                    LogLine("автооффсеты: быстрый проход по началам отображений сигнатуры не нашёл — "
                            "полный постраничный проход отложен до конца скана .so");
                    meta_scan_continue(false);
                    return;
                }
                // Начала не дали ничего — идём по страницам. Метадата может
                // быть отображена не с начала файла (страница внутри .apk)
                // или расшифрована в кучу.
                LogLine("автооффсеты: в начале отображений сигнатуры нет — иду по страницам");
                s_meta_pass = 1;
                s_meta_range_idx = 0;
                s_meta_pos = 0;
            }
            continue;
        }
        // Проход №2 — все страницы диапазонов. Заголовок всегда лежит на
        // границе страницы, поэтому проверяем только каждые 4 КБ: это в 1024
        // раза меньше сравнений, чем у поиска строк, и весь проход укладывается
        // в десяток секунд порционного скана.
        if (s_meta_pos < r.start || s_meta_pos >= r.end) s_meta_pos = r.start;
        const uint64_t remain = r.end - s_meta_pos;
        const uint64_t chunk = (kChunkBytes < remain) ? kChunkBytes : remain;
        if (!qrd_buf(s_meta_pos, buf, chunk)) {
            s_meta_pos += chunk;
            s_meta_scanned += chunk;
        } else {
            for (uint64_t off = 0; off + 8 <= chunk; off += 4096) {
                uint32_t magic;
                memcpy(&magic, buf + off, 4);
                if (magic != kMetaSanity) continue;
                const uint64_t hit = s_meta_pos + off;
                bool rejected = false;
                for (int b = 0; b < s_meta_bad_count; ++b) rejected |= (s_meta_bad[b] == hit);
                if (rejected) continue;
                uint64_t b = 0, e = 0;
                int32_t ver = 0;
                if (!meta_header_ok(hit, &b, &e, &ver)) continue;
                s_meta_blob = hit; s_meta_version = ver;
                s_meta_begin = b; s_meta_end = e;
                s_meta_from_sig = true;
                s_meta_sig_done = true;
                LogLine("автооффсеты: блок метаданных найден по сигнатуре @0x%llx "
                        "(версия %d), таблица строк 0x%llx..0x%llx (%llu МБ)",
                        (unsigned long long)hit, (int)ver,
                        (unsigned long long)b, (unsigned long long)e,
                        (unsigned long long)((e - b) >> 20));
                meta_scan_continue(true);
                return;
            }
            s_meta_pos += chunk;
            s_meta_scanned += chunk;
        }
        if (s_meta_pos >= r.end) {
            ++s_meta_range_idx;
            if (s_meta_range_idx < s_meta_range_count)
                s_meta_pos = s_meta_ranges[s_meta_range_idx].start;
        }
        if (mono_seconds() >= s_scan_deadline) break;
        const double now_s = mono_seconds();
        if (now_s - s_last_progress_log >= 8.0) {
            s_last_progress_log = now_s;
            LogLine("автооффсеты: сигнатура метаданных — просмотрено %llu МБ (диапазон %d/%d)",
                    (unsigned long long)(s_meta_scanned >> 20),
                    s_meta_range_idx, s_meta_range_count);
        }
    }
    if (s_meta_range_idx >= s_meta_range_count || mono_seconds() >= s_scan_deadline) {
        s_meta_sig_done = (s_meta_pass != 0);   // быстрый проход полным не считается
        LogLine("автооффсеты: блока метаданных по сигнатуре НЕТ (просмотрено %llu МБ, "
                "%d диапазонов из %d) — метадата зашифрована или выгружена",
                (unsigned long long)(s_meta_scanned >> 20),
                s_meta_range_idx, s_meta_range_count);
        meta_scan_continue(false);
    }
}

static void phase_strsearch(double deadline) {
    uint8_t* const buf = s_scan_buf;
    if (s_str_needle_idx < 0 || s_str_needle_idx >= (int)(sizeof(kStrNeedles) / sizeof(kStrNeedles[0])))
        s_str_needle_idx = 0;
    const char* const kNeedle = kStrNeedles[s_str_needle_idx];
    const size_t kNeed = strlen(kNeedle) + 1;   // вместе с концевым NUL
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
            const size_t off = (size_t)(hit - buf);
            bool packed = true;
            if (off >= 4) {
                // В Unity-пуле сериализации перед именем лежат длинные серии нулей (\0\0\0\0)
                // и бинарные счётчики (01 00 00 00 4b). В таблице строк метаданных строки
                // идут впритык через один \0.
                if (hit[-1] == '\0' && (hit[-2] == '\0' || hit[-3] == '\0' || hit[-4] == '\0')) {
                    packed = false;
                }
            }
            if (off + kNeed + 2 < chunk) {
                // В таблице строк метаданных после имени и замыкающего \0 сразу
                // идёт первый ASCII-символ следующего имени. Два нуля подряд —
                // признак паддинга в сериализации.
                if (hit[kNeed] == '\0' && hit[kNeed + 1] == '\0') {
                    packed = false;
                }
            }
            // Разреженное окружение (нули вокруг имени) больше НЕ повод
            // выбросить находку. Именно так лежат имена в пуле сериализации
            // Unity, и, как показал лог 26.09, иногда это единственное место,
            // где имя вообще встречается: строгий фильтр превращал «нашёл» в
            // «строка не найдена». Плотные кандидаты просто идут первыми —
            // блок всё равно выбирается по числу найденных в нём имён.
            if (s_str_cand_count < 8) {
                int at = s_str_cand_count;
                if (packed) {
                    while (at > 0 && !s_str_cand_packed[at - 1]) {
                        s_str_cand[at] = s_str_cand[at - 1];
                        s_str_cand_region[at] = s_str_cand_region[at - 1];
                        s_str_cand_packed[at] = s_str_cand_packed[at - 1];
                        --at;
                    }
                }
                s_str_cand[at] = s_str_pos + (uint64_t)off;
                s_str_cand_region[at] = s_str_region;
                s_str_cand_packed[at] = packed;
                ++s_str_cand_count;
            }
            s_str_pos += (uint64_t)off + kNeed;   // ищем следующее
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
        if (s_str_needle_idx + 1 < (int)(sizeof(kStrNeedles) / sizeof(kStrNeedles[0]))) {
            ++s_str_needle_idx;
            LogLine("автооффсеты: строка «%s» не найдена (%llu МБ) — пробую следующую «%s»",
                    kStrNeedles[s_str_needle_idx - 1],
                    (unsigned long long)(s_str_scanned >> 20),
                    kStrNeedles[s_str_needle_idx]);
            s_str_region = 0; s_str_pos = 0; s_str_scanned = 0;
            return;
        }
        LogLine("автооффсеты: строки метаданных нигде НЕ найдены (просмотрено %llu МБ, %d диапазонов из %d) — имена зашифрованы, автообновление по именам невозможно",
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
    if (reg < 0 || reg >= s_ptr_range_count) {
        // Кандидат пришёл не из списка диапазонов (например, окно уже известно
        // по заголовку метаданных) — берём ±32 МБ вокруг находки.
        const uint64_t kPad = 32ull << 20;
        s_meta_begin = (s_str_addr > kPad) ? (s_str_addr - kPad) : 0x1000;
        s_meta_end = s_str_addr + kPad;
        build_wanted_names();
        for (int i = 0; i < s_wanted_name_count; ++i) s_wanted_str[i] = 0;
        s_wanted_str_found = 0;
        s_blob_pos = s_meta_begin;
        s_last_progress_log = 0;
        s_scan_deadline = mono_seconds() + 180.0;
        s_phase = PH_BLOB;
        return;
    }
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
    LogLine("автооффсеты: блок строк №%d из %d — «%s» @0x%llx, окно 0x%llx..0x%llx (%llu МБ), %s",
            idx + 1, s_str_cand_count, kStrNeedles[s_str_needle_idx],
            (unsigned long long)s_str_addr,
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
    uint8_t* const buf = s_scan_buf;
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
            s_scout_done = false; s_scout_idx = 0; s_ptr_enough_at = 0;
            s_ptr_hits = 0; s_ptr_verified = 0; s_ptr_inwin = 0; s_ptr_noverify = 0;
            s_ptr_names = 0; s_ptr_rinwin = 0; s_ptr_best_hits = 0; s_ptr_best_range = -1;
            for (int k = 0; k < 3; ++k) { s_top_hits[k] = 0; s_top_range[k] = -1; }
            s_ptr_readfails = 0;
            s_last_progress_log = 0;
            s_scan_deadline = mono_seconds() + 120.0;
            s_phase = PH_PTR;
        } else if (s_meta_from_sig && s_meta_tries < 3 && s_meta_range_count) {
            // Заголовок был настоящий, а имён нет: скорее всего это декой
            // (у протекторов рядом лежит подставной файл метаданных). Ищем
            // следующий блок с того места, где остановились.
            ++s_meta_tries;
            if (s_meta_bad_count < 4) s_meta_bad[s_meta_bad_count++] = s_meta_blob;
            LogLine("автооффсеты: в таблице строк 0x%llx..0x%llx табличных имён нет — "
                    "ищу следующий блок метаданных (попытка %d из 3)",
                    (unsigned long long)s_meta_begin, (unsigned long long)s_meta_end,
                    s_meta_tries);
            s_meta_from_sig = false;
            s_meta_scan_first = false;
            s_meta_begin = s_meta_end = 0;
            s_meta_blob = 0;
            if (s_meta_pass == 0) { s_meta_pass = 1; s_meta_range_idx = 0; s_meta_pos = 0; }
            s_scan_deadline = mono_seconds() + 120.0;
            s_last_progress_log = 0;
            s_phase = PH_META;
            return;
        } else if (!s_meta_sig_done) {
            // Окно взяли из карты (или из строки в памяти), а имён в нём нет:
            // возможно, это не та область. Пробуем найти настоящий блок по
            // сигнатуре заголовка.
            LogLine("автооффсеты: в окне 0x%llx..0x%llx табличных имён нет — ищу блок метаданных по сигнатуре",
                    (unsigned long long)s_meta_begin, (unsigned long long)s_meta_end);
            s_meta_begin = s_meta_end = 0;
            s_meta_from_sig = false;
            s_meta_quick_only = false;
            start_meta_scan("в окне из карты имён нет");
            return;
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
    for (int i = 0; i < s_xseg_count && s_ptr_range_count < 2048; ++i) {
        if (s_xsegments[i].end <= s_xsegments[i].start) continue;
        s_ptr_ranges[s_ptr_range_count].start = s_xsegments[i].start;
        s_ptr_ranges[s_ptr_range_count].end = s_xsegments[i].end;
        s_ptr_seg_bytes += s_xsegments[i].end - s_xsegments[i].start;
        ++s_ptr_range_count;
    }
    // Скрытые регионы (без бита чтения, но отдающиеся принудительным чтением).
    // Регионы размером от 4 до 64 МБ — главные кандидаты на глобальные
    // метаданные (в Unity размер файла почти всегда 10–50 МБ).
    // Помещаем их ПЕРВЫМИ, чтобы строковый поиск сразу попал в метаданные.
    s_hidden_skipped = 0;
    uint64_t taken_hidden = 0;
    // 1-й проход: кандидаты в метаданные (4..64 МБ)
    for (int i = 0; i < s_hidden_count && s_ptr_range_count < 2048; ++i) {
        if (s_hidden[i].end <= s_hidden[i].start) continue;
        const uint64_t sz = s_hidden[i].end - s_hidden[i].start;
        if (sz >= (4ull << 20) && sz <= (64ull << 20)) {
            s_ptr_ranges[s_ptr_range_count].start = s_hidden[i].start;
            s_ptr_ranges[s_ptr_range_count].end = s_hidden[i].end;
            s_ptr_anon_bytes += sz;
            taken_hidden += sz;
            ++s_ptr_range_count;
        }
    }
    // 2-й проход: остальные скрытые регионы (до бюджета 2048 МБ)
    // Пропускаем гигантские регионы (>128 МБ) — это арены кучи, они съедают весь бюджет
    for (int i = 0; i < s_hidden_count && s_ptr_range_count < 2048; ++i) {
        if (s_hidden[i].end <= s_hidden[i].start) continue;
        const uint64_t sz = s_hidden[i].end - s_hidden[i].start;
        if (sz >= (4ull << 20) && sz <= (64ull << 20)) continue; // уже добавлены
        if (sz > (128ull << 20)) continue;                       // арены кучи
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
    for (int i = 0; i < s_anon_count && s_ptr_range_count < 2048; ++i) {
        if (s_anon[i].end <= s_anon[i].start) continue;
        s_ptr_ranges[s_ptr_range_count].start = s_anon[i].start;
        s_ptr_ranges[s_ptr_range_count].end = s_anon[i].end;
        s_ptr_anon_bytes += s_anon[i].end - s_anon[i].start;
        ++s_ptr_range_count;
    }
    for (int i = 0; i < s_filemisc_count && s_ptr_range_count < 2048; ++i) {
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
    const int cur = (s_ptr_region >= 0 && s_ptr_region < s_ptr_range_count)
                        ? s_ptr_order[s_ptr_region] : s_ptr_region;
    if (s_ptr_rinwin > s_ptr_best_hits) {
        s_ptr_best_hits = s_ptr_rinwin;
        s_ptr_best_range = cur;
    }
    if (s_ptr_rinwin > 0) {
        for (int k = 0; k < 3; ++k) {
            if (s_ptr_rinwin <= s_top_hits[k]) continue;
            for (int j = 2; j > k; --j) { s_top_hits[j] = s_top_hits[j - 1]; s_top_range[j] = s_top_range[j - 1]; }
            s_top_hits[k] = s_ptr_rinwin;
            s_top_range[k] = cur;
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

// Одна проба диапазона: сколько 8-байтовых значений смотрят в окно строк.
static int scout_range(const PtrRange& r) {
    if (!s_meta_begin || r.end <= r.start) return 0;
    const uint64_t size = r.end - r.start;
    int hits = 0;
    for (int p = 0; p < kScoutProbes; ++p) {
        uint64_t at = r.start + (size / (uint64_t)kScoutProbes) * (uint64_t)p;
        at &= ~7ull;
        size_t want = kScoutProbeBytes;
        if (at + want > r.end) {
            if (r.end <= at + 64) continue;
            want = (size_t)(r.end - at);
        }
        if (!qrd_buf(at, s_scan_buf, want)) continue;
        s_scout_bytes += want;
        for (size_t o = 0; o + 8 <= want; o += 8) {
            uint64_t v;
            memcpy(&v, s_scan_buf + o, 8);
            v = memio::untag(v);
            if (v >= s_meta_begin && v < s_meta_end) ++hits;
        }
    }
    return hits;
}

static void phase_ptr(double deadline) {
    uint8_t* const buf = s_scan_buf;
    // --- разведка: порядок обхода по плотности указателей в окно строк ---
    if (!s_scout_done) {
        while (s_scout_idx < s_ptr_range_count && mono_seconds() < deadline) {
            const int i = s_scout_idx++;
            int score = scout_range(s_ptr_ranges[i]);
            // Диапазон, где классы находились в прошлый раз, — вперёд всех.
            for (int h = 0; h < s_hot_count; ++h) {
                if (s_ptr_ranges[i].start < s_hot_end[h] && s_ptr_ranges[i].end > s_hot_begin[h]) {
                    score += 1000000;
                    break;
                }
            }
            s_ptr_score[i] = score;
        }
        if (s_scout_idx < s_ptr_range_count) return;   // продолжим на следующем такте
        for (int i = 0; i < s_ptr_range_count; ++i) s_ptr_order[i] = i;
        // Сортировка вставками по убыванию плотности: диапазонов сотни,
        // сравнение дешёвое, а std::sort сюда тянуть незачем.
        for (int i = 1; i < s_ptr_range_count; ++i) {
            const int cur = s_ptr_order[i];
            int j = i - 1;
            while (j >= 0 && s_ptr_score[s_ptr_order[j]] < s_ptr_score[cur]) {
                s_ptr_order[j + 1] = s_ptr_order[j];
                --j;
            }
            s_ptr_order[j + 1] = cur;
        }
        s_scout_done = true;
        const int first = s_ptr_range_count ? s_ptr_order[0] : -1;
        LogLine("автооффсеты: разведка диапазонов — %d проб на %llu МБ, лучший 0x%llx..0x%llx "
                "(%d попаданий), сканирую по убыванию плотности",
                s_ptr_range_count, (unsigned long long)(s_scout_bytes >> 20),
                first >= 0 ? (unsigned long long)s_ptr_ranges[first].start : 0ull,
                first >= 0 ? (unsigned long long)s_ptr_ranges[first].end : 0ull,
                first >= 0 ? s_ptr_score[first] : 0);
    }
    while (s_ptr_region < s_ptr_range_count && mono_seconds() < deadline) {
        const PtrRange& r = s_ptr_ranges[s_ptr_order[s_ptr_region]];
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
        // Пропускаем диапазон окна метаданных: в нём лежат строки и внутренние
        // таблицы метаданных, а не структуры Il2CppClass в куче игры.
        if (s_meta_begin && r.start < s_meta_end && r.end > s_meta_begin) {
            ptr_range_done();
            s_ptr_pos = r.end;
            ++s_ptr_region;
            continue;
        }
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
        // Дедлайн проверяется и ВНУТРИ куска: разбор полумегабайта с чтениями
        // кандидатов занимает десятки миллисекунд, и такт обязан уметь выйти
        // посередине — иначе кадр (и игра) заметно дёргаются.
        size_t done = 0;
        for (size_t i = 0; i + 8 <= chunk; i += 4) {
            if ((i & 0xFFFF) == 0 && i && mono_seconds() >= deadline) break;
            done = i;
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
            if (s_meta_begin && slot >= s_meta_begin && slot < s_meta_end) continue;
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
                const uint64_t klass = memio::untag(slot - kNameOffs[o]);
                if (s_meta_begin && klass >= s_meta_begin && klass < s_meta_end) continue;
                const uint64_t image = memio::untag(qrd_ptr(klass));
                if (!valid_obj(image)) continue;
                if (kNameOffs[o] == IL2CPP_CLASS_NAME) {
                    const uint64_t ns = memio::untag(qrd_ptr(klass + IL2CPP_CLASS_NAMESPACE));
                    if (ns != 0 && s_meta_begin && (ns < s_meta_begin || ns >= s_meta_end) && !valid_str_ptr(ns)) continue;
                }
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
        // Прошли не весь кусок — сдвигаемся ровно на разобранное, остальное
        // до конца куска доберём следующим тактом.
        const uint64_t advanced = (done + 8 >= chunk) ? chunk : (uint64_t)(done & ~7ull);
        s_ptr_pos += advanced ? advanced : chunk;
        s_ptr_scanned += advanced ? advanced : chunk;
        if (all_wanted_found()) break;
        // Ключевые классы найдены — дальше берём только соседей в том же пуле
        // и заканчиваем: остальные гигабайты ничего не добавят, а минуту
        // ожидания стоят.
        if (!key_classes_missing()) {
            if (!s_ptr_enough_at) {
                s_ptr_enough_at = s_ptr_scanned;
                LogLine("автооффсеты: ключевые классы найдены на %llu МБ — досканирую %llu МБ соседей и заканчиваю",
                        (unsigned long long)(s_ptr_scanned >> 20),
                        (unsigned long long)(kPtrGraceBytes >> 20));
            } else if (s_ptr_scanned - s_ptr_enough_at >= kPtrGraceBytes) {
                break;
            }
        }
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
        // Горячие диапазоны — в память процесса: повтор начнёт с них.
        s_hot_count = 0;
        for (int k = 0; k < 3 && s_hot_count < 4; ++k) {
            const int i = s_top_range[k];
            if (i < 0 || i >= s_ptr_range_count || s_top_hits[k] <= 0) continue;
            s_hot_begin[s_hot_count] = s_ptr_ranges[i].start;
            s_hot_end[s_hot_count] = s_ptr_ranges[i].end;
            ++s_hot_count;
        }
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
            s_scout_done = false; s_scout_idx = 0; s_ptr_enough_at = 0;
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
        // Указатели на имена не дали ни одного класса, но сами имена в памяти
        // есть: возможно, они лежат ВНУТРИ структур (пул сериализации Unity).
        // Пробуем соседей имени — с полной проверкой формы Il2CppClass.
        if (s_ptr_verified == 0 && s_wanted_str_found > 0 && wanted_in_cache() == 0) {
            if (probe_names_neighbourhood() > 0) {
                if (!s_good_base) s_good_base = g_il2cpp_base;
                if (!s_fields_off) {
                    for (int i = 0; i < s_class_count && !s_fields_off; ++i)
                        calibrate_fields(s_classes[i].klass);
                }
            }
        }
        s_bootstrap_done = true;
        // Классы нашлись, но не все: рантайм выделяет Il2CppClass из общего
        // пула подряд, поэтому остальные лежат рядом с уже найденным. Локальный
        // доскан кучи вокруг него стоит секунды против минут полного прохода.
        if (!all_wanted_found() && !s_heap_done && s_class_count > 0) {
            uint64_t seed = 0;
            for (int i = 0; i < s_class_count && !seed; ++i)
                if (name_is_wanted(s_classes[i].name)) seed = s_classes[i].klass;
            if (seed) {
                LogLine("автооффсеты: найдено %d классов из %d — дочищаю кучу вокруг 0x%llx",
                        wanted_in_cache(), s_wanted_name_count, (unsigned long long)seed);
                start_heap_scan(seed);
                return;
            }
        }
        // Бутстрап не дал ключевых классов, а скан .so по форме ещё не
        // гонялся — пробуем его запасным путём (в старых сборках пул классов
        // лежал прямо в сегментах образа).
        if (key_classes_missing() && !s_so_scan_done && s_seg_count) {
            s_seg_idx = 0;
            s_scan_pos = s_segments[0].start;
            s_scanned_bytes = 0;
            s_last_progress_log = 0;
            s_scan_deadline = mono_seconds() + kScanTotalSec;
            LogLine("автооффсеты: по указателям ключевых классов нет — пробую скан .so по форме (%llu МБ)",
                    (unsigned long long)(s_total_bytes >> 20));
            s_phase = PH_SCAN;
            return;
        }
        s_phase = PH_FIELDS;
    }
}

// ---- Запасной путь: класс РЯДОМ с именем ------------------------------------
// Дамп памяти с устройства (dumper.7z, 24.09) показал третий способ хранения
// имён: имя лежит НЕ в таблице строк, а прямо в структуре — 40-байтовыми
// записями «char name[31] + счётчик», а перед ним — пара указателей. Указателя
// НА само имя при этом нет нигде, поэтому phase_ptr такие имена не превращает
// в классы.
//
// Если после полного прохода по указателям не подтвердилось ни одного класса,
// пробуем соседей: читаем окрестность каждого найденного имени и проверяем
// каждое похожее на указатель значение как Il2CppClass — по ФОРМЕ (образ,
// массив полей, счётчик полей, parent у записей). Проверка строгая, поэтому
// хуже не станет: не подтвердилось — ничего и не записали.
static int probe_names_neighbourhood() {
    int found = 0, probed = 0;
    for (int n = 0; n < s_wanted_name_count; ++n) {
        const uint64_t at = s_wanted_str[n];
        if (!at) continue;
        // Окно — 72 байта ДО имени (в записи из дампа указатели лежали за
        // 0x28 и 0x20 байт до имени), и идём от БЛИЖНЕГО к дальнему: соседняя
        // запись не должна перехватывать чужой класс.
        const uint64_t base = at & ~7ull;
        const uint64_t from = (base > 72) ? (base - 72) : 0x1000;
        uint8_t win[72];
        const size_t span = (size_t)(base - from);
        if (span < 8 || !qrd_buf(from, win, span)) continue;
        ++probed;
        for (size_t back = 8; back <= span; back += 8) {
            const size_t o = span - back;
            uint64_t v;
            memcpy(&v, win + o, 8);
            v = memio::untag(v);
            if (!valid_obj(v)) continue;
            // Отсекаем «указатели внутрь таблицы строк» только когда окно
            // получено из настоящего заголовка метаданных. Окно, взятое ±32 МБ
            // вокруг найденного имени, накрывает и саму кучу — по нему нельзя
            // судить, что адрес не может быть классом.
            if (s_meta_from_sig && v >= s_meta_begin && v < s_meta_end) continue;
            if (find_class_slot(v)) continue;
            const uint64_t image = memio::untag(qrd_ptr(v));
            if (!valid_obj(image)) continue;
            const uint64_t fields = memio::untag(qrd_ptr(v + IL2CPP_CLASS_FIELDS));
            if (!valid_obj(fields)) continue;
            const uint16_t count = qrd<uint16_t>(v + IL2CPP_CLASS_FIELD_COUNT);
            if (count == 0 || count > 512) continue;
            const int probe = (count < 6) ? (int)count : 6;
            if (fields_shape_score(v, fields, probe) < ((probe < 3) ? probe : 3)) continue;
            store_verified_class(v, s_wanted_names[n]);
            ++found;
            if (found <= 5)
                LogLine("автооффсеты: класс «%s» найден рядом с именем @0x%llx -> 0x%llx (полей %u)",
                        s_wanted_names[n], (unsigned long long)at,
                        (unsigned long long)v, (unsigned)count);
            break;   // одно имя — один класс
        }
    }
    LogLine("автооффсеты: разведка окрестностей имён — проверено %d имён, классов подтверждено %d",
            probed, found);
    return found;
}

static void phase_scan(double deadline) {
    const char* live_name = nullptr;
    const uint64_t live_ank = anchor_class(&live_name);
    if (live_ank) {
        bool aok = false;
        const uint64_t nm = memio::untag(qrd_ptr(live_ank + IL2CPP_CLASS_NAME));
        const std::string an = valid_str_ptr(nm) ? qrd_string(nm, &aok) : std::string();
        const uint64_t ns = memio::untag(qrd_ptr(live_ank + IL2CPP_CLASS_NAMESPACE));
        const uint64_t img = memio::untag(qrd_ptr(live_ank));
        const bool is_real = valid_obj(img) && aok && !an.empty() && (ns == 0 || valid_str_ptr(ns));
        if (is_real) {
            LogLine("автооффсеты: во время скана .so инициализировался класс-якорь 0x%llx («%s») — переключаюсь на скан кучи",
                    (unsigned long long)live_ank, an.c_str());
            if (!s_meta_begin) {
                const uint64_t kSpan = 32ull << 20;
                s_meta_begin = (nm > kSpan) ? ((nm - kSpan) & ~0xFFFFFFull) : 0x1000;
                s_meta_end = s_meta_begin + (64ull << 20);
                LogLine("автооффсеты: окно метаданных задано по якорю: 0x%llx..0x%llx (64 МБ)",
                        (unsigned long long)s_meta_begin, (unsigned long long)s_meta_end);
            }
            if (s_lib_begin && live_ank >= s_lib_begin && live_ank < s_lib_end) {
                // Внутри .so — продолжаем обычный скан
            } else {
                start_heap_scan(live_ank);
                return;
            }
        }
    }

    // Буфер осознанно статический: 256 КБ на стеке кадра на телефоне —
    // риск переполнения стека потока отрисовки. Резолвер однопоточный.
    uint8_t* const buf = s_scan_buf;
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
            if (anchor) {
                bool aok = false;
                const uint64_t nm = memio::untag(qrd_ptr(anchor + IL2CPP_CLASS_NAME));
                const std::string an = valid_str_ptr(nm) ? qrd_string(nm, &aok) : std::string();
                const uint64_t ns = memio::untag(qrd_ptr(anchor + IL2CPP_CLASS_NAMESPACE));
                const uint64_t img = memio::untag(qrd_ptr(anchor));
                const bool is_real = valid_obj(img) && aok && !an.empty() && (ns == 0 || valid_str_ptr(ns));
                if (is_real) {
                    if (anchor_name && anchor_name[0] && an != anchor_name) {
                        LogLine("автооффсеты: якорь 0x%llx («%s») подтверждён как класс (ждали «%s») — имя @0x%llx",
                                (unsigned long long)anchor, an.c_str(), anchor_name, (unsigned long long)nm);
                    }
                    if (!s_meta_begin) {
                        const uint64_t kSpan = 32ull << 20;
                        s_meta_begin = (nm > kSpan) ? ((nm - kSpan) & ~0xFFFFFFull) : 0x1000;
                        s_meta_end = s_meta_begin + (64ull << 20);
                        LogLine("автооффсеты: окно метаданных задано по якорю: 0x%llx..0x%llx (64 МБ)",
                                (unsigned long long)s_meta_begin, (unsigned long long)s_meta_end);
                    }
                } else {
                    anchor = 0;
                }
            }
            if (anchor && s_lib_begin && anchor >= s_lib_begin && anchor < s_lib_end)
                anchor = 0;
            if (anchor) start_heap_scan(anchor);
            else if (s_phase != PH_STRS) {
                s_so_scan_done = true;
                // Решаем по КЛЮЧЕВЫМ классам. Раньше хватало любых табличных
                // имён: скан .so набирал 256 системных (Delegate, Dictionary,
                // Text), резолвер считал дело сделанным и не шёл в бутстрап —
                // а PlayerManager так и не находился (лог 26.09 18:41).
                if (key_classes_missing() && !s_bootstrap_done) {
                    if (wanted_in_cache() == 0 && anchor_has_pending_token() && s_anchor_retries < 15) {
                        ++s_anchor_retries;
                        s_phase = PH_WAIT;
                        s_anchor_wait_since = mono_seconds();
                        LogLine("автооффсеты: скан .so окончен, но в TypeInfo найден токен — жду инициализации класса (попытка %d/15)", s_anchor_retries);
                    } else {
                        if (wanted_in_cache() == 0) s_so_scan_failed = true;
                        start_metadata_bootstrap(wanted_in_cache() == 0
                                                     ? "табличных классов в .so нет"
                                                     : "в .so нет ключевых классов игры");
                    }
                } else {
                    s_phase = PH_FIELDS;
                }
            }
        } else if (!s_class_count && try_next_base()) {
            // Классов нет: база помечена пустой, скан пойдёт по следующему
            // кандидату — новый список сегментов соберёт PH_MAPS.
        } else {
            s_so_scan_failed = true;
            s_so_scan_done = true;
            start_metadata_bootstrap("в .so живых классов не нашлось");
        }
    }
}

// ---- Фаза FIELDS: калибровка и резолв ---------------------------------------
// Поле-счётчик: сколько записей подряд похожи на FieldInfo этого класса.
static int fields_shape_score(uint64_t klass, uint64_t fields_ptr, int probe) {
    fields_ptr = memio::untag(fields_ptr);
    if (!valid_obj(fields_ptr)) return 0;
    klass = memio::untag(klass);
    int score = 0;
    for (int i = 0; i < probe; ++i) {
        const uint64_t e = fields_ptr + (uint64_t)i * s_fi_size;
        const uint64_t parent = memio::untag(qrd_ptr(e + s_fi_parent));
        if (parent != 0 && parent != klass) break;
        const uint64_t type = memio::untag(qrd_ptr(e + IL2CPP_FIELDINFO_TYPE));
        if (!valid_obj(type)) break;
        const int32_t off = qrd<int32_t>(e + IL2CPP_FIELDINFO_OFFSET);
        if (off < -1 || off > 0x20000) break;
        ++score;
        const uint64_t nmp = memio::untag(qrd_ptr(e + IL2CPP_FIELDINFO_NAME));
        if (s_meta_begin && (nmp < s_meta_begin || nmp >= s_meta_end)) {
            if (!valid_str_ptr(nmp)) break;
        }
    }
    return score;
}

static int probe_fields_shape(uint64_t klass, uint64_t off) {
    const int score = fields_shape_score(klass, qrd_ptr(klass + off), 6);
    if (score > s_calib_best) { s_calib_best = score; s_calib_best_off = off; }
    return score;
}

static bool calibrate_fields(uint64_t klass) {
    klass = memio::untag(klass);
    // Сначала табличное значение — почти всегда оно и есть.
    const uint64_t table_off = IL2CPP_CLASS_FIELDS;
    if (probe_fields_shape(klass, table_off) >= 3) {
        s_fields_off = table_off;
        return true;
    }
    // Шире привычного 0x70–0x178: на другой сборке игры массив полей может
    // съехать дальше обычного (проба дешёвая: мусор отваливается за 1 чтение).
    for (uint64_t f = 0x30; f <= 0x1F8; f += 8) {
        if (f == table_off) continue;
        if (probe_fields_shape(klass, f) >= 3) {
            s_fields_off = f;
            return true;
        }
    }
    return false;
}

// Прочитать имя поля; пустой возврат при нечитаемости.
static bool read_field_name(uint64_t entry, char* out, size_t cap) {
    const uint64_t nmp = memio::untag(qrd_ptr(entry + IL2CPP_FIELDINFO_NAME));
    if (!valid_str_ptr(nmp)) return false;
    bool ok = false;
    const std::string name = qrd_string(nmp, &ok);
    if (!ok || name.empty() || name.size() >= cap) return false;
    snprintf(out, cap, "%s", name.c_str());
    return true;
}

// Найти поле по имени в классе; вернуть смещение (-1 если нет).
static int32_t find_field_offset(uint64_t klass, const char* field) {
    klass = memio::untag(klass);
    const uint64_t fields_ptr = memio::untag(qrd_ptr(klass + s_fields_off));
    if (!valid_obj(fields_ptr)) return -1;
    int count = qrd<uint16_t>(klass + IL2CPP_CLASS_FIELD_COUNT);
    if (count <= 0 || count > 2048) return -1;
    char name[64];
    int parent_mismatch_streak = 0;
    for (int i = 0; i < count; ++i) {
        const uint64_t e = fields_ptr + (uint64_t)i * s_fi_size;
        const uint64_t parent = memio::untag(qrd_ptr(e + s_fi_parent));
        if (parent != 0 && parent != klass) {
            if (++parent_mismatch_streak >= 4) break;
        } else {
            parent_mismatch_streak = 0;
        }
        if (!read_field_name(e, name, sizeof(name))) continue;
        if (strcmp(name, field) == 0) return qrd<int32_t>(e + IL2CPP_FIELDINFO_OFFSET);
    }
    return -1;
}

// ---- Поиск поля по ТИПУ (когда имя ротировано обфускатором) ------------------
// Бэкинг-поля вида «_BYK_k__BackingField» переименовываются в каждом билде
// игры, но их ТИП и класс-владелец не меняются. Сигнатура типа приходит из
// таблицы (пятый столбец): примитив (f4/b1/u4/i4/str), любой ссылочный (obj)
// или конкретный класс (c:Имя). Раскладка Il2CppType — в game_offsets.h.

// klassIndex (индекс определения типа) кэшированных классов с таким именем.
// Имя в сигнатуре короткое («c:HitMarkerItem»), а в кэше может лежать полное
// («MineableObjectExtension_HitMarkerItem») — сравниваем и хвост за последним
// подчёркиванием. Тёзок под одним именем может быть несколько (лог 24.09:
// в кэше лежат и настоящий PlayerManager, и его тёзка-пустышка), и индекс у
// них РАЗНЫЙ — если сверять только с первым, поля не найдутся ни одного.
// Поэтому отдаём все индекры: поле подходит, если совпал хотя бы с одним.
// Возврат: сколько индексов нашли (0 — класса такого типа в кэше нет).
static int cached_class_type_indices(const char* short_name, int32_t* out, int max) {
    if (!short_name || !short_name[0] || max <= 0) return 0;
    // Класса с таким именем в кэше нет, но имя может быть РОТИРОВАННЫМ: kidx
    // тогда берём из цепочки via (см. s_rot_kidx). Без этого поля типа
    // «c:eDw» не находились бы никогда.
    for (int i = 0; i < s_rot_kidx_count; ++i) {
        if (strcmp(s_rot_kidx[i].name, short_name) != 0) continue;
        if (s_rot_kidx[i].kidx > 0) out[0] = s_rot_kidx[i].kidx;
        return 1;
    }
    int n = 0;
    for (int i = 0; i < s_class_count && n < max; ++i) {
        const char* nm = s_classes[i].name;
        bool hit = strcmp(nm, short_name) == 0;
        if (!hit) {
            const char* under = strrchr(nm, '_');
            if (under) hit = strcmp(under + 1, short_name) == 0;
        }
        if (!hit) continue;
        // byval_arg.data.klassIndex — тот же индекс, что в FieldInfo.type
        // всех полей этого типа.
        const int32_t k = qrd<int32_t>(s_classes[i].klass + IL2CPP_CLASS_BYVAL_ARG);
        bool dup = false;
        for (int j = 0; j < n; ++j)
            if (out[j] == k) dup = true;
        if (!dup) out[n++] = k;
    }
    return n;
}

// Соответствует ли enum типа примитивной сигнатуре; obj/obj1 — ссылочные ли.
static bool type_enum_matches(uint32_t te, const char* sig) {
    if (!strcmp(sig, "f4"))  return te == IL2CPP_TYPE_R4;
    if (!strcmp(sig, "b1"))  return te == IL2CPP_TYPE_BOOLEAN;
    if (!strcmp(sig, "u4"))  return te == IL2CPP_TYPE_U4;
    if (!strcmp(sig, "i4"))  return te == IL2CPP_TYPE_I4;
    if (!strcmp(sig, "i2"))  return te == IL2CPP_TYPE_I2;
    if (!strcmp(sig, "str")) return te == IL2CPP_TYPE_STRING;
    if (!strcmp(sig, "obj") || !strcmp(sig, "obj1"))
        return te == IL2CPP_TYPE_CLASS || te == IL2CPP_TYPE_OBJECT ||
               te == IL2CPP_TYPE_SZARRAY || te == IL2CPP_TYPE_ARRAY ||
               te == IL2CPP_TYPE_GENERICINST || te == IL2CPP_TYPE_PTR;
    return false;
}

// Вшитое (табличное) значение слота для текущей версии релиз/бета. Именно с
// ним сверяемся при неоднозначности: смещение из дампа — сильный признак
// «это то самое поле», если тип тоже сошёлся.
static bool baked_for_slot(const std::uint64_t* slot, std::uint64_t* out) {
    std::size_t n = 0;
    const go::Field* f = go::Fields(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (f[i].slot != slot) continue;
        *out = (go::CurrentBuild() == go::Build::Beta) ? f[i].beta : f[i].release;
        return true;
    }
    return false;
}

// Все поля класса, чей тип совпал с сигнатурой (до 16 штук). Полей одного
// типа в классе может быть несколько — тогда решает групповая развязка ниже.
struct TypeMatches { int32_t off[16]; int n; };

// taken — смещения этого же класса, уже отданные другим константам (найдены по
// имени или разведены предыдущей группой): поле одному слоту, не двум.
static bool collect_type_matches(uint64_t klass, const OrField& of,
                                 const int32_t* taken, int ntaken, TypeMatches& m) {
    const char* sig = of.type;
    m.n = 0;
    if (!sig || !sig[0]) return false;
    const uint64_t fields_ptr = qrd_ptr(klass + s_fields_off);
    if (!valid_obj(fields_ptr)) return false;
    int count = qrd<uint16_t>(klass + IL2CPP_CLASS_FIELD_COUNT);
    if (count <= 0 || count > 2048) return false;

    const bool class_sig = (sig[0] == 'c' && sig[1] == ':');
    int32_t want_kidx[4] = {0, 0, 0, 0};
    int nkidx = 0;
    if (class_sig) {
        nkidx = cached_class_type_indices(sig + 2, want_kidx, 4);
        if (!nkidx) return false;   // класс-тип не найден в кэше — сверять не с чем
    }

    for (int i = 0; i < count && m.n < 16; ++i) {
        const uint64_t e = fields_ptr + (uint64_t)i * s_fi_size;
        const uint64_t parent = memio::untag(qrd_ptr(e + s_fi_parent));
        if (parent != 0 && parent != memio::untag(klass)) break;
        const uint64_t typep = memio::untag(qrd_ptr(e + IL2CPP_FIELDINFO_TYPE));
        if (!valid_obj(typep)) continue;
        bool hit = false;
        if (class_sig) {
            const int32_t tk = qrd<int32_t>(typep);
            for (int k = 0; k < nkidx && !hit; ++k)
                hit = (tk == want_kidx[k] && tk != 0);
        } else {
            const uint32_t bits = qrd<uint32_t>(typep + IL2CPP_TYPE_BITS);
            hit = type_enum_matches((bits >> 16) & 0xFF, sig);
        }
        if (!hit) continue;
        const int32_t off = qrd<int32_t>(e + IL2CPP_FIELDINFO_OFFSET);
        bool busy = false;
        for (int t = 0; t < ntaken; ++t)
            if (taken[t] == off) { busy = true; break; }
        if (!busy) m.off[m.n++] = off;
    }
    return m.n > 0;
}

// ---- Групповая развязка полей ОДНОГО типа -----------------------------------
// В классе может быть несколько полей одного типа: два Vector3 (TreeHitstreaks),
// пять строк (PlayerManager), восемь TransformOffset (FPMotion). По типу они
// неразличимы, и если игра сдвинула их ВСЕ сразу (вставила поле выше по классу),
// точного совпадения со вшитым смещением нет ни у одного — по одиночке такие
// константы так и остались бы на устаревших значениях («некоторые функции не
// работают» после апдейта). Решаем задачей о назначениях: какому полю какой
// слот соответствует, минимизируя сумму |живое - вшитое|. Раскладку берём
// ТОЛЬКО если она единственная (нет второй с той же ценой) и каждое поле
// сдвинулось не больше kMaxTypeShift — иначе лучше оставить вшитые значения,
// чем угадать не то.

// Насколько далеко поле может уехать от вшитого смещения за один апдейт.
constexpr int32_t kMaxTypeShift = 0x800;
// «Такое назначение заведомо не то» — цена-заглушка.
constexpr int32_t kBadAssign    = 1 << 28;

struct TypeGroup {
    const OrField* of[16];
    int            n;
};

// Результат развязки: живое смещение для каждого слота группы.
struct TypeAssign { int32_t off[16]; };

// Минимальная сумма |живое - вшитое| по всевозможным парам слот/поле.
// Возврат: сошлось ли; результат (и признак единственности) в out.
static bool assign_type_group(const TypeGroup& g, const int32_t* cand, int ncand,
                              TypeAssign& out) {
    if (g.n <= 0 || g.n > 16 || ncand < g.n || ncand > 12) return false;
    const int n = g.n;
    // «Бесконечность»: любая реальная цена не выше n * kMaxTypeShift (0x8000),
    // так что 1<<24 заведомо больше и в сложения не переполняется.
    constexpr int32_t kInf = 1 << 24;
    // cost[s][c] — цена «слоту s отдать поле c».
    int32_t cost[16][12];
    for (int s = 0; s < n; ++s) {
        std::uint64_t baked = 0;
        const bool has_baked = baked_for_slot(g.of[s]->slot, &baked);
        for (int c = 0; c < ncand; ++c) {
            if (!has_baked) { cost[s][c] = 0; continue; }   // не с чем сверять
            const int32_t d = cand[c] > (int32_t)baked ? cand[c] - (int32_t)baked
                                                       : (int32_t)baked - cand[c];
            cost[s][c] = (d > kMaxTypeShift) ? kBadAssign : d;
        }
    }
    // dp по маске занятых полей: dp[mask] — лучшая цена для первых
    // popcount(mask) слотов. Полагаться на «одно поле — один слот» надо:
    // двум константам одно и то же поле не достанется.
    const int full = 1 << ncand;
    static int32_t dp[1 << 12];
    static int     ways[1 << 12];   // сколько раскладок дают эту цену (сатурация в 2)
    for (int i = 0; i < full; ++i) { dp[i] = kInf; ways[i] = 0; }
    dp[0] = 0; ways[0] = 1;
    for (int mask = 0; mask < full; ++mask) {
        if (!ways[mask]) continue;
        const int s = __builtin_popcount((unsigned)mask);
        if (s >= n) continue;
        for (int c = 0; c < ncand; ++c) {
            if (mask & (1 << c)) continue;
            if (cost[s][c] >= kBadAssign) continue;
            const int nm = mask | (1 << c);
            const int32_t nd = dp[mask] + cost[s][c];
            if (nd < dp[nm]) { dp[nm] = nd; ways[nm] = ways[mask]; }
            else if (nd == dp[nm] && ways[nm] < 2) ways[nm] = (ways[nm] + ways[mask] > 2) ? 2 : ways[nm] + ways[mask];
        }
    }
    // Ответ — любая маска с ровно n занятыми полями.
    int32_t best = kInf;
    int     bestmask = 0, bestways = 0;
    for (int mask = 0; mask < full; ++mask) {
        if (__builtin_popcount((unsigned)mask) != n) continue;
        if (!ways[mask]) continue;
        if (dp[mask] < best) { best = dp[mask]; bestmask = mask; bestways = ways[mask]; }
        else if (dp[mask] == best) bestways = (bestways + ways[mask] > 2) ? 2 : bestways + ways[mask];
    }
    if (!bestmask || best >= kInf) return false;
    if (bestways != 1) return false;   // вторая раскладка с той же ценой — угадайка
    // Восстанавливаем: идём от последнего слота к первому.
    int mask = bestmask;
    for (int s = n - 1; s >= 0; --s) {
        int pick = -1;
        for (int c = 0; c < ncand; ++c) {
            if (!(mask & (1 << c))) continue;
            if (cost[s][c] >= kBadAssign) continue;
            const int pm = mask & ~(1 << c);
            if (__builtin_popcount((unsigned)pm) != s) continue;
            if (dp[pm] + cost[s][c] != dp[mask]) continue;
            pick = c;
            break;
        }
        if (pick < 0) return false;
        out.off[s] = cand[pick];
        mask &= ~(1 << pick);
    }
    return true;
}


static bool resolved_offset_is_valid(bool is_static, std::uint64_t value,
                                     const char* owner_class) {
    // Managed reference objects have a 0x10-byte Il2CppObject header, so no
    // instance field can live in it. Generic Dictionary metadata can expose an
    // uninitialized FieldInfo offset of 0; accepting that corrupts the baked,
    // ABI-verified _entries/_count slots. Value-type fields are stored inline
    // and legitimately start at 0 (e.g. RaycastHit.m_Point, WeaponPiece.Enabled).
    // Static fields use an address within static_fields and may start at 0.
    if (is_static) return value <= 0x4000;
    if (value > 0x20000) return false;
    if (value >= 0x10) return true;
    return owner_class &&
           (strcmp(owner_class, "RaycastHit") == 0 ||
            strcmp(owner_class, "WeaponPiece") == 0);
}

static void apply_resolution(const OrField& of, int32_t off);

// Развести группу слотов одного (класс, тип) по полям класса. Возврат: сколько
// слотов заполнено (0 — класс не подходит, пробуем следующего тёзку).
static int resolve_type_group(const TypeGroup& g, uint64_t klass) {
    // Смещения этого класса, уже занятые другими константами: поля, найденные
    // по имени (первый проход), и разведённые ранее группы второго прохода.
    int32_t taken[64];
    int ntaken = 0;
    for (int i = 0; i < s_applied_count && ntaken < 64; ++i) {
        if (!s_applied_cls[i] || strcmp(s_applied_cls[i], g.of[0]->cls) != 0) continue;
        taken[ntaken++] = (int32_t)s_applied[i].value;
    }
    TypeMatches m;
    if (!collect_type_matches(klass, *g.of[0], taken, ntaken, m)) return 0;
    // Раскладка разом и однозначно. Для одиночного слота это «ближайшее к
    // вшитому смещению поле, если оно единственное такое»: в классе почти
    // всегда НЕСКОЛЬКО полей одного типа (у FPManager десяток float), и после
    // сдвига ни одно не совпадает со вшитым точь-в-точь. Лог с устройства
    // 24.09 показал именно такие потери: FPManager::_BYK,
    // GameControllerBase::_BCX/_BCm/_BCQ, Item::_BKs, ItemPickup::_BQL,
    // PlayerWeapon::_player, VoicePlayerState::_Name — класс в кэше есть, а
    // поле не находится, потому что точного совпадения нет.
    // «obj» — «любой ссылочный»: среди кандидатов половина класса, и для
    // ОДИНОЧНОГО слота «ближайшее» было бы угадайкой. «obj1» разрешает такой
    // сдвиг только если в живом классе ровно один ссылочный кандидат; карта
    // может использовать его лишь когда дампы подтверждают уникальность поля.
    // При нескольких кандидатах обе сигнатуры остаются на точном baked offset.
    const bool generic_object = strcmp(g.of[0]->type, "obj") == 0;
    const bool unique_object = strcmp(g.of[0]->type, "obj1") == 0;
    const bool vague = generic_object || (unique_object && m.n != 1);
    if (m.n >= g.n && !(vague && g.n == 1)) {
        TypeAssign a;
        if (assign_type_group(g, m.off, m.n, a)) {
            bool valid_assignment = true;
            for (int s = 0; s < g.n; ++s) {
                if (a.off[s] < 0 ||
                    !resolved_offset_is_valid(g.of[s]->is_static,
                                              static_cast<std::uint32_t>(a.off[s]),
                                              g.of[s]->cls)) {
                    valid_assignment = false;
                    break;
                }
            }
            if (valid_assignment) {
                for (int s = 0; s < g.n; ++s) apply_resolution(*g.of[s], a.off[s]);
                return g.n;
            }
        }
    }
    // Запасной путь (как раньше): каждый слот сам за себя. Сначала — точное
    // вшитое смещение (сильный признак: совпало и имя класса, и тип, и адрес).
    // Поле, отданное одному слоту, второму уже не достанется: иначе две
    // константы получили бы одно и то же смещение (ровно так OREHS_MARKER и
    // OREHS_MARKER_TEMPLATE чуть не съели одно поле).
    bool used[16] = {};
    int filled = 0;
    for (int s = 0; s < g.n; ++s) {
        const OrField& of = *g.of[s];
        std::uint64_t baked = 0;
        if (!baked_for_slot(of.slot, &baked)) continue;
        for (int c = 0; c < m.n; ++c) {
            if (used[c]) continue;
            if (m.off[c] < 0 ||
                !resolved_offset_is_valid(of.is_static,
                                          static_cast<std::uint32_t>(m.off[c]),
                                          of.cls)) continue;
            if ((std::uint64_t)(uint32_t)m.off[c] != baked) continue;
            used[c] = true;
            apply_resolution(of, m.off[c]);
            ++filled;
            break;
        }
    }
    // Потом — вынужденный выбор: если на оставшиеся слоты осталось ровно
    // столько же неподелённых полей, последнее поле принадлежит последнему
    // слоту по остатку. При прочих раскладах (полей больше, чем слотов) это
    // была бы угадайка — её не делаем. Для «obj» и неоднозначного «obj1»
    // вынужденный выбор НЕ делаем вовсе: ссылочных полей может быть много,
    // и «последнее оставшееся поле» может оказаться чем угодно.
    if (!vague) for (int s = 0; s < g.n; ++s) {
        int slots_left = 0, cands_left = 0, last = -1;
        for (int t = s; t < g.n; ++t) {
            bool done = false;
            for (int j = 0; j < s_applied_count; ++j)
                if (s_applied[j].slot == g.of[t]->slot) { done = true; break; }
            if (!done) ++slots_left;
        }
        if (slots_left != 1) continue;
        for (int c = 0; c < m.n; ++c)
            if (!used[c]) { ++cands_left; last = c; }
        if (cands_left != 1 || last < 0) continue;
        // Слот, который всё ещё не заполнен:
        for (int t = s; t < g.n; ++t) {
            bool done = false;
            for (int j = 0; j < s_applied_count; ++j)
                if (s_applied[j].slot == g.of[t]->slot) { done = true; break; }
            if (done) continue;
            if (m.off[last] < 0 ||
                !resolved_offset_is_valid(g.of[t]->is_static,
                                          static_cast<std::uint32_t>(m.off[last]),
                                          g.of[t]->cls)) continue;
            used[last] = true;
            apply_resolution(*g.of[t], m.off[last]);
            ++filled;
            break;
        }
    }
    return filled;
}


static void apply_resolution(const OrField& of, int32_t off) {
    if (off < 0) return;
    const std::uint64_t value = static_cast<std::uint32_t>(off);
    if (!resolved_offset_is_valid(of.is_static, value, of.cls)) return;
    const std::uint64_t prev = *(of.slot);
    if (prev != value) ++s_changed;
    // Прежнее значение слота — страховка: если этот резолв окажется заметно
    // хуже предыдущего (мусорные классы после перескана), всё вернётся назад.
    {
        bool seen = false;
        for (int i = 0; i < s_rollback_count; ++i)
            if (s_rollback[i].slot == of.slot) { seen = true; break; }
        if (!seen && s_rollback_count < 256) {
            s_rollback[s_rollback_count].slot = of.slot;
            s_rollback[s_rollback_count].value = prev;
            ++s_rollback_count;
        }
    }
    *(of.slot) = value;
    if (s_applied_count < 256) {
        s_applied[s_applied_count].slot = of.slot;
        s_applied[s_applied_count].value = value;
        s_applied_cls[s_applied_count] = of.cls;
        ++s_applied_count;
    }
    ++s_resolved;
    ++s_live_resolved;
    if (s_resolved <= 8 || prev != value) {
        LogLine("автооффсеты: разрешено [%s.%s] = 0x%llx (было 0x%llx)",
                of.cls, of.field, (unsigned long long)value, (unsigned long long)prev);
    }
}

// Дамп первых байт якоря: даже если имена классов затёрты обфускатором, по
// структуре видно, настоящий ли это Il2CppClass (image в окне .so на +0x00,
// правдоподобные указатели в табличных слотах).
static void log_anchor_dump(uint64_t anchor) {
    anchor = memio::untag(anchor);
    uint8_t raw[64] = {};
    if (!qrd_buf(anchor, raw, sizeof(raw))) {
        LogLine("автооффсеты: якорь 0x%llx — дамп структуры не читается", (unsigned long long)anchor);
        return;
    }
    char hex[3 * sizeof(raw) + 16] = {};
    size_t hn = 0;
    for (size_t i = 0; i < sizeof(raw); ++i) {
        if (i && i % 8 == 0 && hn + 3 < sizeof(hex)) { hex[hn++] = ' '; hex[hn++] = '|'; hex[hn++] = ' '; }
        hn += (size_t)snprintf(hex + hn, sizeof(hex) - hn, "%02x ", raw[i]);
    }
    LogLine("автооффсеты: якорь 0x%llx — первые 64 байт: %s", (unsigned long long)anchor, hex);
    const uint64_t image = memio::untag(qrd_ptr(anchor));
    const uint64_t nm = memio::untag(qrd_ptr(anchor + IL2CPP_CLASS_NAME));
    const uint64_t ns = memio::untag(qrd_ptr(anchor + IL2CPP_CLASS_NAMESPACE));
    const uint64_t fp = memio::untag(qrd_ptr(anchor + IL2CPP_CLASS_FIELDS));
    bool nm_ok = false, ns_ok = false;
    const std::string nm_str = valid_str_ptr(nm) ? qrd_string(nm, &nm_ok) : std::string();
    const std::string ns_str = (ns != 0 && valid_str_ptr(ns)) ? qrd_string(ns, &ns_ok) : std::string();
    LogLine("автооффсеты: якорь — image 0x%llx (%s), имя 0x%llx («%s»), нс 0x%llx («%s»), fields@0x80 0x%llx (%s)",
            (unsigned long long)image, valid_obj(image) ? "валиден" : "НЕвалиден",
            (unsigned long long)nm, nm_ok ? nm_str.c_str() : "не читается",
            (unsigned long long)ns, ns_ok ? ns_str.c_str() : (ns == 0 ? "null" : "не читается"),
            (unsigned long long)fp, valid_obj(fp) ? "валиден" : "НЕвалиден");
}

// Автоподбор раскладки: если табличные смещения не сошлись, ищем массив, где
// подряд несколько записей ссылаются обратно на класс (родитель), перебирая
// слот родителя и шаг записи. Возврат: найдено ли; результат в out.
struct FiLayout { uint64_t fields_off; uint64_t parent_off; uint64_t size; };
static bool probe_layout(uint64_t klass, FiLayout& out) {
    klass = memio::untag(klass);
    static const uint64_t kParents[] = {0x08, 0x10, 0x18, 0x20};
    static const uint64_t kSizes[]   = {0x18, 0x20, 0x28};
    for (uint64_t f = 0x30; f <= 0x1F8; f += 8) {
        const uint64_t fp = memio::untag(qrd_ptr(klass + f));
        if (!valid_obj(fp)) continue;
        for (int p = 0; p < 4; ++p) {
            for (int s = 0; s < 3; ++s) {
                int ok = 0;
                for (int i = 0; i < 4; ++i) {
                    const uint64_t e = fp + (uint64_t)i * kSizes[s];
                    const uint64_t parent = memio::untag(qrd_ptr(e + kParents[p]));
                    if (parent == klass) ++ok;
                    else break;
                }
                if (ok >= 3) {
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

// ---- Охота за классом по индексу типа (цепочки via) ------------------------
// Имя класса ротируется, но его ТИП известен: это тип поля стартового класса.
// FieldInfo.type того поля несёт индекс определения типа (klassIndex), а
// byval_arg@0x20 каждого класса несёт его собственный. Ищем в тех же
// диапазонах, что и указатели, структуру с нужным индексом. Порциями: чтения
// десятками мегабайт за кадр на устройстве неприемлемы (см. скан маркеров).

// Похож ли адрес на настоящий Il2CppClass: образ .so на +0x00, имя и
// пространство имён — читаемые строки, массив полей — валидный адрес.
static bool kid_class_ok(uint64_t k) {
    k = memio::untag(k);
    const uint64_t image = memio::untag(qrd_ptr(k));
    if (!valid_obj(image)) return false;
    // В защищённой сборке image у ЖИВОГО якоря лежит вне отображения
    // libil2cpp.so (см. диагностический дамп якоря). Проверка окна .so
    // отбрасывала все классы, найденные по kidx в куче.
    const uint64_t nm = memio::untag(qrd_ptr(k + IL2CPP_CLASS_NAME));
    if (!valid_str_ptr(nm)) return false;
    bool ok = false;
    const std::string name = qrd_string(nm, &ok);
    if (!ok || name.empty() || name.size() > 63) return false;
    const uint64_t ns = memio::untag(qrd_ptr(k + IL2CPP_CLASS_NAMESPACE));
    if (ns != 0 && !valid_str_ptr(ns)) return false;
    if (s_meta_begin && ns != 0 && (ns < s_meta_begin || ns >= s_meta_end) && !valid_str_ptr(ns)) return false;
    const uint64_t foff = s_fields_off ? s_fields_off : IL2CPP_CLASS_FIELDS;
    if (!valid_obj(memio::untag(qrd_ptr(k + foff)))) return false;
    const int fc = qrd<uint16_t>(k + IL2CPP_CLASS_FIELD_COUNT);
    return fc > 0 && fc <= 2048;
}

// kidx класса, чей тип имеет поле field класса klass (-1 если не читается).
static int32_t field_type_kidx(uint64_t klass, const char* field) {
    if (!klass || !field || !field[0]) return -1;
    const uint64_t fields_ptr = qrd_ptr(klass + s_fields_off);
    if (!valid_obj(fields_ptr)) return -1;
    const int count = qrd<uint16_t>(klass + IL2CPP_CLASS_FIELD_COUNT);
    if (count <= 0 || count > 2048) return -1;
    char name[64];
    for (int i = 0; i < count; ++i) {
        const uint64_t e = fields_ptr + (uint64_t)i * s_fi_size;
        if (qrd_ptr(e + IL2CPP_FIELDINFO_PARENT) != klass) break;
        if (!read_field_name(e, name, sizeof(name))) continue;
        if (strcmp(name, field) != 0) continue;
        const uint64_t typep = qrd_ptr(e + IL2CPP_FIELDINFO_TYPE);
        if (!valid_obj(typep)) return -1;
        return qrd<int32_t>(typep);
    }
    return -1;
}

// Пересчитать, что искать: kidx шага 0 — из кэша (стартовый класс известен по
// имени), kidx шага 1 — из класса, найденного на шаге 0.
static void build_via_wants() {
    s_want_kidx_count = 0;
    s_rot_kidx_count = 0;
    for (int i = 0; i < kViaFieldCount; ++i) {
        const ViaField& v = kViaFields[i];
        s_via_kidx[i][0] = -1;
        s_via_kidx[i][1] = -1;
        s_via_klass[i][0] = 0;
        s_via_klass[i][1] = 0;
        // Стартовый класс ищем в кэше по имени. Тёзок может быть несколько
        // (пустышка PlayerManager кэшируется РАНЬШЕ настоящего — лог 24.09),
        // поэтому берём тот, у которого поле шага реально читает тип.
        uint64_t start = 0;
        int32_t k0 = -1;
        for (int j = 0; j < s_class_count && k0 <= 0; ++j) {
            if (strcmp(s_classes[j].name, v.via_cls) != 0) continue;
            const int32_t kk = field_type_kidx(s_classes[j].klass, v.hops[0]);
            if (kk > 0) { start = s_classes[j].klass; k0 = kk; }
        }
        if (!start) continue;
        s_via_kidx[i][0] = k0;
        // Ротированное имя целевого класса этой цепочки -> его kidx. Заполняем
        // ДО охоты: kidx известен и без неё, а тип «c:eDw» другого поля как раз
        // и ссылается на такой класс.
        if (v.rot && v.rot[0] && s_rot_kidx_count < 16 &&
            s_via_kidx[i][v.nhops - 1] > 0) {
            s_rot_kidx[s_rot_kidx_count].name = v.rot;
            s_rot_kidx[s_rot_kidx_count].kidx = s_via_kidx[i][v.nhops - 1];
            ++s_rot_kidx_count;
        }
        if (v.nhops < 2) continue;
        // Шаг 1: класс шага 0 уже мог найтись в прошлом заходе охоты.
        uint64_t mid = 0;
        for (int j = 0; j < s_via_class_count; ++j)
            if (s_via_classes[j].kidx == k0) mid = s_via_classes[j].klass;
        if (!mid) continue;
        const int32_t k1 = field_type_kidx(mid, v.hops[1]);
        if (k1 <= 0) continue;
        s_via_kidx[i][1] = k1;
    }
    for (int i = 0; i < kViaFieldCount && s_want_kidx_count < 8; ++i) {
        for (int hop = 0; hop < kViaFields[i].nhops && s_want_kidx_count < 8; ++hop) {
            if (s_via_kidx[i][hop] <= 0 || s_via_klass[i][hop]) continue;
            bool dup = false;
            for (int j = 0; j < s_want_kidx_count; ++j)
                if (s_want_kidx[j] == s_via_kidx[i][hop]) dup = true;
            if (dup) continue;
            s_want_kidx[s_want_kidx_count] = s_via_kidx[i][hop];
            s_want_owner[s_want_kidx_count] = &kViaFields[i];
            ++s_want_kidx_count;
        }
    }
}

static void phase_kid(double deadline) {
    if (s_ptr_range_count == 0) build_scan_ranges();
    // +0x40: доочитываем хвост куска (см. extra), чтобы класс, чьё начало
    // попало в последние 0x28 байт куска, не потерялся на границе.
    static uint8_t buf[kChunkBytes + 0x40];
    while (s_want_kidx_count > 0 && s_kid_region < s_ptr_range_count &&
           mono_seconds() < deadline) {
        const PtrRange& r = s_ptr_ranges[s_kid_region];
        if (s_kid_pos == r.end) { ++s_kid_region; s_kid_pos = 0; continue; }
        if (s_kid_pos < r.start || s_kid_pos > r.end) s_kid_pos = r.start;
        if (s_kid_pos >= r.end) { ++s_kid_region; s_kid_pos = 0; continue; }
        // Окно строк метаданных пропускаем: там таблицы, а не классы.
        if (s_meta_begin && r.start < s_meta_end && r.end > s_meta_begin) {
            s_kid_pos = r.end; ++s_kid_region; continue;
        }
        const uint64_t remain = r.end - s_kid_pos;
        const uint64_t chunk = (kChunkBytes < remain) ? kChunkBytes : remain;
        if (chunk < 0x40) { s_kid_pos = r.end; ++s_kid_region; continue; }
        const uint64_t extra = (remain - chunk >= 0x28) ? 0x28 : 0;
        if (!qrd_buf(s_kid_pos, buf, chunk + extra)) { s_kid_pos += chunk; s_kid_scanned += chunk; continue; }
        // Шаг 8 байт: Il2CppClass выровнен по указателю. byval_arg лежит на
        // +0x20, поэтому смотрим на 4 байта с отступом от начала кандидата.
        // Кандидаты проверяем только до конца куска (p < chunk): хвост уже
        // будет прочитан в следующем — без него тот же класс увидим дважды.
        for (uint64_t p = 0; p < chunk && p + 0x28 <= chunk + extra; p += 8) {
            int32_t kv = 0;
            memcpy(&kv, buf + p + IL2CPP_CLASS_BYVAL_ARG, 4);
            if (!kv) continue;
            int hit = -1;
            for (int w = 0; w < s_want_kidx_count; ++w)
                if (s_want_kidx[w] == kv) { hit = w; break; }
            if (hit < 0) continue;
            const uint64_t cand = s_kid_pos + p;
            if (!kid_class_ok(cand)) {
                static int rejected_logged = 0;
                if (rejected_logged++ < 4)
                    LogLine("автооффсеты: кандидат kidx=%d @0x%llx отвергнут: image=0x%llx name=0x%llx ns=0x%llx fields=0x%llx count=%u",
                            kv, (unsigned long long)cand, (unsigned long long)qrd_ptr(cand),
                            (unsigned long long)qrd_ptr(cand + IL2CPP_CLASS_NAME),
                            (unsigned long long)qrd_ptr(cand + IL2CPP_CLASS_NAMESPACE),
                            (unsigned long long)qrd_ptr(cand + IL2CPP_CLASS_FIELDS),
                            (unsigned)qrd<uint16_t>(cand + IL2CPP_CLASS_FIELD_COUNT));
                continue;
            }
            // Нашлось. Запоминаем — следующий такт охоты может ещё понадобиться.
            bool known = false;
            for (int j = 0; j < s_via_class_count; ++j)
                if (s_via_classes[j].kidx == kv) known = true;
            if (!known && s_via_class_count < 16) {
                s_via_classes[s_via_class_count].kidx = kv;
                s_via_classes[s_via_class_count].klass = cand;
                ++s_via_class_count;
                ++s_via_found;
            }
            // Класс отдаём ВСЕМ цепочкам, которые ждут именно этот индекс — их
            // обычно много (все цепочки из PlayerManager делят один kidx).
            // Двухшаговая цепочка принимает первый шаг только если в классе
            // ЕСТЬ поле следующего шага: иначе застрянем на мусорном тёзке,
            // и индекс придётся оставить в поиске.
            int still_needed = 0;
            for (int i = 0; i < kViaFieldCount; ++i) {
                for (int hop = 0; hop < kViaFields[i].nhops; ++hop) {
                    if (s_via_kidx[i][hop] != kv || s_via_klass[i][hop]) continue;
                    if (kViaFields[i].nhops == 2 && hop == 0) {
                        const int32_t k1 = field_type_kidx(cand, kViaFields[i].hops[1]);
                        if (k1 <= 0) { ++still_needed; continue; }
                        s_via_kidx[i][1] = k1;
                    }
                    s_via_klass[i][hop] = cand;
                }
            }
            // Второй шаг цепочки стал известен только сейчас (из первого) —
            // кладём его в поиск, чтобы охота не завершилась на полпути.
            for (int i = 0; i < kViaFieldCount && s_want_kidx_count < 8; ++i) {
                if (kViaFields[i].nhops < 2) continue;
                if (!s_via_klass[i][0] || s_via_klass[i][1]) continue;
                if (s_via_kidx[i][1] <= 0) continue;
                const int32_t k1 = s_via_kidx[i][1];
                bool dup = false;
                for (int w = 0; w < s_want_kidx_count; ++w)
                    if (s_want_kidx[w] == k1) dup = true;
                if (dup) continue;
                s_want_kidx[s_want_kidx_count] = k1;
                s_want_owner[s_want_kidx_count] = &kViaFields[i];
                ++s_want_kidx_count;
            }
            // Снимаем индекс с поиска только когда он уже никому не нужен.
            if (still_needed == 0) {
                for (int w = hit; w + 1 < s_want_kidx_count; ++w) {
                    s_want_kidx[w] = s_want_kidx[w + 1];
                    s_want_owner[w] = s_want_owner[w + 1];
                }
                --s_want_kidx_count;
                if (s_want_kidx_count == 0) break;
            }
        }
        s_kid_pos += chunk;
        s_kid_scanned += chunk;
    }
    if (s_want_kidx_count > 0 && s_kid_region >= s_ptr_range_count) {
        // Все диапазоны пройдены, а искомое не нашлось: охоту не повторяем.
        s_want_kidx_count = 0;
        LogLine("автооффсеты: охота по индексу типа — классов не нашлось "
                "(просмотрено %llu МБ)", (unsigned long long)(s_kid_scanned >> 20));
    }
    if (s_want_kidx_count == 0) {
        s_via_done = true;
        s_phase = PH_FIELDS;
        if (s_via_found)
            LogLine("автооффсеты: найдено классов по индексу типа: %d "
                    "(имена ротируются — искали не по имени)", s_via_found);
    }
}

// Второй путь к ротированным классам: живые объекты, а не совпадение
// Il2CppType.data с byval_arg. В защищённой бете data — указатель в пул
// метаданных; поиск его как «индекса» находил только ложные структуры.
// Источник цепочек и начальные смещения — release offsets_map.json (via).
static uint64_t live_via_class(int i, uint64_t local, uint64_t weapon) {
    const ViaField& v = kViaFields[i];
    uint64_t obj = 0;
    if (!strcmp(v.via_cls, "PlayerManager")) obj = local;
    else if (!strcmp(v.via_cls, "FPObject")) obj = weapon;
    if (!valid_obj(obj)) return 0;
    uint64_t klass = qrd_ptr(obj);
    if (!valid_obj(klass)) return 0;
    // Текущее оружие обычно FPWeaponBase/FPTool, а Draw объявлен в FPObject.
    // Ищем базовый класс по релизной цепочке, не доверяя произвольному полю
    // одноимённого объекта.
    uint64_t owner = klass;
    bool match = false;
    for (int depth = 0; depth < 8 && valid_obj(owner); ++depth) {
        bool ok = false;
        if (qrd_string(qrd_ptr(owner + IL2CPP_CLASS_NAME), &ok) == v.via_cls && ok) {
            match = true; break;
        }
        owner = qrd_ptr(owner + 0x58); // Il2CppClass.parent, релизная раскладка ABI
    }
    if (!match) return 0;
    for (int hop = 0; hop < v.nhops; ++hop) {
        const int32_t off = find_field_offset(owner, v.hops[hop]);
        if (off < 0 || off > 0x2000) return 0;
        obj = qrd_ptr(obj + (uint64_t)off);
        if (!valid_obj(obj)) return 0;
        klass = qrd_ptr(obj);
        if (!kid_class_ok(klass)) return 0;
        owner = klass;
        s_via_klass[i][hop] = klass;
    }
    return klass;
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
        // Якорь в кэш классов под своим живым именем: он даёт имя класса и его
        // byval_arg (klassIndex) — по нему сверяются ТИПЫ полей во втором
        // проходе (resolve_type_group). Без этой записи сигнатуры
        // «c:PlayerManager»/«c:CameraManager» не с чем сравнивать.
        if (aok && !aname.empty()) store_verified_class(anchor, aname.c_str());
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
    if (!s_fields_off) {
        // Табличная раскладка не сошлась: пробуем probe_layout на известных классах
        uint64_t cand = first_choice ? first_choice : anchor;
        for (int i = 0; i < s_class_count && !s_fields_off; ++i) {
            uint64_t target = cand ? cand : s_classes[i].klass;
            if (!target) continue;
            FiLayout lay{};
            if (probe_layout(target, lay)) {
                s_fields_off = lay.fields_off;
                s_fi_parent = lay.parent_off;
                s_fi_size = lay.size;
                LogLine("автооффсеты: подобрана ДРУГАЯ раскладка полей на «%s» — fields@0x%llx, parent@0x%llx, шаг 0x%llx",
                        s_classes[i].name,
                        (unsigned long long)lay.fields_off,
                        (unsigned long long)lay.parent_off,
                        (unsigned long long)lay.size);
                break;
            }
            cand = 0;
        }
    }
    if (!s_fields_off) {
        if (s_calib_best_off != 0 && s_calib_best >= 2) {
            s_fields_off = s_calib_best_off;
            LogLine("автооффсеты: калибровка не дала полного совпадения, но лучший счёт формы %d на 0x%llx — применяю его",
                    s_calib_best, (unsigned long long)s_calib_best_off);
        } else {
            s_fields_off = IL2CPP_CLASS_FIELDS;
            s_fi_parent = IL2CPP_FIELDINFO_PARENT;
            s_fi_size = IL2CPP_FIELDINFO_SIZE;
            LogLine("автооффсеты: калибровка полей не сошлась (кэш %d классов, табличных %d, лучший счёт формы %d@0x%llx) — fallback на стандартную раскладку fields@0x%llx, parent@0x%llx, size@0x%llx",
                    s_class_count, wanted, s_calib_best,
                    (unsigned long long)s_calib_best_off,
                    (unsigned long long)s_fields_off,
                    (unsigned long long)s_fi_parent,
                    (unsigned long long)s_fi_size);
        }
    }
    // Цепочки via: классы с ротированными именами. Считаем, какие индексы типа
    // нужны, и уходим в охоту — она порционная, поэтому резолв продолжится
    // следующим тактом и вернётся сюда же (калибровка ниже не повторяется).
    if (!s_via_done && kViaFieldCount > 0) {
        build_via_wants();
        if (s_want_kidx_count > 0) {
            s_kid_region = 0; s_kid_pos = 0; s_kid_scanned = 0;
            s_phase = PH_KID;
            LogLine("автооффсеты: ищу классы по индексу типа (имена ротируются) — "
                    "%d индекс(ов), цепочек %d", s_want_kidx_count, kViaFieldCount);
            return;
        }
        s_via_done = true;
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
    // Диагностика поиска по ТИПУ. Второй проход ищет поля с ротированными
    // именами, сверяя FieldInfo.type с Il2CppType класса (klassIndex в
    // byval_arg@0x20, enum типа в битах 16..23 слова @+0x8). Если на устройстве
    // раскладка окажется другой — по этой строке видно это сразу, а не по
    // молчанию «не разрешено». Для НАСТОЯЩЕГО класса (с табличным именем и
    // подтверждённым массивом полей) печатаем: смещение byval_arg, первый
    // FieldInfo.type и что из этого прочиталось.
    {
        // Сначала класс с табличным именем (это точно настоящий класс из
        // дампа), иначе — любой, у которого массив полей читается и ссылается
        // обратно на класс. Так строка появится в логе при любом исходе.
        uint64_t dklass = 0;
        for (int pass = 0; pass < 2 && !dklass; ++pass) {
            for (int i = 0; i < s_class_count; ++i) {
                if (pass == 0 && !name_is_wanted(s_classes[i].name)) continue;
                const uint64_t fp = memio::untag(qrd_ptr(s_classes[i].klass + s_fields_off));
                if (!valid_obj(fp)) continue;
                if (memio::untag(qrd_ptr(fp + IL2CPP_FIELDINFO_PARENT)) != memio::untag(s_classes[i].klass)) continue;
                dklass = s_classes[i].klass;
                break;
            }
        }
        if (dklass) {
            uint8_t byval[16] = {}, ftype[16] = {};
            const bool bok = qrd_buf(dklass + IL2CPP_CLASS_BYVAL_ARG, byval, sizeof(byval));
            const uint64_t fp = qrd_ptr(dklass + s_fields_off);
            const bool fok2 = valid_obj(fp) &&
                qrd_buf(qrd_ptr(fp + IL2CPP_FIELDINFO_TYPE), ftype, sizeof(ftype));
            char bh[64] = {}, fh[64] = {};
            size_t hn = 0;
            for (size_t i = 0; i < sizeof(byval) && bok; ++i)
                hn += (size_t)snprintf(bh + hn, sizeof(bh) - hn, "%02x", byval[i]);
            hn = 0;
            for (size_t i = 0; i < sizeof(ftype) && fok2; ++i)
                hn += (size_t)snprintf(fh + hn, sizeof(fh) - hn, "%02x", ftype[i]);
            int32_t kidx = 0, tidx = 0;
            if (bok) memcpy(&kidx, byval, 4);
            if (fok2) memcpy(&tidx, ftype, 4);
            const uint32_t bits = fok2 ? *(uint32_t*)(ftype + IL2CPP_TYPE_BITS) : 0;
            LogLine("автооффсеты: типы полей — byval_arg@0x%llx [%s] kidx=%d, "
                    "FieldInfo.type [%s] kidx=%d enum=0x%02x (%s)",
                    (unsigned long long)IL2CPP_CLASS_BYVAL_ARG, bok ? bh : "не читается",
                    (int)kidx, fok2 ? fh : "не читается", (int)tidx,
                    (unsigned)((bits >> 16) & 0xFF),
                    (bok && fok2 && kidx && kidx == tidx) ? "совпадает — поиск по типу работает"
                                                          : "НЕ совпадает — поиск по типу не сработает");
        }
    }

    for (int i = 0; i < kOrFieldCount; ++i) {
        const OrField& of = kOrFields[i];
        bool resolved = false;
        // Классов с одним коротким именем может быть несколько: берём первый,
        // в котором поле реально нашлось (остальные — тёзки).
        for (int j = 0; j < s_class_count; ++j) {
            if (strcmp(s_classes[j].name, of.cls) != 0) continue;
            const int32_t off = find_field_offset(s_classes[j].klass, of.field);
            if (off >= 0 && resolved_offset_is_valid(of.is_static,
                    static_cast<std::uint32_t>(off), of.cls)) {
                apply_resolution(of, off);
                resolved = true;
                break;
            }
        }
        // Если класс PlayerManager не найден по имени (например, в бете он обфусцирован как "U"),
        // но якорь подтверждён, запрашиваем поля напрямую из якоря.
        if (!resolved && strcmp(of.cls, "PlayerManager") == 0 && anchor) {
            const int32_t off = find_field_offset(anchor, of.field);
            if (off >= 0 && resolved_offset_is_valid(of.is_static,
                    static_cast<std::uint32_t>(off), of.cls)) {
                apply_resolution(of, off);
                resolved = true;
            }
        }
        if (!resolved && strcmp(of.cls, "GameControllerBase") == 0 && anchor &&
            anchor_expected && strcmp(anchor_expected, "GameControllerBase") == 0) {
            const int32_t off = find_field_offset(anchor, of.field);
            if (off >= 0 && resolved_offset_is_valid(of.is_static,
                    static_cast<std::uint32_t>(off), of.cls)) {
                apply_resolution(of, off);
                resolved = true;
            }
        }
    }

    // Второй проход — поля с РОТИРОВАННЫМИ именами (_BYK_k__BackingField). Их
    // имя меняется каждый билд игры, поэтому по имени они не находятся; ищем
    // по ТИПУ поля (сигнатура из таблицы). Отдельным проходом, потому что
    // cached_class_type_index должен видеть УЖЕ заполненный кэш классов.
    // Поля одного типа внутри класса неразличимы, поэтому нерешённые слоты
    // группируем по (класс, сигнатура) и разводим их вместе — иначе при общем
    // сдвиге они все остались бы на устаревших вшитых значениях.
    int by_type = 0, by_group = 0;
    bool grouped[kOrFieldCount] = {};
    for (int i = 0; i < kOrFieldCount; ++i) {
        if (grouped[i]) continue;
        const OrField& of = kOrFields[i];
        if (!of.type || !of.type[0]) continue;   // без сигнатуры искать нечем
        bool done = false;
        for (int j = 0; j < s_applied_count; ++j)
            if (s_applied[j].slot == of.slot) { done = true; break; }
        if (done) continue;
        // Группа: этот слот плюс все нерешённые с тем же классом и типом.
        TypeGroup g;
        g.n = 0;
        g.of[g.n++] = &of;
        grouped[i] = true;
        for (int k = i + 1; k < kOrFieldCount && g.n < 16; ++k) {
            if (grouped[k]) continue;
            const OrField& oth = kOrFields[k];
            if (!oth.type || !oth.type[0]) continue;
            if (strcmp(oth.cls, of.cls) != 0 || strcmp(oth.type, of.type) != 0) continue;
            bool taken = false;
            for (int j = 0; j < s_applied_count; ++j)
                if (s_applied[j].slot == oth.slot) { taken = true; break; }
            if (taken) continue;
            g.of[g.n++] = &oth;
            grouped[k] = true;
        }
        // Тёзки класса: берём тот, в котором поля данного типа вообще есть.
        for (int j = 0; j < s_class_count && !done; ++j) {
            if (strcmp(s_classes[j].name, of.cls) != 0) continue;
            const int filled = resolve_type_group(g, s_classes[j].klass);
            if (!filled) continue;
            by_type += filled;
            if (g.n > 1 && filled == g.n) ++by_group;
            done = true;
            break;
        }
    }
    if (by_type)
        LogLine("автооффсеты: полей найдено по типу (имена ротированы): %d "
                "(разведено группами полей одного типа: %d)", by_type, by_group);

    // Берём локальный объект из GameControllerBase.static (путь из релизного
    // дампа). Привязка к реальному владельцу предотвращает ложные совпадения
    // в куче и позволяет читать класс через managed object->klass без имени.
    uint64_t live_local = 0, live_weapon = 0;
    for (int j = 0; j < s_class_count; ++j) {
        if (strcmp(s_classes[j].name, "GameControllerBase") != 0) continue;
        const uint64_t sf = qrd_ptr(s_classes[j].klass + IL2CPP_CLASS_STATIC_FIELDS);
        if (valid_obj(sf)) live_local = qrd_ptr(sf + GAME_CONTROLLER_LOCAL_PLAYER_FIELD);
        if (valid_obj(live_local)) break;
    }
    if (valid_obj(live_local)) {
        const uint64_t fp = qrd_ptr(live_local + PLAYER_FP_MANAGER);
        if (valid_obj(fp)) {
            live_weapon = qrd_ptr(fp + FPMANAGER_CURRENT_OBJECT);
            if (!valid_obj(live_weapon)) live_weapon = qrd_ptr(fp + FPMANAGER_CURRENT_WEAPON);
            if (valid_obj(live_weapon) && qrd_ptr(live_weapon + FPOBJECT_PLAYER_BACKREF) != live_local)
                live_weapon = 0;
        }
    }
    int via_live = 0;
    for (int i = 0; i < kViaFieldCount; ++i) {
        if (s_via_klass[i][kViaFields[i].nhops - 1]) continue;
        if (live_via_class(i, live_local, live_weapon)) ++via_live;
    }
    if (via_live) LogLine("автооффсеты: via по живым объектам из релизной цепочки: %d классов", via_live);

    // Третий проход — классы с РОТИРУЮЩИМСЯ именами. Их находил phase_kid:
    // по типу поля стартового класса брался индекс определения типа, по
    // индексу — живой класс в памяти. Имя и полю, и классу не нужно: поле
    // ищется по типу, ровно как во втором проходе.
    // Второй шаг двухшаговой цепочки стал известен только во время охоты —
    // дописываем его ротированное имя в таблицу имя->индекс, чтобы типы вида
    // «c:eDw» (ссылка на второй шаг) тоже разрешались.
    for (int i = 0; i < kViaFieldCount && s_rot_kidx_count < 16; ++i) {
        if (kViaFields[i].nhops < 2) continue;
        if (!kViaFields[i].rot || !kViaFields[i].rot[0]) continue;
        if (s_via_kidx[i][1] <= 0) continue;
        bool has = false;
        for (int j = 0; j < s_rot_kidx_count; ++j)
            if (strcmp(s_rot_kidx[j].name, kViaFields[i].rot) == 0) has = true;
        if (has) continue;
        s_rot_kidx[s_rot_kidx_count].name = kViaFields[i].rot;
        s_rot_kidx[s_rot_kidx_count].kidx = s_via_kidx[i][1];
        ++s_rot_kidx_count;
    }
    int by_via = 0;
    for (int i = 0; i < kViaFieldCount; ++i) {
        const ViaField& vf = kViaFields[i];
        const uint64_t k = s_via_klass[i][vf.nhops - 1];
        if (!k) continue;
        bool done = false;
        for (int j = 0; j < s_applied_count; ++j)
            if (s_applied[j].slot == vf.slot) { done = true; break; }
        if (done) continue;
        // Метка класса для списка занятых смещений: живое имя целевого класса
        // (оно ротировано, зато уникально). Две константы, пришедшие в один и
        // тот же класс, делят занятые поля через одну и ту же метку.
        bool nok = false;
        const std::string kname = qrd_string(qrd_ptr(k + IL2CPP_CLASS_NAME), &nok);
        if (nok && !kname.empty() && kname.size() < 40)
            snprintf(s_via_mark[i], sizeof(s_via_mark[i]), "%s", kname.c_str());
        else
            snprintf(s_via_mark[i], sizeof(s_via_mark[i]), "via#%d", i);
        OrField of_tmp;
        of_tmp.slot = vf.slot;
        of_tmp.cls = s_via_mark[i];
        of_tmp.field = vf.field;
        of_tmp.is_static = false;
        of_tmp.type = vf.type;
        // Сначала — по имени: имена ПОЛЕЙ внутри ротированного класса обычно
        // стабильны («Aim», «nickname», «LastLocalHitPoint»), и точное имя
        // надёжнее любой эвристики по типу. Ротировалось и имя — уходим к типу.
        const int32_t by_name = find_field_offset(k, vf.field);
        if (by_name >= 0) {
            if (vf.is_static) {
                // Статическое поле: значение лежит в static_fields.
                const uint64_t sfp = qrd_ptr(k + IL2CPP_CLASS_STATIC_FIELDS);
                if (valid_obj(sfp) && by_name < 0x400 &&
                    resolved_offset_is_valid(true, static_cast<std::uint32_t>(by_name), vf.via_cls)) {
                    const uint8_t v = qrd<uint8_t>(sfp + (uint64_t)by_name);
                    if (!(vf.type && vf.type[0] == 'b' && v != 0 && v != 1)) {
                        of_tmp.is_static = true;
                        apply_resolution(of_tmp, by_name);
                        ++by_via;
                        continue;
                    }
                }
            } else if (resolved_offset_is_valid(false,
                                                 static_cast<std::uint32_t>(by_name), vf.via_cls)) {
                apply_resolution(of_tmp, by_name);
                ++by_via;
                continue;
            }
        }
        TypeGroup g;
        g.n = 0;
        g.of[g.n++] = &of_tmp;
        if (resolve_type_group(g, k)) by_via += g.n;
    }
    if (by_via)
        LogLine("автооффсеты: полей найдено через цепочку via (имена классов "
                "ротируются): %d", by_via);
    {
        char line[512];
        size_t used = 0;
        line[0] = '\0';
        int count = 0;
        for (int i = 0; i < kViaFieldCount; ++i) {
            bool found = false;
            for (int j = 0; j < s_applied_count; ++j)
                if (s_applied[j].slot == kViaFields[i].slot) { found = true; break; }
            if (found) continue;
            ++count;
            const int n = snprintf(line + used, sizeof(line) - used, "%s%s.%s",
                                   used ? ", " : "", kViaFields[i].via_cls, kViaFields[i].field);
            if (n < 0) break;
            used += (size_t)n;
            if (used > sizeof(line) - 64) {
                LogLine("автооффсеты: via не подтверждено — %s", line);
                used = 0; line[0] = '\0';
            }
        }
        if (used) LogLine("автооффсеты: via не подтверждено — %s", line);
        if (count) LogLine("автооффсеты: via не подтверждено всего %d (оставлены вшитые)", count);
    }

    // Неразрешённые — ОДНОЙ пачкой. Раньше каждое поле печаталось отдельной
    // строкой: 156 записей в лог за один такт съедали 0.67 с, кадр вставал
    // почти на секунду (в профиле это выглядело как «ESP=956 мс»), позиции
    // устаревали, и боксы прыгали по экрану. Пишем компактно и с потолком.
    {
        char line[512];
        size_t used = 0;
        int printed = 0, total = 0;
        auto flush = [&]() {
            if (!used) return;
            LogLine("автооффсеты: не разрешено — %s", line);
            used = 0; line[0] = '\0';
        };
        line[0] = '\0';
        for (int i = 0; i < kOrFieldCount; ++i) {
            const OrField& of = kOrFields[i];
            bool is_resolved = false;
            for (int j = 0; j < s_applied_count; ++j)
                if (s_applied[j].slot == of.slot) { is_resolved = true; break; }
            if (is_resolved) continue;
            ++total;
            if (printed >= 40) continue;          // потолок: дальше только счётчик
            bool have_class = false;
            for (int j = 0; j < s_class_count; ++j)
                if (strcmp(s_classes[j].name, of.cls) == 0) { have_class = true; break; }
            const int n = snprintf(line + used, sizeof(line) - used, "%s%s::%s%s",
                                   used ? ", " : "", of.cls, of.field, have_class ? "" : "(нет класса)");
            if (n < 0) break;
            used += (size_t)n;
            ++printed;
            if (used > sizeof(line) - 96) flush();
        }
        flush();
        if (total > printed)
            LogLine("автооффсеты: не разрешено ещё %d полей (список обрезан)", total - printed);
    }
    // Главный предохранитель: без ключевых классов результата НЕ БЫВАЕТ.
    // Лог 26.09 19:46: окно имён задалось по мусорному классу «gL», сошлись
    // 769 «классов», из них вычислилось 254 «смещения» (team=0x2a8,
    // clanId=0x78) — их применили и записали в кэш. На устройстве это
    // выглядело как прыгающие боксы, лаги игры и падение чита.
    if (no_key_class_found() && s_live_resolved > 0) {
        for (int i = 0; i < s_rollback_count; ++i) *(s_rollback[i].slot) = s_rollback[i].value;
        LogLine("автооффсеты: ни одного ключевого класса нет, а «разрешилось» %d значений — "
                "это мусор, всё откачено (%d слотов), кэш не трогаю",
                s_live_resolved, s_rollback_count);
        if (s_applied_count > s_applied_base) s_applied_count = s_applied_base;
        s_resolved -= s_live_resolved;
        if (s_resolved < 0) s_resolved = 0;
        s_live_resolved = 0;
        s_changed = 0;
        s_rollback_count = 0;
        s_phase = PH_DONE;
        return;
    }
    // Правило «никогда не хуже». Перескан на живой игре может нарваться на
    // другой срез памяти и собрать мусор: в логе 26.09 второй проход дал
    // 5 констант вместо 153 и записал в PlayerManager.team чужое смещение
    // (0x98 вместо 0x120). Такой результат откатываем целиком.
    if (s_prev_resolved > 0 && s_resolved * 4 < s_prev_resolved * 3) {
        for (int i = 0; i < s_rollback_count; ++i) *(s_rollback[i].slot) = s_rollback[i].value;
        LogLine("автооффсеты: новый резолв ХУЖЕ прежнего (%d против %d констант) — "
                "вернул прежние значения (%d слотов), кэш не трогаю",
                s_resolved, s_prev_resolved, s_rollback_count);
        s_applied_count = 0;
        s_resolved = 0;
        s_changed = 0;
    } else if (s_resolved > s_prev_resolved) {
        s_prev_resolved = s_resolved;
    }
    s_rollback_count = 0;
    s_phase = PH_DONE;
}

// ---- Замер фаз ---------------------------------------------------------------
// Без цифр ускорять нечего: лог должен сам показывать, где уходит время.
Phase    s_timed_phase = PH_WAIT;
double   s_timed_since = 0;
uint64_t s_timed_bytes = 0;

static const char* phase_name(Phase p) {
    switch (p) {
        case PH_WAIT:   return "ожидание";
        case PH_MAPS:   return "карта";
        case PH_SCAN:   return "скан .so";
        case PH_KID:    return "поиск по индексу типа";
        case PH_FIELDS: return "резолв полей";
        case PH_DONE:   return "готово";
        case PH_HEAP:   return "скан кучи";
        case PH_META:   return "сигнатура метаданных";
        case PH_STRS:   return "поиск имён";
        case PH_BLOB:   return "таблица строк";
        case PH_PTR:    return "поиск указателей";
    }
    return "?";
}

static uint64_t scanned_total() {
    return s_scanned_bytes + s_str_scanned + s_ptr_scanned + s_meta_scanned + s_scout_bytes;
}

static void log_phase_timing() {
    if (s_phase == s_timed_phase) return;
    const double now = mono_seconds();
    const uint64_t bytes = scanned_total();
    if (s_timed_since > 0.0) {
        const double dt = now - s_timed_since;
        const uint64_t db = (bytes > s_timed_bytes) ? (bytes - s_timed_bytes) : 0;
        if (dt >= 0.3 || db >= (16ull << 20))
            LogLine("автооффсеты: фаза «%s» — %.1f с, %llu МБ (%.0f МБ/с)",
                    phase_name(s_timed_phase), dt, (unsigned long long)(db >> 20),
                    dt > 0.01 ? (double)(db >> 20) / dt : 0.0);
    }
    s_timed_phase = s_phase;
    s_timed_since = now;
    s_timed_bytes = bytes;
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
            "база 0x%llx, пустых баз %d, бутстрап: строк %d/%d, по указателям классов %llu%s",
            s_class_count, s_shape_pass, s_names_ok, s_names_fail, wanted,
            (unsigned long long)s_fields_off, s_resolved, kOrFieldCount, s_changed,
            s_meta_from_sig ? "по заголовку блока"
                            : (s_meta_begin ? "найдены" : "НЕ найдены (фильтр слабее)"),
            (unsigned long long)g_il2cpp_base, s_dead_count,
            s_wanted_str_found, s_wanted_name_count,
            (unsigned long long)s_ptr_verified,
            s_from_cache ? " [из кэша первого входа]" : "");

    // The global count above does not prove that the particular team fields
    // used by the ally classifier were resolved. Print these slots explicitly
    // so runtime data can be separated from a baked fallback.
    auto offset_applied = [](const std::uint64_t* slot) {
        for (int i = 0; i < s_applied_count; ++i)
            if (s_applied[i].slot == slot) return true;
        return false;
    };
    auto offset_source = [&](const std::uint64_t* slot) {
        return offset_applied(slot) ? "applied" : "baked";
    };
    LogLine("автооффсеты: союзники — PlayerManager.team=0x%llx(%s) teamName=0x%llx(%s) "
            "clanId=0x%llx(%s) clanTag=0x%llx(%s); TeamComponent.player=0x%llx(%s) "
            "team=0x%llx(%s); TeamData.name=0x%llx(%s) members=0x%llx(%s)",
            (unsigned long long)PLAYER_TEAM_OBJECT, offset_source(&PLAYER_TEAM_OBJECT),
            (unsigned long long)PLAYER_TEAM_NAME, offset_source(&PLAYER_TEAM_NAME),
            (unsigned long long)PLAYER_CLAN_ID, offset_source(&PLAYER_CLAN_ID),
            (unsigned long long)PLAYER_CLAN_TAG, offset_source(&PLAYER_CLAN_TAG),
            (unsigned long long)TEAM_OBJECT_PLAYER, offset_source(&TEAM_OBJECT_PLAYER),
            (unsigned long long)TEAM_OBJECT_DATA, offset_source(&TEAM_OBJECT_DATA),
            (unsigned long long)TEAM_DATA_NAME, offset_source(&TEAM_DATA_NAME),
            (unsigned long long)TEAM_DATA_MEMBERS, offset_source(&TEAM_DATA_MEMBERS));
    LogLine("автооффсеты: ally-map — TeamComponent.TeammateStates=0x%llx(%s) "
            "Dictionary._entries=0x%llx(%s) _count=0x%llx(%s)",
            (unsigned long long)TEAM_OBJECT_STATES, offset_source(&TEAM_OBJECT_STATES),
            (unsigned long long)DICT_ENTRIES, offset_source(&DICT_ENTRIES),
            (unsigned long long)DICT_COUNT, offset_source(&DICT_COUNT));

    build_wanted_names();
    char missing_buf[384] = {};
    size_t mpos = 0;
    int missing_cls_count = 0;
    for (int i = 0; i < s_wanted_name_count; ++i) {
        const char* wn = s_wanted_names[i];
        bool found = false;
        for (int j = 0; j < s_class_count; ++j) {
            if (strcmp(s_classes[j].name, wn) == 0) { found = true; break; }
        }
        if (!found) {
            ++missing_cls_count;
            if (mpos + strlen(wn) + 2 < sizeof(missing_buf)) {
                mpos += snprintf(missing_buf + mpos, sizeof(missing_buf) - mpos, "%s%s",
                                 mpos ? ", " : "", wn);
            }
        }
    }
    if (missing_cls_count) {
        LogLine("автооффсеты: не найдены классы (%d из %d): %s",
                missing_cls_count, s_wanted_name_count, missing_buf);
    }
}

// ---- Кэш результата: сканируем только при ПЕРВОМ входе ------------------------
// Полный резолв на телефоне — это минуты (образ .so сотни МБ + куча 250+ МБ) и
// только 4 мс на кадр, поэтому после каждого входа в игру его приходилось
// ждать заново: переподключение смотрителя кадра, респавн, перезапуск клиента —
// и скан начинал с нуля, а часть полей так и не успевала найтись.
//
// Результат резолва теперь пишется в файл рядом с конфигами (как .build/.lang),
// а при следующем входе берётся оттуда МГНОВЕННО: скан не запускается вовсе.
// Ключ — отпечаток самого libil2cpp.so (заголовок ELF + программные заголовки +
// размер файла + содержимое 16 выборок по нему). Апдейт игры меняет образ, ключ
// меняется, кэш не срабатывает — и резолв идёт заново, как и должен. Обратной
// ситуации (старый кэш на новом образе) не бывает: без совпадения ключа кэш
// просто не читается.
//
// В кэше и смещения полей, и адреса классов, которые вызывающий код берёт по
// имени (замена *_TYPEINFO_RVA). Каждый такой адрес при загрузке проверяется:
// по нему должен лежать живой Il2CppClass с тем же именем. Не сошлось — запись
// отбрасывается, остальное берём (смещения полей от адреса классов не зависят).

static std::string cache_path() {
    // XS_RU, а не XS: стенд резолвера (tools/offsets/resolver_test.cpp)
    // собирается одним модулем и не линкует lang.cpp.
#ifdef OR_CACHE_DIR
    // Стенд: каталог кэша подменяется, чтобы проверить кэш на хосте, где
    // /storage/emulated/0 не существует.
    return std::string(OR_CACHE_DIR) + ".offs";
#else
    return std::string(XS_RU("/storage/emulated/0/benzware/")) + XS_RU(".offs");
#endif
}

static uint64_t fnv1a(const void* p, size_t n, uint64_t h) {
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 0x100000001B3ULL; }
    return h;
}

// Размер файла образа: карта процесса даёт конец последнего отображения этого
// же файла (bss тоже в карте, поэтому это верхняя граница — для отпечатка её
// хватает: апдейт игры почти всегда меняет размер образа).
static uint64_t image_file_span() {
    if (!g_il2cpp_base) return 0;
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", (int)g_pid);
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    std::string text;
    char chunk[4096];
    for (;;) {
        const ssize_t n = read(fd, chunk, sizeof(chunk));
        if (n > 0) { text.append(chunk, (size_t)n); if (text.size() > (8u << 20)) break; continue; }
        break;
    }
    close(fd);
    uint64_t span = 0;
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t eol = text.find('\n', pos);
        const size_t line_end = (eol == std::string::npos) ? text.size() : eol;
        // Строка обязана быть NUL-терминированной: иначе strchr/strstr уходят
        // по всему остатку файла (та же ловушка, что в phase_maps).
        char linebuf[512];
        size_t line_len = line_end - pos;
        if (line_len >= sizeof(linebuf)) line_len = sizeof(linebuf) - 1;
        memcpy(linebuf, text.c_str() + pos, line_len);
        linebuf[line_len] = '\0';
        pos = line_end + 1;
        unsigned long long start = 0, end = 0, foff = 0;
        char perms[5] = {};
        if (sscanf(linebuf, "%llx-%llx %4s %llx", &start, &end, perms, &foff) < 4) continue;
        const char* slash = strchr(linebuf, '/');
        if (!slash || !strstr(slash, "libil2cpp.so")) continue;
        if (start != g_il2cpp_base) continue;   // чужой/второй образ
        if (end > g_il2cpp_base && end - g_il2cpp_base > span) span = end - g_il2cpp_base;
    }
    return span;
}

// Отпечаток образа: ~128 КБ чтений, доли секунды. Всё, что можно вычитать, не
// перекапывая 250 МБ целиком.
static uint64_t compute_image_key() {
    if (!g_il2cpp_base) return 0;
    uint64_t h = 0xCBF29CE484222325ULL;
    uint8_t eh[64] = {};
    if (!qrd_buf(g_il2cpp_base, eh, sizeof(eh))) return 0;
    h = fnv1a(eh, sizeof(eh), h);
    uint64_t phoff = 0;
    uint16_t phentsize = 0, phnum = 0;
    memcpy(&phoff, eh + 0x20, 8);
    memcpy(&phentsize, eh + 0x36, 2);
    memcpy(&phnum, eh + 0x38, 2);
    if (phnum > 64) phnum = 64;
    const size_t phwant = (size_t)phentsize * phnum;
    uint8_t ph[64 * 56] = {};
    if (phentsize && phwant && phwant <= sizeof(ph) &&
        qrd_buf(g_il2cpp_base + phoff, ph, phwant))
        h = fnv1a(ph, phwant, h);
    // Не хэшируем span из /proc/maps и край образа: после загрузки игры
    // границы отображений и страницы в хвосте могут меняться без обновления
    // файла. Берём фиксированные страницы начала ELF (заголовки + код),
    // включая маску успешных чтений, чтобы ключ был одинаков при перезаходе.
    for (uint64_t at = 0x1000; at < 0x11000; at += 0x1000) {
        uint8_t buf[0x1000];
        const bool readable = qrd_buf(g_il2cpp_base + at, buf, sizeof(buf));
        h = fnv1a(&readable, sizeof(readable), h);
        if (readable) h = fnv1a(buf, sizeof(buf), h);
    }
    return h;
}

static bool save_offsets_cache() {
    if (!g_il2cpp_base || !s_applied_count) return false;
    if (!s_image_key) s_image_key = compute_image_key();
    if (!s_image_key) {
        LogLine("автооффсеты: кэш не сохранён — не удалось прочитать ключ образа 0x%llx",
                (unsigned long long)g_il2cpp_base);
        return false;
    }
    // Якорь (класс по вшитому TYPEINFO_RVA) нужен только для ВОССТАНОВЛЕНИЯ
    // АДРЕСОВ классов: они лежат в куче и переезжают при перезапуске, а
    // восстанавливаются по смещению относительно якоря. Смещения ПОЛЕЙ от
    // адресов не зависят вовсе.
    //
    // Раньше без живого якоря кэш не сохранялся совсем — и после апдейта игры
    // (RVA устарел) резолв в минуты повторялся при каждом входе: лог 26.09,
    // «кэш НЕ перезаписан — якорь 0x0 не живой класс» каждые 10 секунд.
    // Теперь сохраняем всегда: в следующий раз чит хотя бы стартует с верными
    // смещениями, а классы доищет сканом.
    uint64_t anchor = anchor_class(nullptr);
    if (!kid_class_ok(anchor)) anchor = 0;
    if (s_resolved < 8) {
        LogLine("автооффсеты: кэш НЕ перезаписан — в резолве всего %d констант, это не результат",
                s_resolved);
        return false;
    }
    if (no_key_class_found()) {
        LogLine("автооффсеты: кэш НЕ перезаписан — ни одного ключевого класса не найдено");
        return false;
    }
    const std::string path = cache_path();
    const std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "w");
    if (!f) {
        LogLine("автооффсеты: кэш не сохранён — временный файл .offs не открылся (errno=%d)", errno);
        return false;
    }
    fprintf(f, "# автооффсеты: живой резолв (ключ — отпечаток libil2cpp.so)\n");
    fprintf(f, "key %016llx\n", (unsigned long long)s_image_key);
    // Адреса Il2CppClass находятся в куче и меняются при перезапуске.
    fprintf(f, "anchor %016llx\nformat 3\n", (unsigned long long)anchor);
    for (int i = 0; i < s_applied_count; ++i) {
        int idx = -1;
        for (int j = 0; j < kOrFieldCount; ++j)
            if (kOrFields[j].slot == s_applied[i].slot) { idx = j; break; }
        if (idx >= 0) {
            fprintf(f, "res %d %llx\n", idx, (unsigned long long)s_applied[i].value);
            continue;
        }
        for (int j = 0; j < kViaFieldCount; ++j) {
            if (kViaFields[j].slot == s_applied[i].slot) {
                fprintf(f, "via %d %llx\n", j, (unsigned long long)s_applied[i].value);
                break;
            }
        }
    }
    build_wanted_names();
    for (int i = 0; i < s_wanted_name_count; ++i) {
        for (int j = 0; j < s_class_count; ++j) {
            if (strcmp(s_classes[j].name, s_wanted_names[i]) != 0) continue;
            // Сохраняем ВСЕ одноимённые классы: первый GameControllerBase
            // или PlayerManager бывает пустышкой без живых static_fields.
            fprintf(f, "cls %s %016llx\n", s_classes[j].name,
                    (unsigned long long)s_classes[j].klass);
        }
    }
    const int write_error = ferror(f);
    const int close_error = fclose(f);
    if (write_error || close_error || rename(tmp.c_str(), path.c_str()) != 0) {
        LogLine("автооффсеты: кэш НЕ заменён (ошибка записи/rename, errno=%d)", errno);
        remove(tmp.c_str());
        return false;
    }
    LogLine("автооффсеты: кэш записан: %d значений, якорь 0x%llx%s, ключ 0x%llx",
            s_applied_count, (unsigned long long)anchor,
            anchor ? "" : " (без якоря — в следующий раз применим только смещения)",
            (unsigned long long)s_image_key);
    return true;
}

// true — кэш подошёл и применён (скан не нужен).
static bool load_offsets_cache() {
    s_cache_retryable = false;
    if (!g_il2cpp_base) return false;
    if (!s_image_key) s_image_key = compute_image_key();
    if (!s_image_key) return false;
    FILE* f = fopen(cache_path().c_str(), "r");
    if (!f) return false;
    std::string text;
    char chunk[512];
    for (;;) {
        const ssize_t n = fread(chunk, 1, sizeof(chunk), f);
        if (n > 0) { text.append(chunk, (size_t)n); if (text.size() > (256u << 10)) break; continue; }
        break;
    }
    fclose(f);

    // Сначала ключ. Не сошёлся (игра обновилась) — выходим, НЕ тронув ни
    // одного слота: значения из чужого образа хуже вшитых.
    uint64_t key = 0;
    bool have_key = false;
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t eol = text.find('\n', pos);
        const size_t line_end = (eol == std::string::npos) ? text.size() : eol;
        char linebuf[192];
        size_t line_len = line_end - pos;
        if (line_len >= sizeof(linebuf)) line_len = sizeof(linebuf) - 1;
        memcpy(linebuf, text.c_str() + pos, line_len);
        linebuf[line_len] = '\0';
        pos = line_end + 1;
        unsigned long long val = 0;
        if (sscanf(linebuf, "key %llx", &val) == 1) { key = (uint64_t)val; have_key = true; break; }
    }
    if (!have_key || key != s_image_key) {
        LogLine("автооффсеты: кэш не подошёл — ключ файла 0x%llx, ключ образа 0x%llx (база 0x%llx)",
                (unsigned long long)key, (unsigned long long)s_image_key,
                (unsigned long long)g_il2cpp_base);
        return false;
    }

    // Старый файл не сохранял via-поля вообще. Один раз обновляем формат,
    // иначе FOV/руки на втором входе вновь получат вшитые значения.
    // Формат 3 — кэш, записанный ПОСЛЕ проверки ключевых классов. Файлы
    // второго формата могли быть собраны из мусорных структур (лог 26.09
    // 19:46), поэтому не применяем их вовсе: один скан и запишем заново.
    if (text.find("\nformat 3\n") == std::string::npos) {
        LogLine("автооффсеты: кэш старого формата (мог быть собран без ключевых классов) — пересканирую один раз");
        return false;
    }
    // Старый файл хранил абсолютные адреса классов: на новом PID они все
    // невалидны (лог: 157 полей из кэша, 0/44 классов). Мигрируем его одним
    // полноценным резолвом, а не выдаём безжизненный кэш за готовый.
    uint64_t old_anchor = 0;
    pos = 0;
    while (pos < text.size()) {
        const size_t end = text.find('\n', pos);
        const size_t len = (end == std::string::npos ? text.size() : end) - pos;
        if (len < 80) {
            unsigned long long a = 0;
            if (sscanf(text.c_str() + pos, "anchor %llx", &a) == 1) old_anchor = (uint64_t)a;
        }
        pos = (end == std::string::npos) ? text.size() : end + 1;
    }
    const uint64_t new_anchor = anchor_class(nullptr);
    bool anchor_live = true;
#ifndef OR_CACHE_DIR
    anchor_live = valid_obj(old_anchor) && kid_class_ok(new_anchor);
    if (!anchor_live && valid_obj(old_anchor) && !s_cache_offsets_used) {
        // Якорь есть в файле, но живым ещё не стал: игра, возможно, просто не
        // догрузилась. Один раз подождём — вдруг обойдёмся вовсе без скана.
        s_cache_retryable = true;
        LogLine("автооффсеты: кэш — старый якорь 0x%llx, новый 0x%llx (НЕ живой): жду инициализации живого класса",
                (unsigned long long)old_anchor, (unsigned long long)new_anchor);
        return false;
    }
#endif
    int loaded = 0, classes = 0;
    pos = 0;
    while (pos < text.size()) {
        const size_t eol = text.find('\n', pos);
        const size_t line_end = (eol == std::string::npos) ? text.size() : eol;
        char linebuf[192];
        size_t line_len = line_end - pos;
        if (line_len >= sizeof(linebuf)) line_len = sizeof(linebuf) - 1;
        memcpy(linebuf, text.c_str() + pos, line_len);
        linebuf[line_len] = '\0';
        pos = line_end + 1;
        unsigned idx = 0;
        unsigned long long val = 0;
        char name[64] = {};
        if (sscanf(linebuf, "res %u %llx", &idx, &val) == 2) {
            if (idx >= (unsigned)kOrFieldCount) continue;
            const OrField& of = kOrFields[idx];
            // Границы те же, что у apply_resolution: кэш не должен занести в
            // слот заведомо мусорное значение.
            if (!resolved_offset_is_valid(of.is_static, static_cast<std::uint64_t>(val), of.cls)) continue;
            if (*(of.slot) != val) ++s_changed;
            *(of.slot) = (std::uint64_t)val;
            if (s_applied_count < 256) {
                s_applied[s_applied_count].slot = of.slot;
                s_applied[s_applied_count].value = (std::uint64_t)val;
                s_applied_cls[s_applied_count] = of.cls;
                ++s_applied_count;
            }
            ++s_resolved;
            ++loaded;
        } else if (sscanf(linebuf, "via %u %llx", &idx, &val) == 2) {
            if (idx >= (unsigned)kViaFieldCount) continue;
            const ViaField& vf = kViaFields[idx];
            if (!resolved_offset_is_valid(vf.is_static, static_cast<std::uint64_t>(val), vf.via_cls)) continue;
            if (*(vf.slot) != val) ++s_changed;
            *(vf.slot) = (std::uint64_t)val;
            if (s_applied_count < 256) {
                s_applied[s_applied_count].slot = vf.slot;
                s_applied[s_applied_count].value = (std::uint64_t)val;
                s_applied_cls[s_applied_count] = vf.via_cls;
                ++s_applied_count;
            }
            ++loaded;
        } else if (anchor_live && sscanf(linebuf, "cls %63s %llx", name, &val) == 2) {
            // Только небольшие расстояния внутри пула классов; никаких
            // произвольных адресов из файла без сверки с живым именем.
            const int64_t rel = (int64_t)((uint64_t)val - old_anchor);
#ifndef OR_CACHE_DIR
            if (rel < -(512ll << 20) || rel > (512ll << 20)) continue;
#endif
            const uint64_t k = new_anchor + rel;
            if (!valid_obj(k)) continue;
            const uint64_t nm = qrd_ptr(k + IL2CPP_CLASS_NAME);
            if (!valid_str_ptr(nm)) continue;
            bool ok = false;
            const std::string live = qrd_string(nm, &ok);
            if (!ok || live != name) continue;   // адрес устарел — берём остальное
            store_verified_class(k, name);
            ++classes;
        }
    }
    if (loaded > 0 && classes > 0 && !s_good_base) {
        // База, на которой живые классы читаются, считается живой: на неё же
        // встанет следующая привязка (после самообновления игры в карте бывает
        // два образа libil2cpp.so, и по ELF-заголовку пустой не отличить).
        s_good_base = g_il2cpp_base;
    }
    if (!anchor_live) {
        // Смещения полей уже применены — чит работает верными цифрами с первого
        // кадра. Адреса классов из кэша без якоря восстановить нельзя: их
        // доищет обычный скан, и повторного применения кэша не будет.
        s_cache_offsets_used = true;
        s_cache_retryable = false;
        s_applied_base = s_applied_count;   // это НЕ результат живого прохода
        LogLine("автооффсеты: кэш дал %d смещений (ключ 0x%llx), но живого якоря нет — "
                "классы ищу сканом, смещения уже применены",
                loaded, (unsigned long long)s_image_key);
        return false;
    }
    if (classes == 0 || !offset_resolver_class("PlayerManager", nullptr) ||
        !offset_resolver_class("GameControllerBase", nullptr)) {
#ifndef OR_CACHE_DIR
        LogLine("автооффсеты: кэш смещений прочитан, но ключевые живые классы не восстановились — нужен новый резолв");
        return false;
#endif
    }
    LogLine("автооффсеты: кэш классов — %d подтверждено после перепривязки (якорь 0x%llx -> 0x%llx)",
            classes, (unsigned long long)old_anchor, (unsigned long long)new_anchor);
    return loaded > 0;
}

}  // namespace

// ---- Публичный слой ----------------------------------------------------------

// Не хватает главного: без класса игрока и контроллера не работает ни ESP,
// ни аим, ни фарм — сколько бы смещений ни разрешилось.
static bool class_statics_live(const char* name) {
    for (int n = 0; n < 8; ++n) {
        const uint64_t k = offset_resolver_class_nth(name, nullptr, n);
        if (!k) break;
        const uint64_t statics = memio::untag(qrd_ptr(k + IL2CPP_CLASS_STATIC_FIELDS));
        if (valid_obj(statics)) return true;
    }
    return false;
}

static bool resolve_incomplete() {
    // Мало найти класс: пока игра в меню, рантайм не инициализировал его
    // статику (Il2CppClass.static_fields == 0), а именно через неё ядро берёт
    // локального игрока и реестр объектов. Лог 26.09 17:59 прямо об этом:
    // «нет локального игрока через GameControllerBase.static: … static=0x0».
    if (!class_statics_live("GameControllerBase")) return true;
    if (!offset_resolver_class("PlayerManager", nullptr)) return true;
    if (!class_statics_live("NetworkClient")) return true;
    return s_resolved < kOrFieldCount / 2;
}

// Повторный подбор БЕЗ полного сброса: окно имён, признак «скан .so пустой» и
// качество прежнего резолва сохраняются, поэтому повтор стоит не минуты, а
// один проход по указателям.
static void soft_rescan() {
    LogLine("автооффсеты: повторный подбор (попытка %d из 20): классов %d, разрешено %d/%d",
            s_rescan_count, s_class_count, s_resolved, kOrFieldCount);
    s_class_count = 0;
    s_names_ok = 0; s_names_fail = 0; s_shape_pass = 0;
    s_applied_count = 0; s_resolved = 0; s_changed = 0;
    s_rollback_count = 0; s_applied_base = 0; s_live_resolved = 0;
    s_via_class_count = 0; s_via_done = false; s_via_found = 0;
    s_logged = false;
    s_cache_saved = false;
    s_fields_off = 0; s_calib_best = 0; s_calib_best_off = 0;
    s_ptr_region = 0; s_ptr_pos = 0; s_ptr_scanned = 0;
    s_scout_done = false; s_scout_idx = 0; s_scout_bytes = 0; s_ptr_enough_at = 0;
    s_ptr_hits = 0; s_ptr_verified = 0; s_ptr_inwin = 0;
    s_ptr_noverify = 0; s_ptr_names = 0; s_ptr_readfails = 0;
    s_ptr_rinwin = 0; s_ptr_best_hits = 0; s_ptr_best_range = -1;
    s_ptr_passes = 0;
    s_so_scan_done = false; s_bootstrap_done = false;
    for (int k = 0; k < 3; ++k) { s_top_hits[k] = 0; s_top_range[k] = -1; }
    // Карта процесса за это время изменилась (игра домапила и метадату, и
    // кучу): перечитываем её целиком — фаза карт сама решит, куда идти дальше.
    s_phase = PH_MAPS;
}

void offset_resolver_tick() {
    if (g_pid <= 0 || !g_il2cpp_base) return;
    // Знания о базах (пустые/живая) привязаны к процессу: новый процесс —
    // новая карта памяти, всё недействительно.
    if (g_pid != s_known_pid) {
        s_known_pid = g_pid;
        s_good_base = 0;
        s_dead_count = 0;
        s_good_logged = false;
        s_prev_resolved = 0;   // качество резолва привязано к процессу
        s_rescan_count = 0; s_rescan_at = 0; s_so_scan_failed = false;
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
    // Квант времени: пока нет ключевых классов, чит всё равно ничего не
    // показывает — сканируем крупными кусками времени.
    const double budget = key_classes_missing() ? kTickBudgetBlind : kTickBudgetSec;
    const double deadline = mono_seconds() + budget;
    // Потолок пропускной способности: игра и чит читают одну и ту же память,
    // и безудержный скан душит игру. Если в текущей секунде уже прочитано
    // больше потолка — пропускаем такт целиком.
    {
        static double window_start = 0;
        static uint64_t window_bytes = 0;
        const double now = mono_seconds();
        const uint64_t total = scanned_total();
        if (now - window_start >= 1.0) { window_start = now; window_bytes = total; }
        else if (total - window_bytes > kScanBytesPerSec) return;
    }
    switch (s_phase) {
        case PH_WAIT:
#if OR_SELFTEST_BREAK_BAKED
            selftest_break_baked();
#endif
            // Первый вход на этом образе: если резолв уже когда-то сошёлся и
            // результат лежит в кэше — берём его и скан НЕ запускаем вовсе.
            // Ждать минуты после каждого перезахода в игру не нужно.
            if (!s_cache_checked || (s_cache_retryable && mono_seconds() >= s_cache_retry_at)) {
                s_cache_checked = true;
                if (load_offsets_cache()) {
                    s_from_cache = true;
                    s_phase = PH_DONE;
                    LogLine("автооффсеты: оффсеты взяты из кэша первого входа "
                            "(%d полей, образ 0x%llx) — сканирование не нужно",
                            s_resolved, (unsigned long long)g_il2cpp_base);
                    break;
                }
                if (s_cache_retryable) {
                    const double now = mono_seconds();
                    if (!s_cache_wait_since) s_cache_wait_since = now;
                    if (now - s_cache_wait_since < 90.0) {
                        s_cache_retry_at = now + 4.0;
                        break; // не принимаем указатель в код за класс
                    }
                    LogLine("автооффсеты: кэш — якорь не загрузился за 90 с; сканирую заново");
                    s_cache_retryable = false;
                }
            }
            // Между попытками чтения кэша остаёмся в PH_WAIT. Раньше этот
            // такт проваливался в PH_MAPS и запускал полный скан УЖЕ на
            // следующем кадре, несмотря на совпавший ключ (лог: ожидание
            // якоря @3.456, карта процесса @3.507).
            if (s_cache_retryable) break;

            // Если живой класс ещё не инициализирован, но в слотах есть токен TypeInfo —
            // ждём, пока игра инициализирует класс при загрузке (обычно 2-8 секунд).
            if (anchor_has_pending_token() && !anchor_class(nullptr)) {
                const double now = mono_seconds();
                if (!s_anchor_wait_since) {
                    s_anchor_wait_since = now;
                    LogLine("автооффсеты: найден токен TypeInfo — жду инициализации класса игрой");
                }
                if (now - s_anchor_wait_since < 45.0) {
                    break; // остаёмся в PH_WAIT, ждём следующий кадр
                }
                LogLine("автооффсеты: класс не инициализировался за 45 с — продолжаю скан");
            }
            s_phase = PH_MAPS;
            break;
        case PH_MAPS:   phase_maps(); break;
        case PH_SCAN:   phase_scan(deadline); break;
        case PH_HEAP:   phase_heap(deadline); break;
        case PH_META:   phase_meta(deadline); break;
        case PH_KID:    phase_kid(deadline); break;
        case PH_STRS:   phase_strsearch(deadline); break;
        case PH_BLOB:   phase_blob(deadline); break;
        case PH_PTR:    phase_ptr(deadline); break;
        case PH_FIELDS: phase_fields(); break;
        case PH_DONE:   break;
    }
    log_phase_timing();
    if (s_phase == PH_DONE) {
        log_summary();
        // Скан только при первом входе: результат уходит в кэш, чтобы следующий
        // вход не ждал его заново. Один раз за резолв — не каждый кадр.
        if (!s_from_cache && !s_cache_saved && mono_seconds() >= s_cache_save_retry_at) {
            s_cache_saved = save_offsets_cache();
            if (!s_cache_saved) s_cache_save_retry_at = mono_seconds() + 10.0;
        }
        // Игра могла ещё грузиться: классов Assembly-CSharp в памяти нет, пока
        // мир не поднялся. Ждём и пробуем снова — иначе чит остаётся пустым до
        // перезапуска (лог 26.09 17:56: 6 констант из 160, PlayerManager нет).
        if (!s_from_cache && s_rescan_count < kRescanLimit && resolve_incomplete()) {
            const double now = mono_seconds();
            if (!s_rescan_at) {
                // Загрузка мира занимает от десятков секунд до нескольких
                // минут: первые попытки частые, дальше реже, чтобы фоновый
                // скан не грел телефон. Всего покрываем около 20 минут.
                const double wait = (s_rescan_count < 5) ? 25.0 : 60.0;
                s_rescan_at = now + wait;
                LogLine("автооффсеты: ключевых классов игры ещё нет (разрешено %d/%d) — "
                        "повторю подбор через %d с (мир, видимо, ещё не загружен), попытка %d из %d",
                        s_resolved, kOrFieldCount, (int)wait, s_rescan_count + 1, kRescanLimit);
            } else if (now >= s_rescan_at) {
                ++s_rescan_count;
                s_rescan_at = 0;
                soft_rescan();
            }
        } else if (!s_from_cache && s_rescan_count >= kRescanLimit && s_rescan_at == 0) {
            s_rescan_at = -1.0;   // печатаем один раз
            LogLine("автооффсеты: %d повторов подряд без ключевых классов — "
                    "останавливаюсь. Если игра уже в мире, нажми «Пересканировать оффсеты» в меню",
                    kRescanLimit);
        }
    }
}

void offset_resolver_reset() {
    s_phase = PH_WAIT;
    s_anchor_wait_since = 0;
    s_anchor_retries = 0;
    s_seg_count = 0; s_seg_idx = 0; s_scan_pos = 0;
    s_xseg_count = 0; s_xseg_bytes = 0;
    s_lib_begin = s_lib_end = 0; s_meta_begin = s_meta_end = 0;
    s_total_bytes = 0; s_scanned_bytes = 0;
    s_maps_lib_lines = 0; s_maps_cut_window = 0; s_maps_cut_x = 0;
    s_maps_cut_deleted = 0; s_maps_open_failed = false;
    s_class_count = 0; s_names_ok = 0; s_names_fail = 0; s_shape_pass = 0;
    s_fields_off = 0; s_calib_best = 0; s_calib_best_off = 0;
    s_fi_size = IL2CPP_FIELDINFO_SIZE; s_fi_parent = IL2CPP_FIELDINFO_PARENT;
    s_resolved = 0; s_changed = 0; s_rollback_count = 0;
    s_applied_base = 0; s_live_resolved = 0;
    s_rescan_at = 0;   // расписание повтора — свойство прошлого прохода
    s_heap_range_count = 0; s_heap_range_idx = 0;
    s_heap_pos = s_heap_end = 0; s_heap_done = false;
    s_meta_range_count = 0; s_meta_range_idx = 0;
    s_meta_pos = 0; s_meta_scanned = 0; s_meta_pass = 0;
    s_meta_sig_done = false; s_meta_scan_first = false; s_meta_from_sig = false;
    s_meta_quick_only = false;
    s_so_scan_done = false; s_bootstrap_done = false;
    s_meta_bad_count = 0; s_meta_tries = 0;
    s_meta_blob = 0; s_meta_version = 0; s_meta_reason = nullptr;
    s_filesig_count = 0; s_filesig_bytes = 0;
    s_anon_count = 0; s_anon_total = 0; s_filemisc_count = 0;
    s_anon_noread_bytes = 0; s_anon_noread_count = 0;
    s_anon_bytes = 0; s_anon_skip_x = 0; s_anon_skip_x_bytes = 0;
    s_anon_skip_small = 0; s_anon_biggest = 0;
    s_hidden_count = 0; s_hidden_bytes = 0; s_hidden_skipped = 0;
    s_probe_ok = 0; s_probe_bytes = 0; s_probe_zero = 0; s_probe_zero_bytes = 0;
    s_str_addr = 0; s_str_region = 0; s_str_pos = 0; s_str_scanned = 0;
    s_str_cand_count = 0; s_str_cand_idx = 0; s_str_needle_idx = 0;
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
    s_image_key = 0; s_from_cache = false; s_cache_checked = false; s_cache_saved = false;
    s_cache_retryable = false; s_cache_wait_since = 0; s_cache_retry_at = 0;
    s_cache_offsets_used = false;
    s_cache_save_retry_at = 0;
    // Кандидаты баз и сканируемая база собираются заново из карты. Знания
    // «какие базы пусты, а где классы живые» НЕ сбрасываются: они привязаны
    // к процессу (s_known_pid) и переживают перепривязки к той же игре.
    s_candidate_count = 0;
    s_scan_base = 0;
    s_via_class_count = 0; s_via_done = false; s_via_found = 0;
    s_rot_kidx_count = 0;
    s_want_kidx_count = 0; s_kid_region = 0; s_kid_pos = 0; s_kid_scanned = 0;
    for (int i = 0; i < kViaFieldCount; ++i) {
        s_via_kidx[i][0] = s_via_kidx[i][1] = -1;
        s_via_klass[i][0] = s_via_klass[i][1] = 0;
    }
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
    return (s_phase == PH_WAIT && (s_cache_retryable || s_anchor_wait_since > 0)) ||
           s_phase == PH_MAPS || s_phase == PH_SCAN ||
           s_phase == PH_META ||
           s_phase == PH_HEAP || s_phase == PH_STRS ||
           s_phase == PH_BLOB || s_phase == PH_PTR ||
           s_phase == PH_KID ||
           s_phase == PH_FIELDS;
}

// Найденный класс: короткое имя (+ пространство имён, если задано).
// 0 — не найден (вызывающий код падает на вшитый TYPEINFO_RVA).
uint64_t offset_resolver_class_nth(const char* name, const char* ns, int nth) {
    if (s_phase != PH_DONE && s_phase != PH_FIELDS) return 0;
    if (!name || !name[0] || nth < 0) return 0;
    for (int i = 0; i < s_class_count; ++i) {
        if (strcmp(s_classes[i].name, name) != 0) continue;
        if (ns && ns[0] && s_classes[i].ns[0] && strcmp(s_classes[i].ns, ns) != 0) continue;
        if (nth-- == 0) return s_classes[i].klass;
    }
    return 0;
}

// Пришло ли значение слота из ЖИВЫХ метаданных этой сборки (резолв или кэш
// того же образа), а не из вшитой таблицы. Нужно функциям, которые ПИШУТ в
// память игры: писать по вшитому смещению на незнакомом билде нельзя.
bool offset_resolver_is_live(const uint64_t* slot) {
    if (!slot) return false;
    for (int i = 0; i < s_applied_count; ++i)
        if (s_applied[i].slot == slot) return true;
    return false;
}

uint64_t offset_resolver_class(const char* name, const char* ns) {
    return offset_resolver_class_nth(name, ns, 0);
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
        if (s_cache_retryable)
            snprintf(out, cap, "кэш найден: жду живой класс-якорь (без полного скана)");
        else if (s_anchor_wait_since > 0)
            snprintf(out, cap, "жду инициализации класса игрой (%d с)",
                     (int)(mono_seconds() - s_anchor_wait_since));
        else
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
    if (s_phase == PH_KID) {
        // Охота за классами с ротируемыми именами по индексу типа. «Готов»
        // показывать нельзя: поля ещё не разрешены (phase_fields впереди).
        snprintf(out, cap, "ищет классы по индексу типа: %llu МБ, найдено %d",
                 (unsigned long long)(s_kid_scanned >> 20), s_via_found);
        return;
    }
    if (s_phase == PH_META) {
        snprintf(out, cap, "ищет блок метаданных: %llu МБ (диапазон %d/%d)",
                 (unsigned long long)(s_meta_scanned >> 20),
                 s_meta_range_idx, s_meta_range_count);
        return;
    }
    if (s_phase == PH_STRS || s_phase == PH_BLOB || s_phase == PH_PTR) {
        snprintf(out, cap, "поиск по строкам: просмотрено %llu МБ, имён %d, классов %d",
                 (unsigned long long)((s_str_scanned + s_ptr_scanned) >> 20),
                 s_wanted_str_found, s_class_count);
        return;
    }
    // PH_DONE
    if (s_rescan_at < 0 && !s_from_cache) {
        snprintf(out, cap, "классы игры так и не появились: полей %d/%d — нажми «Пересканировать оффсеты»",
                 s_resolved, kOrFieldCount);
        return;
    }
    if (s_rescan_at > 0 && !s_from_cache) {
        // Резолв прошёл, но классов игры в памяти ещё нет (мир не загружен).
        // Слово «готов» в строке сохраняем: результат применён, просто он
        // неполный и будет уточнён повтором.
        const int left = (int)(s_rescan_at - mono_seconds());
        snprintf(out, cap, "готов частично: полей %d/%d, классов игры нет (мир грузится) — повтор через %d с",
                 s_resolved, kOrFieldCount, left > 0 ? left : 0);
        return;
    }
    if (s_from_cache) {
        snprintf(out, cap, "готов (кэш первого входа): полей %d/%d", s_resolved, kOrFieldCount);
        return;
    }
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
