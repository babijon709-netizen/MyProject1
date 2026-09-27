#pragma once
// Object-data implementation for Autofarm memory features:
// - Always hits weakspots/crosshairs (FPTool.alwaysCrit = 1)
// - No camera/view angle restriction & 360-degree reach (FPMelee.m_MaxReach = 20.0f, hitRadius = 15.0f, RaycastManager rays/sphere)
// - No durability loss / tool unbreakable (ItemConfigValues.durabilityUnbreakable = 1, restore item condition)
// - No tool swing animation & no motion offset (FPMotion.m_AttackOffset pos/rot/BYE = 0, FPMeleeAnimator.attackClipLength = 0, FPMelee.m_ImpactForce = 0)
// - Clean restoration on toggle off, tool switch, respawn, disconnect
// - 100% dynamic offset resolution via go_active::* (compatible with offset_resolver)
// - NO library binary patching

#include "viewmodel.h"
#include "game_offsets_active.h"
#include <array>
#include <cstring>
#include <cmath>

namespace farm_tuning {

struct Position3 { float x, y, z; };
inline bool valid(Position3 v) {
    return viewmodel::finite(v.x) && viewmodel::finite(v.y) && viewmodel::finite(v.z) &&
           v.x > -100.f && v.x < 100.f && v.y > -100.f && v.y < 100.f && v.z > -100.f && v.z < 100.f;
}
struct FloatVal { float value; };
inline bool valid(FloatVal v) { return viewmodel::finite(v.value) && v.value >= -10000.f && v.value <= 10000.f; }
struct ByteVal { uint8_t value; };
inline bool valid(ByteVal v) { return v.value <= 1; }

template<class T> bool same(const T& a, const T& b) { return std::memcmp(&a, &b, sizeof(T)) == 0; }

template<class IO> bool pointer(IO& io, uint64_t a, uint64_t& value) {
    if (!io.read(a, value)) return false;
    value &= 0x00ffffffffffffffULL;
    return true;
}

struct Identity {
    uint64_t object = 0, klass = 0, native = 0;
    bool unity = false;
    template<class IO> bool capture(IO& io, uint64_t obj, bool is_unity = false) {
        object = obj; unity = is_unity;
        if (object < 0x10000) return false;
        if (!pointer(io, object, klass) || klass < 0x10000) return false;
        if (unity) {
            if (!pointer(io, object + go_active::MANAGED_CACHED_PTR, native) || native < 0x10000)
                return false;
        }
        return true;
    }
    template<class IO> bool alive(IO& io) const {
        if (object < 0x10000 || klass < 0x10000) return false;
        uint64_t k = 0;
        if (!pointer(io, object, k) || k != klass) return false;
        if (unity) {
            uint64_t n = 0;
            if (!pointer(io, object + go_active::MANAGED_CACHED_PTR, n) || n != native)
                return false;
        }
        return true;
    }
    bool operator==(const Identity& b) const {
        return object == b.object && klass == b.klass && native == b.native && unity == b.unity;
    }
    bool operator!=(const Identity& b) const { return !(*this == b); }
};

template<class T> struct FarmPatch {
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
        if (!address || !valid(value) || !io.write_heap(address, value)) return false;
        last = value; written = true; return true;
    }
    template<class IO> bool restore(IO& io) {
        if (!written) { *this = {}; return true; }
        T current{};
        if (!io.read(address, current)) return false;
        if (same(current, last) && !io.write_heap(address, original)) return false;
        *this = {}; return true;
    }
};

