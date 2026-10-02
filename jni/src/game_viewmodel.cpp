#include "fps_memory.h"
#include "weapon_tuning_memory.h"
#include "weapon_speed_memory.h"
#include "loot_memory.h"
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
    const std::string* text = maps_snapshot(false);
    if (!text || !object_data::heap_writable_range(*text, address, size, lo, hi)) {
        // Снимок мог устареть (отображение появилось только что) — решает
        // всегда свежая карта, а не кэш.
        text = maps_snapshot(true);
        if (!text || !object_data::heap_writable_range(*text, address, size, lo, hi)) {
            // Отказ ворот — самая непрозрачная из причин «функция не
            // применилась»: сама запись не делается, а наружу видно только
            // «не применено». Пишем адрес, чтобы его можно было сверить с
            // картой процесса.
            static double refused_at = -1e9;
            const double moment = memio::now_seconds();
            if (moment - refused_at >= 5.0) {
                refused_at = moment;
                LogLine("запись в кучу отклонена воротами: адрес 0x%llx, %zu байт "
                        "(карта %zu КБ) — отображение не анонимное rw-p",
                        (unsigned long long)address, size,
                        text ? text->size() / 1024 : (size_t)0);
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
    bool named(uint64_t object, const char* name, const char* namespaze) {
        return object>=0x10000 && named_klass(memory.ptr(object),name,namespaze);
    }
    // Calculator names rotate between builds (Ze/FJ/...). Its empty namespace
    // and both typed backlinks are checked by weapon_tuning before any write.
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
weapon_speed::Controller<HeapMemory> weapon_speeds(heap_memory);
fast_loot::Controller<HeapMemory> loot(heap_memory);
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
    if (!found && count > 0) {
        uint64_t id = 0;
        if (memory.read(candidates[0] + 0x318, id) && id)
            fps_unknown_build_id = id;
        if (fps_unknown_build_id)
            LogLine("fps: libunity не опознан, build id %016llx (игра обновилась?)",
                    (unsigned long long)fps_unknown_build_id);
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
    if (!unity_base && enabled && now - fps_last_resolve >= std::chrono::seconds(1)) {
        fps_last_resolve = now; unity_base = find_unity();
    }
    fps_controller.tick(session, unity_base, limit);
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
    if (!weapon_speeds.reset(restore && g_mem.bound()))
        LogLine("weapon speed: originals could not all be restored before disconnect; restart game");
    loot.reset();
    unity_base = 0; fps_last_tick = {}; fps_last_resolve = {}; fps_last_limit = 0;
    weapon_last_tick = {}; weapon_last_player = 0; weapon_last_flags = ~0u;
    invalidate_maps_snapshot();
    ++epoch;
}

void esp_viewmodel_tick(const ViewmodelSettings& settings) {
    using namespace go_active;
    std::lock_guard<std::recursive_mutex> guard(viewmodel_session_mutex());
    if (!g_mem.bound() || !g_il2cpp_base || g_pid <= 0) {
        controller.reset(false); fps_controller.reset(false); weapons.reset(false);
        weapon_speeds.reset(false); loot.reset();
        const bool want_spread = settings.no_spread || esp_silent_wants_no_spread();
        weapons.recoil = settings.no_recoil ? viewmodel::Status::Waiting : viewmodel::Status::Off;
        weapons.spread = want_spread ? viewmodel::Status::Waiting : viewmodel::Status::Off;
        loot.status = settings.fast_loot ? viewmodel::Status::Waiting : viewmodel::Status::Off;
        weapon_speeds.fire = g_state.fast_fire ? viewmodel::Status::Waiting : viewmodel::Status::Off;
        weapon_speeds.reload = g_state.fast_reload ? viewmodel::Status::Waiting : viewmodel::Status::Off;
        unity_base = 0; fps_last_resolve = {};
        controller.status = {settings.fov_enabled ? viewmodel::Status::Waiting : viewmodel::Status::Off,
                             settings.hands_enabled ? viewmodel::Status::Waiting : viewmodel::Status::Off};
        return;
    }
    tick_fps(epoch, settings.fps_limit);
    loot.tick(epoch, g_il2cpp_base, settings.fast_loot);
    const bool want_no_spread = settings.no_spread || esp_silent_wants_no_spread();
    // Use the local identity already validated by the world reader first. The
    // fixed TypeInfo RVA can lag behind a game update even when every managed
    // field offset is unchanged; that used to disable all four weapon toggles.
    uint64_t player = 0;
    static bool local_source_failed = false;
    if (settings.fov_enabled || settings.hands_enabled || controller.has_state() ||
        settings.no_recoil || want_no_spread || weapons.has_state() ||
        g_state.fast_fire || g_state.fast_reload || weapon_speeds.has_state() ||
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
            weapons.recoil=settings.no_recoil||weapons.has_state()?viewmodel::Status::Failed:viewmodel::Status::Off;
            weapons.spread=want_no_spread||weapons.has_state()?viewmodel::Status::Failed:viewmodel::Status::Off;
            weapon_speeds.fire=g_state.fast_fire||weapon_speeds.has_state()?viewmodel::Status::Failed:viewmodel::Status::Off;
            weapon_speeds.reload=g_state.fast_reload||weapon_speeds.has_state()?viewmodel::Status::Failed:viewmodel::Status::Off;
            return;
        }
        local_source_failed=false;
    }
    // Weapon chains are expensive (class names, backlinks and readbacks for
    // every scalar).  They only need maintenance at game-tick cadence, not at
    // the uncapped overlay rate.  Setting/player changes bypass the limiter so
    // toggles and respawns still apply immediately.
    const unsigned weapon_flags = (settings.no_recoil ? 1u : 0u) |
                                  (want_no_spread ? 2u : 0u) |
                                  (g_state.fast_fire ? 4u : 0u) |
                                  (g_state.fast_reload ? 8u : 0u);
    const auto weapon_now = Clock::now();
    const bool weapon_due = weapon_flags != weapon_last_flags ||
                            player != weapon_last_player ||
                            weapon_now - weapon_last_tick >= std::chrono::milliseconds(33);
    if (weapon_due) {
        weapon_last_tick = weapon_now;
        weapon_last_player = player;
        weapon_last_flags = weapon_flags;
        weapons.tick(epoch, g_il2cpp_base, player, settings.no_recoil, want_no_spread);
        weapon_speeds.tick(epoch, g_il2cpp_base, player,
                           g_state.fast_fire, g_state.fast_reload);
    }
    {
        // Ступень, на которой оборвалась цепочка отдачи/разброса. Логируем
        // только смену ступени, иначе строка шла бы каждый кадр.
        static int last_stage = -1;
        const int stage = (settings.no_recoil || want_no_spread) ? weapons.stage() : 0;
        if (stage != last_stage) {
            last_stage = stage;
            if (stage)
                LogLine("оружие: отдача/разброс не применились, ступень %d "
                        "(1-5 игрок/FPManager, 6-8 оружие и его конфиг, 9-11 RecoilConfig, "
                        "12-14 калькулятор, 15-16 DispersionConfig, 17-19 значение, "
                        "20 ворота записи, 21 запись не легла)", stage);
            else
                LogLine("оружие: цепочка отдачи/разброса разрешилась полностью");
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
        st.no_recoil != last_log.no_recoil || st.no_spread != last_log.no_spread ||
        st.fast_loot != last_log.fast_loot || st.fast_fire != last_log.fast_fire ||
        st.fast_reload != last_log.fast_reload) {
        LogLine("viewmodel: fov=%d hands=%d; recoil=%d spread=%d loot=%d fire=%d reload=%d (0 off,1 waiting,2 active,3 ADS,4 guard/memory error)",
                (int)st.fov, (int)st.hands, (int)st.no_recoil, (int)st.no_spread,
                (int)st.fast_loot, (int)st.fast_fire, (int)st.fast_reload);
        last_log = st;
    }
}

viewmodel::Diagnostics esp_viewmodel_diagnostics() {
    std::lock_guard<std::recursive_mutex> guard(viewmodel_session_mutex());
    auto status = controller.status;
    status.no_recoil = weapons.recoil;
    status.no_spread = weapons.spread;
    status.fast_loot = loot.status;
    status.fast_fire = weapon_speeds.fire;
    status.fast_reload = weapon_speeds.reload;
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
