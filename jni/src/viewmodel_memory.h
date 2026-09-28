#pragma once
// Testable memory controller. IO must provide fresh read<T>, write<T>, ptr,
// and is(object, className) (strict Oxide namespace check).
#include "viewmodel.h"
#include "game_offsets_active.h"
#include <array>

namespace viewmodel {
struct Position { float x, y, z; };
struct BlendSpeed { float value; };
inline bool valid(BlendSpeed v) { return finite(v.value) && v.value >= 0.f && v.value <= 10000.f; }
inline bool valid(float v) { return finite(v) && v > 1.f && v < 179.f; }
inline bool valid(Position v) {
    return finite(v.x) && finite(v.y) && finite(v.z) &&
           v.x > -10.f && v.x < 10.f && v.y > -10.f && v.y < 10.f && v.z > -10.f && v.z < 10.f;
}
template<class T> bool same(const T& a, const T& b) { return std::memcmp(&a, &b, sizeof(T)) == 0; }

struct Identity {
    uint64_t object = 0, klass = 0, native = 0;
    template<class IO> bool capture(IO& io, uint64_t obj) {
        object = obj; klass = io.ptr(obj);
        native = io.ptr(obj + go_active::MANAGED_CACHED_PTR);
        return object && klass && native;
    }
    template<class IO> bool alive(IO& io) const {
        return object && klass && native && io.ptr(object) == klass &&
            io.ptr(object + go_active::MANAGED_CACHED_PTR) == native;
    }
};

template<class T> struct Patch {
    uint64_t address = 0;
    T original{}, last{};
    bool written = false;
    template<class IO> bool sample(IO& io, uint64_t addr) {
        T current{};
        if (!io.read(addr, current) || !valid(current)) return false;
        if (!written || address != addr || !same(current, last)) original = current;
        address = addr;
        return true;
    }
    template<class IO> bool put(IO& io, T value) {
        if (!valid(value) || !io.write(address, value)) return false;
        last = value; written = true; return true;
    }
    template<class IO> bool restore(IO& io) {
        if (!written) { *this = {}; return true; }
        T current{};
        if (!io.read(address, current)) return false;
        // The game owns this field too. Do not restore an obsolete value over
        // an ADS/weapon-state update which has already replaced our last write.
        if (same(current, last) && !io.write(address, original)) return false;
        *this = {}; return true;
    }
};

template<class IO> class Controller {
    IO& io;
    uint64_t session_ = 0;
    Identity player_, fp_, weapon_, motion_;
    Patch<float> fov_;
    struct HandPatch {
        uint64_t object = 0, klass = 0, slot = 0;
        Patch<Position> target;
        Patch<Position> current;
        Patch<BlendSpeed> speed;
    };
    std::array<HandPatch, 9> hands_{};
    unsigned retry_ = 0;

    bool restore_hands() {
        using namespace go_active;
        if (!player_.alive(io) || !weapon_.alive(io) || !motion_.alive(io) ||
            io.ptr(weapon_.object + FPOBJECT_PLAYER_BACKREF) != player_.object ||
            io.ptr(motion_.object + FPMOTION_OBJECT) != weapon_.object) {
            hands_ = {}; return true;
        }
        bool ok = true;
        for (auto& h : hands_) {
            if (!h.object) continue;
            if (io.ptr(h.slot) != h.object || io.ptr(h.object) != h.klass) { h = {}; continue; }
            bool targetRestored = h.target.restore(io);
            bool currentRestored = h.current.restore(io);
            bool speedRestored = h.speed.restore(io);
            if (targetRestored && currentRestored && speedRestored) h = {};
            else ok = false;
        }
        return ok;
    }
    bool restore_fov() {
        if (!player_.alive(io) || !fp_.alive(io)) { fov_ = {}; return true; }
        return fov_.restore(io);
    }
    uint64_t motion_from_action(uint64_t action, uint64_t weapon, unsigned depth, unsigned& budget) {
        using namespace go_active;
        if (!action || depth > 2 || !budget) return 0;
        --budget;
        uint64_t target = io.ptr(action + DELEGATE_TARGET);
        if (target && io.is(target, "FPMotion") &&
            io.ptr(target + FPMOTION_OBJECT) == weapon &&
            io.ptr(target + FPMOTION_PLAYER) == io.ptr(player_.object + PLAYER_EVENT_HANDLER) &&
            io.ptr(target + MANAGED_CACHED_PTR)) return target;
        uint64_t list = io.ptr(action + MULTICAST_DELEGATES), count = 0;
        if (!list || !io.read(list + IL2CPP_ARRAY_LENGTH, count) || count > 32) return 0;
        for (uint64_t i = 0; i < count && budget; ++i) {
            auto found = motion_from_action(io.ptr(list + IL2CPP_ARRAY_FIRST_ELEMENT + i * 8), weapon, depth + 1, budget);
            if (found) return found;
        }
        return 0;
    }
public:
    Diagnostics status{};
    explicit Controller(IO& memory) : io(memory) {}
    bool has_state() const { return player_.object != 0; }
    void reset(bool restore) {
        if (restore) { restore_fov(); restore_hands(); }
        player_ = {}; fp_ = {}; weapon_ = {}; motion_ = {};
        fov_ = {}; hands_ = {}; retry_ = 0; session_ = 0; status = {};
    }
    void tick(uint64_t session, uint64_t player, ViewmodelSettings s) {
        using namespace go_active;
        s = sanitize(s);
        if (session != session_) { reset(false); session_ = session; }
        status = {s.fov_enabled ? Status::Waiting : Status::Off, s.hands_enabled ? Status::Waiting : Status::Off};
        if (!s.fov_enabled && !s.hands_enabled && !player_.object) return;
        // No writes through a cached local-player address after scene/respawn.
        if (player != player_.object || !player_.alive(io)) {
            reset(false); session_ = session;
            status = {s.fov_enabled ? Status::Waiting : Status::Off, s.hands_enabled ? Status::Waiting : Status::Off};
            if (!player || !io.is(player, "PlayerManager") || !player_.capture(io, player)) return;
        }
        uint64_t fp = io.ptr(player + PLAYER_FP_MANAGER);
        if (!fp || !io.is(fp, "FPManager")) return;
        if (fp != fp_.object || !fp_.alive(io)) {
            if (!restore_fov() || !restore_hands()) { status = {Status::Failed, Status::Failed}; return; }
            weapon_ = {}; motion_ = {}; retry_ = 0;
            if (!fp_.capture(io, fp)) return;
        }
        uint64_t handler = io.ptr(player + PLAYER_EVENT_HANDLER);
        const bool handlerKnown = handler &&
            io.ptr(handler + EVENT_HANDLER_MANAGER_BACKREF) == player;
        // Custom FOV is authoritative in every state, including ADS. The old
        // controller restored the game's ADS value here, which looked like the
        // setting randomly switched off whenever the player aimed.
        if (!s.fov_enabled) {
            const bool ok=restore_fov();
            status.fov=ok?Status::Off:Status::Failed;
        } else {
            const bool ok=fov_.sample(io,fp+FPMANAGER_WORLD_FOV_TARGET) &&
                          fov_.put(io,s.fov);
            status.fov=ok?Status::Active:Status::Failed;
        }
        uint64_t weapon = io.ptr(fp + FPMANAGER_CURRENT_OBJECT);
        if (!weapon) weapon = io.ptr(fp + FPMANAGER_CURRENT_WEAPON);
        if (weapon != weapon_.object || !weapon_.alive(io)) {
            if (!restore_hands()) { status.hands = Status::Failed; return; }
            motion_ = {}; retry_ = 0; weapon_ = {};
            if (weapon && io.ptr(weapon + FPOBJECT_PLAYER_BACKREF) == player) weapon_.capture(io, weapon);
        }
        // Custom hands no longer restore in ADS. Disabled is the only normal
        // state that restores originals; otherwise every motion slot receives
        // the same static position below.
        if (!s.hands_enabled) {
            const bool ok=restore_hands();
            status.hands=ok?Status::Off:Status::Failed;
            if (!s.fov_enabled && status.fov==Status::Off && status.hands==Status::Off) {
                reset(false); session_=session;
            }
            return;
        }
        if (!handlerKnown || !weapon_.alive(io) ||
            io.ptr(weapon+FPOBJECT_PLAYER_BACKREF)!=player) return;
        if (!motion_.alive(io) || io.ptr(motion_.object + FPMOTION_OBJECT) != weapon ||
            io.ptr(motion_.object + FPMOTION_PLAYER) != handler) {
            if (!restore_hands()) { status.hands = Status::Failed; return; }
            motion_ = {};
            if (retry_) { --retry_; return; }
            retry_ = 30; // bounded search; at most once per 30 overlay frames
            unsigned budget = 64;
            auto event = io.ptr(weapon + FPOBJECT_DRAW_EVENT);
            auto found = motion_from_action(event ? io.ptr(event + FPDRAW_ACTION) : 0, weapon, 0, budget);
            if (!found || !motion_.capture(io, found)) return;
            retry_ = 0;
        }
        const uint64_t offsets[] = {FPMOTION_IDLE_OFFSET, FPMOTION_RUN_OFFSET,
            FPMOTION_RUN_RELOAD_OFFSET, FPMOTION_ATTACK_OFFSET, FPMOTION_AIM_OFFSET,
            FPMOTION_LADDER_OFFSET, FPMOTION_JUMP_OFFSET, FPMOTION_LOOK_OFFSET,
            FPMOTION_CLOSE_OFFSET};
        bool any = false, ok = true;
        for (size_t i = 0; i < hands_.size(); ++i) {
            auto& h = hands_[i];
            uint64_t slot = motion_.object + offsets[i], object = io.ptr(slot);
            if (h.object != object) {
                // An unlinked offset may have been freed: never restore via it.
                h = {};
                // A prefab may start sharing an offset between poses. Transfer
                // its existing snapshot instead of sampling our own shifted value.
                for (size_t j = i + 1; object && j < hands_.size(); ++j) {
                    if (hands_[j].object == object && io.ptr(hands_[j].slot) == object) {
                        h = hands_[j]; hands_[j] = {}; break;
                    }
                }
            }
            if (!object) continue;
            bool duplicate = false;
            for (size_t j = 0; j < i; ++j) if (hands_[j].object == object) duplicate = true;
            if (duplicate) continue;
            if (!io.is(object, "TransformOffset")) { ok = false; continue; }
            const uint64_t klass = io.ptr(object);
            if (h.klass && h.klass != klass) h = {};
            h.object = object; h.klass = klass; h.slot = slot;
            if (!h.target.sample(io, object + TRANSFORMOFFSET_POSITION) ||
                !h.current.sample(io, object + TRANSFORMOFFSET_CURRENT_POSITION)) {
                ok = false;
                continue;
            }
            // FPMotion selects one of these nine targets, then TransformOffset
            // interpolates m_Position into its separate current-position cache
            // at +0x44.  Some idle poses have a zero/very low lerp speed, which
            // is why changing only m_Position appeared to work only while
            // running.  Hold both the target and current cache at the requested
            // absolute value; state transitions are now static immediately.
            const Position value{s.x*0.01f,s.y*0.01f,s.z*0.01f};
            const bool targetApplied = h.target.put(io, value);
            const bool currentApplied = h.current.put(io, value);
            // Retired ADS snap helper may still own a speed from an older tick;
            // restore it once, then leave animation speed to the game.
            const bool speedApplied = h.speed.restore(io);
            if (!targetApplied || !currentApplied || !speedApplied) ok=false;
            else any=true;
        }
        status.hands = !ok ? Status::Failed : (any ? Status::Active : Status::Waiting);
    }
};
} // namespace viewmodel
