#pragma once
// Memory-based tuning controller for Autofarm:
// 1. Weakpoint auto-crit: alwaysCrit = 1 on FPTool / FPChainsaw (forces max hitstreak & crits).
// 2. Unbreakable tool durability: ItemConfigValues.durabilityUnbreakable = 1 & condition preservation.
// 3. Extended reach & 360-degree sphere: FPMelee.m_MaxReach = 20.0f, hitRadius = 15.0f, timeBetweenAttacks = 0.08f, RaycastManager ray & sphere.
// 4. Complete animation & motion suppression:
//    - Zero delegate action at offset 0x10 (EDT_ACTION) of FPMelee.BYR (Attack), FPMelee.BYI (Woosh), FPMelee.BYT (Hit), and FPWeaponBase.BBk.
//    - Zero FPMotion.m_AttackOffset position/rotation/current with instant lerp.
//    - Zero FPMeleeAnimator attackClipLength, wooshShake, and hitShake.
//    - Zero FPMelee.m_ImpactForce.
// 5. LookDirection memory targeting:
//    - Points PlayerEventHandler.LookDirection directly at target crosshair / node, ensuring RaycastManager ray hits the weakpoint regardless of player camera look direction.
// 6. Comprehensive diagnostic logging via LogLine.
// 7. 100% dynamic offset resolution via go_active::* (compatible with offset_resolver).
// 8. Zero library binary patching (pure heap data modification with automatic clean restoration).

#include "viewmodel.h"
#include "game_offsets_active.h"
#include "game.h"
#include "logfile.h"
#include <array>
#include <cstring>
#include <cmath>

namespace farm_tuning {

struct Position3 { float x, y, z; };
inline bool valid(Position3 v) {
    return viewmodel::finite(v.x) && viewmodel::finite(v.y) && viewmodel::finite(v.z) &&
           v.x > -100000.f && v.x < 100000.f && v.y > -100000.f && v.y < 100000.f && v.z > -100000.f && v.z < 100000.f;
}
struct FloatVal { float value; };
inline bool valid(FloatVal v) { return viewmodel::finite(v.value) && v.value >= -10000.f && v.value <= 10000.f; }
struct ByteVal { uint8_t value; };
inline bool valid(ByteVal v) { return v.value <= 1; }
struct PtrVal { uint64_t value; };
inline bool valid(PtrVal v) { return v.value == 0 || (v.value >= 0x10000 && v.value < 0x00ffffffffffffffULL); }

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
    Identity byr_{};
    Identity byi_{};
    Identity byt_{};
    Identity bbk_{};
    Identity look_dir_wrapper_{};

    // Patches
    FarmPatch<ByteVal> patch_always_crit_{};
    FarmPatch<ByteVal> patch_unbreakable_{};
    FarmPatch<FloatVal> patch_max_reach_{};
    FarmPatch<FloatVal> patch_hit_radius_{};
    FarmPatch<FloatVal> patch_time_between_attacks_{};
    FarmPatch<FloatVal> patch_pause_after_attack_{};
    FarmPatch<FloatVal> patch_impact_force_{};
    FarmPatch<FloatVal> patch_attack_clip_len_{};
    FarmPatch<PtrVal> patch_woosh_shake_{};
    FarmPatch<PtrVal> patch_hit_shake_{};
    FarmPatch<PtrVal> patch_event_attack_action_{};
    FarmPatch<PtrVal> patch_event_woosh_action_{};
    FarmPatch<PtrVal> patch_event_hit_action_{};
    FarmPatch<PtrVal> patch_event_base_attack_action_{};
    FarmPatch<Position3> patch_offset_pos_{};
    FarmPatch<Position3> patch_offset_cur_{};
    FarmPatch<Position3> patch_offset_rot_{};
    FarmPatch<FloatVal> patch_offset_lerp_{};
    FarmPatch<FloatVal> patch_ray_len_{};
    FarmPatch<FloatVal> patch_aim_ray_len_{};
    FarmPatch<FloatVal> patch_sphere_rad_{};
    FarmPatch<FloatVal> patch_too_close_{};
    FarmPatch<Position3> patch_look_direction_{};

