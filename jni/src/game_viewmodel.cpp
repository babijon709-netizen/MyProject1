#include "fps_memory.h"
#include "bullet_tp_memory.h"
#include "game.h"
#include "weapon_tuning_memory.h"
#include "fast_fire_memory.h"
#include "fast_loot_memory.h"
#include "no_recoil_memory.h"
#include "no_ballistics_memory.h"
#include "object_data_memory.h"
#include "app_state.h"
#include "config.h"
#include "farm.h"
#include "maps_lookup.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cerrno>
#include <cstdarg>
#include <ctime>
#include <limits>
#include <mutex>
#include <thread>
#include <sys/resource.h>
#include <sys/stat.h>
#include <cstdio>
#include <fcntl.h>
#include <fstream>
#include <unistd.h>
#include "game_viewmodel_internal.h"
#include "game_internal.h"

namespace {
void bullet_tp_log(const char* format, ...) __attribute__((format(printf, 1, 2)));
void bullet_tp_log(const char* format, ...) {
    static std::mutex log_mutex;
    std::lock_guard<std::mutex> guard(log_mutex);

    char path[512], backup[520];
    std::snprintf(path, sizeof(path), "%sbullet_tp.log", CfgDir());
    std::snprintf(backup, sizeof(backup), "%s.1", path);
    if (::mkdir(CfgDir(), 0777) != 0 && errno != EEXIST) return;

    struct stat st{};
    if (::lstat(path, &st) == 0) {
        if (!S_ISREG(st.st_mode)) {
            ::unlink(path);
        } else if (st.st_size >= (1 << 20)) {
            ::unlink(backup);
            ::rename(path, backup);
        }
    }
    const int fd = ::open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC |
                          O_NOFOLLOW | O_NONBLOCK, 0644);
    if (fd < 0) return;
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        ::close(fd);
        return;
    }

    char message[1024]{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    timespec ts{};
    ::clock_gettime(CLOCK_REALTIME, &ts);
    tm local{};
    ::localtime_r(&ts.tv_sec, &local);
    char stamp[40]{};
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local);
    char line[1200]{};
    const int length = std::snprintf(line, sizeof(line), "[%s.%03ld] %s\n",
                                     stamp, ts.tv_nsec / 1000000L, message);
    if (length > 0) {
        size_t offset = 0;
        const size_t total = std::min<size_t>(static_cast<size_t>(length), sizeof(line) - 1);
        while (offset < total) {
            const ssize_t written = ::write(fd, line + offset, total - offset);
            if (written > 0) offset += static_cast<size_t>(written);
            else if (written < 0 && errno == EINTR) continue;
            else break;
        }
    }
    ::close(fd);
}

struct Memory {
    template<class T> bool read(uint64_t address, T& value) {
        return address >= 0x10000 && esp_silent_read(address, &value, sizeof(value));
    }
    template<class T> bool write(uint64_t address, const T& value) {
        return address >= 0x10000 && esp_silent_target(g_pid) &&
            esp_silent_write(address, &value, sizeof(value));
    }
    uint64_t ptr(uint64_t address) {
        uint64_t p = 0;
        return read(address, p) ? memio::untag(p) : 0;
    }
    bool klass_is(uint64_t klass, const char* name) {
        using namespace go_active;
        if (!klass) return false;
        char n[64]{}, ns[64]{};
        auto np = ptr(klass + IL2CPP_CLASS_NAME), sp = ptr(klass + IL2CPP_CLASS_NAMESPACE);
        // Указатели имён невалидны — класс структурно мёртв, это не «нет данных».
        if (np < 0x10000 || sp < 0x10000) return false;
        // Строки имён лежат в памяти метаданных, которая на части устройств не
        // читается транспортом (EIO; см. class_identity в game.cpp — там же
        // прецедент структурного допуска). Не прочиталось — верим структуре:
        // объект уже пришёл по проверенной цепочке (свой пайплайн игрока или
        // TypeInfo-слот этого релиза). Прочиталось — только точное имя.
        if (!esp_silent_read(np, n, sizeof(n)) || !esp_silent_read(sp, ns, sizeof(ns))) return true;
        n[63] = ns[63] = 0;
        return std::strcmp(n, name) == 0 && std::strcmp(ns, "Oxide") == 0;
    }
    bool is(uint64_t object, const char* name) { return object && klass_is(ptr(object), name); }
};
Memory memory;

// ---- Снимок /proc/<pid>/maps ----------------------------------------------
//
// Карта памяти процесса игры — это тысячи строк (сотни килобайт). Разбирать её
// заново на каждую запись в кучу означало разбирать её десятки раз за кадр:
// поток отрисовки при этом просто стоит, и вместе с ним встают гуи и визуалы
// (жалоба: «после включения быстрой стрельбы и перезарядки весь оверлей
// лагает»). Снимок читается целиком одним read() в переиспользуемый буфер и
// живёт kMapsSnapshotTtl; любая неподтверждённая запись принудительно берёт
// свежий снимок, поэтому проверка остаётся строгой.
// Object identity/backlinks are revalidated before every write, so the mapping
// itself does not need to be reparsed four times a second.  A longer range TTL
// removes the remaining periodic /proc/maps hitch while keeping the gate tied
// to a short-lived snapshot and to the current pid.
constexpr double kMapsSnapshotTtl = 2.0;   // секунды

std::string maps_text_cache;
pid_t       maps_text_pid = -1;
double      maps_text_at = -1e9;
bool        maps_text_ok = false;

bool read_proc_maps(pid_t pid, std::string& out) {
    out.clear();
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%d/maps", (int)pid);
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char buffer[64 * 1024];
    for (;;) {
        const ssize_t got = ::read(fd, buffer, sizeof(buffer));
        if (got < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            return false;
        }
        if (got == 0) break;
        if (out.size() + (size_t)got > (8u << 20)) { ::close(fd); return false; }
        out.append(buffer, (size_t)got);
    }
    ::close(fd);
    return !out.empty();
}

// Подтверждённые ворота: границы отображений, которые уже прошли проверку.
// Разбор карты стоит миллисекунды, а быстрая стрельба/перезарядка пишут по
// несколько полей в кадре — и все они попадают в одно и то же отображение
// управляемой кучи. Пока диапазон свежий, запись в него стоит двух сравнений.
enum class HeapWriteFailure { None, Session, Gate, Transport };
struct HeapWriteDiagnostic {
    HeapWriteFailure failure = HeapWriteFailure::None;
    object_data::Reject reject = object_data::Reject::None;
    bool maps_available = true;
    char map_line[192]{};
};
struct HeapGate { uint64_t lo=0, hi=0; double at=-1e9; };
constexpr int kHeapGateSlots = 8;
HeapGate heap_gates[kHeapGateSlots];
int heap_gate_next = 0;

void invalidate_maps_snapshot() {
    maps_text_cache.clear();
    maps_text_pid = -1;
    maps_text_at = -1e9;
    maps_text_ok = false;
    for (HeapGate& gate : heap_gates) gate = {};
    heap_gate_next = 0;
}

// force — перечитать карту, даже если снимок ещё не просрочен.
const std::string* maps_snapshot(bool force) {
    if (g_pid <= 0) { invalidate_maps_snapshot(); return nullptr; }
    const double now = memio::now_seconds();
    if (!force && maps_text_ok && maps_text_pid == g_pid &&
        now - maps_text_at < kMapsSnapshotTtl && now >= maps_text_at)
        return &maps_text_cache;
    maps_text_ok = read_proc_maps(g_pid, maps_text_cache);
    maps_text_pid = g_pid;
    maps_text_at = now;
    if (!maps_text_ok) { maps_text_cache.clear(); return nullptr; }
    return &maps_text_cache;
}

