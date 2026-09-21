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
#include "object_data_memory.h"
#include "game_internal.h"
#include "logfile.h"

#include <cstdio>
#include <cstring>
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

void esp_loot_reset() {
    std::lock_guard<std::recursive_mutex> guard(session_mutex());
    controller.reset();
    std::snprintf(refusal_text, sizeof(refusal_text), "-");
    last_log = {};
    last_log_valid = false;
    ++epoch;
}

void esp_loot_tick(bool enabled) {
    std::lock_guard<std::recursive_mutex> guard(session_mutex());
    const uint64_t base = (g_mem.bound() && g_pid > 0) ? g_il2cpp_base : 0;
    controller.tick(epoch, base, enabled);
    const loot::Diagnostics d = controller.diagnostics();
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

const char* esp_loot_refusal() {
    std::lock_guard<std::recursive_mutex> guard(session_mutex());
    return refusal_text;
}