    bool is_tool_ = false;
    uint64_t last_target_id_ = 0;
    uint32_t log_tick_count_ = 0;
    bool logged_activation_ = false;

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
        if (io_.named(obj, name, "Oxide")) return true;
        uint64_t k = 0;
        if (!pointer(io_, obj, k) || k < 0x10000) return false;
        for (int depth = 0; depth < 4; ++depth) {
            uint64_t parent = 0;
            if (!pointer(io_, k + 0x58, parent) || parent < 0x10000) break;
            if (io_.named_klass(parent, name, "Oxide")) return true;
            k = parent;
        }
        return false;
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
        if (!patch_woosh_shake_.restore(io_)) ok = false;
        if (!patch_hit_shake_.restore(io_)) ok = false;
        if (!patch_event_attack_action_.restore(io_)) ok = false;
        if (!patch_event_woosh_action_.restore(io_)) ok = false;
        if (!patch_event_hit_action_.restore(io_)) ok = false;
        if (!patch_event_base_attack_action_.restore(io_)) ok = false;
        if (!patch_offset_pos_.restore(io_)) ok = false;
        if (!patch_offset_cur_.restore(io_)) ok = false;
        if (!patch_offset_rot_.restore(io_)) ok = false;
        if (!patch_offset_lerp_.restore(io_)) ok = false;
        if (!patch_ray_len_.restore(io_)) ok = false;
        if (!patch_aim_ray_len_.restore(io_)) ok = false;
        if (!patch_sphere_rad_.restore(io_)) ok = false;
        if (!patch_too_close_.restore(io_)) ok = false;
        if (!patch_look_direction_.restore(io_)) ok = false;
        return ok;
    }

