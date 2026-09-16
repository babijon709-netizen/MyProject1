// aim_points.cpp — Точки прицела: голова/шея/грудь и локальный ADS.
//
// Модуль разрезан из прежнего монолита jni/src/game.cpp;
// что здесь лежит и кто это зовёт — в шапке aim_points.h и в docs/CODE_MAP.md.

#include "esp/common.h"
#include "esp/boxes.h"
#include "esp/camera.h"
#include "esp/frame.h"
#include "esp/il2cpp.h"
#include "esp/math.h"
#include "esp/mem.h"
#include "esp/skeleton_build.h"
#include "esp/skeleton_cache.h"
#include "esp/skeleton_names.h"
#include "esp/transform.h"
#include "aim_points.h"

LocalAimState g_aim_state{};

// Angular offset of a world point from the camera axis. Prefers the live
// camera pose; falls back to inverting the projection from the screen point,
// so aim angles never depend on the transform-pose path succeeding.
static bool aim_angles_for(const Vec3& world, const Vec2& screen, float sw, float sh, float& yaw_deg, float& pitch_deg) {
    constexpr float rad2deg = 57.29577951F;
    if (g_cam_pose_valid || g_aim_ref_valid) {
        // Prefer the real firing reference (look root direction from the eye
        // point); the camera pose is the fallback.
        const bool use_ref = g_aim_ref_valid;
        const Vec3& origin = use_ref ? g_aim_ref_origin : g_cam_pos;
        const Vec3& fwd = use_ref ? g_aim_ref_forward : g_cam_forward;
        const Vec3& right = use_ref ? g_aim_ref_right : g_cam_right;
        const Vec3& up = use_ref ? g_aim_ref_up : g_cam_up;
        Vec3 d = {world.x - origin.x, world.y - origin.y, world.z - origin.z};
        float fx = d.x * fwd.x + d.y * fwd.y + d.z * fwd.z;
        float rx = d.x * right.x + d.y * right.y + d.z * right.z;
        float ux = d.x * up.x + d.y * up.y + d.z * up.z;
        if (std::isfinite(fx) && std::isfinite(rx) && std::isfinite(ux) && fx > 0.05F) {
            yaw_deg = atan2f(rx, fx) * rad2deg;
            pitch_deg = atan2f(ux, sqrtf(fx * fx + rx * rx)) * rad2deg;
            if (std::isfinite(yaw_deg) && std::isfinite(pitch_deg)) return true;
        }
    }
    float fov = (g_cam_fov_deg > 1.0F && g_cam_fov_deg < 179.0F) ? g_cam_fov_deg : 60.0F;
    float tan_half_v = tanf(fov * 0.5F / rad2deg);
    float aspect = sw / sh;
    float ndc_x = (screen.x / sw) * 2.0F - 1.0F;
    float ndc_y = 1.0F - (screen.y / sh) * 2.0F;
    yaw_deg = atanf(ndc_x * tan_half_v * aspect) * rad2deg;
    pitch_deg = atanf(ndc_y * tan_half_v) * rad2deg;
    return std::isfinite(yaw_deg) && std::isfinite(pitch_deg);
}

// Extra vertical offset applied to every head aim point (metres, world up).
// Tuned in-game at fighting range: +10 cm lands centre-head. This is the
// far-range figure; head_lift_for_range() fades down to the near one below.
static constexpr float g_aim_head_lift = 0.10F;

// Up close the same 14 cm is a completely different shot: at five metres it
// is well over a degree, which puts the round over the top of the head, while
// at fifty it is a tenth of that and still inside the skull. The offset is
// therefore ramped in with range instead of being a constant.
static constexpr float g_aim_head_lift_near = 0.04F;

static float head_lift_for_range(const Vec3& world) {
    if (!g_cam_pose_valid) return g_aim_head_lift;
    float dx = world.x - g_cam_pos.x, dy = world.y - g_cam_pos.y, dz = world.z - g_cam_pos.z;
    float range = sqrtf(dx * dx + dy * dy + dz * dz);
    if (!std::isfinite(range)) return g_aim_head_lift;
    float t = (range - 8.0F) / 30.0F;          // full offset from ~38 m out
    if (t < 0.0F) t = 0.0F; else if (t > 1.0F) t = 1.0F;
    return g_aim_head_lift_near + (g_aim_head_lift - g_aim_head_lift_near) * t;
}

Vec3 g_aim_lead{};