bool copy_maps_snapshot(pid_t pid, uint64_t module_base, std::string& out) {
    std::lock_guard<std::recursive_mutex> session_guard(viewmodel_session_mutex());
    if (pid <= 0 || !module_base || g_pid != pid || g_il2cpp_base != module_base ||
        !esp_silent_target(pid)) return false;
    const std::string* snapshot = maps_snapshot(false);
    if (!snapshot) return false;
    out = *snapshot;
    return true;
}

// Единственные ворота записи в кучу игры: адрес обязан лежать в анонимном
// rw-отображении, подтверждённом разбором /proc/<pid>/maps. Подтверждение
// кэшируется диапазоном и живёт не дольше kMapsSnapshotTtl; всё, что кэш не
// подтвердил, проверяется по свежей карте.
bool heap_gate_allows(uint64_t address, size_t size, HeapWriteDiagnostic* diagnostic = nullptr) {
    if (diagnostic) *diagnostic = {};
    if (g_pid <= 0 || address < 0x10000 || !size || size > 64 ||
        address > std::numeric_limits<uint64_t>::max() - size) {
        if (diagnostic) {
            diagnostic->failure = HeapWriteFailure::Gate;
            diagnostic->reject = object_data::Reject::BadArgs;
        }
        return false;
    }
    if (maps_text_pid != g_pid) invalidate_maps_snapshot();
    const double now = memio::now_seconds();
    for (const HeapGate& gate : heap_gates) {
        if (!gate.hi || now - gate.at >= kMapsSnapshotTtl || now < gate.at) continue;
        if (address >= gate.lo && address + size <= gate.hi) return true;
    }

    uint64_t lo = 0, hi = 0;
    object_data::Reject why = object_data::Reject::None;
    char line[192] = {};
    bool maps_available = false;
    const std::string* text = maps_snapshot(false);
    if (text) {
        maps_available = true;
        if (object_data::heap_writable_range(*text, address, size, lo, hi,
                                             &why, line, sizeof(line))) {
            heap_gates[heap_gate_next] = {lo, hi, now};
            heap_gate_next = (heap_gate_next + 1) % kHeapGateSlots;
            return true;
        }
    }

    const object_data::Reject cached_why = why;
    char cached_line[sizeof(line)]{};
    std::snprintf(cached_line, sizeof(cached_line), "%s", line);
    const bool cached_maps_available = maps_available;
    why = object_data::Reject::None;
    line[0] = '\0';
    text = maps_snapshot(true);
    if (text) {
        maps_available = true;
        if (object_data::heap_writable_range(*text, address, size, lo, hi,
                                             &why, line, sizeof(line))) {
            heap_gates[heap_gate_next] = {lo, hi, now};
            heap_gate_next = (heap_gate_next + 1) % kHeapGateSlots;
            return true;
        }
    } else if (cached_maps_available) {
        why = cached_why;
        std::snprintf(line, sizeof(line), "%s", cached_line);
    }
    if (diagnostic) {
        diagnostic->failure = HeapWriteFailure::Gate;
        diagnostic->reject = why;
        diagnostic->maps_available = maps_available;
        std::snprintf(diagnostic->map_line, sizeof(diagnostic->map_line), "%s", line);
    }
    return false;
}

viewmodel::Controller<Memory> controller(memory);
viewmodel::FpsController<Memory> fps_controller(memory);
enum class ClassIdentityResult { Match, Mismatch, Unreadable, Invalid };

// Deliberately do not inherit Memory::write: these controllers can only write
// through the fresh anonymous-heap mapping gate.
struct HeapMemory {
    bool bound() const { return g_mem.bound() && g_pid > 0 && g_mem.pid() == g_pid; }
    uint64_t object_class(uint64_t object) { return memory.ptr(object); }
    template<class T> bool read(uint64_t address, T& value) { return memory.read(address,value); }
    // Диапазонное чтение (пакетные обходы плеер-лупа быстрого подбора):
    // один запрос на непрерывный кусок вместо чтения по элементу.
    bool read_range(uint64_t address, void* out, std::size_t size) {
        return address >= 0x10000 && size > 0 && esp_silent_read(address, out, size);
    }
    bool named_klass(uint64_t klass, const char* name, const char* namespaze) {
        if (klass < 0x10000) return false;
        uint64_t np=memory.ptr(klass+go_active::IL2CPP_CLASS_NAME);
        uint64_t sp=memory.ptr(klass+go_active::IL2CPP_CLASS_NAMESPACE);
        if (np < 0x10000 || sp < 0x10000) return false;   // мёртвый класс, не «нет данных»
        char n[96]{}, ns[96]{};
        const size_t nl=std::strlen(name)+1, sl=std::strlen(namespaze)+1;
        if (nl>sizeof(n) || sl>sizeof(ns)) return false;
        // Имена классов лежат в памяти метаданных: на части устройств она не
        // читается транспортом ВООБЩЕ (EIO), а под нагрузкой чтения падают и
        // на «здоровых» (до 4% за кадр, лог 16.09). Раньше любой такой сбой
        // ронял всю цепочку оружия («не применено», «ожидание») — теперь
        // нечитаемое имя допускает объект ПО СТРУКТУРЕ (прецедент —
        // class_identity в game.cpp), а прочитанное сверяется как раньше.
        if (!esp_silent_read(np,n,nl) || !esp_silent_read(sp,ns,sl)) return true;
        if (std::memcmp(ns,namespaze,sl)!=0) return false;
        if (std::memcmp(n,name,nl)==0) return true;
        // Читаемое, но другое имя: принимаем класс-НАСЛЕДНИК (имя содержит
        // базовое) — у части стволов конфиги и компоненты в своих подклассах
        // («…WeaponConfig» и т.п.), и точное сравнение оставляло их без
        // настроенных полей. Остальная цепочка (бэклинки, кросс-ссылки
        // калькулятора, ворота записи) подтверждает объект независимо.
        n[sizeof(n)-1]=0;
        return std::strstr(n,name)!=nullptr;
    }
    ClassIdentityResult exact_result(uint64_t object, const char* name, const char* namespaze) {
        if (object < 0x10000) return ClassIdentityResult::Invalid;
        const uint64_t klass = memory.ptr(object);
        if (klass < 0x10000 || (klass & (alignof(void*) - 1)) != 0)
            return ClassIdentityResult::Invalid;
        const uint64_t np = memory.ptr(klass + go_active::IL2CPP_CLASS_NAME);
        const uint64_t sp = memory.ptr(klass + go_active::IL2CPP_CLASS_NAMESPACE);
        char n[96]{}, ns[96]{};
        const size_t nl = std::strlen(name) + 1, sl = std::strlen(namespaze) + 1;
        if (np < 0x10000 || sp < 0x10000 || nl > sizeof(n) || sl > sizeof(ns))
            return ClassIdentityResult::Invalid;
        if (!esp_silent_read(np, n, nl) || !esp_silent_read(sp, ns, sl))
            return ClassIdentityResult::Unreadable;
        return std::memcmp(n, name, nl) == 0 && std::memcmp(ns, namespaze, sl) == 0
            ? ClassIdentityResult::Match : ClassIdentityResult::Mismatch;
    }
    bool exact(uint64_t object, const char* name, const char* namespaze) {
        return exact_result(object, name, namespaze) == ClassIdentityResult::Match;
    }
    bool named(uint64_t object, const char* name, const char* namespaze) {
        return object>=0x10000 && named_klass(memory.ptr(object),name,namespaze);
    }
    bool describe(uint64_t object, char* name, size_t name_size,
                  char* namespaze, size_t namespace_size) {
        if (object < 0x10000 || !name || !namespaze || name_size < 2 || namespace_size < 2)
            return false;
        const uint64_t klass = memory.ptr(object);
        if (klass < 0x10000) return false;
        const uint64_t name_ptr = memory.ptr(klass + go_active::IL2CPP_CLASS_NAME);
        const uint64_t namespace_ptr = memory.ptr(klass + go_active::IL2CPP_CLASS_NAMESPACE);
        auto copy_cstr = [&](uint64_t source, char* out, size_t capacity) {
            if (source < 0x10000) return false;
            for (size_t i = 0; i + 1 < capacity; ++i) {
                if (!memory.read(source + i, out[i])) { out[0] = 0; return false; }
                if (out[i] == 0) return true;
            }
            out[capacity - 1] = 0;
            return true;
        };
        return copy_cstr(name_ptr, name, name_size) &&
               copy_cstr(namespace_ptr, namespaze, namespace_size);
    }
    // Calculator names rotate between builds (Ze/FJ/...). Its empty namespace
    // and both typed backlinks are checked by the spread controller before any write.
    bool calculator_class(uint64_t object) {
        if (object < 0x10000) return false;
        const uint64_t klass=memory.ptr(object);
        if (klass < 0x10000) return false;
        const uint64_t np=memory.ptr(klass+go_active::IL2CPP_CLASS_NAME);
        const uint64_t sp=memory.ptr(klass+go_active::IL2CPP_CLASS_NAMESPACE);
        char ns=1, name[33]{};
        if (!np || !sp || !esp_silent_read(sp,&ns,1) || ns ||
            !esp_silent_read(np,name,sizeof(name))) return false;
        for (unsigned i=0;i<sizeof(name);++i) {
            if (!name[i]) return i>0;
            const unsigned char c=(unsigned char)name[i];
            if (!((c>='a'&&c<='z') || (c>='A'&&c<='Z') ||
                  (c>='0'&&c<='9') || c=='_')) return false;
        }
        return false;
    }
    template<class T> bool write_heap(uint64_t address, const T& value,
                                      HeapWriteDiagnostic* diagnostic = nullptr) {
        if (diagnostic) *diagnostic = {};
        std::lock_guard<std::recursive_mutex> session_guard(viewmodel_session_mutex());
        if (!g_mem.bound() || g_pid<=0 || g_mem.pid() != g_pid) {
            if (diagnostic) diagnostic->failure = HeapWriteFailure::Session;
            return false;
        }
        // Чтение и разбор всей /proc/<pid>/maps на КАЖДУЮ запись (а быстрая
        // стрельба и перезарядка пишут до семи полей в кадре) — это сотни
        // килобайт текста и миллисекунды на запись: оверлей вместе с гуи и
        // визуалами вставал именно на этом. Ворота не ослаблены — просто
        // подтверждение отображения кэшируется на kMapsSnapshotTtl, а всё
        // неподтверждённое проверяется по свежесчитанной карте.
        if (!heap_gate_allows(address, sizeof(T), diagnostic)) return false;
        if (!esp_silent_write(address, &value, sizeof(T))) {
            if (diagnostic) diagnostic->failure = HeapWriteFailure::Transport;
            return false;
        }
        return true;
    }
};
HeapMemory heap_memory;

