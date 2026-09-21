#include "fps_memory.h"
#include "fast_loot_memory.h"
#include "weapon_tuning_memory.h"
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
fast_loot::Controller<Memory> loot_controller(memory, fast_loot::instruction_supported());
weapon_tuning::Controller<Memory> weapon_controller(memory, weapon_tuning::instruction_supported());
uint64_t unity_base = 0;
using Clock = std::chrono::steady_clock;
Clock::time_point fps_last_tick{}, fps_last_resolve{}, loot_last_tick{};
Clock::time_point weapon_last_tick{};
bool weapon_last_recoil = false, weapon_last_spread = false;
weapon_tuning::Status weapon_last_log{};
bool loot_last_enabled = false;
viewmodel::Status loot_last_log = viewmodel::Status::Off;
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
void tick_fast_loot(uint64_t session, bool enabled) {
    const auto now = Clock::now();
    const bool changed = enabled != loot_last_enabled;
    if (!changed && now - loot_last_tick < std::chrono::milliseconds(250)) return;
    loot_last_tick = now; loot_last_enabled = enabled;
    loot_controller.tick(session, g_il2cpp_base, enabled);
    if (loot_last_log != loot_controller.status || changed) {
        loot_last_log = loot_controller.status;
        if (enabled && !fast_loot::instruction_supported())
            LogLine("fast-loot: instruction requires ARM64");
        LogLine("fast-loot: status=%d (0 off,1 waiting,2 active,4 unsupported CPU/image/code or memory error); collect mode=batch (inter-item await bypass; server validates transfers)",
                (int)loot_last_log);
    }
}
void tick_weapons(uint64_t session, bool recoil, bool spread) {
    const auto now = Clock::now();
    const bool changed = recoil != weapon_last_recoil || spread != weapon_last_spread;
    if (!changed && now - weapon_last_tick < std::chrono::milliseconds(250)) return;
    weapon_last_tick = now; weapon_last_recoil = recoil; weapon_last_spread = spread;
    weapon_controller.tick(session, g_il2cpp_base, recoil, spread);
    const auto st = weapon_controller.status;
    if (changed || st.recoil != weapon_last_log.recoil || st.spread != weapon_last_log.spread) {
        weapon_last_log = st;
        LogLine("weapons: no-recoil=%d no-spread=%d (0 off,1 waiting,2 active,4 unsupported CPU/image/code or memory error); client FPHitscan only",
                (int)st.recoil, (int)st.spread);
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
    if (!loot_controller.reset(restore && g_mem.bound()))
        LogLine("fast-loot: unable to restore original collect delay before session reset");
    loot_last_tick = {}; loot_last_enabled = false;
    if (!weapon_controller.reset(restore && g_mem.bound()))
        LogLine("weapons: unable to restore all original instructions before session reset");
    weapon_last_tick = {}; weapon_last_recoil = weapon_last_spread = false;
    unity_base = 0; fps_last_tick = {}; fps_last_resolve = {}; fps_last_limit = 0;
    ++epoch;
}

void esp_viewmodel_tick(const ViewmodelSettings& settings) {
    using namespace go_active;
    std::lock_guard<std::recursive_mutex> guard(viewmodel_session_mutex());
    if (!g_mem.bound() || !g_il2cpp_base || g_pid <= 0) {
        controller.reset(false); fps_controller.reset(false); loot_controller.reset(false);
        loot_last_tick = {}; loot_last_enabled = false;
        weapon_controller.reset(false);
        weapon_last_tick = {}; weapon_last_recoil = weapon_last_spread = false;
        weapon_controller.status = {settings.no_recoil ? viewmodel::Status::Waiting : viewmodel::Status::Off,
                                    settings.no_spread ? viewmodel::Status::Waiting : viewmodel::Status::Off};
        loot_controller.status = settings.fast_loot ? viewmodel::Status::Waiting : viewmodel::Status::Off;
        unity_base = 0; fps_last_resolve = {};
        controller.status = {settings.fov_enabled ? viewmodel::Status::Waiting : viewmodel::Status::Off,
                             settings.hands_enabled ? viewmodel::Status::Waiting : viewmodel::Status::Off};
        return;
    }
    tick_fps(epoch, settings.fps_limit);
    tick_fast_loot(epoch, settings.fast_loot);
    tick_weapons(epoch, settings.no_recoil, settings.no_spread);
    // Resolve the current local player from the static source, NOT the ESP
    // cache which intentionally tolerates stale identities during respawn.
    uint64_t player = 0;
    if (settings.fov_enabled || settings.hands_enabled || controller.has_state()) {
        uint64_t klass = memory.ptr(g_il2cpp_base + GAME_CONTROLLER_TYPEINFO_RVA);
        uint64_t sf = memory.klass_is(klass, "GameControllerBase") ? get_class_static_fields(klass) : 0;
        uint64_t raw = 0;
        if (!sf || !memory.read(sf + GAME_CONTROLLER_LOCAL_PLAYER_FIELD, raw)) {
            // A failed read is NOT a confirmed scene change. Keep the originals
            // so a later successful frame can restore a disabled feature.
            auto wait = controller.has_state() ? viewmodel::Status::Failed : viewmodel::Status::Waiting;
            controller.status = {wait, wait};
            return;
        }
        player = memio::untag(raw); // successful null read means no local player
    }
    controller.tick(epoch, player, settings);
    auto st = controller.status;
    if (st.fov != last_log.fov || st.hands != last_log.hands) {
        LogLine("viewmodel: fov=%d hands=%d (0 off,1 waiting,2 active,3 ADS,4 memory error)",
                (int)st.fov, (int)st.hands);
        last_log = st;
    }
}

viewmodel::Diagnostics esp_viewmodel_diagnostics() {
    std::lock_guard<std::recursive_mutex> guard(viewmodel_session_mutex());
    auto status = controller.status;
    status.fast_loot = loot_controller.status;
    status.no_recoil = weapon_controller.status.recoil;
    status.no_spread = weapon_controller.status.spread;
    return status;
}
