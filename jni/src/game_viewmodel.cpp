#include "fps_memory.h"
#include "weapon_tuning_memory.h"
#include "loot_memory.h"
#include "object_data_memory.h"
#include "maps_lookup.h"
#include <chrono>
#include <fstream>
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
        if (!np || !sp || !g_mem.read_quiet(np, n, sizeof(n)) || !g_mem.read_quiet(sp, ns, sizeof(ns))) return false;
        n[63] = ns[63] = 0;
        return std::strcmp(n, name) == 0 && std::strcmp(ns, "Oxide") == 0;
    }
    bool is(uint64_t object, const char* name) { return object && klass_is(ptr(object), name); }
};
Memory memory;
viewmodel::Controller<Memory> controller(memory);
viewmodel::FpsController<Memory> fps_controller(memory);
// Deliberately do not inherit Memory::write: these controllers can only write
// through the fresh anonymous-heap mapping gate.
struct HeapMemory {
    template<class T> bool read(uint64_t address, T& value) { return memory.read(address,value); }
    bool named(uint64_t object, const char* name, const char* namespaze) {
        if (object < 0x10000) return false;
        uint64_t k=memory.ptr(object);
        if (k < 0x10000) return false;
        uint64_t np=memory.ptr(k+0x10), sp=memory.ptr(k+0x18);
        char n[96]{}, ns[96]{};
        const size_t nl=std::strlen(name)+1, sl=std::strlen(namespaze)+1;
        return nl<=sizeof(n) && sl<=sizeof(ns) && np && sp &&
            g_mem.read_quiet(np,n,nl) && g_mem.read_quiet(sp,ns,sl) &&
            std::memcmp(n,name,nl)==0 && std::memcmp(ns,namespaze,sl)==0;
    }
    template<class T> bool write_heap(uint64_t address, const T& value) {
        if (!g_mem.bound() || g_pid<=0) return false;
        std::ifstream file("/proc/"+std::to_string(g_pid)+"/maps");
        if (!file) return false;
        std::string text, line;
        while (std::getline(file,line)) {
            text += line; text += '\n';
            if (text.size() > (8u<<20)) return false;
        }
        return file.eof() && object_data::heap_writable(text,address,sizeof(T)) &&
            g_mem.write(address,&value,sizeof(T));
    }
};
HeapMemory heap_memory;
weapon_tuning::Controller<HeapMemory> weapons(heap_memory);
fast_loot::Controller<HeapMemory> loot(heap_memory);
uint64_t unity_base = 0;
using Clock = std::chrono::steady_clock;
Clock::time_point fps_last_tick{}, fps_last_resolve{};
int fps_last_limit = 0;
viewmodel::Status fps_last_log = viewmodel::Status::Off;