class BulletMemory {
public:
    struct Diagnostics {
        uint64_t class_names_unreadable = 0, class_names_mismatch = 0, class_identity_invalid = 0;
        uint64_t write_attempts = 0, write_successes = 0, session_rejections = 0;
        uint64_t gate_rejections = 0, transport_failures = 0;
        uint64_t last_write_address = 0;
        HeapWriteDiagnostic last_write{};
    };

private:
    HeapMemory& heap_;
    Diagnostics diagnostics_{};
    std::atomic<bool> enabled_{false}, target_valid_{false};
    std::atomic<int> desired_pid_{-1}, work_pid_{-1};
    std::atomic<uint64_t> desired_base_{0}, work_base_{0};
    std::atomic<uint64_t> desired_generation_{0}, work_generation_{0};

    bool session_valid_locked() const {
        const int pid = work_pid_.load(std::memory_order_relaxed);
        const uint64_t base = work_base_.load(std::memory_order_relaxed);
        const uint64_t generation = work_generation_.load(std::memory_order_relaxed);
        return enabled_.load(std::memory_order_acquire) &&
            target_valid_.load(std::memory_order_acquire) && pid > 0 &&
            pid == desired_pid_.load(std::memory_order_relaxed) &&
            base == desired_base_.load(std::memory_order_relaxed) &&
            generation == desired_generation_.load(std::memory_order_relaxed) &&
            g_pid == pid && g_il2cpp_base == base && esp_silent_target(pid);
    }
    bool session_valid() const {
        if (!enabled_.load(std::memory_order_acquire)) return false;
        const int pid = work_pid_.load(std::memory_order_relaxed);
        const uint64_t base = work_base_.load(std::memory_order_relaxed);
        return target_valid_.load(std::memory_order_acquire) && pid > 0 &&
            pid == desired_pid_.load(std::memory_order_relaxed) &&
            base == desired_base_.load(std::memory_order_relaxed) &&
            work_generation_.load(std::memory_order_relaxed) ==
                desired_generation_.load(std::memory_order_relaxed) &&
            esp_silent_target(pid);
    }

public:
    explicit BulletMemory(HeapMemory& heap) : heap_(heap) {}

    void set_desired(bool enabled, bool target_valid, int pid, uint64_t base, uint64_t generation) {
        const bool changed = desired_pid_.load(std::memory_order_relaxed) != pid ||
            desired_base_.load(std::memory_order_relaxed) != base ||
            desired_generation_.load(std::memory_order_relaxed) != generation;
        if (!enabled || changed) enabled_.store(false, std::memory_order_release);
        desired_pid_.store(pid, std::memory_order_relaxed);
        desired_base_.store(base, std::memory_order_relaxed);
        desired_generation_.store(generation, std::memory_order_relaxed);
        target_valid_.store(enabled && target_valid, std::memory_order_release);
        if (enabled) enabled_.store(true, std::memory_order_release);
    }

    void begin_work(int pid, uint64_t base, uint64_t generation) {
        work_pid_.store(pid, std::memory_order_relaxed);
        work_base_.store(base, std::memory_order_relaxed);
        work_generation_.store(generation, std::memory_order_relaxed);
    }

    bool bound() const { return session_valid(); }

    bool read_range(uint64_t address, void* out, size_t size) {
        return session_valid() && heap_.read_range(address, out, size);
    }

    uint64_t object_class(uint64_t object) {
        return session_valid() ? heap_.object_class(object) : 0;
    }

    ClassIdentityResult exact_result(uint64_t object, const char* name, const char* namespaze) {
        std::lock_guard<std::recursive_mutex> session_guard(viewmodel_session_mutex());
        if (!session_valid_locked()) return ClassIdentityResult::Invalid;
        const ClassIdentityResult result = heap_.exact_result(object, name, namespaze);
        if (result == ClassIdentityResult::Unreadable) ++diagnostics_.class_names_unreadable;
        else if (result == ClassIdentityResult::Mismatch) ++diagnostics_.class_names_mismatch;
        else if (result == ClassIdentityResult::Invalid) ++diagnostics_.class_identity_invalid;
        return result;
    }

