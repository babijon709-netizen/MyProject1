#include "viewmodel_memory.h"
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
    ++epoch;
}

void esp_viewmodel_tick(const ViewmodelSettings& settings) {
    using namespace go_active;
    std::lock_guard<std::recursive_mutex> guard(viewmodel_session_mutex());
    if (!g_mem.bound() || !g_il2cpp_base || g_pid <= 0) {
        controller.reset(false);
        controller.status = {settings.fov_enabled ? viewmodel::Status::Waiting : viewmodel::Status::Off,
                             settings.hands_enabled ? viewmodel::Status::Waiting : viewmodel::Status::Off};
        return;
    }
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
    return controller.status;
}
