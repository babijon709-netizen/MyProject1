// Мгновенный подбор: адаптер контроллера из loot_memory.h к памяти игры.
//
// Один поток: тик идёт из главного цикла оверлея рядом с esp_viewmodel_tick,
// отдельного писателя нет. Сброс (потеря процесса, выход, смена версии игры)
// может прийти из другого потока, поэтому всё под своим мьютексом.
//
// Запись ровно одна и только в анонимную кучу: перед каждым реальным записом
// заново читается /proc/<pid>/maps и адрес проверяется гейтом
// object_data::heap_writable — тот же контракт, что у контроллера оружейных
// данных. В код и данные libil2cpp.so / libunity.so не пишется ничего; оттуда
// читаются только build-id, оригинальные инструкции точек старого патча и
// указатель TypeInfo (см. layout_for в loot_memory.h).
#include "loot_memory.h"
#include "loot_tap_memory.h"
#include "object_data_memory.h"
#include "Android_touch/TouchHelperA.h"
#include "game_internal.h"
#include "logfile.h"

#include <cstdio>
#include <cstring>
#include <chrono>
#include <fstream>
#include <mutex>
#include <string>


namespace {

// Почему гейт кучи отказал: та же разборка строки /proc/<pid>/maps, что и в
// object_data::heap_writable, только вместо «нет» сохраняет причину строкой.
// Без неё отказ записи на устройстве неотличим от сотни других, а журнал
// в этой сборке выключен.
char refusal_text[128] = "-";

void note_refusal(const std::string& maps, uint64_t address) {
    uint64_t previous_end = 0;
    bool file_tail = false, seen = false;
    size_t pos = 0;
    while (pos < maps.size()) {
        auto end = maps.find('\n', pos);
        if (end == std::string::npos) end = maps.size();
        const std::string line = maps.substr(pos, end - pos);
        pos = end + 1;
        unsigned long long lo = 0, hi = 0, offset = 0, inode = 0;
        unsigned major = 0, minor = 0;
        char perms[5]{};
        int consumed = 0;
        if (std::sscanf(line.c_str(), "%llx-%llx %4s %llx %x:%x %llu %n",
                        &lo, &hi, perms, &offset, &major, &minor, &inode, &consumed) != 7 ||
            !consumed || hi <= lo || lo < previous_end) break;
        std::string name = line.substr(static_cast<size_t>(consumed));
        while (!name.empty() && (name.back() == ' ' || name.back() == '\r')) name.pop_back();
        const bool file = inode != 0 || name.find('/') != std::string::npos;
        file_tail = file || (lo == previous_end && file_tail);
        previous_end = hi;
        if (address >= lo && address < hi) {
            std::snprintf(refusal_text, sizeof(refusal_text), "%s %s ino=%llu off=%llx%s",
                          perms, name.empty() ? "[anon]" : name.c_str(), inode, offset,
                          file_tail ? " file-tail" : "");
            seen = true;
            break;
        }
    }
    if (!seen) std::snprintf(refusal_text, sizeof(refusal_text), "no mapping for %llx",
                             (unsigned long long)address);
}

// Журнал подбора/детекта окна (по просьбе пользователя «больше инфы, чем
// тосты»): один append-файл в общедоступных Загрузках первого по списку
// доступного пути. Проба путей — один раз за процесс, лениво, при первой
// строке; неудача — журнал молча молчит, канал необязательный.
static FILE* g_loot_log = nullptr;
static const char* g_loot_log_path = "";
static const std::chrono::steady_clock::time_point loot_log_start =
    std::chrono::steady_clock::now();

static FILE* ensure_loot_log_file() {
    if (g_loot_log) return g_loot_log;
    static const char* const candidates[] = {
        "/sdcard/Download/fastloot_di.log",
        "/storage/emulated/0/Download/fastloot_di.log",
        "/data/local/tmp/fastloot_di.log",
    };
    for (const char* p : candidates) {
        FILE* f = std::fopen(p, "a");
        if (f) { g_loot_log = f; g_loot_log_path = p; break; }
    }
    return g_loot_log;
}

struct Memory {
    // Кэш блоков живёт один кадр и для ПОИСКА подходит: устаревший элемент
    // списка отбракует проверка принадлежности, а не запись.
    template<class T> bool read(uint64_t address, T& value) {
        return address >= 0x10000 && g_mem.read(address, &value, sizeof(value));
    }
    uint64_t ptr(uint64_t address) {
        uint64_t p = 0;
        return read(address, p) ? memio::untag(p) : 0;
    }
    // Проверка принадлежности и оба float'а вокруг записи — только в обход кэша.
    template<class T> bool read_fresh(uint64_t address, T& value) {
        return address >= 0x10000 && g_mem.read_quiet(address, &value, sizeof(value));
    }
    uint64_t ptr_fresh(uint64_t address) {
        uint64_t p = 0;
        return read_fresh(address, p) ? memio::untag(p) : 0;
    }
    // Точное совпадение имени и пространства имён Il2CppClass. Вложенные типы
    // (DelayIgnoreTimeScalePromise, AsyncUniTask`1) могут сообщать
    // пространство внешнего типа, поэтому контроллер передаёт несколько
    // допустимых вариантов — см. kPromiseNs/kRunnerNs в loot_memory.h.
    bool klass_named(uint64_t klass, const char* name, const char* namespaze) {
        using namespace go_active;
        if (klass < 0x10000) return false;
        const uint64_t np = ptr(klass + IL2CPP_CLASS_NAME);
        if (!np) return false;
        char n[96]{}, ns[96]{};
        const size_t nl = std::strlen(name) + 1, sl = std::strlen(namespaze) + 1;
        if (nl > sizeof(n) || sl > sizeof(ns)) return false;
        if (!g_mem.read_quiet(np, n, nl) || std::memcmp(n, name, nl) != 0) return false;
        const uint64_t sp = ptr(klass + IL2CPP_CLASS_NAMESPACE);
        // Пустого указателя пространства имён достаточно для пустого имени:
        // вложенные типы и generic-инстанции могут не сообщать namespace.
        if (!sp) return namespaze[0] == '\0';
        return g_mem.read_quiet(sp, ns, sl) && std::memcmp(ns, namespaze, sl) == 0;
    }
    // Живое имя класса для свидетельного канала диагностики (ключи биндингов
    // Zenject, тост сводки): только чтение; неудача — пустая строка, ни один
    // гейт на содержимое не опирается. Свежие чтения в обход кадрового кэша.
    bool klass_name(uint64_t klass, char* out, size_t out_len) {
        using namespace go_active;
        if (!out || out_len < 2) return false;
        out[0] = 0;
        if (klass < 0x10000) return false;
        const uint64_t np = ptr(klass + IL2CPP_CLASS_NAME);
        if (!np) return false;
        if (!g_mem.read_quiet(np, out, out_len - 1)) { out[0] = 0; return false; }
        out[out_len - 1] = 0;
        return true;
    }
    // Журнал функции в Загрузки устройства (по просьбе пользователя): полный
    // след проходов и событий — то, что не влезает в 200-символьный тост.
    // Файл открывается один раз лениво; каждая строка сбрасывается на диск
    // сразу (журнал переживает аварийный выход).
    template<class... Args>
    void trace(const char* fmt, Args... args) {
        FILE* f = ensure_loot_log_file();
        if (!f) return;
        const double t = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - loot_log_start).count();
        std::fprintf(f, "[%9.2f] ", t);
        std::fprintf(f, fmt, args...);
        std::fputc('\n', f);
        std::fflush(f);
    }
    // Единственный писатель. Не наследует read(): карта памяти обязана быть
    // свежей на момент КАЖДОЙ записи, а не на момент начала кадра.
    template<class T> bool write_heap(uint64_t address, const T& value) {
        if (!g_mem.bound() || g_pid <= 0) return false;
        std::ifstream file("/proc/"+std::to_string(g_pid)+"/maps");
        if (!file) return false;
        std::string text, line;
        while (std::getline(file, line)) {
            text += line; text += '\n';
            if (text.size() > (8u << 20)) return false;
        }
        if (!file.eof()) return false;
        if (!object_data::heap_writable(text, address, sizeof(T))) {
            note_refusal(text, address);
            return false;
        }
        return g_mem.write(address, &value, sizeof(T));
    }
    // Никаких записей в код или данные libil2cpp.so / libunity.so — только
    // анонимная куча через write_heap. Патч двух инструкций задержки удалён
    // целиком: модификация .text библиотеки игры — и телеметрическая брешь
    // (снятие ожидания «за тик» видно серверу), и поверхность для CRC
    // целостности кода со стороны античита.
};