    bool exact(uint64_t object, const char* name, const char* namespaze) {
        const ClassIdentityResult result = exact_result(object, name, namespaze);
        if (result == ClassIdentityResult::Match) return true;
        if (result == ClassIdentityResult::Unreadable) {
            // The object fields and shared IL2CPP class pointer are revalidated before writes.
            return true;
        }
        return false;
    }

    template<class T> bool write_heap(uint64_t address, const T& value) {
        std::lock_guard<std::recursive_mutex> session_guard(viewmodel_session_mutex());
        ++diagnostics_.write_attempts;
        diagnostics_.last_write_address = address;
        HeapWriteDiagnostic result{};
        if (!session_valid_locked()) {
            result.failure = HeapWriteFailure::Session;
            ++diagnostics_.session_rejections;
            diagnostics_.last_write = result;
            return false;
        }
        if (!heap_.write_heap(address, value, &result)) {
            if (result.failure == HeapWriteFailure::Gate) ++diagnostics_.gate_rejections;
            else if (result.failure == HeapWriteFailure::Transport) ++diagnostics_.transport_failures;
            else if (result.failure == HeapWriteFailure::Session) ++diagnostics_.session_rejections;
            diagnostics_.last_write = result;
            return false;
        }
        ++diagnostics_.write_successes;
        diagnostics_.last_write = {};
        return true;
    }

    void reset_diagnostics() { diagnostics_ = {}; }
    Diagnostics diagnostics() const { return diagnostics_; }
};

const char* bullet_tp_heap_failure_name(HeapWriteFailure failure) {
    switch (failure) {
        case HeapWriteFailure::None: return "none";
        case HeapWriteFailure::Session: return "session";
        case HeapWriteFailure::Gate: return "heap_gate";
        case HeapWriteFailure::Transport: return "transport_write";
    }
    return "unknown";
}

const char* bullet_tp_transient_name(int tag) {
    switch (tag) {
        case 1: return "player_list_incomplete";
        case 5: return "camera_frame_unavailable";
        case 8: return "camera_position_unavailable";
        case 9: return "transform_camera_unavailable";
        default: return tag ? "unknown_transient" : "none";
    }
}

const char* bullet_tp_gate_reject_name(object_data::Reject reject) {
    switch (reject) {
        case object_data::Reject::None: return "none";
        case object_data::Reject::BadArgs: return "bad_args";
        case object_data::Reject::ParseAbort: return "maps_parse_abort";
        case object_data::Reject::NoMapping: return "no_mapping";
        case object_data::Reject::FileBacked: return "file_backed";
        case object_data::Reject::FileTail: return "file_tail";
        case object_data::Reject::NamedRegion: return "named_region";
        case object_data::Reject::Perms: return "permissions";
        case object_data::Reject::SpansBoundary: return "spans_boundary";
    }
    return "unknown";
}

class BulletTpWorker {
    using Clock = std::chrono::steady_clock;
    struct Request {
        bool enabled = false;
        int pid = -1;
        uint64_t module_base = 0;
        uint64_t target_id = 0;
        uint64_t generation = 0;
        bullet_tp::Vec3 target{};
        EspBulletTpFrameDiagnostics frame{};
        Clock::time_point published_at{};
    };

    BulletMemory memory_;
    bullet_tp::Controller<BulletMemory> controller_;
    std::mutex mutex_;
    std::condition_variable wake_;
    Request request_{};
    uint64_t generation_ = 0;
    uint64_t revision_ = 0;
    bool stopping_ = false;
    std::thread thread_;
    Clock::time_point last_summary_{};
    Clock::time_point last_rate_sample_{};
    uint64_t last_scan_bytes_reported_ = 0;
    uint64_t maps_refreshes_ = 0, maps_refresh_failures_ = 0;

    bool start_locked() {
        if (thread_.joinable()) return true;
        try {
            thread_ = std::thread([this] { run(); });
            return true;
        } catch (...) {
            return false;
        }
    }

    void log_summary(const Request& request, size_t maps_bytes, bool maps_available) {
        const Clock::time_point now = Clock::now();
        if (last_summary_ != Clock::time_point{} && now - last_summary_ < std::chrono::seconds(5))
            return;
        const double elapsed = last_rate_sample_ == Clock::time_point{} ? 0.0 :
            std::chrono::duration<double>(now - last_rate_sample_).count();
        const bullet_tp::Diagnostics scan = controller_.diagnostics();
        const BulletMemory::Diagnostics memory = memory_.diagnostics();
        const uint64_t scanned_delta = scan.scan_bytes >= last_scan_bytes_reported_
            ? scan.scan_bytes - last_scan_bytes_reported_ : scan.scan_bytes;
        const double scan_mib_s = elapsed > 0.0
            ? static_cast<double>(scanned_delta) / elapsed / (1024.0 * 1024.0) : 0.0;
        last_summary_ = now;
        last_rate_sample_ = now;
        last_scan_bytes_reported_ = scan.scan_bytes;
        const long long target_age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - request.published_at).count();
        char labels[300]{};
        size_t used = 0;
        for (size_t i = 0; i < 4 && used < sizeof(labels) - 1; ++i) {
            if (!scan.maps.rejected_names[i][0]) continue;
            const int n = std::snprintf(labels + used, sizeof(labels) - used,
                "%s%s:%zu", used ? "," : "", scan.maps.rejected_names[i],
                scan.maps.rejected_name_counts[i]);
            if (n <= 0) break;
            used += std::min<size_t>(static_cast<size_t>(n), sizeof(labels) - used - 1);
        }
        if (!labels[0]) std::snprintf(labels, sizeof(labels), "none");

