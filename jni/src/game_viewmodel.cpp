#include "fps_memory.h"
#include "game.h"
#include "weapon_tuning_memory.h"
#include "fast_fire_memory.h"
#include "fast_loot_memory.h"
#include "no_recoil_memory.h"
#include "no_ballistics_memory.h"
#include "object_data_memory.h"
#include "app_state.h"
#include "farm.h"
#include "maps_lookup.h"
#include <chrono>
#include <cerrno>
#include <limits>
#include <cstdio>
#include <fcntl.h>
#include <fstream>
#include <unistd.h>
#include "game_viewmodel_internal.h"
#include "game_internal.h"
#include "logfile.h"

namespace {
struct Memory {
    template<class T> bool read(uint64_t address, T& value) {
        return address >= 0x10000 && g_mem.read_quiet(address, &value, sizeof(value));
    }
    template<class T> bool write(uint64_t address, const T& value) {
        return address >= 0x10000 && g_mem.write(address, &value, sizeof(value));
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
        if (!g_mem.read_quiet(np, n, sizeof(n)) || !g_mem.read_quiet(sp, ns, sizeof(ns))) return true;
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

// Единственные ворота записи в кучу игры: адрес обязан лежать в анонимном
// rw-отображении, подтверждённом разбором /proc/<pid>/maps. Подтверждение
// кэшируется диапазоном и живёт не дольше kMapsSnapshotTtl; всё, что кэш не
// подтвердил, проверяется по свежей карте.
bool heap_gate_allows(uint64_t address, size_t size) {
    if (g_pid <= 0 || address < 0x10000 || !size || size > 64 ||
        address > std::numeric_limits<uint64_t>::max() - size) return false;
    // Кэш принадлежит конкретному процессу: сменился pid — подтверждения из
    // прошлой сессии недействительны.
    if (maps_text_pid != g_pid) invalidate_maps_snapshot();
    const double now = memio::now_seconds();
    for (const HeapGate& gate : heap_gates) {
        if (!gate.hi || now - gate.at >= kMapsSnapshotTtl || now < gate.at) continue;
        if (address >= gate.lo && address + size <= gate.hi) return true;
    }
    uint64_t lo = 0, hi = 0;
    object_data::Reject why = object_data::Reject::None;
    char line[192] = {};
    const std::string* text = maps_snapshot(false);
    if (!text || !object_data::heap_writable_range(*text, address, size, lo, hi, &why, line, sizeof(line))) {
        // Снимок мог устареть (отображение появилось только что) — решает
        // всегда свежая карта, а не кэш.
        why = object_data::Reject::None; line[0] = '\0';
        text = maps_snapshot(true);
        if (!text || !object_data::heap_writable_range(*text, address, size, lo, hi, &why, line, sizeof(line))) {
            // Отказ ворот — самая непрозрачная из причин «функция не
            // применилась»: сама запись не делается, а наружу видно только
            // «не применено». Печатаем причину правила и строку карты,
            // накрывающую адрес, — по ним понятно, какое отображение не
            // пустило запись и можно ли это правило ослабить.
            // Троттлинг по коду причины: разные причины не глушат друг друга.
            static double refused_at[16] = {};
            static int refused_kind[16] = {};
            const int kind = ((int)why % 15) + 1; // 0 = «занято», потому не годится
            const double moment = memio::now_seconds();
            bool print = true;
            for (int i = 0; i < 16; ++i) {
                if (refused_kind[i] == kind) {
                    print = moment - refused_at[i] >= 5.0;
                    refused_at[i] = moment;
                    break;
                }
                if (refused_kind[i] == 0) { refused_kind[i] = kind; refused_at[i] = moment; break; }
                if (i == 15) { // свободных нет — забираем самый старый слот
                    int oldest = 0;
                    for (int j = 1; j < 16; ++j) if (refused_at[j] < refused_at[oldest]) oldest = j;
                    refused_kind[oldest] = kind; refused_at[oldest] = moment;
                }
            }
            if (print) {
                const char* reason = "unknown";
                switch (why) {
                    case object_data::Reject::BadArgs:      reason = "bad args"; break;
                    case object_data::Reject::ParseAbort:   reason = "maps parse aborted"; break;
                    case object_data::Reject::NoMapping:    reason = "no mapping covers the address"; break;
                    case object_data::Reject::FileBacked:   reason = "file-backed mapping"; break;
                    case object_data::Reject::FileTail:     reason = "anon tail of a file mapping (bss guard)"; break;
                    case object_data::Reject::NamedRegion:  reason = "anon mapping with unknown label"; break;
                    case object_data::Reject::Perms:        reason = "perms are not rw-p"; break;
                    case object_data::Reject::SpansBoundary:reason = "write spans the mapping end"; break;
                    default: break;
                }
                LogLine("запись в кучу отклонена воротами: адрес 0x%llx, %zu байт — %s; карта: %s",
                        (unsigned long long)address, size, reason,
                        line[0] ? line : (text ? "(строка не найдена)" : "(карта не прочиталась)"));
            }
            return false;
        }
    }
    heap_gates[heap_gate_next] = {lo, hi, now};
    heap_gate_next = (heap_gate_next + 1) % kHeapGateSlots;
    return true;
}

viewmodel::Controller<Memory> controller(memory);
viewmodel::FpsController<Memory> fps_controller(memory);
// Deliberately do not inherit Memory::write: these controllers can only write
// through the fresh anonymous-heap mapping gate.
struct HeapMemory {
    template<class T> bool read(uint64_t address, T& value) { return memory.read(address,value); }
    // Диапазонное чтение (пакетные обходы плеер-лупа быстрого подбора):
    // один запрос на непрерывный кусок вместо чтения по элементу.
    bool read_range(uint64_t address, void* out, std::size_t size) {
        return address >= 0x10000 && size > 0 && g_mem.read_quiet(address, out, size);
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
        if (!g_mem.read_quiet(np,n,nl) || !g_mem.read_quiet(sp,ns,sl)) return true;
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
    bool exact(uint64_t object, const char* name, const char* namespaze) {
        if (object < 0x10000) return false;
        const uint64_t klass=memory.ptr(object);
        if (klass < 0x10000) return false;
        const uint64_t np=memory.ptr(klass+go_active::IL2CPP_CLASS_NAME);
        const uint64_t sp=memory.ptr(klass+go_active::IL2CPP_CLASS_NAMESPACE);
        char n[96]{}, ns[96]{};
        const size_t nl=std::strlen(name)+1, sl=std::strlen(namespaze)+1;
        // Type-specific timing offsets must never use the permissive
        // metadata-read fallback used by the legacy object-data chain.
        return np>=0x10000 && sp>=0x10000 && nl<=sizeof(n) && sl<=sizeof(ns) &&
            g_mem.read_quiet(np,n,nl) && g_mem.read_quiet(sp,ns,sl) &&
            std::memcmp(n,name,nl)==0 && std::memcmp(ns,namespaze,sl)==0;
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
        if (!np || !sp || !g_mem.read_quiet(sp,&ns,1) || ns ||
            !g_mem.read_quiet(np,name,sizeof(name))) return false;
        for (unsigned i=0;i<sizeof(name);++i) {
            if (!name[i]) return i>0;
            const unsigned char c=(unsigned char)name[i];
            if (!((c>='a'&&c<='z') || (c>='A'&&c<='Z') ||
                  (c>='0'&&c<='9') || c=='_')) return false;
        }
        return false;
    }
    template<class T> bool write_heap(uint64_t address, const T& value) {
        if (!g_mem.bound() || g_pid<=0) return false;
        // Чтение и разбор всей /proc/<pid>/maps на КАЖДУЮ запись (а быстрая
        // стрельба и перезарядка пишут до семи полей в кадре) — это сотни
        // килобайт текста и миллисекунды на запись: оверлей вместе с гуи и
        // визуалами вставал именно на этом. Ворота не ослаблены — просто
        // подтверждение отображения кэшируется на kMapsSnapshotTtl, а всё
        // неподтверждённое проверяется по свежесчитанной карте.
        return heap_gate_allows(address,sizeof(T)) && g_mem.write(address,&value,sizeof(T));
    }
};
HeapMemory heap_memory;
weapon_tuning::Controller<HeapMemory> weapons(heap_memory);
fast_fire::Controller<HeapMemory> rapid_fire(heap_memory);
fast_loot::Controller<HeapMemory> loot_speed(heap_memory);
no_recoil::Controller<HeapMemory> no_recoil_ctl(heap_memory);
no_ballistics::Controller<HeapMemory> no_ballistics_ctl(heap_memory);

// Имя Il2CppClass для диагностики быстрого подбора: какой класс элементов
// плеер-лупа попался вместо DelayPromise. Чтение метаданных местами не
// стабильно (см. named_klass) — тогда отдаём hex указателя.
void fast_loot_klass_name(uint64_t klass, char* out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = 0;
    uint64_t np = memory.ptr(klass + go_active::IL2CPP_CLASS_NAME);
    char raw[24] = {};
    if (np >= 0x10000 && g_mem.read_quiet(np, raw, sizeof(raw))) {
        raw[sizeof(raw) - 1] = 0;
        size_t w = 0;
        for (size_t i = 0; raw[i] && w + 1 < cap; ++i) {
            const unsigned char c = (unsigned char)raw[i];
            if (c >= 0x20 && c < 0x7F) out[w++] = (char)c;  // фильтр от мусора
        }
        out[w] = 0;
        if (out[0]) return;
    }
    snprintf(out, cap, "0x%llx", (unsigned long long)klass);
}

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
// Переходы статуса быстрой стрельбы в лог; отдельно подсвечивается сам факт
// остановки («перестала работать») с причиной. Зовётся после КАЖДОГО
// rapid_fire.tick и после ручных смен статуса на отвязанном пути.
void log_fast_fire_transition() {
    static int last_stage = -1;
    static viewmodel::Status last_status = viewmodel::Status::Off;
    const int stage = rapid_fire.stage();
    const viewmodel::Status status = rapid_fire.status;
    if (stage == last_stage && status == last_status) return;
    if (last_status == viewmodel::Status::Active && status != viewmodel::Status::Active)
        LogLine("fast fire: stopped — %s (status=%d stage=%d)",
                fast_fire_stop_reason(status, stage), (int)status, stage);
    else
        LogLine("fast fire: status=%d stage=%d (1 local,2 FPManager,3 slot,4 type,5 owner,6 mode/wait,7 timing,8 changed,9 restore,10 write,11 burst overlap guard)",
                (int)status, stage);
    last_stage = stage; last_status = status;
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
void log_no_recoil_transition() {
    static int last_stage = -1;
    static viewmodel::Status last_status = viewmodel::Status::Off;
    const int stage = no_recoil_ctl.stage();
    const viewmodel::Status status = no_recoil_ctl.status;
    if (stage == last_stage && status == last_status) return;
    if (last_status == viewmodel::Status::Active && status != viewmodel::Status::Active)
        LogLine("no recoil: stopped — %s (status=%d stage=%d)",
                no_recoil_stop_reason(status, stage), (int)status, stage);
    else
        LogLine("no recoil: status=%d stage=%d (1-2 local,3-4 identity,5 FPManager,6 not hitscan,7 owner,8 config,9 recoil asset,10-14 write)",
                (int)status, stage);
    last_stage = stage; last_status = status;
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
void log_no_ballistics_transition() {
    using Clock = std::chrono::steady_clock;
    static int last_stage = -1, last_error_stage = -1;
    static viewmodel::Status last_status = viewmodel::Status::Off;
    static uint64_t last_context = ~uint64_t{0}, last_writes = ~uint64_t{0};
    static uint64_t last_restores = ~uint64_t{0}, last_error_address = ~uint64_t{0};
    static Clock::time_point last_emit{};

    const int stage = no_ballistics_ctl.stage();
    const viewmodel::Status status = no_ballistics_ctl.status;
    const no_ballistics::DebugInfo d = no_ballistics_ctl.debug_info();
    const auto now = Clock::now();
    const bool transition = stage != last_stage || status != last_status ||
                            d.context_generation != last_context;
    const bool error_changed = d.error_stage != last_error_stage ||
        (d.error_stage && d.error_address != last_error_address);
    const bool restore_changed = d.restore_generation != last_restores;
    const bool writes_pending = d.write_generation != last_writes;
    const bool write_due = writes_pending && now - last_emit >= std::chrono::seconds(5);
    const bool error_due = error_changed &&
        (transition || now - last_emit >= std::chrono::seconds(1));
    const bool heartbeat = status == viewmodel::Status::Active &&
        now - last_emit >= std::chrono::seconds(30);
    if (!transition && !restore_changed && !write_due && !error_due && !heartbeat) return;

    const char* reason = status == viewmodel::Status::Active
        ? "ok" : no_ballistics_stop_reason(status, stage);
    const char* game = go_active::selected_version == go_active::GameVersion::Beta
        ? "beta" : "release";
    char weapon_name[72]{}, weapon_ns[96]{};
    char config_name[72]{}, config_ns[96]{};
    char profile_name[72]{}, profile_ns[96]{};
    char projectile_name[72]{}, projectile_ns[96]{};
    auto describe = [&](uint64_t object, char* name, size_t name_size,
                        char* namespaze, size_t namespace_size) {
        if (heap_memory.describe(object, name, name_size, namespaze, namespace_size)) return;
        std::snprintf(name, name_size, "%s", object ? "unreadable" : "-");
        std::snprintf(namespaze, namespace_size, "%s", object ? "unreadable" : "-");
    };
    describe(d.probe_weapon, weapon_name, sizeof(weapon_name), weapon_ns, sizeof(weapon_ns));
    describe(d.probe_config, config_name, sizeof(config_name), config_ns, sizeof(config_ns));
    describe(d.probe_profile, profile_name, sizeof(profile_name), profile_ns, sizeof(profile_ns));
    describe(d.probe_projectile, projectile_name, sizeof(projectile_name), projectile_ns, sizeof(projectile_ns));
    LogLine("no ballistics: game=%s status=%s(%d) stage=%d reason=%s kind=%s probe_kind=%s "
            "weapon=0x%llx config=0x%llx profile=0x%llx prefab=0x%llx "
            "types[weapon=%s.%s config=%s.%s profile=%s.%s prefab=%s.%s] "
            "probe[player=0x%llx fp=0x%llx weapon=0x%llx config=0x%llx profile=0x%llx prefab=0x%llx]",
            game, no_ballistics_status_name(status), (int)status, stage, reason,
            no_ballistics_kind_name(d.kind), no_ballistics_kind_name(d.probe_kind),
            (unsigned long long)d.weapon, (unsigned long long)d.config,
            (unsigned long long)d.profile, (unsigned long long)d.projectile,
            weapon_name, weapon_ns, config_name, config_ns,
            profile_name, profile_ns, projectile_name, projectile_ns,
            (unsigned long long)d.probe_player, (unsigned long long)d.probe_fp,
            (unsigned long long)d.probe_weapon, (unsigned long long)d.probe_config,
            (unsigned long long)d.probe_profile, (unsigned long long)d.probe_projectile);
    LogLine("no ballistics: fields target=%02x read=%02x zero=%02x wrote=%02x saved=%02x "
            "restore_try=%02x restored=%02x skipped=%02x saved_values=[%.5g,%.5g,%.5g;%.5g,%.5g,%.5g] "
            "last=0x%llx %.5g->%.5g generations=context:%llu write:%llu restore:%llu "
            "(bits0-2=profile gravity/air/underwater, bits3-5=prefab gravity/air/underwater)",
            (unsigned)d.target_mask, (unsigned)d.read_mask, (unsigned)d.zero_mask,
            (unsigned)d.patched_mask, (unsigned)d.saved_mask,
            (unsigned)d.restore_attempted_mask, (unsigned)d.restored_mask,
            (unsigned)d.restore_skipped_mask,
            (double)d.original_values[0], (double)d.original_values[1], (double)d.original_values[2],
            (double)d.original_values[3], (double)d.original_values[4], (double)d.original_values[5],
            (unsigned long long)d.last_address, (double)d.last_before, (double)d.last_after,
            (unsigned long long)d.context_generation, (unsigned long long)d.write_generation,
            (unsigned long long)d.restore_generation);
    if (d.error_stage) {
        LogLine("no ballistics: error stage=%d reason=%s address=0x%llx value=%.7g",
                d.error_stage, no_ballistics_stop_reason(viewmodel::Status::Failed, d.error_stage),
                (unsigned long long)d.error_address, (double)d.error_value);
    }
    last_stage = stage;
    last_status = status;
    last_context = d.context_generation;
    last_writes = d.write_generation;
    last_restores = d.restore_generation;
    last_error_stage = d.error_stage;
    last_error_address = d.error_address;
    last_emit = now;
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
                LogLine("fps: libunity не опознан, build id %016llx, mapped=%d (игра обновилась?)",
                        (unsigned long long)id, count);
            else
                LogLine("fps: libunity не опознан, build id не прочитан (+0x318), mapped=%d", count);
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
        LogLine("fps: requested=%d target limit status=%d (0 off,1 waiting,2 set,4 memory error); actual FPS is device/pacing dependent",
                limit, (int)fps_last_log);
    }
}
uint64_t epoch = 1;
viewmodel::Diagnostics last_log{};
}

std::recursive_mutex& viewmodel_session_mutex() {
    static std::recursive_mutex mutex;
    return mutex;
}

void esp_viewmodel_reset(bool restore) {
    std::lock_guard<std::recursive_mutex> guard(viewmodel_session_mutex());
    controller.reset(restore && g_mem.bound());
    fps_controller.reset(restore && g_mem.bound());
    if (!weapons.reset(restore && g_mem.bound()))
        LogLine("weapon data: originals could not all be restored before disconnect; restart game");
    if (!rapid_fire.reset(restore && g_mem.bound()))
        LogLine("fast fire: originals could not all be restored before disconnect; restart game");
    log_fast_fire_transition();
    if (!no_recoil_ctl.reset(restore && g_mem.bound()))
        LogLine("no recoil: originals could not all be restored before disconnect; restart game");
    log_no_recoil_transition();
    if (!no_ballistics_ctl.reset(restore && g_mem.bound()))
        LogLine("no ballistics: originals could not all be restored before disconnect; restart game");
    log_no_ballistics_transition();
    loot_speed.reset();
    unity_base = 0; fps_last_tick = {}; fps_last_resolve = {}; fps_last_limit = 0;
    weapon_last_tick = {}; weapon_last_player = 0; weapon_last_flags = ~0u;
    invalidate_maps_snapshot();
    ++epoch;
}

void esp_viewmodel_tick(const ViewmodelSettings& settings) {
    using namespace go_active;
    std::lock_guard<std::recursive_mutex> guard(viewmodel_session_mutex());
    const bool want_fast_fire = g_state.fast_fire;
    if (!g_mem.bound() || !g_il2cpp_base || g_pid <= 0) {
        controller.reset(false); fps_controller.reset(false); weapons.reset(false);
        rapid_fire.reset(false);
        rapid_fire.status = want_fast_fire ? viewmodel::Status::Waiting : viewmodel::Status::Off;
        log_fast_fire_transition();
        no_recoil_ctl.reset(false);
        no_recoil_ctl.status = settings.no_recoil_button ? viewmodel::Status::Waiting : viewmodel::Status::Off;
        log_no_recoil_transition();
        no_ballistics_ctl.reset(false);
        no_ballistics_ctl.status = settings.no_ballistics ? viewmodel::Status::Waiting : viewmodel::Status::Off;
        log_no_ballistics_transition();
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
        static bool loot_last_on = false;
        static fast_loot::Clock::time_point loot_last_report{};
        static fast_loot::Clock::time_point loot_last_zero_at{};
        static int  loot_burst_count = 0;
        static bool loot_burst_open = false;
        static fast_loot::Diag loot_prev_diag{};

        // Включение фичи обнуляет диагностику: жалоба «не сработало» читается
        // по чистым счётчикам одной сессии, без хвостов прошлых игр.
        if (settings.fast_loot && !loot_last_on) {
            loot_speed.diag_reset();
            loot_prev_diag = {};
            loot_last_report = fast_loot::Clock::now();
            loot_last_zero_at = {};
            loot_burst_count = 0; loot_burst_open = false;
            LogLine("fast loot: включён, диагностика сброшена (%s)", go_active::VersionName());
        }
        loot_last_on = settings.fast_loot;

        const int loot_zeroed = loot_speed.tick(g_il2cpp_base, settings.fast_loot);
        if (loot_speed.state() != loot_last_state) {
            loot_last_state = loot_speed.state();
            LogLine("fast loot: state=%d stage=%d (0 off,1 waiting,2 active,3 failed; stage: 1 helper slot,2 statics,3 promise classes)",
                    (int)loot_last_state, loot_speed.stage());
        }
        if (loot_zeroed > 0 && !loot_announced) {
            loot_announced = true;
            LogLine("fast loot: first zeroing — %d delay(s) of 0.35f completed early", loot_zeroed);
        }

        const auto loot_now = fast_loot::Clock::now();
        // Серия сбора: открывается первым обнулением, закрывается полусекундой
        // тишины — факт «собрал ли вообще» виден по логу напрямую.
        if (loot_zeroed > 0) {
            loot_burst_open = true;
            loot_burst_count += loot_zeroed;
            loot_last_zero_at = loot_now;
        } else if (loot_burst_open &&
                   loot_now - loot_last_zero_at > std::chrono::milliseconds(500)) {
            loot_burst_open = false;
            LogLine("fast loot: серия сбора закончена, обнулено задержек: %d", loot_burst_count);
            loot_burst_count = 0;
        }

        // «Не работает»: когда модуль резолвнут, но попаданий нет — раз в 10 с
        // краткая сводка того, КАЖДАЯ точка обхода что нашла. Без аномалий
        // лог не растёт вообще.
        if (settings.fast_loot && loot_speed.state() == fast_loot::State::Active &&
            !loot_burst_open && loot_now - loot_last_report >= std::chrono::seconds(10)) {
            const fast_loot::Diag& d = loot_speed.diag();
            const bool loop_empty = d.items_seen == loot_prev_diag.items_seen;
            const bool anomaly = d.abort_mask != 0 || d.cadence_misses > 0 ||
                                 d.write_fails > 0 || d.elapsed_rejects > 0 ||
                                 (d.items_seen > 0 && d.klass_hits == 0) || loop_empty;
            if (anomaly) {
                loot_last_report = loot_now;
                char spans[80] = {};
                for (int i = 0; i < 4 && d.seen_timespan_hits[i] > 0; ++i) {
                    const size_t l = strlen(spans);
                    snprintf(spans + l, sizeof(spans) - l, "%s%.2fx%d",
                             i ? ", " : "", (double)d.seen_timespans[i], d.seen_timespan_hits[i]);
                }
                char others[100] = {};
                bool others_ok = false;
                if (d.klass_hits == 0) {
                    for (int i = 0; i < 3 && d.other_klass[i]; ++i) {
                        char nm[28]; fast_loot_klass_name(d.other_klass[i], nm, sizeof(nm));
                        const size_t l = strlen(others);
                        snprintf(others + l, sizeof(others) - l, "%s%s", i ? ", " : "", nm);
                        others_ok = true;
                    }
                }
                LogLine("fast loot: нет попаданий 10с — проходов +%d, элементов +%d, класс-хитов +%d; "
                        "обрывы=0x%x (1 runners,2 длина,4 массив,8 шапка,16 capacity,32 элементы), "
                        "класс-не-читается +%d, каденция-другая %d (%s), elapsed-отказ +%d, "
                        "запись-отказ %d, статика-потеряна %d%s%s%s",
                        d.sweeps - loot_prev_diag.sweeps,
                        d.items_seen - loot_prev_diag.items_seen,
                        d.klass_hits - loot_prev_diag.klass_hits,
                        d.abort_mask,
                        d.klass_read_fails - loot_prev_diag.klass_read_fails,
                        d.cadence_misses, spans[0] ? spans : "-",
                        d.elapsed_rejects - loot_prev_diag.elapsed_rejects,
                        d.write_fails, d.static_lost,
                        others_ok ? ", в лупе вижу: " : "", others_ok ? others : "",
                        loop_empty ? ", луп пуст (нет активных промисов)" : "");
                loot_prev_diag = d;
            }
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
                LogLine("viewmodel: local PlayerManager unavailable; cached=0x%llx class=0x%llx rva=0x%llx",
                        (unsigned long long)g_local_player,
                        (unsigned long long)g_game_controller_class,
                        (unsigned long long)GAME_CONTROLLER_TYPEINFO_RVA);
            }
            // A failed read is NOT a confirmed scene change. Keep pending
            // originals so a later successful frame can still restore them.
            auto wait=controller.has_state()?viewmodel::Status::Failed:viewmodel::Status::Waiting;
            controller.status={wait,wait};
            weapons.spread=want_no_spread||weapons.has_state()?viewmodel::Status::Failed:viewmodel::Status::Off;
            rapid_fire.tick(epoch, g_il2cpp_base, 0, want_fast_fire);
            log_fast_fire_transition();
            no_recoil_ctl.tick(epoch, g_il2cpp_base, 0, settings.no_recoil_button);
            log_no_recoil_transition();
            no_ballistics_ctl.tick(epoch, g_il2cpp_base, 0, settings.no_ballistics);
            log_no_ballistics_transition();
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
        log_fast_fire_transition();
        no_recoil_ctl.tick(epoch, g_il2cpp_base, player, settings.no_recoil_button);
        log_no_recoil_transition();
        no_ballistics_ctl.tick(epoch, g_il2cpp_base, player, settings.no_ballistics);
        log_no_ballistics_transition();
    }
    {
        // Log no-spread resolution changes only.
        static int last_stage = -1;
        const int stage = want_no_spread ? weapons.stage() : 0;
        if (stage != last_stage) {
            last_stage = stage;
            if (stage)
                LogLine("weapon no-spread chain failed at stage %d (1-13 resolve, 14-18 validate/write)", stage);
            else
                LogLine("weapon no-spread chain resolved");
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
        LogLine("viewmodel: fov=%d hands=%d; spread=%d (0 off,1 waiting,2 active,3 ADS,4 guard/memory error)",
                (int)st.fov, (int)st.hands, (int)st.no_spread);
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