bool set_aim_point(EspBox& box, int slot, const Vec3& world_in, const Mat4& vp, float sw, float sh) {
    Vec3 world = world_in;
    world.x += g_aim_lead.x; world.y += g_aim_lead.y; world.z += g_aim_lead.z;
    if (slot == 0) world.y += head_lift_for_range(world_in);
    Vec2 screen{};
    if (!w2s(vp, world, sw, sh, screen, false)) return false;
    if (fabsf(screen.x) > sw * 4.0F || fabsf(screen.y) > sh * 4.0F) return false;
    float yaw = 0.0F, pitch = 0.0F;
    if (!aim_angles_for(world, screen, sw, sh, yaw, pitch)) return false;
    box.aim_pts[slot][0] = screen.x;
    box.aim_pts[slot][1] = screen.y;
    box.aim_yaw[slot] = yaw;
    box.aim_pitch[slot] = pitch;
    box.aim_valid[slot] = true;
    return true;
}

bool fill_skeleton_box(uint64_t player, const Mat4& view_projection, float sw, float sh, EspBox& box) {
    box.has_skeleton = false;
    for (int bone = 0; bone < ESP_BONE_COUNT; ++bone) box.bone_valid[bone] = false;
    for (int i = 0; i < 3; ++i) box.aim_valid[i] = false;

    CachedSkeleton& skeleton = g_skeletons[player];
    if (!skeleton.valid) {
        if (skeleton.retry_cooldown > 0) { --skeleton.retry_cooldown; return false; }
        // Building walks the whole model; allow at most one rebuild per frame
        // so multiple new players never stall the overlay.
        if (g_skeleton_builds_this_frame >= 1) return false;
        ++g_skeleton_builds_this_frame;
        if (!build_skeleton(player, skeleton)) {
            skeleton.valid = false;
            skeleton.retry_cooldown = 20;
            return false;
        }
    }

    // Periodically make sure the player still uses the same character model.
    if (++skeleton.revalidate_timer >= 120) {
        skeleton.revalidate_timer = 0;
        if (skeleton_model_root(player) != skeleton.model_root) {
            skeleton = CachedSkeleton{};
            skeleton.retry_cooldown = 2;
            return false;
        }
    }

    // Bones may legitimately live in SEVERAL TransformHierarchies: the game
    // re-parents bones at runtime (Ragdoll.m_BonesToReparent, aim rigs, foot
    // IK), which moves them into a different hierarchy. Never drop a bone for
    // that — group bones by hierarchy data and bulk-read every group.
    constexpr int kMaxGroups = 4;
    uint64_t group_data[kMaxGroups] = {};
    int32_t  group_max[kMaxGroups] = {};
    int      group_count = 0;
    int32_t  bone_index[ESP_BONE_COUNT];
    int      bone_group[ESP_BONE_COUNT];
    for (int bone = 0; bone < ESP_BONE_COUNT; ++bone) {
        bone_index[bone] = -1;
        bone_group[bone] = -1;
        uint64_t transform = skeleton.bone_transform[bone];
        if (!transform) continue;
        uint64_t bone_data = rd_ptr(transform + g_skeleton_layout.data_offset);
        if (!bone_data) continue;
        int32_t index = rd<int32_t>(transform + g_skeleton_layout.index_offset);
        if (index < 0 || index > 100000) continue;
        int group = -1;
        for (int g = 0; g < group_count; ++g)
            if (group_data[g] == bone_data) { group = g; break; }
        if (group < 0) {
            if (group_count >= kMaxGroups) continue;
            group = group_count++;
            group_data[group] = bone_data;
            group_max[group] = -1;
        }
        bone_index[bone] = index;
        bone_group[bone] = group;
        if (index > group_max[group]) group_max[group] = index;
    }
    // Liveness: the hips transform no longer resolves => model despawned.
    if (group_count == 0 || bone_index[BONE_HIPS] < 0) {
        if (++skeleton.fail_streak > 30) { skeleton = CachedSkeleton{}; skeleton.retry_cooldown = 30; }
        return false;
    }

    // Bulk read the TRS + parent-index arrays of every hierarchy in use.
    static std::vector<Matrix34> local_matrices[kMaxGroups];
    static std::vector<int32_t>  local_parents[kMaxGroups];
    uint64_t group_matrices[kMaxGroups] = {};
    uint64_t group_indices[kMaxGroups] = {};
    bool     group_ok[kMaxGroups] = {};
    for (int g = 0; g < group_count; ++g) {
        int32_t needed = group_max[g] + 1;
        if (needed <= 0 || needed > 8192) continue;
        uint64_t matrices = rd_ptr(group_data[g] + g_skeleton_layout.matrices_offset);
        uint64_t indices = rd_ptr(group_data[g] + g_skeleton_layout.indices_offset);
        if (g_skeleton_layout.matrices_indirect) matrices = rd_ptr(matrices);
        if (g_skeleton_layout.indices_indirect) indices = rd_ptr(indices);
        if (!matrices || !indices) continue;
        local_matrices[g].resize((size_t)needed);
        local_parents[g].resize((size_t)needed);
        if (!rd_buf(matrices, local_matrices[g].data(), (size_t)needed * sizeof(Matrix34)) ||
            !rd_buf(indices, local_parents[g].data(), (size_t)needed * sizeof(int32_t))) continue;
        group_matrices[g] = matrices;
        group_indices[g] = indices;
        group_ok[g] = true;
    }

    int projected = 0;
    int remote_fallbacks = 0;
    for (int bone = 0; bone < ESP_BONE_COUNT; ++bone) {
        Vec3 world{};
        bool have_world = false;
        if (bone_index[bone] >= 0 && bone_group[bone] >= 0) {
            int g = bone_group[bone];
            int walked = 0;
            if (group_ok[g]) {
                walked = skeleton_local_walk(local_matrices[g].data(), local_parents[g].data(),
                                             (int32_t)local_matrices[g].size(), bone_index[bone], world);
                if (walked < 0 && remote_fallbacks < 8) {
                    // Parent chain leaves the buffered range (rare): remote walk.
                    ++remote_fallbacks;
                    walked = read_transform_hierarchy_arrays(group_matrices[g], group_indices[g],
                                                             bone_index[bone], world) ? 1 : 0;
                }
            } else if (remote_fallbacks < 8) {
                ++remote_fallbacks;
                walked = read_transform_hierarchy_layout(skeleton.bone_transform[bone],
                                                         g_skeleton_layout, world) ? 1 : 0;
            }
            if (walked == 1) {
                have_world = true;
                skeleton.bone_world[bone] = world;
                skeleton.bone_world_age[bone] = 1;
            }
        }
        // Transient read glitch (game mid-update): briefly reuse the last known
        // world position instead of letting the bone flicker off.
        if (!have_world && skeleton.bone_world_age[bone] >= 1 && skeleton.bone_world_age[bone] <= 8) {
            world = skeleton.bone_world[bone];
            ++skeleton.bone_world_age[bone];
            have_world = true;
        }
        if (!have_world) continue;
        Vec2 screen{};
        if (!w2s(view_projection, world, sw, sh, screen, false)) continue;
        if (fabsf(screen.x) > sw * 4.0F || fabsf(screen.y) > sh * 4.0F) continue;
        box.bones[bone][0] = screen.x;
        box.bones[bone][1] = screen.y;
        box.bone_valid[bone] = true;
        ++projected;
    }
    // Aim points: exact bone world positions -> screen + angular offsets.
    //   [0] head  : skull centre. The Head joint sits at the base of the skull,
    //               the head hitbox extends ~20 cm above it. Aim ~11 cm up the
    //               neck axis, plus a small distance-dependent lift so that at
    //               long range the shot lands inside the skull rather than at
    //               its lower edge (steering/animation error grows with range).
    //   [1] neck  : between the Neck and Head joints.
    //   [2] chest : upper spine.
    {
        auto bone_world_ok = [&](int bone, Vec3& out) -> bool {
            if (!box.bone_valid[bone]) return false;
            if (skeleton.bone_world_age[bone] < 1 || skeleton.bone_world_age[bone] > 9) return false;
            out = skeleton.bone_world[bone];
            return vec3_is_finite(out);
        };
        Vec3 target[3]{};
        bool have[3] = {false, false, false};

        Vec3 head{}, neck{};
        const bool have_head_bone = bone_world_ok(BONE_HEAD, head);
        const bool have_neck_bone = bone_world_ok(BONE_NECK, neck);
        if (have_head_bone) {
            Vec3 dir = {0.0F, 1.0F, 0.0F};
            if (have_neck_bone) {
                Vec3 d = {head.x - neck.x, head.y - neck.y, head.z - neck.z};
                float len = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
                if (len > 0.02F && len < 0.6F) dir = {d.x / len, d.y / len, d.z / len};
            }
            float range = 0.0F;
            if (g_cam_pose_valid) {
                float dx = head.x - g_cam_pos.x, dy = head.y - g_cam_pos.y, dz = head.z - g_cam_pos.z;
                range = sqrtf(dx * dx + dy * dy + dz * dz);
            }
            float lift = range * 0.0010F;          // +10 cm per 100 m
            if (lift > 0.08F) lift = 0.08F;
            target[0] = {head.x + dir.x * 0.11F, head.y + dir.y * 0.11F + lift, head.z + dir.z * 0.11F};
            have[0] = true;
        }
        if (have_neck_bone && have_head_bone) {
            target[1] = {(neck.x + head.x) * 0.5F, (neck.y + head.y) * 0.5F, (neck.z + head.z) * 0.5F};
            have[1] = true;
        } else if (have_neck_bone) {
            target[1] = {neck.x, neck.y + 0.05F, neck.z}; have[1] = true;
        } else if (have_head_bone) {
            target[1] = {head.x, head.y - 0.06F, head.z}; have[1] = true;
        }
        Vec3 chest{};
        if (bone_world_ok(BONE_SPINE2, chest) || bone_world_ok(BONE_SPINE1, chest) || bone_world_ok(BONE_SPINE, chest)) {
            target[2] = chest; have[2] = true;
        }

        for (int i = 0; i < 3; ++i) {
            if (!have[i]) continue;
            if (set_aim_point(box, i, target[i], view_projection, sw, sh)) box.aim_source = 1;
        }
    }

    if (projected < 4) {
        if (++skeleton.fail_streak > 30) { skeleton = CachedSkeleton{}; skeleton.retry_cooldown = 30; }
        return false;
    }
    skeleton.fail_streak = 0;
    box.has_skeleton = true;
    return true;
}