        bullet_tp_log("summary frame enabled=%d pid=%d base=0x%llx target=0x%llx xyz=%.2f,%.2f,%.2f age_ms=%lld transient=%d/%s players=%u boxed=%u no_id=%u dead=%u alive=%u enemies=%u bots=%u human=%u range=%u world=%u",
            request.enabled ? 1 : 0, request.pid,
            static_cast<unsigned long long>(request.module_base),
            static_cast<unsigned long long>(request.target_id), request.target.x,
            request.target.y, request.target.z, target_age_ms, request.frame.transient_tag,
            bullet_tp_transient_name(request.frame.transient_tag),
            request.frame.player_records, request.frame.boxed_records,
            request.frame.no_network_id, request.frame.dead, request.frame.alive,
            request.frame.enemies, request.frame.bot_candidates,
            request.frame.human_candidates, request.frame.distance_candidates,
            request.frame.world_position_candidates);
        bullet_tp_log("summary scan maps_ok=%d maps_bytes=%zu refresh=%llu refresh_fail=%llu lines=%zu parse_fail=%zu rw_p=%zu accepted=%zu heap_bytes=%llu file=%zu perms=%zu file_tail=%zu named=%zu size=%zu labels=%s errno=%d cycles=%llu chunks=%llu chunk_fail=%llu bytes=%llu mib_s=%.1f fields=%llu class_ptr=%llu bad_class=%llu identity_checks=%llu identity_accept=%llu identity_reject=%llu unreadable_names=%llu class_mismatch=%llu identity_bad=%llu added=%llu tracked=%zu projectile_class=0x%llx last_candidate_class=0x%llx active=%d last_candidate=0x%llx",
            maps_available ? 1 : 0, maps_bytes,
            static_cast<unsigned long long>(maps_refreshes_),
            static_cast<unsigned long long>(maps_refresh_failures_), scan.maps.lines,
            scan.maps.parse_failures, scan.maps.rw_private, scan.maps.accepted,
            static_cast<unsigned long long>(scan.maps.accepted_bytes), scan.maps.file_backed,
            scan.maps.bad_permissions, scan.maps.file_tail, scan.maps.named_regions,
            scan.maps.bad_size, labels, esp_mem_last_errno(),
            static_cast<unsigned long long>(scan.scan_cycles),
            static_cast<unsigned long long>(scan.scan_chunks),
            static_cast<unsigned long long>(scan.scan_read_failures),
            static_cast<unsigned long long>(scan.scan_bytes), scan_mib_s,
            static_cast<unsigned long long>(scan.candidates_with_valid_fields),
            static_cast<unsigned long long>(scan.valid_class_pointers),
            static_cast<unsigned long long>(scan.invalid_class_pointers),
            static_cast<unsigned long long>(scan.exact_checks),
            static_cast<unsigned long long>(scan.exact_matches),
            static_cast<unsigned long long>(scan.exact_rejects),
            static_cast<unsigned long long>(memory.class_names_unreadable),
            static_cast<unsigned long long>(memory.class_names_mismatch),
            static_cast<unsigned long long>(memory.class_identity_invalid),
            static_cast<unsigned long long>(scan.projectiles_added),
            scan.tracked_projectiles,
            static_cast<unsigned long long>(scan.projectile_class),
            static_cast<unsigned long long>(scan.last_class),
            scan.scan_active ? 1 : 0,
            static_cast<unsigned long long>(scan.last_candidate));
        bullet_tp_log("summary apply candidates=%llu class_bad=%llu read_fail=%llu fields_bad=%llu already=%llu write_pairs=%llu current=%llu/%llu previous=%llu/%llu calls=%llu ok=%llu session_reject=%llu gate_reject=%llu transport_fail=%llu last_addr=0x%llx last_reason=%s gate_reason=%s maps_ok=%d map='%s'",
            static_cast<unsigned long long>(scan.apply_candidates),
            static_cast<unsigned long long>(scan.apply_class_mismatches),
            static_cast<unsigned long long>(scan.apply_read_failures),
            static_cast<unsigned long long>(scan.apply_invalid_fields),
            static_cast<unsigned long long>(scan.already_at_target),
            static_cast<unsigned long long>(scan.write_pairs_ok),
            static_cast<unsigned long long>(scan.current_writes_ok),
            static_cast<unsigned long long>(scan.current_write_attempts),
            static_cast<unsigned long long>(scan.previous_writes_ok),
            static_cast<unsigned long long>(scan.previous_write_attempts),
            static_cast<unsigned long long>(memory.write_attempts),
            static_cast<unsigned long long>(memory.write_successes),
            static_cast<unsigned long long>(memory.session_rejections),
            static_cast<unsigned long long>(memory.gate_rejections),
            static_cast<unsigned long long>(memory.transport_failures),
            static_cast<unsigned long long>(memory.last_write_address),
            bullet_tp_heap_failure_name(memory.last_write.failure),
            bullet_tp_gate_reject_name(memory.last_write.reject),
            memory.last_write.maps_available ? 1 : 0,
            memory.last_write.map_line[0] ? memory.last_write.map_line : "none");
    }

    void run() {
        // Keep the heap scanner below the render thread's scheduling priority.
        ::setpriority(PRIO_PROCESS, 0, 10);
        std::string maps_cache;
        int maps_pid = -1;
        uint64_t maps_base = 0;
        double maps_at = -1e9;
        uint64_t handled_generation = std::numeric_limits<uint64_t>::max();
        std::unique_lock<std::mutex> lock(mutex_);

        while (!stopping_) {
            if (request_.generation != handled_generation) {
                handled_generation = request_.generation;
                lock.unlock();
                controller_.reset();
                memory_.reset_diagnostics();
                maps_cache.clear();
                maps_pid = -1;
                maps_base = 0;
                maps_at = -1e9;
                maps_refreshes_ = 0;
                maps_refresh_failures_ = 0;
                last_summary_ = {};
                last_rate_sample_ = {};
                last_scan_bytes_reported_ = 0;
                lock.lock();
                continue;
            }
            if (!request_.enabled) {
                const uint64_t revision = revision_;
                wake_.wait(lock, [&] { return stopping_ || revision_ != revision; });
                continue;
            }

            const Request request = request_;
            const uint64_t revision = revision_;
            if (!request.target_id || Clock::now() - request.published_at >
                    std::chrono::milliseconds(150)) {
                lock.unlock();
                log_summary(request, maps_cache.size(), !maps_cache.empty());
                lock.lock();
                wake_.wait_for(lock, std::chrono::milliseconds(100), [&] {
                    return stopping_ || revision_ != revision || !request_.enabled;
                });
                continue;
            }
            lock.unlock();

            memory_.begin_work(request.pid, request.module_base, request.generation);
            const double now = memio::now_seconds();
            if (maps_pid != request.pid || maps_base != request.module_base ||
                maps_cache.empty() || now < maps_at || now - maps_at >= 1.0) {
                ++maps_refreshes_;
                std::string fresh_maps;
                if (copy_maps_snapshot(request.pid, request.module_base, fresh_maps)) {
                    maps_cache.swap(fresh_maps);
                    maps_pid = request.pid;
                    maps_base = request.module_base;
                } else {
                    ++maps_refresh_failures_;
                    maps_cache.clear();
                    maps_pid = -1;
                    maps_base = 0;
                }
                maps_at = now;
            }
            if (!maps_cache.empty()) {
                controller_.tick(true, request.pid, request.module_base,
                                 request.target_id, request.target, &maps_cache, now);
            }
            log_summary(request, maps_cache.size(), !maps_cache.empty());

            lock.lock();
            wake_.wait_for(lock, std::chrono::milliseconds(16), [&] {
                return stopping_ || !request_.enabled || request_.generation != request.generation ||
                       revision_ != revision;
            });
        }

        lock.unlock();
        controller_.reset();
    }

public:
    explicit BulletTpWorker(HeapMemory& heap) : memory_(heap), controller_(memory_) {}

    ~BulletTpWorker() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
            memory_.set_desired(false, false, -1, 0, ++generation_);
        }
        wake_.notify_one();
        if (thread_.joinable()) thread_.join();
    }

    bool publish(int pid, uint64_t module_base, const EspBulletTpTarget& target,
                 const EspBulletTpFrameDiagnostics& frame) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || !start_locked()) return false;
        const bool generation_changed = !request_.enabled || request_.pid != pid ||
            request_.module_base != module_base;
        const bool target_changed = request_.target_id != target.id;
        if (generation_changed) ++generation_;
        request_.enabled = true;
        request_.pid = pid;
        request_.module_base = module_base;
        request_.target_id = target.id;
        request_.target = {target.x, target.y, target.z};
        request_.frame = frame;
        request_.generation = generation_;
        request_.published_at = Clock::now();
        memory_.set_desired(true, target.id != 0, pid, module_base, generation_);
        if (generation_changed || target_changed) {
            ++revision_;
            wake_.notify_one();
        }
        return true;
    }

    void disable() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || (!request_.enabled && request_.pid <= 0)) return;
        ++generation_;
        request_ = {};
        request_.pid = -1;
        request_.generation = generation_;
        memory_.set_desired(false, false, -1, 0, generation_);
        ++revision_;
        wake_.notify_one();
    }

    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return;
        ++generation_;
        request_ = {};
        request_.pid = -1;
        request_.generation = generation_;
        memory_.set_desired(false, false, -1, 0, generation_);
        ++revision_;
        wake_.notify_one();
    }
};