Memory memory;
loot::Controller<Memory> controller(memory);
uint64_t epoch = 1;
loot::Diagnostics last_log{};
bool last_log_valid = false;

// Тик приходит из главного цикла, сброс — из потока привязки (esp_reset) и из
// меню (смена версии игры), поэтому состояние под своим мьютексом.
std::recursive_mutex& session_mutex() {
    static std::recursive_mutex mutex;
    return mutex;
}

} // namespace

// Вне анонимного namespace: main.cpp показывает путь тостом при включении.
const char* esp_loot_log_path() {
    if (!g_loot_log) ensure_loot_log_file();
    return g_loot_log_path;
}

loot_tap::Tap tap;
loot_tap::Inject tap_inject = {[](float x, float y) { Touch_Down_N(2, x, y); },
                               []() { Touch_Up_N(2); }};
int prev_tap_ids[8] = {};
int prev_n_taps = 0;
uint64_t tick_frame = 1;

void esp_loot_reset() {
    std::lock_guard<std::recursive_mutex> guard(session_mutex());
    controller.reset();
    tap.reset();
    prev_n_taps = 0;
    std::snprintf(refusal_text, sizeof(refusal_text), "-");
    last_log = {};
    last_log_valid = false;
    ++epoch;
}

void esp_loot_tick(bool enabled) {
    std::lock_guard<std::recursive_mutex> guard(session_mutex());
    {
        // Края тумблера — заголовки сессий в журнале, чтобы отделять заходы.
        static bool prev_enabled = false;
        if (enabled != prev_enabled) {
            prev_enabled = enabled;
            memory.trace("=== fast_loot %s ===", enabled ? "включён" : "выключен");
        }
    }
    const uint64_t base = (g_mem.bound() && g_pid > 0) ? g_il2cpp_base : 0;
    controller.tick(epoch, base, enabled);
    const loot::Diagnostics d = controller.diagnostics();
    // Авто-тап «Забрать всё»: окно по DI-детекту, жесты игрока, один синтетический
    // палец. Детект не готов/сломан -> детект-параметр -1 -> точно не стреляем.
    {
        float xs[8] = {}, ys[8] = {};
        int ids[8] = {};
        const int nf = Touch_GetActive(xs, ys, ids, 8);
        bool tap_edge = false;
        float tx = 0.f, ty = 0.f;
        for (int i = 0; i < nf; ++i) {
            bool seen = false;
            for (int j = 0; j < prev_n_taps; ++j) if (prev_tap_ids[j] == ids[i]) { seen = true; break; }
            if (!seen) { tap_edge = true; tx = xs[i]; ty = ys[i]; }
        }
        for (int i = 0; i < nf && i < 8; ++i) prev_tap_ids[i] = ids[i];
        prev_n_taps = nf > 8 ? 8 : nf;
        const int root_windows = (d.di_stage == 5) ? static_cast<int>(d.di_root) : -1;
        const bool window_flow = d.kind == loot::Kind::Container;
        tap.on_frame(enabled, root_windows, window_flow, tap_edge, tx, ty,
                     tick_frame++, tap_inject);
    }
    if (!last_log_valid || d.status != last_log.status || d.kind != last_log.kind ||
        d.reason != last_log.reason || d.detail != last_log.detail ||
        d.hooked != last_log.hooked || d.flows != last_log.flows) {
        last_log = d;
        last_log_valid = true;
        LogLine("instant pickup: status=%d kind=%s reason=%s detail=%llx flows=%u "
                "hooked=%u hooked_run=%llx hooked_code=%08x %08x %08x %08x %08x "
                "timing=%d expired=%llu races=%llu scans=%llu "
                "(status 0 off,1 waiting,2 active,3 refused)",
                (int)d.status, loot::kind_name(d.kind), loot::reason_name(d.reason),
                (unsigned long long)d.detail, d.flows, d.hooked,
                (unsigned long long)d.hooked_run, d.hooked_code[0], d.hooked_code[1],
                d.hooked_code[2], d.hooked_code[3], d.hooked_code[4], d.timing,
                (unsigned long long)d.expired, (unsigned long long)d.races,
                (unsigned long long)d.scans);
    }
}

loot::Diagnostics esp_loot_diagnostics() {
    std::lock_guard<std::recursive_mutex> guard(session_mutex());
    return controller.diagnostics();
}

loot_tap::Diagnostics esp_loot_tap_diagnostics() {
    std::lock_guard<std::recursive_mutex> guard(session_mutex());
    return tap.diagnostics();
}

const char* esp_loot_refusal() {
    std::lock_guard<std::recursive_mutex> guard(session_mutex());
    return refusal_text;
}