uint64_t find_unity() {
    std::ifstream file("/proc/" + std::to_string(g_pid) + "/maps");
    std::string text, line;
    while (std::getline(file, line)) {
        text += line; text += '\n';
        if (text.size() > (8u << 20)) return 0;
    }
    uint64_t candidates[8]{}, found = 0;
    int count = maps::lookup_library_bases(text, "libunity.so", candidates, 8);
    for (int i = 0; i < count; ++i) {
        if (!viewmodel::fps_layout(memory, candidates[i])) continue;
        if (found) return 0; // ambiguous live/stale mappings: do not guess
        found = candidates[i];
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
    // Быстрому сбору восстанавливать нечего: поле темпа переписывает сама
    // игра при каждом новом ожидании, поэтому сброс — это только «забыть адреса».
    loot.reset();
    unity_base = 0; fps_last_tick = {}; fps_last_resolve = {}; fps_last_limit = 0;
    ++epoch;
}

void esp_viewmodel_tick(const ViewmodelSettings& settings) {
    using namespace go_active;
    std::lock_guard<std::recursive_mutex> guard(viewmodel_session_mutex());
    if (!g_mem.bound() || !g_il2cpp_base || g_pid <= 0) {
        controller.reset(false); fps_controller.reset(false); weapons.reset(false); loot.reset();
        weapons.recoil = settings.no_recoil ? viewmodel::Status::Waiting : viewmodel::Status::Off;
        weapons.spread = settings.no_spread ? viewmodel::Status::Waiting : viewmodel::Status::Off;
        loot.status = settings.fast_loot ? viewmodel::Status::Waiting : viewmodel::Status::Off;
        unity_base = 0; fps_last_resolve = {};
        controller.status = {settings.fov_enabled ? viewmodel::Status::Waiting : viewmodel::Status::Off,
                             settings.hands_enabled ? viewmodel::Status::Waiting : viewmodel::Status::Off};
        return;
    }
    tick_fps(epoch, settings.fps_limit);
    // Быстрый сбор не зависит от локального игрока — тикается всегда,
    // до ветки с резолвом игрока (она ниже может выйти раньше времени).
    loot.tick(epoch, g_il2cpp_base, settings.fast_loot);
    // Resolve the current local player from the static source, NOT the ESP
    // cache which intentionally tolerates stale identities during respawn.
    uint64_t player = 0;
    if (settings.fov_enabled || settings.hands_enabled || controller.has_state() ||
        settings.no_recoil || settings.no_spread || weapons.has_state()) {
        uint64_t klass = memory.ptr(g_il2cpp_base + GAME_CONTROLLER_TYPEINFO_RVA);
        uint64_t sf = memory.klass_is(klass, "GameControllerBase") ? get_class_static_fields(klass) : 0;
        uint64_t raw = 0;
        if (!sf || !memory.read(sf + GAME_CONTROLLER_LOCAL_PLAYER_FIELD, raw)) {
            // A failed read is NOT a confirmed scene change. Keep the originals
            // so a later successful frame can restore a disabled feature.
            auto wait = controller.has_state() ? viewmodel::Status::Failed : viewmodel::Status::Waiting;
            controller.status = {wait, wait};
            weapons.recoil = settings.no_recoil || weapons.has_state() ? viewmodel::Status::Failed : viewmodel::Status::Off;
            weapons.spread = settings.no_spread || weapons.has_state() ? viewmodel::Status::Failed : viewmodel::Status::Off;
            return;
        }
        player = memio::untag(raw); // successful null read means no local player
    }
    weapons.tick(epoch, g_il2cpp_base, player, settings.no_recoil, settings.no_spread);
    controller.tick(epoch, player, settings);
    auto st = esp_viewmodel_diagnostics();
    if (st.fov != last_log.fov || st.hands != last_log.hands ||
        st.no_recoil != last_log.no_recoil || st.no_spread != last_log.no_spread ||
        st.fast_loot != last_log.fast_loot) {
        LogLine("viewmodel: fov=%d hands=%d; object-data recoil=%d spread=%d; instant loot=%d (0 off,1 waiting,2 active,4 unsupported/modified image or memory error)",
                (int)st.fov, (int)st.hands, (int)st.no_recoil, (int)st.no_spread, (int)st.fast_loot);
        last_log = st;
    }
}
viewmodel::Diagnostics esp_viewmodel_diagnostics() {
    std::lock_guard<std::recursive_mutex> guard(viewmodel_session_mutex());
    auto status = controller.status;
    status.no_recoil = weapons.recoil;
    status.no_spread = weapons.spread;
    status.fast_loot = loot.status;
    return status;
}

viewmodel::FpsUiInfo esp_viewmodel_fps_ui() {
    std::lock_guard<std::recursive_mutex> guard(viewmodel_session_mutex());
    viewmodel::FpsUiInfo info;
    info.requested = fps_last_limit;
    info.status = fps_controller.status;
    info.field_value = fps_controller.field_seen();
    info.pace_kind = fps_controller.pace_seen();
    return info;
}