BulletTpWorker bullet_tp_worker(heap_memory);
weapon_tuning::Controller<HeapMemory> weapons(heap_memory);
fast_fire::Controller<HeapMemory> rapid_fire(heap_memory);
fast_loot::Controller<HeapMemory> loot_speed(heap_memory);
no_recoil::Controller<HeapMemory> no_recoil_ctl(heap_memory);
no_ballistics::Controller<HeapMemory> no_ballistics_ctl(heap_memory);

// Причина остановки быстрой стрельбы для лога: расшифровка стадии, которую
// выставляет fast_fire::Controller (resolve/build_plan/apply в
// fast_fire_memory.h). Стадия 0 + не-Active — ожидание привязки/игрока.
const char* fast_fire_stop_reason(viewmodel::Status status, int stage) {
    if (status == viewmodel::Status::Off) return "disabled or game unbound";
    switch (stage) {
        case 1:  return "no local player or network identity";
        case 2:  return "FPManager missing or dead";
        case 3:  return "weapon slot in hands is empty";
        case 4:  return "weapon type not supported";
        case 5:  return "weapon backlink to player mismatch";
        case 6:  return "fire mode or burst wait unreadable";
        case 7:  return "weapon timings unreadable or out of range";
        case 8:  return "weapon context changed mid-write";
        case 9:  return "originals could not be restored (restart the game)";
        case 10: return "period write not confirmed by readback";
        case 11: return "burst overlap guard: cache shorter than full burst cycle";
        default: return "waiting for bind or player";
    }
}
// «Без отдачи»: расшифровка стадий из no_recoil_memory.h для лога.
const char* no_recoil_stop_reason(viewmodel::Status status, int stage) {
    if (status == viewmodel::Status::Off) return "disabled or game unbound";
    switch (stage) {
        case 1: case 2:  return "no local player";
        case 3: case 4:  return "network identity missing or not local";
        case 5:          return "FPManager missing or dead";
        case 6:          return "not a hitscan in hands";
        case 7:          return "weapon backlink to player mismatch";
        case 8:          return "HitscanWeaponConfig not found";
        case 9:          return "RecoilConfig asset not found";
        case 10: case 12: return "recoil values unreadable";
        case 11:         return "weapon context changed mid-write";
        case 13: case 14: return "write not confirmed by readback";
        default:         return "waiting for bind or player";
    }
}
const char* no_ballistics_kind_name(no_ballistics::Kind kind) {
    using no_ballistics::Kind;
    switch (kind) {
        case Kind::Hitscan: return "hitscan";
        case Kind::Bow: return "bow";
        case Kind::Crossbow: return "crossbow";
        case Kind::RocketLauncher: return "rocket";
        case Kind::ExplosiveCharge: return "charge";
        case Kind::Snowball: return "snowball";
        case Kind::Spear: return "spear";
        case Kind::Melee: return "melee/tool";
        default: return "unknown";
    }
}
const char* no_ballistics_status_name(viewmodel::Status status) {
    switch (status) {
        case viewmodel::Status::Off: return "off";
        case viewmodel::Status::Waiting: return "waiting";
        case viewmodel::Status::Active: return "active";
        case viewmodel::Status::Failed: return "failed";
        case viewmodel::Status::ADS: return "ads";
        default: return "unknown";
    }
}
const char* no_ballistics_stop_reason(viewmodel::Status status, int stage) {
    if (status == viewmodel::Status::Off) return "disabled or game unbound";
    switch (stage) {
        case 1: case 2: return "no local player";
        case 3: case 4: return "network identity missing or not local";
        case 5: return "FPManager missing or dead";
        case 6: return "unknown weapon type";
        case 7: return "weapon backlink to player mismatch";
        case 8: return "weapon config not found";
        case 9: return "ballistic profile not found";
        case 10: return "projectile prefab field unreadable";
        case 11: return "ShaftedProjectile prefab not found";
        case 12: return "no ballistic targets on active weapon";
        case 13: case 15: return "ballistic values unreadable or out of range";
        case 14: return "weapon context changed mid-write";
        case 16: case 17: return "zero write not confirmed by readback";
        case 18: return "original values could not be restored";
        default: return "waiting for bind or player";
    }
}
uint64_t unity_base = 0;
using Clock = std::chrono::steady_clock;
Clock::time_point fps_last_tick{}, fps_last_resolve{};
Clock::time_point weapon_last_tick{};
uint64_t weapon_last_player = 0;
unsigned weapon_last_flags = ~0u;
int fps_last_limit = 0;
viewmodel::Status fps_last_log = viewmodel::Status::Off;
// Build ID libunity, который не прошёл таблицу kFpsLayouts (0 — движок
// опознан или данных нет). Показывается в меню: без него вечное «ожидание»
// неотличимо от «игра ещё не на связи», а по самому ID новый лейаут
// добавляется в fps_memory.h без доступа к устройству.
uint64_t fps_unknown_build_id = 0;

uint64_t find_unity() {
    std::string text;
    if (!read_proc_maps(g_pid, text)) return 0;
    uint64_t candidates[8]{}, found = 0;
    int count = maps::lookup_library_bases(text, "libunity.so", candidates, 8);
    for (int i = 0; i < count; ++i) {
        if (!viewmodel::fps_layout(memory, candidates[i])) continue;
        if (found) return 0; // ambiguous live/stale mappings: do not guess
        found = candidates[i];
    }
    // Неопознанная версия движка (игра обновилась): запоминаем build ID
    // первого образа для меню и журнала. Не прочитался — 0, тогда в меню
    // останется просто «ожидание».
    fps_unknown_build_id = 0;
    // Повтор той же неопознанной сборки — не новость: в логе 11:52-11:54 эта
    // строка шла 92 раза за две минуты и топила всё остальное. Печатаем смену
    // build id или числа mapped-образов сразу, одинаковое — раз в 30 c.
    // mapped отличает «игра обновилась» от «карты памяти не читаются», а
    // непрочитанный id говорит, что лейаут добавлять ещё рано.
    static uint64_t last_logged_build = 0;
    static int last_logged_maps = -1;
    static Clock::time_point last_logged_at{};
    if (found) {
        last_logged_build = 0;   // следующая неопознанная сборка — сразу в лог
        last_logged_maps = -1;
    }
    if (!found && count > 0) {
        uint64_t id = 0;
        const bool id_read = memory.read(candidates[0] + 0x318, id) && id != 0;
        if (id_read) fps_unknown_build_id = id;
        const auto now_log = Clock::now();
        const bool changed = id != last_logged_build || count != last_logged_maps;
        if (changed || now_log - last_logged_at > std::chrono::seconds(30)) {
            last_logged_at = now_log;
            last_logged_build = id;
            last_logged_maps = count;
            if (id_read)
                ;
            else
                ;
        }
    }
    return found;
}
void tick_fps(uint64_t session, int requested_limit) {
    const int limit = viewmodel::sanitize_fps_limit(requested_limit);
    const bool enabled = limit != 0;
    if (!enabled && !fps_controller.has_state()) { fps_last_limit = 0; return; }
    const auto now = Clock::now();
    if (limit == fps_last_limit && now - fps_last_tick < std::chrono::milliseconds(100)) return;
    const bool changed = limit != fps_last_limit;
    fps_last_tick = now; fps_last_limit = limit;
    // A libunity mapping can be replaced during a scene reload. Keeping the
    // old base after one failed validation leaves the FPS control permanently
    // stuck in "waiting/failed", because the resolver was only retried while
    // unity_base was zero. Re-resolve periodically both before the first
    // success and after a failed controller tick.
    const bool need_resolve = !unity_base || fps_controller.status == viewmodel::Status::Failed;
    if (enabled && need_resolve && now - fps_last_resolve >= std::chrono::seconds(1)) {
        fps_last_resolve = now;
        unity_base = find_unity();
    }
    fps_controller.tick(session, unity_base, limit);
    if (enabled && fps_controller.status == viewmodel::Status::Failed &&
        now - fps_last_resolve >= std::chrono::seconds(1)) {
        fps_last_resolve = now;
        unity_base = find_unity();
        fps_controller.tick(session, unity_base, limit);
    }
    if (changed || fps_last_log != fps_controller.status) {
        fps_last_log = fps_controller.status;
    }
}
uint64_t epoch = 1;
viewmodel::Diagnostics last_log{};
}