template<class IO> class Controller {
    IO& io_;
    uint64_t session_ = 0;
    Identity player_{};
    Identity weapon_{};
    Identity config_{};
    Identity motion_{};
    Identity attack_offset_{};
    Identity animator_{};
    Identity rayman_{};

    // Patches
    FarmPatch<ByteVal> patch_always_crit_{};
    FarmPatch<ByteVal> patch_unbreakable_{};
    FarmPatch<FloatVal> patch_max_reach_{};
    FarmPatch<FloatVal> patch_hit_radius_{};
    FarmPatch<FloatVal> patch_time_between_attacks_{};
    FarmPatch<FloatVal> patch_pause_after_attack_{};
    FarmPatch<FloatVal> patch_impact_force_{};
    FarmPatch<FloatVal> patch_attack_clip_len_{};
    FarmPatch<Position3> patch_offset_pos_{};
    FarmPatch<Position3> patch_offset_cur_{};
    FarmPatch<Position3> patch_offset_rot_{};
    FarmPatch<FloatVal> patch_offset_lerp_{};
    FarmPatch<FloatVal> patch_ray_len_{};
    FarmPatch<FloatVal> patch_aim_ray_len_{};
    FarmPatch<FloatVal> patch_sphere_rad_{};
    FarmPatch<FloatVal> patch_too_close_{};

    bool is_tool_ = false;

    bool motion_from_action(uint64_t action, uint64_t weapon, int depth, int& budget, uint64_t& out_motion) {
        using namespace go_active;
        if (!action || depth > 2 || !budget) return false;
        --budget;
        uint64_t target = 0;
        if (!pointer(io_, action + DELEGATE_TARGET, target) || !target) return false;
        uint64_t obj = 0, pl = 0;
        pointer(io_, target + FPMOTION_OBJECT, obj);
        pointer(io_, target + FPMOTION_PLAYER, pl);
        uint64_t handler = 0;
        pointer(io_, player_.object + PLAYER_EVENT_HANDLER, handler);
        if (obj == weapon && pl == handler && is_class(target, "FPMotion")) {
            out_motion = target;
            return true;
        }
        uint64_t list = 0;
        int32_t count = 0;
        if (pointer(io_, action + MULTICAST_DELEGATES, list) && list &&
            io_.read(list + IL2CPP_ARRAY_LENGTH, count) && count > 0 && count <= 32) {
            for (int32_t i = 0; i < count && budget > 0; ++i) {
                uint64_t sub_act = 0;
                if (pointer(io_, list + IL2CPP_ARRAY_FIRST_ELEMENT + (uint64_t)i * 8, sub_act) && sub_act) {
                    if (motion_from_action(sub_act, weapon, depth + 1, budget, out_motion)) return true;
                }
            }
        }
        return false;
    }

    bool is_class(uint64_t obj, const char* name) {
        return io_.named(obj, name, "Oxide");
    }

    bool restore_all() {
        bool ok = true;
        if (!patch_always_crit_.restore(io_)) ok = false;
        if (!patch_unbreakable_.restore(io_)) ok = false;
        if (!patch_max_reach_.restore(io_)) ok = false;
        if (!patch_hit_radius_.restore(io_)) ok = false;
        if (!patch_time_between_attacks_.restore(io_)) ok = false;
        if (!patch_pause_after_attack_.restore(io_)) ok = false;
        if (!patch_impact_force_.restore(io_)) ok = false;
        if (!patch_attack_clip_len_.restore(io_)) ok = false;
        if (!patch_offset_pos_.restore(io_)) ok = false;
        if (!patch_offset_cur_.restore(io_)) ok = false;
        if (!patch_offset_rot_.restore(io_)) ok = false;
        if (!patch_offset_lerp_.restore(io_)) ok = false;
        if (!patch_ray_len_.restore(io_)) ok = false;
        if (!patch_aim_ray_len_.restore(io_)) ok = false;
        if (!patch_sphere_rad_.restore(io_)) ok = false;
        if (!patch_too_close_.restore(io_)) ok = false;
        return ok;
    }

public:
    viewmodel::Status status = viewmodel::Status::Off;

    explicit Controller(IO& io) : io_(io) {}

    bool has_state() const {
        return weapon_.object != 0 || patch_always_crit_.written || patch_unbreakable_.written ||
               patch_max_reach_.written || patch_offset_pos_.written;
    }

    bool reset(bool restore = true) {
        bool ok = true;
        if (restore) ok = restore_all();
        else {
            patch_always_crit_ = {};
            patch_unbreakable_ = {};
            patch_max_reach_ = {};
            patch_hit_radius_ = {};
            patch_time_between_attacks_ = {};
            patch_pause_after_attack_ = {};
            patch_impact_force_ = {};
            patch_attack_clip_len_ = {};
            patch_offset_pos_ = {};
            patch_offset_cur_ = {};
            patch_offset_rot_ = {};
            patch_offset_lerp_ = {};
            patch_ray_len_ = {};
            patch_aim_ray_len_ = {};
            patch_sphere_rad_ = {};
            patch_too_close_ = {};
        }
        player_ = {}; weapon_ = {}; config_ = {}; motion_ = {};
        attack_offset_ = {}; animator_ = {}; rayman_ = {};
        session_ = 0; is_tool_ = false;
        status = viewmodel::Status::Off;
        return ok;
    }

    void tick(uint64_t session, uint64_t player, bool enabled) {
        using namespace go_active;
        using viewmodel::Status;

        if (!enabled) {
            if (has_state()) reset(true);
            status = Status::Off;
            return;
        }

        if (session != session_) {
            reset(false);
            session_ = session;
        }

        if (player < 0x10000) {
            if (has_state()) reset(true);
            status = Status::Waiting;
            return;
        }

        if (!player_.alive(io_) || player_.object != player) {
            if (!player_.capture(io_, player, true)) {
                if (has_state()) reset(true);
                status = Status::Waiting;
                return;
            }
        }

        // Get FPManager -> current weapon
        uint64_t fp_manager = 0;
        if (!pointer(io_, player + PLAYER_FP_MANAGER, fp_manager) || fp_manager < 0x10000) {
            if (has_state()) reset(true);
            status = Status::Waiting;
            return;
        }

        uint64_t weapon = 0;
        pointer(io_, fp_manager + FPMANAGER_CURRENT_WEAPON, weapon);
        uint64_t backref = 0;
        if (!weapon || !pointer(io_, weapon + FPOBJECT_PLAYER_BACKREF, backref) || backref != player) {
            weapon = 0;
            pointer(io_, fp_manager + FPMANAGER_CURRENT_OBJECT, weapon);
            if (!weapon || !pointer(io_, weapon + FPOBJECT_PLAYER_BACKREF, backref) || backref != player) {
                if (has_state()) reset(true);
                status = Status::Waiting;
                return;
            }
        }

        // Check if weapon is melee / tool
        const bool is_tool = is_class(weapon, "FPTool") || is_class(weapon, "FPChainsaw");
        const bool is_melee = is_tool || is_class(weapon, "FPMelee") || is_class(weapon, "FPSpear") ||
                              is_class(weapon, "FPBuildingHammer") || is_class(weapon, "FPTorch");

        if (!is_melee) {
            if (has_state()) reset(true);
            status = Status::Waiting;
            return;
        }

        // Weapon changed: restore previous patches first
        if (weapon_.object != weapon || !weapon_.alive(io_)) {
            restore_all();
            if (!weapon_.capture(io_, weapon, true)) {
                status = Status::Waiting;
                return;
            }
            is_tool_ = is_tool;
        }

        bool ok = true;

        // 1. Weakpoint / Crit (alwaysCrit) for FPTool / FPChainsaw
        if (is_tool_) {
            if (patch_always_crit_.sample(io_, weapon + FPTOOL_ALWAYS_CRIT)) {
                if (!patch_always_crit_.put(io_, ByteVal{1})) ok = false;
            }
        }

        // 2. Durability protection (durabilityUnbreakable = 1 on ItemConfigValues)
        uint64_t item = 0;
        if (pointer(io_, weapon + FPOBJECT_ITEM, item) && item >= 0x10000) {
            uint64_t item_data = 0;
            if (pointer(io_, item + ITEM_DATA, item_data) && item_data >= 0x10000) {
                uint64_t config = 0;
                if (pointer(io_, item_data + ITEMDATA_CONFIG_VALUES, config) && config >= 0x10000) {
                    if (config_.object != config || !config_.alive(io_)) {
                        config_.capture(io_, config, false);
                    }
                    if (patch_unbreakable_.sample(io_, config + ITEMCONFIG_DURABILITY_UNBREAKABLE)) {
                        if (!patch_unbreakable_.put(io_, ByteVal{1})) ok = false;
                    }
                }
            }
            // Restore current condition to max if damaged
            uint64_t item_state = 0;
            if (pointer(io_, item + 0x28, item_state) && item_state >= 0x10000) {
                float cur_dur = 0.f, max_dur = 0.f;
                if (io_.read(item_state + 0x10, cur_dur) && io_.read(item_state + 0x1C, max_dur) &&
                    viewmodel::finite(cur_dur) && viewmodel::finite(max_dur) && max_dur > 0.f && cur_dur < max_dur) {
                    io_.write_heap(item_state + 0x10, max_dur);
                }
            }
        }

        // 3. Reach expansion & 360-degree sphere
        if (patch_max_reach_.sample(io_, weapon + FPMELEE_MAX_REACH)) {
            if (!patch_max_reach_.put(io_, FloatVal{20.0f})) ok = false;
        }
        if (patch_hit_radius_.sample(io_, weapon + FPMELEE_HIT_RADIUS)) {
            if (!patch_hit_radius_.put(io_, FloatVal{15.0f})) ok = false;
        }
        if (patch_time_between_attacks_.sample(io_, weapon + FPMELEE_TIME_BETWEEN_ATTACKS)) {
            if (!patch_time_between_attacks_.put(io_, FloatVal{0.08f})) ok = false;
        }
        if (patch_pause_after_attack_.sample(io_, weapon + FPMELEE_PAUSE_AFTER_ATTACK)) {
            if (!patch_pause_after_attack_.put(io_, FloatVal{0.0f})) ok = false;
        }
        if (patch_impact_force_.sample(io_, weapon + FPMELEE_IMPACT_FORCE)) {
            if (!patch_impact_force_.put(io_, FloatVal{0.0f})) ok = false;
        }

        // RaycastManager
        uint64_t rayman = 0;
        if (pointer(io_, weapon + FPOBJECT_RAYCAST_MANAGER, rayman) && rayman >= 0x10000) {
            uint64_t rm_player = 0;
            if (pointer(io_, rayman + RAYCASTMAN_PLAYER, rm_player) && rm_player == player) {
                if (rayman_.object != rayman || !rayman_.alive(io_)) {
                    rayman_.capture(io_, rayman, true);
                }
                if (patch_ray_len_.sample(io_, rayman + RAYCASTMAN_RAY_LENGTH)) {
                    patch_ray_len_.put(io_, FloatVal{20.0f});
                }
                if (patch_aim_ray_len_.sample(io_, rayman + RAYCASTMAN_AIM_RAY_LENGTH)) {
                    patch_aim_ray_len_.put(io_, FloatVal{20.0f});
                }
                if (patch_sphere_rad_.sample(io_, rayman + RAYCASTMAN_SPHERE_RADIUS)) {
                    patch_sphere_rad_.put(io_, FloatVal{15.0f});
                }
                if (patch_too_close_.sample(io_, rayman + RAYCASTMAN_TOO_CLOSE)) {
                    patch_too_close_.put(io_, FloatVal{0.0f});
                }
            }
        }

        // 4. Motion suppression (m_AttackOffset pos/rot/BYE = 0)
        uint64_t draw = 0;
        if (pointer(io_, weapon + FPOBJECT_DRAW_EVENT, draw) && draw >= 0x10000) {
            uint64_t action = 0;
            if (pointer(io_, draw + FPDRAW_ACTION, action) && action >= 0x10000) {
                int budget = 16;
                uint64_t motion = 0;
                if (motion_from_action(action, weapon, 0, budget, motion) && motion >= 0x10000) {
                    if (motion_.object != motion || !motion_.alive(io_)) {
                        motion_.capture(io_, motion, true);
                    }
                    uint64_t attack_offset = 0;
                    if (pointer(io_, motion + FPMOTION_ATTACK_OFFSET, attack_offset) && attack_offset >= 0x10000) {
                        if (attack_offset_.object != attack_offset || !attack_offset_.alive(io_)) {
                            attack_offset_.capture(io_, attack_offset, false);
                        }
                        if (patch_offset_pos_.sample(io_, attack_offset + TRANSFORMOFFSET_POSITION)) {
                            patch_offset_pos_.put(io_, Position3{0.f, 0.f, 0.f});
                        }
                        if (patch_offset_cur_.sample(io_, attack_offset + TRANSFORMOFFSET_CURRENT_POSITION)) {
                            patch_offset_cur_.put(io_, Position3{0.f, 0.f, 0.f});
                        }
                        if (patch_offset_rot_.sample(io_, attack_offset + TRANSFORMOFFSET_ROTATION)) {
                            patch_offset_rot_.put(io_, Position3{0.f, 0.f, 0.f});
                        }
                        if (patch_offset_lerp_.sample(io_, attack_offset + TRANSFORMOFFSET_LERP_SPEED)) {
                            patch_offset_lerp_.put(io_, FloatVal{100.0f});
                        }
                    }
                }
            }
        }

        // 5. Animator attack clip length suppression
        uint64_t anim_ev = 0;
        if (pointer(io_, weapon + FPMELEE_EVENT_ATTACK, anim_ev) && anim_ev >= 0x10000) {
            uint64_t action = 0;
            if (pointer(io_, anim_ev + FPDRAW_ACTION, action) && action >= 0x10000) {
                uint64_t target = 0;
                if (pointer(io_, action + DELEGATE_TARGET, target) && target >= 0x10000) {
                    if (animator_.object != target || !animator_.alive(io_)) {
                        animator_.capture(io_, target, true);
                    }
                    if (patch_attack_clip_len_.sample(io_, target + FPMELEE_ANIM_ATTACK_CLIP_LEN)) {
                        patch_attack_clip_len_.put(io_, FloatVal{0.0f});
                    }
                }
            }
        }

        status = ok ? Status::Active : Status::Failed;
    }
};

} // namespace farm_tuning