static bool read_local_aim_state() {
    uint64_t local = resolve_local_player();
    if (!local) { g_aim_state = {}; return false; }

    // Cheap fast path on cached pointers; re-validate the chain periodically.
    if (g_aim_state.aim_activity && --g_aim_state.revalidate > 0) {
        uint8_t active = 0;
        if (rd_exact(g_aim_state.aim_activity + ACTIVITY_ACTIVE_FLAG, active)) {
            g_aim_state.aiming = (active != 0);
            g_aim_state.source = 1;
            return true;
        }
    }
    if (!g_aim_state.aim_activity && g_aim_state.weapon && --g_aim_state.revalidate > 0) {
        uint8_t is_aiming = 0;
        if (rd_ptr(g_aim_state.weapon + FPOBJECT_PLAYER_BACKREF) == local &&
            rd_exact(g_aim_state.weapon + FPWEAPON_IS_AIMING, is_aiming)) {
            g_aim_state.aiming = (is_aiming != 0);
            g_aim_state.source = 2;
            return true;
        }
    }

    LocalAimState fresh{};
    fresh.revalidate = 60;

    // Primary: PlayerManager.playerEventHandler.Aim.Active
    uint64_t handler = rd_ptr(local + PLAYER_EVENT_HANDLER);
    if (handler && rd_ptr(handler + EVENT_HANDLER_MANAGER_BACKREF) == local) {
        uint64_t activity = rd_ptr(handler + EVENT_HANDLER_AIM_ACTIVITY);
        uint8_t active = 0;
        if (activity && rd_exact(activity + ACTIVITY_ACTIVE_FLAG, active) && active <= 1) {
            fresh.event_handler = handler;
            fresh.aim_activity = activity;
            fresh.aiming = (active != 0);
            fresh.source = 1;
            g_aim_state = fresh;
            return true;
        }
    }

    // Fallback: current first-person weapon isAiming flag.
    uint64_t fp_manager = rd_ptr(local + PLAYER_FP_MANAGER);
    if (fp_manager) {
        uint64_t weapon = rd_ptr(fp_manager + FPMANAGER_CURRENT_WEAPON);
        if (weapon && rd_ptr(weapon + FPOBJECT_PLAYER_BACKREF) == local) {
            uint8_t is_aiming = 0;
            if (rd_exact(weapon + FPWEAPON_IS_AIMING, is_aiming) && is_aiming <= 1) {
                fresh.fp_manager = fp_manager;
                fresh.weapon = weapon;
                fresh.aiming = (is_aiming != 0);
                fresh.source = 2;
                g_aim_state = fresh;
                return true;
            }
        }
        // Last resort: the FOV blend factor FPManager drives toward aimFOV.
        float blend = 0.0F;
        if (rd_exact(fp_manager + FPMANAGER_AIM_BLEND, blend) && std::isfinite(blend) && blend >= 0.0F && blend <= 1.0F) {
            fresh.fp_manager = fp_manager;
            fresh.aiming = blend > 0.5F;
            fresh.source = 3;
            g_aim_state = fresh;
            return true;
        }
    }

    g_aim_state = {};
    return false;
}

bool esp_local_player_is_aiming() {
    if (g_pid <= 0 || !g_il2cpp_base) return false;
    if (!read_local_aim_state()) return false;
    return g_aim_state.aiming;
}
