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
struct FloatVal { float value; };
inline bool valid(FloatVal v) { return finite(v.value) && v.value >= -10000.f && v.value <= 10000.f; }
struct ByteVal { uint8_t value; };
inline bool valid(ByteVal v) { return v.value <= 1; }
struct Vector2Val { float x, y; };
inline bool valid(Vector2Val v) { return finite(v.x) && finite(v.y); }
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
    struct HandPatch { uint64_t object = 0, klass = 0, slot = 0; Patch<Position> patch; Patch<Position> current; Patch<BlendSpeed> speed; };
    std::array<HandPatch, 8> hands_{};
    Patch<FloatVal> ads_speed_;
    Patch<FloatVal> aim_scale_;
    Patch<ByteVal>  sway_move_en_, sway_rot_en_;
    Patch<Vector2Val> sway_move_mag_, sway_rot_mag_;
    Patch<ByteVal>  bob_walk_en_, bob_aim_en_, bob_run_en_;
    Patch<Vector2Val> bob_walk_amt_, bob_aim_amt_, bob_run_amt_;
    Patch<FloatVal> bob_land_amt_;
    unsigned retry_ = 0, snap_frames_ = 0;

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
            bool positionRestored = h.patch.restore(io);
            bool currentRestored = h.current.written ? h.current.restore(io) : true;
            bool speedRestored = h.speed.restore(io);
            if (positionRestored && currentRestored && speedRestored) h = {}; else ok = false;
        }
        return ok;
    }
    bool restore_fov() {
        if (!player_.alive(io) || !fp_.alive(io)) { fov_ = {}; return true; }
        return fov_.restore(io);
    }
    bool restore_ads() {
        return ads_speed_.restore(io);
    }
    bool restore_sway() {
        bool ok = true;
        if (!aim_scale_.restore(io)) ok = false;
        if (!sway_move_en_.restore(io)) ok = false;
        if (!sway_move_mag_.restore(io)) ok = false;
        if (!sway_rot_en_.restore(io)) ok = false;
        if (!sway_rot_mag_.restore(io)) ok = false;
        if (!bob_walk_en_.restore(io)) ok = false;
        if (!bob_walk_amt_.restore(io)) ok = false;
        if (!bob_aim_en_.restore(io)) ok = false;
        if (!bob_aim_amt_.restore(io)) ok = false;
        if (!bob_run_en_.restore(io)) ok = false;
        if (!bob_run_amt_.restore(io)) ok = false;
        if (!bob_land_amt_.restore(io)) ok = false;
        return ok;
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
        if (restore) { restore_fov(); restore_hands(); restore_sway(); restore_ads(); }
        player_ = {}; fp_ = {}; weapon_ = {}; motion_ = {};
        fov_ = {}; hands_ = {};
        ads_speed_ = {}; aim_scale_ = {};
        sway_move_en_ = {}; sway_move_mag_ = {};
        sway_rot_en_ = {}; sway_rot_mag_ = {};
        bob_walk_en_ = {}; bob_walk_amt_ = {};
        bob_aim_en_ = {}; bob_aim_amt_ = {};
        bob_run_en_ = {}; bob_run_amt_ = {};
        bob_land_amt_ = {};
        retry_ = 0; snap_frames_ = 0; session_ = 0; status = {};
    }
    void tick(uint64_t session, uint64_t player, ViewmodelSettings s) {
        using namespace go_active;
        s = sanitize(s);
        if (session != session_) { reset(false); session_ = session; }
        status = {s.fov_enabled ? Status::Waiting : Status::Off,
                  s.hands_enabled ? Status::Waiting : Status::Off,
                  Status::Off, Status::Off, Status::Off,
                  s.no_sway ? Status::Waiting : Status::Off,
                  s.instant_ads ? Status::Waiting : Status::Off};
        if (!s.fov_enabled && !s.hands_enabled && !s.no_sway && !s.instant_ads && !player_.object) return;
        // No writes through a cached local-player address after scene/respawn.
        if (player != player_.object || !player_.alive(io)) {
            reset(false); session_ = session;
            status = {s.fov_enabled ? Status::Waiting : Status::Off,
                      s.hands_enabled ? Status::Waiting : Status::Off,
                      Status::Off, Status::Off, Status::Off,
                      s.no_sway ? Status::Waiting : Status::Off,
                      s.instant_ads ? Status::Waiting : Status::Off};
            if (!player || !io.is(player, "PlayerManager") || !player_.capture(io, player)) return;
        }
        uint64_t fp = io.ptr(player + PLAYER_FP_MANAGER);
        if (!fp || !io.is(fp, "FPManager")) return;
        if (fp != fp_.object || !fp_.alive(io)) {
            if (!restore_fov() || !restore_hands() || !restore_sway() || !restore_ads()) {
                status = {Status::Failed, Status::Failed, Status::Off, Status::Off, Status::Off,
                          Status::Failed, Status::Failed};
                return;
            }
            weapon_ = {}; motion_ = {}; retry_ = 0;
            if (!fp_.capture(io, fp)) return;
        }
        uint64_t handler = io.ptr(player + PLAYER_EVENT_HANDLER);
        uint64_t activity = handler ? io.ptr(handler + EVENT_HANDLER_AIM_ACTIVITY) : 0;
        uint8_t aim = 0; float blend = 0.f;
        bool aimKnown = handler && io.ptr(handler + EVENT_HANDLER_MANAGER_BACKREF) == player && activity &&
            io.read(activity + ACTIVITY_ACTIVE_FLAG, aim) && aim <= 1 &&
            io.read(fp + FPMANAGER_AIM_BLEND, blend) && finite(blend) && blend >= 0.f && blend <= 1.f;

        // Instant ADS: boost m_AimSpeedMultiplier (0x34) and snap aimBlend (0xA8)
        if (s.instant_ads) {
            bool speedOk = ads_speed_.sample(io, fp + FPMANAGER_AIM_SPEED_MULTIPLIER) &&
                           ads_speed_.put(io, FloatVal{100.f});
            if (aimKnown) {
                if (aim && blend < 1.0f) io.write(fp + FPMANAGER_AIM_BLEND, 1.0f);
                else if (!aim && blend > 0.0f) io.write(fp + FPMANAGER_AIM_BLEND, 0.0f);
            }
            status.instant_ads = speedOk ? Status::Active : Status::Failed;
        } else {
            bool ok = restore_ads();
            status.instant_ads = !ok ? Status::Failed : Status::Off;
        }

        bool paused = !aimKnown || aim || blend > 0.01f;
        if (!s.fov_enabled || paused) {
            bool ok = restore_fov();
            status.fov = !ok ? Status::Failed : (!s.fov_enabled ? Status::Off : (aimKnown ? Status::ADS : Status::Waiting));
        } else {
            bool ok = fov_.sample(io, fp + FPMANAGER_WORLD_FOV_TARGET) && fov_.put(io, s.fov);
            status.fov = ok ? Status::Active : Status::Failed;
        }

        uint64_t weapon = io.ptr(fp + FPMANAGER_CURRENT_OBJECT);
        if (!weapon) weapon = io.ptr(fp + FPMANAGER_CURRENT_WEAPON);
        if (weapon != weapon_.object || !weapon_.alive(io)) {
            if (!restore_hands() || !restore_sway()) {
                status.hands = Status::Failed;
                status.no_sway = Status::Failed;
                return;
            }
            motion_ = {}; retry_ = 0; weapon_ = {};
            if (weapon && io.ptr(weapon + FPOBJECT_PLAYER_BACKREF) == player) weapon_.capture(io, weapon);
        }

        if (aimKnown && aim && s.hands_enabled) snap_frames_ = 3;
        if (!s.hands_enabled) snap_frames_ = 0;
        const bool snap = snap_frames_ != 0 || (aimKnown && blend > 0.01f);
        if (aimKnown && !aim && snap_frames_) --snap_frames_;
        const bool handsPaused = !aimKnown || aim;

        if (!s.hands_enabled || handsPaused) {
            bool ok = restore_hands();
            status.hands = !ok ? Status::Failed : (!s.hands_enabled ? Status::Off : (aimKnown ? Status::ADS : Status::Waiting));
        }

        if (!weapon_.alive(io) || io.ptr(weapon + FPOBJECT_PLAYER_BACKREF) != player) {
            if (s.no_sway) status.no_sway = Status::Waiting;
            return;
        }

        if (!motion_.alive(io) || io.ptr(motion_.object + FPMOTION_OBJECT) != weapon ||
            io.ptr(motion_.object + FPMOTION_PLAYER) != handler) {
            if (!restore_hands() || !restore_sway()) {
                status.hands = Status::Failed;
                status.no_sway = Status::Failed;
                return;
            }
            motion_ = {};
            if (retry_) { --retry_; }
            else {
                retry_ = 30; // bounded search; at most once per 30 overlay frames
                unsigned budget = 64;
                auto event = io.ptr(weapon + FPOBJECT_DRAW_EVENT);
                auto found = motion_from_action(event ? io.ptr(event + FPDRAW_ACTION) : 0, weapon, 0, budget);
                if (found && motion_.capture(io, found)) {
                    retry_ = 0;
                }
            }
        }

        // No Sway: eliminate sway & bobbing on FPMotion
        if (s.no_sway) {
            if (motion_.alive(io)) {
                bool ok = true;
                if (!aim_scale_.sample(io, motion_.object + FPMOTION_AIM_SCALE) ||
                    !aim_scale_.put(io, FloatVal{0.f})) ok = false;
                uint64_t moveSway = io.ptr(motion_.object + FPMOTION_SWAY_MOVEMENT);
                if (moveSway) {
                    sway_move_en_.sample(io, moveSway + SWAY_ENABLED);
                    sway_move_en_.put(io, ByteVal{0});
                    sway_move_mag_.sample(io, moveSway + SWAY_MAGNITUDE);
                    sway_move_mag_.put(io, Vector2Val{0.f, 0.f});
                }
                uint64_t rotSway = io.ptr(motion_.object + FPMOTION_SWAY_ROTATION);
                if (rotSway) {
                    sway_rot_en_.sample(io, rotSway + SWAY_ENABLED);
                    sway_rot_en_.put(io, ByteVal{0});
                    sway_rot_mag_.sample(io, rotSway + SWAY_MAGNITUDE);
                    sway_rot_mag_.put(io, Vector2Val{0.f, 0.f});
                }
                uint64_t walkBob = io.ptr(motion_.object + FPMOTION_WALK_BOB);
                if (walkBob) {
                    bob_walk_en_.sample(io, walkBob + TRIGBOB_ENABLED);
                    bob_walk_en_.put(io, ByteVal{0});
                    bob_walk_amt_.sample(io, walkBob + TRIGBOB_AMOUNT);
                    bob_walk_amt_.put(io, Vector2Val{0.f, 0.f});
                }
                uint64_t aimBob = io.ptr(motion_.object + FPMOTION_AIM_BOB);
                if (aimBob) {
                    bob_aim_en_.sample(io, aimBob + TRIGBOB_ENABLED);
                    bob_aim_en_.put(io, ByteVal{0});
                    bob_aim_amt_.sample(io, aimBob + TRIGBOB_AMOUNT);
                    bob_aim_amt_.put(io, Vector2Val{0.f, 0.f});
                }
                uint64_t runBob = io.ptr(motion_.object + FPMOTION_RUN_BOB);
                if (runBob) {
                    bob_run_en_.sample(io, runBob + TRIGBOB_ENABLED);
                    bob_run_en_.put(io, ByteVal{0});
                    bob_run_amt_.sample(io, runBob + TRIGBOB_AMOUNT);
                    bob_run_amt_.put(io, Vector2Val{0.f, 0.f});
                }
                uint64_t landBob = io.ptr(motion_.object + FPMOTION_LAND_BOB);
                if (landBob) {
                    bob_land_amt_.sample(io, landBob + LERPBOB_AMOUNT);
                    bob_land_amt_.put(io, FloatVal{0.f});
                }
                status.no_sway = ok ? Status::Active : Status::Failed;
            } else {
                status.no_sway = Status::Waiting;
            }
        } else {
            bool ok = restore_sway();
            status.no_sway = !ok ? Status::Failed : Status::Off;
        }

        // Custom hands poses (if enabled and not paused)
        if (s.hands_enabled && !handsPaused && motion_.alive(io)) {
            const uint64_t offsets[] = {FPMOTION_IDLE_OFFSET, FPMOTION_RUN_OFFSET,
                FPMOTION_RUN_RELOAD_OFFSET, FPMOTION_ATTACK_OFFSET, FPMOTION_LADDER_OFFSET,
                FPMOTION_JUMP_OFFSET, FPMOTION_LOOK_OFFSET, FPMOTION_CLOSE_OFFSET};
            bool any = false, ok = true;
            for (size_t i = 0; i < hands_.size(); ++i) {
                auto& h = hands_[i];
                uint64_t slot = motion_.object + offsets[i], object = io.ptr(slot);
                if (h.object != object) {
                    h = {};
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
                if (!h.patch.sample(io, object + TRANSFORMOFFSET_POSITION)) { ok = false; continue; }
                h.current.sample(io, object + TRANSFORMOFFSET_CURRENT_POSITION);
                Position value = h.patch.original;
                value.x += s.x * 0.01f; value.y += s.y * 0.01f; value.z += s.z * 0.01f;
                bool positionApplied = h.patch.put(io, value);
                if (h.current.address) h.current.put(io, value);
                bool speedApplied = snap
                    ? (h.speed.sample(io, object + TRANSFORMOFFSET_LERP_SPEED) &&
                       h.speed.put(io, BlendSpeed{10000.f}))
                    : h.speed.restore(io);
                if (!positionApplied || !speedApplied) ok = false; else any = true;
            }
            status.hands = !ok ? Status::Failed : (any ? Status::Active : Status::Waiting);
        }

        if (!s.fov_enabled && !s.hands_enabled && !s.no_sway && !s.instant_ads &&
            status.fov == Status::Off && status.hands == Status::Off &&
            status.no_sway == Status::Off && status.instant_ads == Status::Off) {
            reset(false); session_ = session;
        }
    }
};
} // namespace viewmodel