public:
    viewmodel::Status status = viewmodel::Status::Off;

    explicit Controller(IO& io) : io_(io) {}

    bool has_state() const {
        return weapon_.object != 0 || patch_always_crit_.written || patch_unbreakable_.written ||
               patch_max_reach_.written || patch_offset_pos_.written || patch_event_attack_action_.written ||
               patch_look_direction_.written;
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
            patch_woosh_shake_ = {};
            patch_hit_shake_ = {};
            patch_event_attack_action_ = {};
            patch_event_woosh_action_ = {};
            patch_event_hit_action_ = {};
            patch_event_base_attack_action_ = {};
            patch_offset_pos_ = {};
            patch_offset_cur_ = {};
            patch_offset_rot_ = {};
            patch_offset_lerp_ = {};
            patch_ray_len_ = {};
            patch_aim_ray_len_ = {};
            patch_sphere_rad_ = {};
            patch_too_close_ = {};
            patch_look_direction_ = {};
        }
        player_ = {}; weapon_ = {}; config_ = {}; motion_ = {};
        attack_offset_ = {}; animator_ = {}; rayman_ = {};
        byr_ = {}; byi_ = {}; byt_ = {}; bbk_ = {}; look_dir_wrapper_ = {};
        session_ = 0; is_tool_ = false; last_target_id_ = 0;
        log_tick_count_ = 0; logged_activation_ = false;
        status = viewmodel::Status::Off;
        return ok;
    }

    void tick(uint64_t session, uint64_t player, bool enabled) {
        using namespace go_active;
        using viewmodel::Status;

        if (!enabled) {
            if (has_state()) {
                LogLine("farm_mem: autofarm disabled, restoring all patches");
                reset(true);
            }
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
            logged_activation_ = false;
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

        // 4. RaycastManager tuning
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

        // 5. Complete animation & swing motion suppression
        // Event delegates suppression on FPMelee (BYR = 0x148, BYI = 0x150, BYT = 0x158) and FPWeaponBase (BBk = 0x118)
        uint64_t ev_byr = 0;
        if (pointer(io_, weapon + FPMELEE_EVENT_ATTACK, ev_byr) && ev_byr >= 0x10000) {
            if (patch_event_attack_action_.sample(io_, ev_byr + EDT_ACTION)) {
                patch_event_attack_action_.put(io_, PtrVal{0});
            }
        }
        uint64_t ev_byi = 0;
        if (pointer(io_, weapon + FPMELEE_EVENT_WOOSH, ev_byi) && ev_byi >= 0x10000) {
            if (patch_event_woosh_action_.sample(io_, ev_byi + EDT_ACTION)) {
                patch_event_woosh_action_.put(io_, PtrVal{0});
            }
        }
        uint64_t ev_byt = 0;
        if (pointer(io_, weapon + FPMELEE_EVENT_HIT, ev_byt) && ev_byt >= 0x10000) {
            if (patch_event_hit_action_.sample(io_, ev_byt + EDT_ACTION)) {
                patch_event_hit_action_.put(io_, PtrVal{0});
            }
        }
        uint64_t ev_bbk = 0;
        if (pointer(io_, weapon + FPWEAPONBASE_EVENT_ATTACK, ev_bbk) && ev_bbk >= 0x10000) {
            if (patch_event_base_attack_action_.sample(io_, ev_bbk + EDT_ACTION)) {
                patch_event_base_attack_action_.put(io_, PtrVal{0});
            }
        }

        // Motion suppression (m_AttackOffset pos/rot/BYE = 0)
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

        // Animator attack clip length & shakes suppression
        if (ev_byr >= 0x10000) {
            uint64_t act_ptr = patch_event_attack_action_.original.value;
            if (act_ptr >= 0x10000) {
                uint64_t target = 0;
                if (pointer(io_, act_ptr + DELEGATE_TARGET, target) && target >= 0x10000) {
                    if (animator_.object != target || !animator_.alive(io_)) {
                        animator_.capture(io_, target, true);
                    }
                    if (patch_attack_clip_len_.sample(io_, target + FPMELEE_ANIM_ATTACK_CLIP_LEN)) {
                        patch_attack_clip_len_.put(io_, FloatVal{0.0f});
                    }
                    if (patch_woosh_shake_.sample(io_, target + FPMELEE_ANIM_WOOSH_SHAKE)) {
                        patch_woosh_shake_.put(io_, PtrVal{0});
                    }
                    if (patch_hit_shake_.sample(io_, target + FPMELEE_ANIM_HIT_SHAKE)) {
                        patch_hit_shake_.put(io_, PtrVal{0});
                    }
                }
            }
        }

        // 6. LookDirection memory targeting
        FarmTarget tgt{};
        float eye_x = 0.f, eye_y = 0.f, eye_z = 0.f;
        const bool have_target = esp_farm_get_target(tgt) && tgt.valid;
        const bool have_eye = esp_local_eye_position(eye_x, eye_y, eye_z);

        if (have_target && have_eye) {
            uint64_t handler = 0;
            if (pointer(io_, player + PLAYER_EVENT_HANDLER, handler) && handler >= 0x10000) {
                uint64_t look_wrapper = 0;
                if (pointer(io_, handler + EVENT_HANDLER_LOOK_DIRECTION, look_wrapper) && look_wrapper >= 0x10000) {
                    if (look_dir_wrapper_.object != look_wrapper || !look_dir_wrapper_.alive(io_)) {
                        look_dir_wrapper_.capture(io_, look_wrapper, false);
                    }
                    float dx = tgt.aim_x - eye_x;
                    float dy = tgt.aim_y - eye_y;
                    float dz = tgt.aim_z - eye_z;
                    float len = std::sqrt(dx * dx + dy * dy + dz * dz);
                    if (viewmodel::finite(len) && len > 0.05f) {
                        Position3 dir = { dx / len, dy / len, dz / len };
                        if (patch_look_direction_.sample(io_, look_wrapper + GUI_VALUE)) {
                            patch_look_direction_.put(io_, dir);
                        }
                    }
                }
            }
        } else {
            patch_look_direction_.restore(io_);
        }

        // Periodic & transition logging
        ++log_tick_count_;
        if (!logged_activation_ || (log_tick_count_ % 120 == 0)) {
            logged_activation_ = true;
            LogLine("farm_mem: active weapon=0x%llx is_tool=%d crit=%d unbreak=%d reach=%.1f rad=%.1f anim_ev_zeroed=%d look_locked=%d",
                    (unsigned long long)weapon, (int)is_tool_, (int)patch_always_crit_.written,
                    (int)patch_unbreakable_.written, patch_max_reach_.last.value, patch_hit_radius_.last.value,
                    (int)patch_event_attack_action_.written, (int)patch_look_direction_.written);
        }

        if (have_target && tgt.id != last_target_id_) {
            last_target_id_ = tgt.id;
            LogLine("farm_mem: target locked id=0x%llx kind=%d spot=%d aim=(%.2f, %.2f, %.2f) eye=(%.2f, %.2f, %.2f) dist=%.2f reach=%.2f",
                    (unsigned long long)tgt.id, tgt.kind, (int)tgt.has_spot,
                    tgt.aim_x, tgt.aim_y, tgt.aim_z, eye_x, eye_y, eye_z, tgt.aim_dist, tgt.melee_reach);
        }

        status = ok ? Status::Active : Status::Failed;
    }
};

} // namespace farm_tuning