std::recursive_mutex& viewmodel_session_mutex() {
    static std::recursive_mutex mutex;
    return mutex;
}

void esp_bullet_tp_tick(bool enabled, const EspBulletTpTarget& target,
                        EspBulletTpFrameDiagnostics diagnostics) {
    using SteadyClock = std::chrono::steady_clock;
    int pid = -1;
    uint64_t module_base = 0;
    int session_reason = 0;
    char backend[32] = "unknown";
    if (enabled) {
        std::lock_guard<std::recursive_mutex> guard(viewmodel_session_mutex());
        pid = g_pid;
        module_base = g_il2cpp_base;
        if (pid <= 0) session_reason = 1;
        else if (!module_base) session_reason = 2;
        else if (!g_mem.bound()) session_reason = 3;
        else if (g_mem.pid() != pid || !esp_silent_target(pid)) session_reason = 4;
        if (!session_reason)
            std::snprintf(backend, sizeof(backend), "%s", g_mem.backend_name());
    }

    struct LogState {
        std::mutex mutex;
        int session_reason = -1;
        int transient_tag = 0;
        uint64_t target_id = UINT64_MAX;
        SteadyClock::time_point last_transient_report{};
        SteadyClock::time_point last_target_report{};
        SteadyClock::time_point last_block_report{};
        SteadyClock::time_point last_worker_failure{};
    };
    static LogState state;
    bool log_session = false;
    bool log_transient = false;
    bool log_target = false;
    bool log_blocked = false;
    bool log_worker_failure = false;
    {
        std::lock_guard<std::mutex> guard(state.mutex);
        const auto now = SteadyClock::now();
        log_session = enabled && state.session_reason != session_reason;
        if (enabled && session_reason &&
            (log_session || state.last_block_report == SteadyClock::time_point{} ||
             now - state.last_block_report >= std::chrono::seconds(15))) {
            log_blocked = true;
            state.last_block_report = now;
        }
        const bool transient_changed = diagnostics.transient_tag != state.transient_tag &&
            (diagnostics.transient_tag != 0 || state.transient_tag != 0);
        if (enabled && transient_changed &&
            (state.last_transient_report == SteadyClock::time_point{} ||
             now - state.last_transient_report >= std::chrono::seconds(1))) {
            log_transient = true;
            state.last_transient_report = now;
        }
        if (enabled && target.id != state.target_id &&
            (state.last_target_report == SteadyClock::time_point{} ||
             now - state.last_target_report >= std::chrono::seconds(1))) {
            log_target = true;
            state.last_target_report = now;
        }
        state.session_reason = enabled ? session_reason : -1;
        if (!enabled || log_transient) state.transient_tag = diagnostics.transient_tag;
        if (!enabled || log_target) state.target_id = enabled ? target.id : UINT64_MAX;
        if (!enabled) {
            state.last_transient_report = {};
            state.last_target_report = {};
        }
    }

    if (log_session && !session_reason)
        bullet_tp_log("event=session ready pid=%d base=0x%llx backend=%s",
            pid, static_cast<unsigned long long>(module_base), backend);
    if (log_blocked) {
        const char* reason = session_reason == 1 ? "pid_missing" :
            session_reason == 2 ? "il2cpp_base_missing" :
            session_reason == 3 ? "memory_transport_unbound" : "pid_mismatch";
        bullet_tp_log("event=blocked reason=%s pid=%d base=0x%llx frame_missing_session=%d",
            reason, pid, static_cast<unsigned long long>(module_base),
            diagnostics.missing_session ? 1 : 0);
    }
    if (log_transient)
        bullet_tp_log("event=esp_frame transient_tag=%d reason=%s players=%u",
            diagnostics.transient_tag, bullet_tp_transient_name(diagnostics.transient_tag),
            diagnostics.player_records);
    if (log_target)
        bullet_tp_log("event=target id=0x%llx xyz=%.2f,%.2f,%.2f players=%u boxed=%u no_id=%u dead=%u alive=%u enemies=%u bots=%u human=%u range=%u world=%u",
            static_cast<unsigned long long>(target.id), target.x, target.y, target.z,
            diagnostics.player_records, diagnostics.boxed_records,
            diagnostics.no_network_id, diagnostics.dead, diagnostics.alive,
            diagnostics.enemies, diagnostics.bot_candidates, diagnostics.human_candidates,
            diagnostics.distance_candidates, diagnostics.world_position_candidates);

    if (!enabled || session_reason) {
        bullet_tp_worker.disable();
        return;
    }
    if (!bullet_tp_worker.publish(pid, module_base, target, diagnostics)) {
        {
            std::lock_guard<std::mutex> guard(state.mutex);
            const auto now = SteadyClock::now();
            if (state.last_worker_failure == SteadyClock::time_point{} ||
                now - state.last_worker_failure >= std::chrono::seconds(15)) {
                state.last_worker_failure = now;
                log_worker_failure = true;
            }
        }
        if (log_worker_failure) bullet_tp_log("event=worker_start_failed");
    }
}

void esp_viewmodel_reset(bool restore) {
    std::lock_guard<std::recursive_mutex> guard(viewmodel_session_mutex());
    controller.reset(restore && g_mem.bound());
    fps_controller.reset(restore && g_mem.bound());
    if (!weapons.reset(restore && g_mem.bound()))
        ;
    if (!rapid_fire.reset(restore && g_mem.bound()))
        ;
    if (!no_recoil_ctl.reset(restore && g_mem.bound()))
        ;
    if (!no_ballistics_ctl.reset(restore && g_mem.bound()))
        ;
    bullet_tp_worker.reset();
    loot_speed.reset();
    unity_base = 0; fps_last_tick = {}; fps_last_resolve = {}; fps_last_limit = 0;
    weapon_last_tick = {}; weapon_last_player = 0; weapon_last_flags = ~0u;
    invalidate_maps_snapshot();
    ++epoch;
}

void esp_viewmodel_tick(const ViewmodelSettings& settings) {
    using namespace go_active;
    {
        static std::mutex setting_log_mutex;
        static bool initialized = false;
        static bool previous = false;
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(setting_log_mutex);
            changed = !initialized || previous != settings.bullet_tp;
            initialized = true;
            previous = settings.bullet_tp;
        }
        if (changed) bullet_tp_log("event=setting enabled=%d", settings.bullet_tp ? 1 : 0);
    }
    std::lock_guard<std::recursive_mutex> guard(viewmodel_session_mutex());
    const bool want_fast_fire = g_state.fast_fire;
    if (!settings.bullet_tp) bullet_tp_worker.disable();
    if (!g_mem.bound() || !g_il2cpp_base || g_pid <= 0) {
        controller.reset(false); fps_controller.reset(false); weapons.reset(false);
        bullet_tp_worker.disable();
        rapid_fire.reset(false);
        rapid_fire.status = want_fast_fire ? viewmodel::Status::Waiting : viewmodel::Status::Off;
        no_recoil_ctl.reset(false);
        no_recoil_ctl.status = settings.no_recoil_button ? viewmodel::Status::Waiting : viewmodel::Status::Off;
        no_ballistics_ctl.reset(false);
        no_ballistics_ctl.status = settings.no_ballistics ? viewmodel::Status::Waiting : viewmodel::Status::Off;
        const bool want_spread = settings.no_spread || esp_silent_wants_no_spread();
        weapons.spread = want_spread ? viewmodel::Status::Waiting : viewmodel::Status::Off;
        unity_base = 0; fps_last_resolve = {};
        controller.status = {settings.fov_enabled ? viewmodel::Status::Waiting : viewmodel::Status::Off,
                             settings.hands_enabled ? viewmodel::Status::Waiting : viewmodel::Status::Off};
        return;
    }
    tick_fps(epoch, settings.fps_limit);
    {
        // Быстрый подбор («Забрать все»): живёт в плеер-лупе UniTask и не
        // зависит от цепочки игрока, поэтому тикается до её резолва.
        static fast_loot::State loot_last_state = fast_loot::State::Off;
        static bool loot_announced = false;
        const int loot_zeroed = loot_speed.tick(g_il2cpp_base, settings.fast_loot);
        if (loot_speed.state() != loot_last_state) {
            loot_last_state = loot_speed.state();
        }
        if (loot_zeroed > 0 && !loot_announced) {
            loot_announced = true;
        }
        if (!settings.fast_loot) loot_announced = false;
    }
    const bool want_no_spread = settings.no_spread || esp_silent_wants_no_spread();
    // Use the local identity already validated by the world reader first. The
    // fixed TypeInfo RVA can lag behind a game update even when every managed
    // field offset is unchanged; that used to disable all four weapon toggles.
    uint64_t player = 0;
    static bool local_source_failed = false;
    if (settings.fov_enabled || settings.hands_enabled || controller.has_state() ||
        want_no_spread || weapons.has_state() ||
        want_fast_fire || rapid_fire.has_state() ||
        settings.no_recoil_button || no_recoil_ctl.has_state() ||
        settings.no_ballistics || no_ballistics_ctl.has_state() ||
        g_farmActive) {
        auto local_player_ok = [&](uint64_t candidate) {
            uint64_t net=0; uint8_t local=0;
            return candidate>=0x10000 && memory.is(candidate,"PlayerManager") &&
                memory.read(candidate+go_active::NETWORK_BEHAVIOUR_IDENTITY,net) &&
                heap_memory.named(memio::untag(net),"NetworkIdentity","Mirror") &&
                memory.read(memio::untag(net)+go_active::NETWORK_IDENTITY_IS_LOCAL_PLAYER,local) && local==1;
        };
        if (local_player_ok(g_local_player)) player=g_local_player;
        auto try_class = [&](uint64_t klass) {
            if (player || !klass || !memory.klass_is(klass,"GameControllerBase")) return;
            const uint64_t sf=get_class_static_fields(klass);
            uint64_t raw=0;
            if (!sf || !memory.read(sf+GAME_CONTROLLER_LOCAL_PLAYER_FIELD,raw)) return;
            raw=memio::untag(raw);
            if (local_player_ok(raw)) player=raw;
        };
        try_class(g_game_controller_class);
        try_class(memory.ptr(g_il2cpp_base+GAME_CONTROLLER_TYPEINFO_RVA));
        if (!player) {
            if (!local_source_failed) {
                local_source_failed=true;
            }
            // A failed read is NOT a confirmed scene change. Keep pending
            // originals so a later successful frame can still restore them.
            auto wait=controller.has_state()?viewmodel::Status::Failed:viewmodel::Status::Waiting;
            controller.status={wait,wait};
            weapons.spread=want_no_spread||weapons.has_state()?viewmodel::Status::Failed:viewmodel::Status::Off;
            rapid_fire.tick(epoch, g_il2cpp_base, 0, want_fast_fire);
            no_recoil_ctl.tick(epoch, g_il2cpp_base, 0, settings.no_recoil_button);
            no_ballistics_ctl.tick(epoch, g_il2cpp_base, 0, settings.no_ballistics);
            return;
        }
        local_source_failed=false;
    }
    // Weapon chains are expensive (class names, backlinks and readbacks for
    // every scalar). They only need a watchdog cadence, not the uncapped
    // overlay rate. Setting/player changes bypass the limiter immediately.
    const unsigned weapon_flags = (want_no_spread ? 1u : 0u) |
                                  (want_fast_fire ? 2u : 0u) |
                                  (settings.no_recoil_button ? 4u : 0u) |
                                  (settings.no_ballistics ? 8u : 0u);
    const auto weapon_now = Clock::now();
    const bool weapon_due = weapon_flags != weapon_last_flags ||
                            player != weapon_last_player ||
                            weapon_now - weapon_last_tick >= std::chrono::milliseconds(100);
    if (weapon_due) {
        weapon_last_tick = weapon_now;
        weapon_last_player = player;
        weapon_last_flags = weapon_flags;
        weapons.tick(epoch, g_il2cpp_base, player, want_no_spread);
        rapid_fire.tick(epoch, g_il2cpp_base, player, want_fast_fire);
        no_recoil_ctl.tick(epoch, g_il2cpp_base, player, settings.no_recoil_button);
        no_ballistics_ctl.tick(epoch, g_il2cpp_base, player, settings.no_ballistics);
    }
    {
        // Log no-spread resolution changes only.
        static int last_stage = -1;
        const int stage = want_no_spread ? weapons.stage() : 0;
        if (stage != last_stage) {
            last_stage = stage;
            if (stage)
                ;
            else
                ;
        }
    }
    // Magic-Farm keeps the verified normal hit path (and therefore server-side
    // harvesting/cooldowns), but hides every non-ADS first-person tool pose
    // while mining. To the player only the crosshair and hit-streak marker are
    // visible; there is no local swing animation. Walking and aiming remain the
    // existing touch controller, and the user's own hands setting is restored
    // immediately outside the mining phase.
    ViewmodelSettings visible_settings = settings;
    if (g_farmActive && !g_farmPaused && g_farmPhase == 3) {
        visible_settings.hands_enabled = true;
        visible_settings.x = 0.f;
        visible_settings.y = -50.f;
        visible_settings.z = 0.f;
    }
    controller.tick(epoch, player, visible_settings);
    auto st = esp_viewmodel_diagnostics();
    if (st.fov != last_log.fov || st.hands != last_log.hands ||
        st.no_spread != last_log.no_spread) {
        last_log = st;
    }
}

viewmodel::Diagnostics esp_viewmodel_diagnostics() {
    std::lock_guard<std::recursive_mutex> guard(viewmodel_session_mutex());
    auto status = controller.status;
    status.no_spread = weapons.spread;
    status.fast_fire = rapid_fire.status;
    return status;
}

viewmodel::FpsUiInfo esp_viewmodel_fps_ui() {
    std::lock_guard<std::recursive_mutex> guard(viewmodel_session_mutex());
    viewmodel::FpsUiInfo info;
    info.requested = fps_last_limit;
    info.status = fps_controller.status;
    info.field_value = fps_controller.field_seen();
    info.pace_kind = fps_controller.pace_seen();
    info.build_id = fps_unknown_build_id;
    return info;
}
