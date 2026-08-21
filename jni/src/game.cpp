#include "game.h"
#include "game_offsets.h"
#include "Vector.h"

#include <string.h>
#include <sys/uio.h>
#include <stdio.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <chrono>
#include <functional>
#include <iterator>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <unistd.h>
#include <sys/syscall.h>

static ssize_t process_vm_readv(pid_t pid, const struct iovec* local_iov, unsigned long liovcnt, const struct iovec* remote_iov, unsigned long riovcnt, unsigned long flags) {
    return syscall(__NR_process_vm_readv, pid, local_iov, liovcnt, remote_iov, riovcnt, flags);
}

using namespace game_offsets;

static pid_t     g_pid         = -1;
static uint64_t  g_il2cpp_base = 0;
static uint64_t  g_player_manager_class = 0;
static uint64_t  g_player_manager_static_fields = 0;
static uint64_t  g_game_controller_class = 0;
static uint64_t  g_local_player = 0;
static bool      g_matrix_configuration_validated = false;
static bool      g_camera_matrix_physical_match = false;
static uint64_t  g_player_position_offset = PLAYER_POSITION;
static float     g_last_camera_fov_deg = -1.f;
static Mat4      g_last_vp{};
static bool      g_last_vp_valid = false;
static std::vector<uint64_t> g_player_snapshot;
static std::chrono::steady_clock::time_point g_player_snapshot_stamp{};

struct PlayerTrack {
    Vec3 pos;
    double t;
    Vec3 vel;
};
// Per-player velocity history used to extrapolate the network tick position to
// the smoothly rendered model. Cleared on (re)attach and on scene reload so a
// reused PlayerManager pointer cannot inherit a dead player's velocity.
static std::unordered_map<uint64_t, PlayerTrack> g_player_track;

struct TransformHierarchyLayout {
    uint64_t data_offset = 0x38;
    uint64_t index_offset = 0x40;
    uint64_t matrices_offset = 0x18;
    uint64_t indices_offset = 0x20;
    bool matrices_indirect = false;
    bool indices_indirect = false;
};
static TransformHierarchyLayout g_transform_hierarchy_layout{};
static bool g_transform_hierarchy_layout_valid = false;

static bool      g_use_direct_player_position = true;
static bool      g_player_position_validated = false;
static std::mutex g_game_mutex;

static bool vec3_is_finite(const Vec3& value);
static bool player_crouching(uint64_t player);

template<typename T>
static T rd(uint64_t addr) {
    T v{};
    struct iovec lv = { &v, sizeof(T) };
    struct iovec rv = { (void*)addr, sizeof(T) };
    process_vm_readv(g_pid, &lv, 1, &rv, 1, 0);
    return v;
}
template<typename T>
static bool rd_exact(uint64_t addr, T& value) {
    value = {};
    if (!addr) return false;
    struct iovec local = {&value, sizeof(T)};
    struct iovec remote = {(void*)addr, sizeof(T)};
    return process_vm_readv(g_pid, &local, 1, &remote, 1, 0) == (ssize_t)sizeof(T);
}
static uint64_t rd_ptr(uint64_t a) { return rd<uint64_t>(a); }
static Vec3     rd_v3 (uint64_t a) { return rd<Vec3>(a);     }
static Mat4     rd_m4 (uint64_t a) { return rd<Mat4>(a);     }

static std::string read_remote_string(uint64_t address) {
    if (!address) return {};
    char buffer[96]{};
    struct iovec local = {buffer, sizeof(buffer) - 1};
    struct iovec remote = {(void*)address, sizeof(buffer) - 1};
    ssize_t count = process_vm_readv(g_pid, &local, 1, &remote, 1, 0);
    if (count <= 0) return {};
    buffer[sizeof(buffer) - 1] = '\0';
    return std::string(buffer);
}

static bool remote_string_equals(uint64_t address, const char* expected) {
    if (!address || !expected) return false;
    return read_remote_string(address) == expected;
}

static uint64_t get_base(const char* lib) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", g_pid);
    FILE* file = fopen(path, "r");
    if (!file) return 0;
    char line[512];
    uint64_t fallback = 0;
    while (fgets(line, sizeof(line), file)) {
        if (!strstr(line, lib)) continue;
        uint64_t start = 0, end = 0, file_offset = 0;
        char permissions[5]{};
        if (sscanf(line, "%lx-%lx %4s %lx", &start, &end, permissions, &file_offset) != 4) continue;
        uint64_t load_bias = start - file_offset;
        if (!fallback || load_bias < fallback) fallback = load_bias;
        if (file_offset == 0) { fclose(file); return start; }
    }
    fclose(file);
    return fallback;
}

static bool validate_player_list(uint64_t list, uint64_t player_class) {
    if (!list || !player_class) return false;
    uint64_t items = rd_ptr(list + IL2CPP_LIST_ITEMS);
    int32_t count = rd<int32_t>(list + IL2CPP_LIST_SIZE);
    if (!items || count < 0 || count > 512) return false;
    if (count == 0) {
        uint64_t list_class = rd_ptr(list);
        return remote_string_equals(rd_ptr(list_class + 0x10), "List`1") &&
            remote_string_equals(rd_ptr(list_class + 0x18), "System.Collections.Generic");
    }
    int32_t checked = 0;
    for (int32_t index = 0; index < count && checked < 4; ++index) {
        uint64_t player = rd_ptr(items + IL2CPP_ARRAY_FIRST_ELEMENT + (uint64_t)index * sizeof(uint64_t));
        if (!player) continue;
        if (rd_ptr(player) != player_class) return false;
        ++checked;
    }
    return checked > 0;
}

static bool player_list_contains(uint64_t list, uint64_t player) {
    if (!list || !player) return false;
    uint64_t items = rd_ptr(list + IL2CPP_LIST_ITEMS);
    int32_t count = rd<int32_t>(list + IL2CPP_LIST_SIZE);
    if (!items || count <= 0 || count > 512) return false;
    for (int32_t index = 0; index < count; ++index) {
        if (rd_ptr(items + IL2CPP_ARRAY_FIRST_ELEMENT + (uint64_t)index * sizeof(uint64_t)) == player)
            return true;
    }
    return false;
}

static constexpr uint64_t IL2CPP_CLASS_STATIC_FIELDS = 0xB8;

static uint64_t get_class_static_fields(uint64_t klass) {
    if (!klass) return 0;
    return rd_ptr(klass + IL2CPP_CLASS_STATIC_FIELDS);
}

static uint64_t resolve_runtime_player_list() {
    if (!g_player_manager_class && PLAYER_MANAGER_TYPEINFO_RVA != 0) {
        uint64_t candidate = rd_ptr(g_il2cpp_base + PLAYER_MANAGER_TYPEINFO_RVA);
        if (candidate) {
            std::string name = read_remote_string(rd_ptr(candidate + 0x10));
            std::string ns   = read_remote_string(rd_ptr(candidate + 0x18));
            if (name == "PlayerManager" && ns == "Oxide")
                g_player_manager_class = candidate;
        }
    }
    if (!g_player_manager_class) {
        return 0;
    }
    if (!g_player_manager_static_fields)
        g_player_manager_static_fields = get_class_static_fields(g_player_manager_class);
    if (!g_player_manager_static_fields) {
        return 0;
    }
    uint64_t list = rd_ptr(g_player_manager_static_fields + PLAYER_MANAGER_STATIC_FIELDS_LIST);
    if (!validate_player_list(list, g_player_manager_class)) {
        g_player_manager_static_fields = 0;
        return 0;
    }
    return list;
}

static uint64_t resolve_local_player() {
    if (g_local_player && rd_ptr(g_local_player) == g_player_manager_class)
        return g_local_player;
    g_local_player = 0;

    if (!g_game_controller_class && GAME_CONTROLLER_TYPEINFO_RVA != 0) {
        uint64_t candidate = rd_ptr(g_il2cpp_base + GAME_CONTROLLER_TYPEINFO_RVA);
        if (candidate) {
            std::string name = read_remote_string(rd_ptr(candidate + 0x10));
            std::string ns   = read_remote_string(rd_ptr(candidate + 0x18));
            if (name == "GameControllerBase" && ns == "Oxide")
                g_game_controller_class = candidate;
        }
    }

    if (!g_game_controller_class || !g_player_manager_class) return 0;

    uint64_t gcb_static_fields = get_class_static_fields(g_game_controller_class);
    if (!gcb_static_fields) return 0;
    uint64_t local_player = rd_ptr(gcb_static_fields + GAME_CONTROLLER_LOCAL_PLAYER_FIELD);
    if (local_player && rd_ptr(local_player) == g_player_manager_class) {
        g_local_player = local_player;
        return local_player;
    }
    return 0;
}

static uint64_t resolve_native_transform(uint64_t transform) {
    if (!transform) return 0;
    return rd_ptr(transform + MANAGED_CACHED_PTR);
}

static bool vec3_is_finite(const Vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) &&
        fabsf(value.x) < 1000000.0F && fabsf(value.y) < 1000000.0F && fabsf(value.z) < 1000000.0F;
}

static Vec3 cross_product(const Vec3& left, const Vec3& right) {
    return {left.y * right.z - left.z * right.y, left.z * right.x - left.x * right.z, left.x * right.y - left.y * right.x};
}

static Vec3 rotate_vector(const Vec4& quaternion, const Vec3& vector) {
    Vec3 q = {quaternion.x, quaternion.y, quaternion.z};
    Vec3 first_cross = cross_product(q, vector);
    Vec3 doubled = {first_cross.x * 2.0F, first_cross.y * 2.0F, first_cross.z * 2.0F};
    Vec3 second_cross = cross_product(q, doubled);
    return {vector.x + quaternion.w * doubled.x + second_cross.x, vector.y + quaternion.w * doubled.y + second_cross.y, vector.z + quaternion.w * doubled.z + second_cross.z};
}

static Vec4 multiply_quaternion(const Vec4& left, const Vec4& right) {
    return {
        left.w * right.x + left.x * right.w + left.y * right.z - left.z * right.y,
        left.w * right.y - left.x * right.z + left.y * right.w + left.z * right.x,
        left.w * right.z + left.x * right.y - left.y * right.x + left.z * right.w,
        left.w * right.w - left.x * right.x - left.y * right.y - left.z * right.z
    };
}

static bool normalize_quaternion(Vec4& quaternion) {
    float length_squared = quaternion.x * quaternion.x + quaternion.y * quaternion.y + quaternion.z * quaternion.z + quaternion.w * quaternion.w;
    if (!std::isfinite(length_squared) || length_squared < 0.000001F) return false;
    float inverse_length = 1.0F / sqrtf(length_squared);
    quaternion.x *= inverse_length; quaternion.y *= inverse_length; quaternion.z *= inverse_length; quaternion.w *= inverse_length;
    return true;
}

static bool matrix34_is_valid(const Matrix34& matrix) {
    const float values[] = {
        matrix.translation.x, matrix.translation.y, matrix.translation.z, matrix.translation.w,
        matrix.rotation.x, matrix.rotation.y, matrix.rotation.z, matrix.rotation.w,
        matrix.scale.x, matrix.scale.y, matrix.scale.z, matrix.scale.w
    };
    for (float value : values) { if (!std::isfinite(value) || fabsf(value) > 1000000.0F) return false; }
    float quaternion_length = matrix.rotation.x * matrix.rotation.x + matrix.rotation.y * matrix.rotation.y + matrix.rotation.z * matrix.rotation.z + matrix.rotation.w * matrix.rotation.w;
    return quaternion_length >= 0.20F && quaternion_length <= 2.0F && fabsf(matrix.scale.x) <= 10000.0F && fabsf(matrix.scale.y) <= 10000.0F && fabsf(matrix.scale.z) <= 10000.0F;
}

static bool read_transform_hierarchy_arrays(uint64_t matrices, uint64_t indices, int32_t transform_index, Vec3& position, Vec4* world_rotation = nullptr) {
    if (!matrices || !indices || transform_index < 0 || transform_index > 100000) return false;
    Matrix34 current{};
    if (!rd_exact(matrices + (uint64_t)transform_index * sizeof(Matrix34), current) || !matrix34_is_valid(current)) return false;
    Vec3 result = {current.translation.x, current.translation.y, current.translation.z};
    Vec4 result_rotation = current.rotation;
    if (!vec3_is_finite(result)) return false;
    int32_t parent = -2;
    if (!rd_exact(indices + (uint64_t)transform_index * sizeof(int32_t), parent)) return false;
    int32_t previous_parent = transform_index;
    int depth = 0;
    while (parent >= 0 && depth++ < 128) {
        if (parent > 100000 || parent == previous_parent) return false;
        Matrix34 matrix{};
        if (!rd_exact(matrices + (uint64_t)parent * sizeof(Matrix34), matrix) || !matrix34_is_valid(matrix)) return false;
        Vec3 scaled = {result.x * matrix.scale.x, result.y * matrix.scale.y, result.z * matrix.scale.z};
        Vec3 rotated = rotate_vector(matrix.rotation, scaled);
        result = {matrix.translation.x + rotated.x, matrix.translation.y + rotated.y, matrix.translation.z + rotated.z};
        result_rotation = multiply_quaternion(matrix.rotation, result_rotation);
        if (!vec3_is_finite(result)) return false;
        previous_parent = parent;
        if (!rd_exact(indices + (uint64_t)parent * sizeof(int32_t), parent)) return false;
    }
    if (parent != -1 || depth >= 128 || !vec3_is_finite(result)) return false;
    if (world_rotation) { if (!normalize_quaternion(result_rotation)) return false; *world_rotation = result_rotation; }
    position = result;
    return true;
}

static bool read_transform_hierarchy_layout(uint64_t native_transform, const TransformHierarchyLayout& layout, Vec3& position, Vec4* world_rotation = nullptr) {
    if (!native_transform) return false;
    uint64_t transform_data = rd_ptr(native_transform + layout.data_offset);
    int32_t transform_index = rd<int32_t>(native_transform + layout.index_offset);
    if (!transform_data || transform_index < 0 || transform_index > 100000) return false;
    uint64_t matrices = rd_ptr(transform_data + layout.matrices_offset);
    uint64_t indices = rd_ptr(transform_data + layout.indices_offset);
    if (layout.matrices_indirect) matrices = rd_ptr(matrices);
    if (layout.indices_indirect) indices = rd_ptr(indices);
    return read_transform_hierarchy_arrays(matrices, indices, transform_index, position, world_rotation);
}

static bool read_transform_hierarchy_position(uint64_t native_transform, Vec3& position) {
    if (!native_transform) return false;
    if (g_transform_hierarchy_layout_valid)
        return read_transform_hierarchy_layout(native_transform, g_transform_hierarchy_layout, position);
    uint64_t transform_data = rd_ptr(native_transform + 0x38);
    int32_t transform_index = rd<int32_t>(native_transform + 0x40);
    if (!transform_data || transform_index < 0 || transform_index > 100000) return false;
    const uint64_t data_offsets[][2] = {{0x18, 0x20}, {0x08, 0x10}};
    for (const auto& offsets : data_offsets) {
        uint64_t matrix_pointer = rd_ptr(transform_data + offsets[0]);
        uint64_t index_pointer = rd_ptr(transform_data + offsets[1]);
        if (!matrix_pointer || !index_pointer) continue;
        const uint64_t matrix_candidates[] = {matrix_pointer, rd_ptr(matrix_pointer)};
        const uint64_t index_candidates[] = {index_pointer, rd_ptr(index_pointer)};
        for (uint64_t matrices : matrix_candidates) {
            for (uint64_t indices_ptr : index_candidates) {
                if (read_transform_hierarchy_arrays(matrices, indices_ptr, transform_index, position)) return true;
            }
        }
    }
    return false;
}

static uint64_t resolve_player_native_transform(uint64_t player) {
    if (!player) return 0;
    return resolve_native_transform(rd_ptr(player + PLAYER_TRANSFORM));
}

static bool likely_native_pointer(uint64_t value) {
    return value >= 0x10000 && value < 0x0001000000000000ULL && (value & 0x7) == 0;
}

// ---------------------------------------------------------------------------
// Live model skeleton
// ---------------------------------------------------------------------------
// Reads the enemy model's humanoid rig each frame so the drawn skeleton tracks
// the actual bones: running, aiming, reloading and crouching all move the
// bones and therefore the skeleton automatically.  Bone positions come from
// the animated rig itself, not from guessed offsets — the same data the game
// uses to render the model.
//
// Chain:  PlayerManager.animator (managed Animator) -> m_CachedPtr
//         -> native Animator -> Avatar -> AvatarData
//         -> m_HumanBoneIndex[HumanBodyBones] -> TransformOffsetStructure
//         -> transform index -> existing Transform hierarchy walk.
//
// The Animator/Avatar/AvatarData native offsets are Unity engine internals
// (not present in the dump); they are discovered and validated at runtime
// against a humanoid-proportion check, so a wrong guess falls back to
// scanning instead of drawing garbage.
//
// The resulting positions are real world-space rig data.  No calibration or
// anchoring shift is applied to them: shifting is what previously made the
// whole skeleton float above the model.

// HumanBodyBones enum values for the 20 bones we draw, in draw order
// (matches UnityEngine.HumanBodyBones from the dump).
static constexpr int kSkeletonBoneCount = 20;
static constexpr int kSkeletonBoneHuman[kSkeletonBoneCount] = {
    11 /*Head*/,       10 /*Neck*/,      9 /*UpperChest*/, 8 /*Chest*/, 7 /*Spine*/, 0 /*Hips*/,
    12 /*LShoulder*/,  14 /*LUpperArm*/, 16 /*LLowerArm*/,  18 /*LHand*/,
    13 /*RShoulder*/,  15 /*RUpperArm*/, 17 /*RLowerArm*/,  19 /*RHand*/,
    1  /*LUpperLeg*/,  3  /*LLowerLeg*/, 5  /*LFoot*/,
    2  /*RUpperLeg*/,  4  /*RLowerLeg*/, 6  /*RFoot*/
};

// Resolved (and validated) Avatar layout. All engine-internal offsets live
// here; they are discovered once and cached for the session.
struct BoneLayout {
    uint64_t animator_avatar_off = ANIMATOR_AVATAR;
    uint64_t avatar_data_off     = AVATAR_DATA;
    uint64_t avatar_size_off     = AVATAR_SIZE;
    uint64_t tos_off             = AVATAR_DATA_TOS;
    uint64_t hbi_off             = AVATAR_DATA_HUMAN_BONE_INDEX;
    uint64_t tos_stride          = TOS_STRIDE;
    uint64_t tos_count_off       = TOS_COUNT_OFF;
    uint64_t tos_first_index_off = TOS_FIRST_INDEX_OFF;
    bool     use_tos             = true;
};
static BoneLayout g_bone_layout{};
static bool g_bone_layout_valid = false;
static std::chrono::steady_clock::time_point g_bone_layout_retry_after{};
static bool g_bone_layout_extended = false; // tier-1 scan failed: widen the search
static int g_bone_layout_failures = 0;      // consecutive failed discovery attempts

static std::mutex g_skeleton_mutex;
// PlayerManager pointer set from the previous scene. A wholesale replacement
// (server switch / scene reload) invalidates the cached bone layout, because
// the engine-side Avatar allocation changes with the scene.
static std::unordered_set<uint64_t> g_skeleton_scene_players;

// Callers must hold g_skeleton_mutex.
static void skeleton_invalidate_locked() {
    g_bone_layout = {};
    g_bone_layout_valid = false;
    g_bone_layout_retry_after = {};
    g_bone_layout_extended = false;
    g_bone_layout_failures = 0;
}

// Extract the shared Transform-hierarchy matrices/indices arrays from any
// native Transform (all scene transforms share one hierarchy).  These are the
// same arrays the world-position reader uses, so a layout validated there is
// reused here.
static bool get_hierarchy_arrays(uint64_t native_transform, uint64_t& matrices, uint64_t& indices) {
    matrices = 0; indices = 0;
    if (!native_transform) return false;

    if (g_transform_hierarchy_layout_valid) {
        uint64_t transform_data = rd_ptr(native_transform + g_transform_hierarchy_layout.data_offset);
        int32_t transform_index = rd<int32_t>(native_transform + g_transform_hierarchy_layout.index_offset);
        if (likely_native_pointer(transform_data) && transform_index >= 0 && transform_index <= 100000) {
            uint64_t m = rd_ptr(transform_data + g_transform_hierarchy_layout.matrices_offset);
            uint64_t ix = rd_ptr(transform_data + g_transform_hierarchy_layout.indices_offset);
            if (g_transform_hierarchy_layout.matrices_indirect) m = rd_ptr(m);
            if (g_transform_hierarchy_layout.indices_indirect) ix = rd_ptr(ix);
            if (likely_native_pointer(m) && likely_native_pointer(ix)) {
                matrices = m; indices = ix;
                return true;
            }
        }
    }

    // Standard Unity transform hierarchy layout (data=0x38/index=0x40, arrays
    // at +0x18/+0x20 or +0x08/+0x10, with or without an extra indirection).
    uint64_t transform_data = rd_ptr(native_transform + 0x38);
    if (!likely_native_pointer(transform_data)) return false;
    const uint64_t data_offsets[][2] = {{0x18, 0x20}, {0x08, 0x10}};
    for (const auto& offsets : data_offsets) {
        uint64_t matrix_pointer = rd_ptr(transform_data + offsets[0]);
        uint64_t index_pointer = rd_ptr(transform_data + offsets[1]);
        if (!likely_native_pointer(matrix_pointer) || !likely_native_pointer(index_pointer)) continue;
        const uint64_t matrix_candidates[] = {matrix_pointer, rd_ptr(matrix_pointer)};
        const uint64_t index_candidates[] = {index_pointer, rd_ptr(index_pointer)};
        for (uint64_t m : matrix_candidates)
            for (uint64_t ix : index_candidates)
                if (likely_native_pointer(m) && likely_native_pointer(ix)) {
                    matrices = m; indices = ix;
                    return true;
                }
    }
    return false;
}

// Resolve a player's native Animator (managed Animator -> m_CachedPtr).
// PlayerManager.animator is dump-confirmed; the KCC fallbacks
// (m_Animator, CharacterAnimation.animator) are too.
static uint64_t resolve_native_animator(uint64_t player) {
    if (!player) return 0;

    auto managed_to_native = [](uint64_t managed) -> uint64_t {
        if (!likely_native_pointer(managed)) return 0;
        uint64_t native = rd_ptr(managed + MANAGED_CACHED_PTR);
        return likely_native_pointer(native) ? native : 0;
    };

    // Primary: PlayerManager.animator.
    uint64_t native = managed_to_native(rd_ptr(player + PLAYER_ANIMATOR));
    if (native) return native;

    // Fallback: the player's KCC (InterfaceReference<ti>) owns the live model:
    // KCC.m_Animator, then KCC.pjq (CharacterAnimation).animator.
    uint64_t reference = rd_ptr(player + PLAYER_KCC_REFERENCE);
    const uint64_t candidates[] = {
        reference,
        reference ? rd_ptr(reference + 0x10) : 0,
        reference ? rd_ptr(reference + 0x18) : 0,
        reference ? rd_ptr(reference + 0x20) : 0
    };
    for (uint64_t candidate : candidates) {
        if (!likely_native_pointer(candidate)) continue;
        if (rd_ptr(candidate + KCC_PLAYER) != player) continue;
        native = managed_to_native(rd_ptr(candidate + KCC_ANIMATOR));
        if (native) return native;
        uint64_t character_animation = rd_ptr(candidate + KCC_CHARACTER_ANIMATION);
        if (likely_native_pointer(character_animation))
            native = managed_to_native(rd_ptr(character_animation + CHARACTER_ANIMATION_ANIMATOR));
        return native;
    }
    return 0;
}

// Resolve the world positions of all 20 bones given a native Animator and the
// shared hierarchy arrays. Returns false when ANY bone fails, so a partial or
// mis-mapped rig is never drawn.
static bool read_bone_positions(uint64_t native_animator, uint64_t matrices, uint64_t indices, Vec3* out) {
    if (!g_bone_layout_valid || !native_animator || !matrices || !indices || !out) return false;

    uint64_t avatar = rd_ptr(native_animator + g_bone_layout.animator_avatar_off);
    if (!likely_native_pointer(avatar)) return false;
    uint64_t avatar_data = rd_ptr(avatar + g_bone_layout.avatar_data_off);
    if (!likely_native_pointer(avatar_data)) return false;
    uint32_t size = rd<uint32_t>(avatar + g_bone_layout.avatar_size_off);
    if (size < 20 || size > 5000) return false;
    uint64_t hbi = rd_ptr(avatar_data + g_bone_layout.hbi_off);
    if (!likely_native_pointer(hbi)) return false;
    uint64_t tos = 0;
    if (g_bone_layout.use_tos) {
        tos = rd_ptr(avatar_data + g_bone_layout.tos_off);
        if (!likely_native_pointer(tos)) return false;
    }

    for (int i = 0; i < kSkeletonBoneCount; ++i) {
        uint32_t node = rd<uint32_t>(hbi + (uint64_t)kSkeletonBoneHuman[i] * 4u);
        if (node >= size) return false;
        int32_t transform_index;
        if (g_bone_layout.use_tos) {
            uint64_t entry = tos + (uint64_t)node * g_bone_layout.tos_stride;
            uint32_t count = rd<uint32_t>(entry + g_bone_layout.tos_count_off);
            uint32_t first = rd<uint32_t>(entry + g_bone_layout.tos_first_index_off);
            if (count < 1 || count > 8) return false;
            transform_index = (int32_t)first;
        } else {
            transform_index = (int32_t)node;
        }
        Vec3 position{};
        if (!read_transform_hierarchy_arrays(matrices, indices, transform_index, position)) return false;
        out[i] = position;
    }
    return true;
}

// Sanity-check a resolved skeleton: head above neck above hips above feet,
// roughly human proportions, and — crucially — the rig must belong to THIS
// player (hips near the network position, feet near ground level).  This
// rejects bogus Avatar layouts and other players' skeletons before anything
// is drawn.
static bool skeleton_positions_plausible(const Vec3* bones, const Vec3& feet) {
    for (int i = 0; i < kSkeletonBoneCount; ++i)
        if (!vec3_is_finite(bones[i])) return false;

    const float head_y = bones[0].y;   // Head
    const float neck_y = bones[1].y;   // Neck
    const float hips_y = bones[5].y;   // Hips
    const float foot_y = std::min(bones[16].y, bones[19].y); // LFoot / RFoot

    float body_height = head_y - foot_y;
    if (body_height < 0.35f || body_height > 3.0f) return false;
    if (neck_y > head_y + 0.05f || neck_y < hips_y - 0.05f) return false;
    if (hips_y < foot_y || hips_y > head_y + 0.1f) return false;

    // Horizontal coherence between head and hips (rejects unrelated transforms).
    float dx = bones[0].x - bones[5].x;
    float dz = bones[0].z - bones[5].z;
    if (dx * dx + dz * dz > 4.0f) return false;

    // Proximity to the player's network root: hips within ~3 m horizontally
    // and the feet near ground level.  Prevents drawing another model's rig.
    float hx = bones[5].x - feet.x;
    float hy = bones[5].y - feet.y;
    float hz = bones[5].z - feet.z;
    if (hx * hx + hz * hz > 9.0f) return false;
    if (!std::isfinite(hy) || fabsf(hy) > 2.2f) return false;
    float fy = foot_y - feet.y;
    if (!std::isfinite(fy) || fabsf(fy) > 1.2f) return false;
    return true;
}

static bool try_bone_layout(const BoneLayout& layout, uint64_t native_animator,
                            uint64_t matrices, uint64_t indices, const Vec3& feet) {
    BoneLayout saved = g_bone_layout;
    bool saved_valid = g_bone_layout_valid;
    g_bone_layout = layout;
    g_bone_layout_valid = true;

    Vec3 bones[kSkeletonBoneCount]{};
    bool ok = read_bone_positions(native_animator, matrices, indices, bones) &&
              skeleton_positions_plausible(bones, feet);

    g_bone_layout = saved;
    g_bone_layout_valid = saved_valid;
    return ok;
}

// One-time discovery of the Avatar native offsets (Unity engine internals, not
// present in the dump). Scans plausible offsets and keeps the first layout
// that produces a humanoid-shaped skeleton belonging to the player. Cached
// for the rest of the session; invalidated on scene switches.
static bool scan_bone_layout(uint64_t native_animator, uint64_t matrices, uint64_t indices,
                             const Vec3& feet,
                             const uint64_t* animator_avatar_offs, size_t animator_avatar_count,
                             const uint64_t* avatar_data_offs, size_t avatar_data_count,
                             const uint64_t* avatar_size_offs, size_t avatar_size_count,
                             const uint64_t* hbi_offs, size_t hbi_count,
                             const uint64_t* tos_offs, size_t tos_count) {
    const uint64_t tos_strides[] = {0x18, 0x10, 0x20, 0x08};
    const uint64_t tos_count_offs[] = {0x08, 0x00};

    for (size_t ai = 0; ai < animator_avatar_count; ++ai) {
        uint64_t aoff = animator_avatar_offs[ai];
        uint64_t avatar = rd_ptr(native_animator + aoff);
        if (!likely_native_pointer(avatar)) continue;

        for (size_t di = 0; di < avatar_data_count; ++di) {
            uint64_t doff = avatar_data_offs[di];
            uint64_t avatar_data = rd_ptr(avatar + doff);
            if (!likely_native_pointer(avatar_data)) continue;

            for (size_t si = 0; si < avatar_size_count; ++si) {
                uint64_t soff = avatar_size_offs[si];
                uint32_t size = rd<uint32_t>(avatar + soff);
                if (size < 20 || size > 5000) continue;

                for (size_t hi = 0; hi < hbi_count; ++hi) {
                    uint64_t hoff = hbi_offs[hi];
                    uint64_t hbi = rd_ptr(avatar_data + hoff);
                    if (!likely_native_pointer(hbi)) continue;

                    // Cheap pre-check on key bone node indices before doing the
                    // expensive hierarchy walks.
                    uint32_t head  = rd<uint32_t>(hbi + 11u * 4u);
                    uint32_t hips  = rd<uint32_t>(hbi + 0u * 4u);
                    uint32_t lfoot = rd<uint32_t>(hbi + 5u * 4u);
                    uint32_t rfoot = rd<uint32_t>(hbi + 6u * 4u);
                    if (head >= size || hips >= size || lfoot >= size || rfoot >= size) continue;
                    if (head == hips && head == lfoot && head == rfoot) continue;

                    BoneLayout candidate{};
                    candidate.animator_avatar_off = aoff;
                    candidate.avatar_data_off = doff;
                    candidate.avatar_size_off = soff;
                    candidate.hbi_off = hoff;
                    candidate.use_tos = false;
                    if (try_bone_layout(candidate, native_animator, matrices, indices, feet)) {
                        g_bone_layout = candidate;
                        g_bone_layout_valid = true;
                        return true;
                    }

                    for (size_t ti = 0; ti < tos_count; ++ti) {
                        uint64_t toff = tos_offs[ti];
                        uint64_t tos = rd_ptr(avatar_data + toff);
                        if (!likely_native_pointer(tos)) continue;
                        for (uint64_t stride : tos_strides) {
                            for (uint64_t count_off : tos_count_offs) {
                                candidate.use_tos = true;
                                candidate.tos_off = toff;
                                candidate.tos_stride = stride;
                                candidate.tos_count_off = count_off;
                                candidate.tos_first_index_off = count_off + 4;
                                if (try_bone_layout(candidate, native_animator, matrices, indices, feet)) {
                                    g_bone_layout = candidate;
                                    g_bone_layout_valid = true;
                                    return true;
                                }
                                std::swap(candidate.tos_count_off, candidate.tos_first_index_off);
                                if (try_bone_layout(candidate, native_animator, matrices, indices, feet)) {
                                    g_bone_layout = candidate;
                                    g_bone_layout_valid = true;
                                    return true;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    return false;
}

static bool discover_bone_layout(uint64_t native_animator, uint64_t matrices,
                                 uint64_t indices, const Vec3& feet) {
    if (!likely_native_pointer(native_animator)) return false;

    // Fast path: the known-good defaults for this Unity branch, both with and
    // without the TransformOffsetStructure indirection.
    {
        BoneLayout candidate{};
        candidate.use_tos = false;
        if (try_bone_layout(candidate, native_animator, matrices, indices, feet)) {
            g_bone_layout = candidate;
            g_bone_layout_valid = true;
            return true;
        }
        candidate = BoneLayout{};
        candidate.use_tos = true;
        if (try_bone_layout(candidate, native_animator, matrices, indices, feet)) {
            g_bone_layout = candidate;
            g_bone_layout_valid = true;
            return true;
        }
    }

    // Tier 1: the offset space validated against this game (Unity 2021/2022
    // engine branch).  Kept first because it is the smallest space that is
    // known to work.
    if (!g_bone_layout_extended) {
        const uint64_t animator_avatar_offs[] = {0x38, 0x40, 0x48, 0x50, 0x58, 0x60, 0x68, 0x70, 0x78,
                                                 0x80, 0x88, 0x90, 0x98, 0xA0, 0xA8, 0xB0, 0xB8, 0xC0};
        const uint64_t avatar_data_offs[] = {0x20, 0x28, 0x30, 0x38, 0x40, 0x48};
        const uint64_t avatar_size_offs[] = {0x28, 0x30, 0x38, 0x40};
        const uint64_t hbi_offs[] = {0x38, 0x40, 0x48, 0x50};
        const uint64_t tos_offs[] = {0x10, 0x08, 0x18};
        if (scan_bone_layout(native_animator, matrices, indices, feet,
                             animator_avatar_offs, std::size(animator_avatar_offs),
                             avatar_data_offs, std::size(avatar_data_offs),
                             avatar_size_offs, std::size(avatar_size_offs),
                             hbi_offs, std::size(hbi_offs),
                             tos_offs, std::size(tos_offs)))
            return true;
        // Not found this attempt; widen the search on the next retry instead
        // of burning more time in this frame.
        g_bone_layout_extended = true;
        return false;
    }

    // Tier 2: a superset of tier 1 (plus wider bounds), in case a game update
    // moved the engine internals (e.g. a newer Unity LTS).  Re-scanning the
    // tier-1 space also recovers from a transient tier-1 failure (e.g. the
    // discovery ran while the sample player was mid-ragdoll).
    {
        const uint64_t animator_avatar_offs[] = {0x28, 0x30, 0x38, 0x40, 0x48, 0x50, 0x58, 0x60, 0x68, 0x70,
                                                 0x78, 0x80, 0x88, 0x90, 0x98, 0xA0, 0xA8, 0xB0, 0xB8, 0xC0,
                                                 0xC8, 0xD0, 0xD8, 0xE0, 0xE8, 0xF0, 0xF8, 0x100};
        const uint64_t avatar_data_offs[] = {0x18, 0x20, 0x28, 0x30, 0x38, 0x40, 0x48, 0x50, 0x58, 0x60, 0x68, 0x70};
        const uint64_t avatar_size_offs[] = {0x20, 0x28, 0x30, 0x38, 0x40, 0x48, 0x50, 0x58, 0x60};
        const uint64_t hbi_offs[] = {0x18, 0x20, 0x28, 0x30, 0x38, 0x40, 0x48, 0x50, 0x58, 0x60, 0x68, 0x70};
        const uint64_t tos_offs[] = {0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38, 0x40, 0x48};
        if (scan_bone_layout(native_animator, matrices, indices, feet,
                             animator_avatar_offs, std::size(animator_avatar_offs),
                             avatar_data_offs, std::size(avatar_data_offs),
                             avatar_size_offs, std::size(avatar_size_offs),
                             hbi_offs, std::size(hbi_offs),
                             tos_offs, std::size(tos_offs)))
            return true;
    }
    return false;
}

static void build_fallback_skeleton(const Vec3& feet, float height,
                                    std::vector<std::pair<Vec3, Vec3>>& segments,
                                    Vec3& head, Vec3& chest, Vec3& pelvis) {
    if (!std::isfinite(height) || height < 0.5f) height = PLAYER_HEIGHT;
    head = {feet.x, feet.y + height, feet.z};
    chest = {feet.x, feet.y + height * 0.66f, feet.z};
    pelvis = {feet.x, feet.y + height * 0.47f, feet.z};

    const Vec3 neck = {feet.x, feet.y + height * 0.80f, feet.z};
    const Vec3 left_shoulder = {feet.x - height * 0.18f, feet.y + height * 0.70f, feet.z};
    const Vec3 right_shoulder = {feet.x + height * 0.18f, feet.y + height * 0.70f, feet.z};
    const Vec3 left_elbow = {feet.x - height * 0.28f, feet.y + height * 0.51f, feet.z};
    const Vec3 right_elbow = {feet.x + height * 0.28f, feet.y + height * 0.51f, feet.z};
    const Vec3 left_hand = {feet.x - height * 0.32f, feet.y + height * 0.34f, feet.z};
    const Vec3 right_hand = {feet.x + height * 0.32f, feet.y + height * 0.34f, feet.z};
    const Vec3 left_hip = {feet.x - height * 0.12f, feet.y + height * 0.43f, feet.z};
    const Vec3 right_hip = {feet.x + height * 0.12f, feet.y + height * 0.43f, feet.z};
    const Vec3 left_knee = {feet.x - height * 0.13f, feet.y + height * 0.23f, feet.z};
    const Vec3 right_knee = {feet.x + height * 0.13f, feet.y + height * 0.23f, feet.z};
    const Vec3 left_foot = {feet.x - height * 0.14f, feet.y, feet.z};
    const Vec3 right_foot = {feet.x + height * 0.14f, feet.y, feet.z};

    const std::pair<Vec3, Vec3> fallback_lines[] = {
        {head, neck}, {neck, chest}, {chest, pelvis},
        {neck, left_shoulder}, {left_shoulder, left_elbow}, {left_elbow, left_hand},
        {neck, right_shoulder}, {right_shoulder, right_elbow}, {right_elbow, right_hand},
        {pelvis, left_hip}, {left_hip, left_knee}, {left_knee, left_foot},
        {pelvis, right_hip}, {right_hip, right_knee}, {right_knee, right_foot}
    };
    segments.assign(std::begin(fallback_lines), std::end(fallback_lines));
}

static bool read_skeleton_segments(uint64_t player, const Vec3& feet,
                                   std::vector<std::pair<Vec3, Vec3>>& segments,
                                   Vec3& animated_head,
                                   Vec3& skeleton_chest,
                                   Vec3& skeleton_pelvis) {
    std::lock_guard<std::mutex> skeleton_lock(g_skeleton_mutex);
    segments.clear();
    animated_head = {};
    skeleton_chest = {};
    skeleton_pelvis = {};
    if (!player) return false;

    auto use_fallback = [&]() -> bool {
        float height = player_crouching(player) ? 1.12f : PLAYER_HEIGHT;
        build_fallback_skeleton(feet, height, segments, animated_head,
                                skeleton_chest, skeleton_pelvis);
        return !segments.empty();
    };

    uint64_t native_animator = resolve_native_animator(player);
    if (!likely_native_pointer(native_animator)) return use_fallback();

    uint64_t matrices = 0, indices = 0;
    if (!get_hierarchy_arrays(resolve_player_native_transform(player), matrices, indices))
        return use_fallback();

    if (!g_bone_layout_valid) {
        // Discovery scans many offsets; only retry a few times per second so a
        // layout that never validates can't tank the frame rate.  After a few
        // consecutive failures, back off to a slow retry cadence.
        auto now = std::chrono::steady_clock::now();
        if (now < g_bone_layout_retry_after) return use_fallback();
        g_bone_layout_retry_after = now + std::chrono::milliseconds(750);
        if (++g_bone_layout_failures > 1) {
            g_bone_layout_retry_after = now + std::chrono::seconds(5);
            g_bone_layout_failures = 0;
        }
        discover_bone_layout(native_animator, matrices, indices, feet);
    }
    if (!g_bone_layout_valid) return use_fallback();

    Vec3 bones[kSkeletonBoneCount]{};
    if (!read_bone_positions(native_animator, matrices, indices, bones) ||
        !skeleton_positions_plausible(bones, feet))
        return use_fallback();

    // All 20 bones resolved: build the 19 humanoid segments.  The positions
    // are real world-space rig data, so the skeleton sits exactly on the
    // rendered model with no shift applied.
    const int kSegments[][2] = {
        {0, 1},  {1, 2},  {2, 3},  {3, 4},  {4, 5},   // head -> neck -> chest -> spine -> hips
        {2, 6},  {6, 7},  {7, 8},  {8, 9},            // left  arm
        {2, 10}, {10, 11}, {11, 12}, {12, 13},        // right arm
        {5, 14}, {14, 15}, {15, 16},                  // left  leg
        {5, 17}, {17, 18}, {18, 19}                   // right leg
    };
    segments.reserve(19);
    for (const auto& link : kSegments) {
        const Vec3& a = bones[link[0]];
        const Vec3& b = bones[link[1]];
        float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
        float length = sqrtf(dx * dx + dy * dy + dz * dz);
        if (!std::isfinite(length) || length < 0.005f || length > 4.f) return use_fallback();
        segments.emplace_back(a, b);
    }

    animated_head = bones[0];   // Head
    skeleton_chest = bones[2];  // UpperChest
    skeleton_pelvis = bones[5]; // Hips
    return !segments.empty() && vec3_is_finite(animated_head) &&
           vec3_is_finite(skeleton_chest) && vec3_is_finite(skeleton_pelvis);
}

static bool evaluate_transform_hierarchy_layout(const std::vector<uint64_t>& native_transforms, const TransformHierarchyLayout& layout, size_t& position_count, double& extent) {
    position_count = 0; extent = 0.0;
    Vec3 minimum{}, maximum{};
    bool initialized = false;
    for (uint64_t native_transform : native_transforms) {
        Vec3 position{};
        if (!read_transform_hierarchy_layout(native_transform, layout, position)) continue;
        ++position_count;
        if (!initialized) { minimum = position; maximum = position; initialized = true; }
        else {
            minimum.x = std::min(minimum.x, position.x); minimum.y = std::min(minimum.y, position.y); minimum.z = std::min(minimum.z, position.z);
            maximum.x = std::max(maximum.x, position.x); maximum.y = std::max(maximum.y, position.y); maximum.z = std::max(maximum.z, position.z);
        }
    }
    if (!initialized) return false;
    extent = fabs((double)maximum.x - minimum.x) + fabs((double)maximum.y - minimum.y) + fabs((double)maximum.z - minimum.z);
    return position_count >= 2 && std::isfinite(extent) && extent >= 0.1 && extent <= 1000000.0;
}

static bool discover_transform_hierarchy_layout(const std::vector<uint64_t>& players, size_t& best_position_count, size_t& candidate_count) {
    std::vector<uint64_t> native_transforms;
    std::unordered_set<uint64_t> unique_transforms;
    for (uint64_t player : players) {
        uint64_t native_transform = resolve_player_native_transform(player);
        if (native_transform && unique_transforms.insert(native_transform).second)
            native_transforms.push_back(native_transform);
    }
    if (native_transforms.size() < 2) return false;

    const int64_t index_deltas[] = {-8, 8, 16, 24};
    TransformHierarchyLayout best_layout{};
    double best_score = 0.0;
    best_position_count = 0; candidate_count = 0;
    size_t seed_count = std::min<size_t>(native_transforms.size(), 3);

    for (size_t seed_index = 0; seed_index < seed_count; ++seed_index) {
        uint64_t seed = native_transforms[seed_index];
        for (uint64_t data_offset = 0x10; data_offset <= 0x200; data_offset += 8) {
            uint64_t transform_data = rd_ptr(seed + data_offset);
            if (!likely_native_pointer(transform_data)) continue;
            for (int64_t index_delta : index_deltas) {
                int64_t signed_index_offset = (int64_t)data_offset + index_delta;
                if (signed_index_offset < 0x10 || signed_index_offset > 0x220) continue;
                uint64_t index_offset = (uint64_t)signed_index_offset;
                int32_t transform_index = rd<int32_t>(seed + index_offset);
                if (transform_index < 0 || transform_index > 100000) continue;
                for (uint64_t matrices_offset = 0; matrices_offset <= 0x100; matrices_offset += 8) {
                    uint64_t indices_offset = matrices_offset + 8;
                    uint64_t matrices = rd_ptr(transform_data + matrices_offset);
                    uint64_t indices_ptr = rd_ptr(transform_data + indices_offset);
                    if (!likely_native_pointer(matrices) || !likely_native_pointer(indices_ptr)) continue;
                    for (int matrices_indirect = 0; matrices_indirect < 2; ++matrices_indirect) {
                        for (int indices_indirect = 0; indices_indirect < 2; ++indices_indirect) {
                            TransformHierarchyLayout layout{};
                            layout.data_offset = data_offset; layout.index_offset = index_offset;
                            layout.matrices_offset = matrices_offset; layout.indices_offset = indices_offset;
                            layout.matrices_indirect = matrices_indirect != 0; layout.indices_indirect = indices_indirect != 0;
                            Vec3 seed_position{};
                            if (!read_transform_hierarchy_layout(seed, layout, seed_position)) continue;
                            ++candidate_count;
                            size_t position_count = 0; double extent = 0.0;
                            bool valid = evaluate_transform_hierarchy_layout(native_transforms, layout, position_count, extent);
                            best_position_count = std::max(best_position_count, position_count);
                            if (!valid) continue;
                            double score = (double)position_count * 1000000.0 + std::min(extent, 999999.0);
                            if (score > best_score) { best_score = score; best_layout = layout; }
                        }
                    }
                }
            }
        }
    }
    if (best_score <= 0.0) return false;
    g_transform_hierarchy_layout = best_layout;
    g_transform_hierarchy_layout_valid = true;
    return true;
}

static bool read_entity_position(uint64_t source, Vec3& position) {
    if (!source) return false;
    if (g_use_direct_player_position && g_player_position_offset != 0) { position = rd_v3(source + g_player_position_offset); return vec3_is_finite(position); }
    uint64_t native = resolve_player_native_transform(source);
    if (!native) return false;
    return read_transform_hierarchy_position(native, position);
}

static bool read_entity_pose(uint64_t source, Vec3& position, Vec4& rotation) {
    if (!source || g_use_direct_player_position || !g_transform_hierarchy_layout_valid) return false;
    uint64_t native = resolve_player_native_transform(source);
    if (!native) return false;
    return read_transform_hierarchy_layout(native, g_transform_hierarchy_layout, position, &rotation);
}

static bool evaluate_player_position_offset(const std::vector<uint64_t>& players, uint64_t offset, double& score) {
    score = 0.0;
    if (!offset) return false;
    size_t valid = 0, non_zero = 0;
    Vec3 minimum{}, maximum{};
    bool initialized = false;
    for (uint64_t player : players) {
        if (!player) continue;
        Vec3 position = rd_v3(player + offset);
        if (!vec3_is_finite(position)) continue;
        float magnitude = fabsf(position.x) + fabsf(position.y) + fabsf(position.z);
        if (magnitude < 0.01F) continue;
        ++valid; ++non_zero;
        if (!initialized) { minimum = position; maximum = position; initialized = true; }
        else {
            minimum.x = std::min(minimum.x, position.x); minimum.y = std::min(minimum.y, position.y); minimum.z = std::min(minimum.z, position.z);
            maximum.x = std::max(maximum.x, position.x); maximum.y = std::max(maximum.y, position.y); maximum.z = std::max(maximum.z, position.z);
        }
    }
    // A single (local) player is enough: the known-good offset list below is
    // trusted as long as the position itself is plausible. With two or more
    // players we additionally require them to be spread out, so a stale offset
    // that reads identical garbage for everyone is rejected.
    if (!initialized || valid < 1 || non_zero < 1) return false;
    double extent = fabs((double)maximum.x - minimum.x) + fabs((double)maximum.y - minimum.y) + fabs((double)maximum.z - minimum.z);
    if (!std::isfinite(extent) || extent > 1000000.0) return false;
    if (valid >= 2 && extent < 0.1) return false;
    score = (double)valid * 1000000.0 + std::min(extent, 999999.0);
    return true;
}

static bool discover_player_position_offset(const std::vector<uint64_t>& players) {
    // Only PlayerManager.lastTickPosition (PLAYER_POSITION) and
    // lastSavedPosition (0x1DC) are live positions. The other previously
    // probed offsets are stale snapshots (lastDeathPosition 0x1E8,
    // originalPosition 0x338) or unrelated private Vector3s (0x2D8/0x2E4):
    // with a single player the spread check is meaningless, so probing them
    // could latch a position that never tracks the rendered model and make the
    // whole ESP detach from the enemy.
    const uint64_t known_offsets[] = {PLAYER_POSITION, 0x1DC};
    uint64_t best_offset = 0;
    double best_score = 0.0;
    for (uint64_t offset : known_offsets) {
        double score = 0.0;
        if (evaluate_player_position_offset(players, offset, score) && score > best_score) { best_offset = offset; best_score = score; }
    }
    if (best_offset) {
        g_use_direct_player_position = true; g_player_position_offset = best_offset;
        g_player_position_validated = true; g_matrix_configuration_validated = false;
        return true;
    }
    size_t discovered_position_count = 0, hierarchy_candidate_count = 0;
    if (discover_transform_hierarchy_layout(players, discovered_position_count, hierarchy_candidate_count)) {
        g_use_direct_player_position = false;
        g_player_position_validated = true; g_matrix_configuration_validated = false;
        return true;
    }
    return false;
}

static float mat_get(const Mat4& matrix, int row, int column) {
    return matrix.m[(size_t)row * 4 + column];
}
static void mat_set(Mat4& matrix, int row, int column, float value) {
    matrix.m[(size_t)row * 4 + column] = value;
}
static bool matrix_is_finite(const Mat4& matrix) {
    bool has_non_zero = false;
    for (float value : matrix.m) {
        if (!std::isfinite(value) || fabsf(value) > 1000000.0F) return false;
        if (fabsf(value) > 0.000001F) has_non_zero = true;
    }
    return has_non_zero;
}

static bool matrices_are_coherent(const Mat4& first, const Mat4& second,
                                  float max_delta) {
    for (int i = 0; i < 16; ++i) {
        if (!std::isfinite(first.m[i]) || !std::isfinite(second.m[i]) ||
            fabsf(first.m[i] - second.m[i]) > max_delta) return false;
    }
    return true;
}

static bool read_stable_camera_matrices(uint64_t native_camera,
                                        Mat4& projection, Mat4& view) {
    if (!native_camera) return false;
    // Unity can update the camera transform and projection in separate writes.
    // Prefer a coherent pair, but keep the newest finite pair as a fallback:
    // rejecting every frame while the player is turning would disable ESP
    // completely after a restart on some devices.
    Mat4 latest_projection{}, latest_view{};
    bool have_latest = false;
    for (int attempt = 0; attempt < 3; ++attempt) {
        Mat4 projection_a = rd_m4(native_camera + CAMERA_PROJECTION_MATRIX);
        Mat4 view_a = rd_m4(native_camera + CAMERA_VIEW_MATRIX);
        Mat4 projection_b = rd_m4(native_camera + CAMERA_PROJECTION_MATRIX);
        Mat4 view_b = rd_m4(native_camera + CAMERA_VIEW_MATRIX);
        if (!matrix_is_finite(projection_a) || !matrix_is_finite(view_a) ||
            !matrix_is_finite(projection_b) || !matrix_is_finite(view_b)) continue;
        latest_projection = projection_b;
        latest_view = view_b;
        have_latest = true;
        if (matrices_are_coherent(projection_a, projection_b, 0.05f) &&
            matrices_are_coherent(view_a, view_b, 0.05f)) {
            projection = projection_b;
            view = view_b;
            return true;
        }
    }
    if (have_latest) {
        projection = latest_projection;
        view = latest_view;
        return true;
    }
    return false;
}

static Mat4 mat_mul(const Mat4& a, const Mat4& b) {
    Mat4 result{};
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column) {
            float value = 0.0F;
            for (int k = 0; k < 4; ++k) value += mat_get(a, row, k) * mat_get(b, k, column);
            mat_set(result, row, column, value);
        }
    return result;
}

static bool camera_position_from_view(const Mat4& view, Vec3& position) {
    double augmented[4][8]{};
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) augmented[row][column] = mat_get(view, row, column);
        augmented[row][row + 4] = 1.0;
    }
    for (int column = 0; column < 4; ++column) {
        int pivot = column;
        for (int row = column + 1; row < 4; ++row)
            if (fabs(augmented[row][column]) > fabs(augmented[pivot][column])) pivot = row;
        if (!std::isfinite(augmented[pivot][column]) || fabs(augmented[pivot][column]) < 0.0000001) return false;
        if (pivot != column) for (int i = 0; i < 8; ++i) std::swap(augmented[pivot][i], augmented[column][i]);
        double divisor = augmented[column][column];
        for (int i = 0; i < 8; ++i) augmented[column][i] /= divisor;
        for (int row = 0; row < 4; ++row) {
            if (row == column) continue;
            double factor = augmented[row][column];
            for (int i = 0; i < 8; ++i) augmented[row][i] -= factor * augmented[column][i];
        }
    }
    double w = augmented[3][7];
    if (!std::isfinite(w) || fabs(w) < 0.0000001) return false;
    position = {(float)(augmented[0][7] / w), (float)(augmented[1][7] / w), (float)(augmented[2][7] / w)};
    return vec3_is_finite(position);
}

static bool w2s(const Mat4& vp, const Vec3& world, float sw, float sh, Vec2& out, bool clip_to_screen = true) {
    float clip_x = mat_get(vp, 0, 0) * world.x + mat_get(vp, 0, 1) * world.y + mat_get(vp, 0, 2) * world.z + mat_get(vp, 0, 3);
    float clip_y = mat_get(vp, 1, 0) * world.x + mat_get(vp, 1, 1) * world.y + mat_get(vp, 1, 2) * world.z + mat_get(vp, 1, 3);
    float clip_w = mat_get(vp, 3, 0) * world.x + mat_get(vp, 3, 1) * world.y + mat_get(vp, 3, 2) * world.z + mat_get(vp, 3, 3);
    if (!std::isfinite(clip_x) || !std::isfinite(clip_y) || !std::isfinite(clip_w) || clip_w <= 0.001F) return false;
    out.x = ((clip_x / clip_w) + 1.0F) * 0.5F * sw;
    out.y = ((-clip_y / clip_w) + 1.0F) * 0.5F * sh;
    if (!std::isfinite(out.x) || !std::isfinite(out.y)) return false;
    if (clip_to_screen && (out.x < 0.0F || out.x > sw || out.y < 0.0F || out.y > sh)) return false;
    return true;
}

static bool w2s_transform_camera(const Vec3& camera_position, const Vec4& camera_rotation, const Vec3& world, float screen_width, float screen_height, Vec2& output, bool clip_to_screen = true) {
    if (screen_width < 100.0F || screen_height < 100.0F) return false;
    Vec3 relative = {world.x - camera_position.x, world.y - camera_position.y, world.z - camera_position.z};
    Vec4 inverse_rotation = {-camera_rotation.x, -camera_rotation.y, -camera_rotation.z, camera_rotation.w};
    Vec3 camera_space = rotate_vector(inverse_rotation, relative);
    if (!vec3_is_finite(camera_space) || camera_space.z <= 0.05F) return false;
    constexpr float vertical_fov_radians = 1.0471975512F;
    float tangent = tanf(vertical_fov_radians * 0.5F);
    float aspect = screen_width / screen_height;
    float normalized_x = camera_space.x / (camera_space.z * tangent * aspect);
    float normalized_y = camera_space.y / (camera_space.z * tangent);
    if (!std::isfinite(normalized_x) || !std::isfinite(normalized_y)) return false;
    if (clip_to_screen && (fabsf(normalized_x) > 1.0F || fabsf(normalized_y) > 1.0F)) return false;
    output.x = (normalized_x + 1.0F) * 0.5F * screen_width;
    output.y = (1.0F - normalized_y) * 0.5F * screen_height;
    return std::isfinite(output.x) && std::isfinite(output.y);
}

static bool optimize_matrix_configuration(uint64_t native_camera, const std::vector<uint64_t>& transforms) {
    std::vector<Vec3> samples;
    for (uint64_t source : transforms) {
        Vec3 position{};
        if (!read_entity_position(source, position)) continue;
        samples.push_back(position);
        if (samples.size() >= 24) break;
    }
    if (samples.empty()) {
        g_player_position_validated = false;
        return false;
    }

    Vec3 minimum = samples[0], maximum = samples[0];
    for (const Vec3& position : samples) {
        minimum.x = std::min(minimum.x, position.x); minimum.y = std::min(minimum.y, position.y); minimum.z = std::min(minimum.z, position.z);
        maximum.x = std::max(maximum.x, position.x); maximum.y = std::max(maximum.y, position.y); maximum.z = std::max(maximum.z, position.z);
    }
    if (samples.size() >= 2) {
        float extent = fabsf(maximum.x - minimum.x) + fabsf(maximum.y - minimum.y) + fabsf(maximum.z - minimum.z);
        if (extent < 0.1F) {
            g_player_position_validated = false;
            return false;
        }
    }

    Mat4 validated_projection = rd_m4(native_camera + CAMERA_PROJECTION_MATRIX);
    Mat4 validated_view = rd_m4(native_camera + CAMERA_VIEW_MATRIX);
    if (matrix_is_finite(validated_projection) && matrix_is_finite(validated_view)) {
        Vec3 camera_position{};
        double nearest_camera_distance_squared = INFINITY;
        if (camera_position_from_view(validated_view, camera_position)) {
            for (const Vec3& sample : samples) {
                double dx = (double)sample.x - camera_position.x, dy = (double)sample.y - camera_position.y, dz = (double)sample.z - camera_position.z;
                double distance_squared = dx * dx + dy * dy + dz * dz;
                if (std::isfinite(distance_squared)) nearest_camera_distance_squared = std::min(nearest_camera_distance_squared, distance_squared);
            }
        }
        g_camera_matrix_physical_match = std::isfinite(nearest_camera_distance_squared) && nearest_camera_distance_squared <= 100.0;
        g_matrix_configuration_validated = true;
        return true;
    }
    return false;
}

static std::vector<uint64_t> read_configured_player_transforms() {
    std::vector<uint64_t> transforms;
    uint64_t list = resolve_runtime_player_list();
    if (!list) { return transforms; }

    uint64_t local_player = resolve_local_player();
    if (local_player && !player_list_contains(list, local_player)) {
        if (local_player == g_local_player) g_local_player = 0;
        local_player = 0;
    }
    uint64_t local_source = local_player;

    for (int attempt = 0; attempt < 3; ++attempt) {
        if (attempt > 0) {
            uint64_t refreshed_list = resolve_runtime_player_list();
            if (refreshed_list) list = refreshed_list;
        }
        uint64_t items = rd_ptr(list + IL2CPP_LIST_ITEMS);
        int32_t count = rd<int32_t>(list + IL2CPP_LIST_SIZE);
        if (!items || count <= 0 || count > 512) continue;

        std::vector<uint64_t> snapshot;
        snapshot.reserve((size_t)count + 1);
        if (local_source) snapshot.push_back(local_source);

        for (int32_t index = 0; index < count; ++index) {
            uint64_t player = rd_ptr(items + IL2CPP_ARRAY_FIRST_ELEMENT + (uint64_t)index * sizeof(uint64_t));
            if (!player) continue;
            if (g_player_manager_class && rd_ptr(player) != g_player_manager_class) continue;
            if (player == local_source) continue;
            snapshot.push_back(player);
        }

        uint64_t confirmed_items = rd_ptr(list + IL2CPP_LIST_ITEMS);
        int32_t confirmed_count = rd<int32_t>(list + IL2CPP_LIST_SIZE);
        if (items == confirmed_items && count == confirmed_count && !snapshot.empty())
            return snapshot;
    }
    return transforms;
}

bool esp_init(pid_t pid) {
    std::lock_guard<std::mutex> game_lock(g_game_mutex);
    g_pid = pid;
    g_il2cpp_base = get_base("libil2cpp.so");
    if (!g_il2cpp_base) return false;

    // A restarted client can reuse the same pid while libil2cpp and all
    // managed objects have new addresses. Do not carry validation state or the
    // previous player snapshot into the new process image.
    g_player_manager_class = 0;
    g_player_manager_static_fields = 0;
    g_game_controller_class = 0;
    g_local_player = 0;
    g_matrix_configuration_validated = false;
    g_camera_matrix_physical_match = false;
    g_last_camera_fov_deg = -1.f;
    g_last_vp_valid = false;
    g_player_snapshot.clear();
    g_player_snapshot_stamp = {};
    g_player_track.clear();
    g_player_position_offset = PLAYER_POSITION;
    g_transform_hierarchy_layout = {};
    g_transform_hierarchy_layout_valid = false;
    {
        std::lock_guard<std::mutex> skeleton_lock(g_skeleton_mutex);
        skeleton_invalidate_locked();
        g_skeleton_scene_players.clear();
    }
    g_use_direct_player_position = true;
    g_player_position_validated = false;
    return true;
}

void esp_reset() {
    std::lock_guard<std::mutex> game_lock(g_game_mutex);
    g_pid = -1; g_il2cpp_base = 0;
    g_player_manager_class = 0; g_player_manager_static_fields = 0;
    g_game_controller_class = 0; g_local_player = 0;
    g_matrix_configuration_validated = false; g_camera_matrix_physical_match = false;
    g_last_camera_fov_deg = -1.f;
    g_last_vp_valid = false;
    g_player_snapshot.clear();
    g_player_snapshot_stamp = {};
    g_player_track.clear();
    g_player_position_offset = PLAYER_POSITION;
    g_transform_hierarchy_layout = {}; g_transform_hierarchy_layout_valid = false;
    {
        std::lock_guard<std::mutex> skeleton_lock(g_skeleton_mutex);
        skeleton_invalidate_locked();
        g_skeleton_scene_players.clear();
    }
    g_use_direct_player_position = true;
    g_player_position_validated = false;
}


std::vector<EspBox> esp_get_boxes(int overlay_width, int overlay_height) {
    std::lock_guard<std::mutex> game_lock(g_game_mutex);
    std::vector<EspBox> result;
    // Never let the aimbot or a later frame consume a projection matrix from a
    // death/respawn transition or from a failed camera read.
    g_last_vp_valid = false;

    if (g_pid <= 0 || !g_il2cpp_base) {
        return result;
    }

    uint64_t native_cam = 0;
    Mat4 projection{}, view{}, vp{};
    float sw = overlay_width >= 100 ? (float)overlay_width : 1080.0F;
    float sh = overlay_height >= 100 ? (float)overlay_height : 2400.0F;
    if (!std::isfinite(sw) || sw < 100.0F || sw > 10000.0F) sw = 1080.0F;
    if (!std::isfinite(sh) || sh < 100.0F || sh > 10000.0F) sh = 2400.0F;

    // The player list walk (string validation etc.) is the expensive part, so
    // refresh it at ~3 Hz. Positions and camera matrices are read fresh on
    // every call, so boxes never lag behind the model or slide when the camera
    // moves.
    {
        auto tnow = std::chrono::steady_clock::now();
        int tms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
            tnow - g_player_snapshot_stamp).count();
        if (g_player_snapshot.empty() || tms < 0 || tms >= 300) {
            std::vector<uint64_t> refreshed = read_configured_player_transforms();
            if (!refreshed.empty()) {
                g_player_snapshot = std::move(refreshed);
                g_player_snapshot_stamp = tnow;
            }
        }

        // A wholesale replacement of the player pointer set means the scene
        // (and all its TransformData allocations) was rebuilt after a server
        // switch. Drop the cached skeleton layout for the old scene and re-derive
        // the position/camera state for the new one.
        if (!g_player_snapshot.empty() && !g_skeleton_scene_players.empty()) {
            size_t overlap = 0;
            for (uint64_t p : g_player_snapshot)
                if (g_skeleton_scene_players.count(p)) ++overlap;
            if (overlap == 0) {
                {
                    std::lock_guard<std::mutex> skeleton_lock(g_skeleton_mutex);
                    skeleton_invalidate_locked();
                }
                g_player_track.clear();
                g_player_position_validated = false;
                g_use_direct_player_position = true;
                g_player_position_offset = PLAYER_POSITION;
                g_matrix_configuration_validated = false;
                g_camera_matrix_physical_match = false;
            }
        }
        g_skeleton_scene_players.clear();
        for (uint64_t p : g_player_snapshot) g_skeleton_scene_players.insert(p);
    }
    if (g_player_snapshot.empty()) {
        return result;
    }

    const std::vector<uint64_t>& s_transforms = g_player_snapshot;
    if (!g_player_position_validated) {
        if (!discover_player_position_offset(s_transforms)) return result;
    }

    bool transform_camera_mode = !g_use_direct_player_position && g_transform_hierarchy_layout_valid;
    if (!transform_camera_mode) {
        uint64_t managed_cam = 0;
        if (g_game_controller_class) {
            uint64_t gcb_sf = get_class_static_fields(g_game_controller_class);
            if (gcb_sf) {
                uint64_t cam_mgr = rd_ptr(gcb_sf + GAME_CONTROLLER_CAMERA_MANAGER_FIELD);
                if (cam_mgr) managed_cam = rd_ptr(cam_mgr + CAMERA_MANAGER_CAMERA_FIELD);
            }
        }
        if (!managed_cam) {
            g_last_vp_valid = false;
            return result;
        }
        native_cam = rd_ptr(managed_cam + MANAGED_CACHED_PTR);
        if (!native_cam) {
            g_last_vp_valid = false;
            return result;
        }
        if (!read_stable_camera_matrices(native_cam, projection, view)) {
            g_last_vp_valid = false;
            return result;
        }

        // Do not reject the camera merely because the player is looking up or
        // down. Camera transition handling below uses the local player's actual
        // respawn/spectate state and camera height rather than a single view
        // matrix element that can also change during normal pitch.

        if (!g_matrix_configuration_validated) {
            if (!optimize_matrix_configuration(native_cam, s_transforms)) {
                g_last_vp_valid = false;
                return result;
            }
            if (!read_stable_camera_matrices(native_cam, projection, view)) {
                g_last_vp_valid = false;
                return result;
            }
        }
        vp = mat_mul(projection, view);
        g_last_vp = vp;
        g_last_vp_valid = true;

        // Vertical field-of-view from the projection matrix: m[1][1] = cot(fov/2).
        // Aiming down sights narrows the FOV, so this is used to detect ADS.
        {
            float cot_half = mat_get(projection, 1, 1);
            if (std::isfinite(cot_half) && cot_half > 1e-6f) {
                g_last_camera_fov_deg = 2.0f * atanf(1.0f / cot_half) * (180.0f / 3.14159265358979f);
                if (g_last_camera_fov_deg > 179.f) g_last_camera_fov_deg = 60.f;
            } else {
                g_last_camera_fov_deg = -1.f;
            }
        }
    }

    bool has_local_position = false;
    Vec3 local{};
    size_t local_entity_index = s_transforms.size();

    Vec3 world_camera_position{};
    bool have_world_camera = camera_position_from_view(view, world_camera_position);
    uint64_t resolved_local_player = resolve_local_player();
    bool local_player_transition = false;
    {
        Vec3 camera_position = world_camera_position;
        bool has_camera_position = have_world_camera && g_camera_matrix_physical_match;
        double nearest_distance_squared = INFINITY;
        size_t first_valid_index = s_transforms.size();
        size_t resolved_local_index = s_transforms.size();
        Vec3 first_valid_position{};
        Vec3 resolved_local_position{};
        for (size_t index = 0; index < s_transforms.size(); ++index) {
            Vec3 candidate{};
            if (!read_entity_position(s_transforms[index], candidate)) continue;
            if (first_valid_index == s_transforms.size()) {
                first_valid_index = index;
                first_valid_position = candidate;
            }
            if (resolved_local_player && s_transforms[index] == resolved_local_player) {
                resolved_local_index = index;
                resolved_local_position = candidate;
            }
            if (!has_camera_position) continue;
            double dx = (double)candidate.x - camera_position.x;
            double dy = (double)candidate.y - camera_position.y;
            double dz = (double)candidate.z - camera_position.z;
            double distance_squared = dx * dx + dy * dy + dz * dz;
            if (std::isfinite(distance_squared) && distance_squared < nearest_distance_squared) {
                nearest_distance_squared = distance_squared;
                local_entity_index = index;
                local = candidate;
            }
        }

        // Prefer the actual GameController.localPlayer over “nearest to camera”.
        // During the death camera animation the camera is intentionally moved
        // away from the player, so nearest-player selection can lock onto an
        // enemy and poison the ESP state until the next respawn.
        if (resolved_local_index != s_transforms.size()) {
            local_entity_index = resolved_local_index;
            local = resolved_local_position;
        } else if (local_entity_index == s_transforms.size() &&
                   first_valid_index != s_transforms.size()) {
            local_entity_index = first_valid_index;
            local = first_valid_position;
        }
        has_local_position = local_entity_index != s_transforms.size();
        if (!has_local_position) {
            g_player_position_validated = false;
            g_last_vp_valid = false;
            return result;
        }
        // Spectating is detected via the observed-player back-reference (more
        // reliable than guessing a PlayerFlags bit value): when set, the local
        // player is following another player's camera.
        local_player_transition = resolved_local_player &&
            (rd<uint8_t>(resolved_local_player + PLAYER_RESPAWNING) != 0 ||
             rd_ptr(resolved_local_player + PLAYER_OBSERVED) != 0);
    }

    // During the death/fall animation the game camera is moved down and tilted
    // before the local player is respawned. Never project a frame from that
    // camera: keeping the previous VP matrix is what made ESP lines appear to
    // fly independently from models after respawn.
    bool camera_transition = local_player_transition;
    if (!transform_camera_mode && g_camera_matrix_physical_match &&
        have_world_camera && has_local_position) {
        float camera_height_over_feet = world_camera_position.y - local.y;
        camera_transition = camera_transition ||
            (std::isfinite(camera_height_over_feet) && camera_height_over_feet < 0.55f);
    }
    if (camera_transition) {
        // Invalidate calibration for the next frame, but keep this coherent
        // camera snapshot usable. Returning an empty result here made a false
        // respawn/flag read disable the entire ESP until another reconnect.
        g_matrix_configuration_validated = false;
        g_camera_matrix_physical_match = false;
        g_last_camera_fov_deg = -1.f;
    }

    Vec3 transform_camera_position{};
    Vec4 transform_camera_rotation{};
    if (transform_camera_mode) {
        if (local_entity_index >= s_transforms.size() || !read_entity_pose(s_transforms[local_entity_index], transform_camera_position, transform_camera_rotation)) {
            g_player_position_validated = false;
            g_last_vp_valid = false;
            return result;
        }
        local = transform_camera_position; has_local_position = true;
    }

    for (size_t i = 0; i < s_transforms.size(); ++i) {
        if (i == local_entity_index) continue;
        if (!s_transforms[i]) continue;
        Vec3 feet{};
        if (!read_entity_position(s_transforms[i], feet)) continue;

        float distance = -1.0F;
        if (has_local_position) {
            float dx = feet.x - local.x, dy = feet.y - local.y, dz = feet.z - local.z;
            distance = sqrtf(dx * dx + dy * dy + dz * dz);
            if (!std::isfinite(distance) || distance < MIN_PLAYER_DISTANCE || distance > MAX_PLAYER_DISTANCE) continue;
        }

        // Crouch state from the player's input (reliable: huU.Crouch is a huT,
        // same pattern as the verified Aim flag). Crouching lowers the head by a
        // fixed ratio — the feet stay.
        bool crouched = player_crouching(s_transforms[i]);
        float feet_y = feet.y;
        float head_height = crouched ? 1.12f : PLAYER_HEIGHT;

        // --- velocity + position extrapolation (computed BEFORE projecting) ---
        // Positions update on discrete network ticks; the client renders the
        // model smoothly between them. We extrapolate the last-tick position
        // forward with the smoothed velocity so the box sticks to the rendered
        // model instead of lagging behind it.
        Vec3 vel{};
        float render_x = feet.x, render_z = feet.z;
        {
            double now = std::chrono::duration<double>(
                std::chrono::steady_clock::now().time_since_epoch()).count();

            auto it = g_player_track.find(s_transforms[i]);
            if (it != g_player_track.end()) {
                double dt = now - it->second.t;
                float ddx = feet.x - it->second.pos.x;
                float ddz = feet.z - it->second.pos.z;
                float moved = fabsf(ddx) + fabsf(ddz);
                vel = it->second.vel; // default: hold last estimate

                if (moved > 0.005f) {
                    if (dt > 0.03) {
                        double cdt = dt > 0.5 ? 0.5 : dt; // clamp stale gaps
                        float ivx = ddx / (float)cdt;
                        float ivz = ddz / (float)cdt;
                        float sp = sqrtf(ivx * ivx + ivz * ivz);
                        if (sp < 15.f) {
                            vel.x += (ivx - vel.x) * 0.6f;
                            vel.z += (ivz - vel.z) * 0.6f;
                        }
                    }
                    // advance reference on any real movement (tick)
                    it->second.pos = feet;
                    it->second.t = now;
                } else if (dt > 0.5) {
                    // standing still for a while: decay to zero
                    vel.x *= 0.8f; vel.z *= 0.8f;
                    if (fabsf(vel.x) < 0.05f && fabsf(vel.z) < 0.05f) { vel.x = 0.f; vel.z = 0.f; }
                    it->second.pos = feet;
                    it->second.t = now;
                }
                it->second.vel = vel;

                float age = (float)(now - it->second.t); // time since last tick
                if (age > 0.f && age < 0.12f) {
                    render_x = it->second.pos.x + vel.x * age;
                    render_z = it->second.pos.z + vel.z * age;
                }
            } else {
                g_player_track[s_transforms[i]] = {feet, now, vel};
            }
        }

        // Read the animated hierarchy before building the box.  The box remains
        // a safe fallback when a model is temporarily being rebuilt, but when
        // the hierarchy is available its head and skeleton are taken from the
        // exact same pose sample.
        std::vector<std::pair<Vec3, Vec3>> skeleton_segments;
        Vec3 animated_head{};
        Vec3 skeleton_chest{};
        Vec3 skeleton_pelvis{};
        Vec3 render_feet = {render_x, feet_y, render_z};
        // In transform-camera mode `feet` is the camera/head anchor (the box
        // bottom is feet.y - 1.60), so give the skeleton reader the real
        // ground anchor it validates against.
        Vec3 skeleton_ground = transform_camera_mode
            ? Vec3{render_x, feet.y - 1.60f, render_z}
            : render_feet;
        bool have_skeleton = read_skeleton_segments(s_transforms[i], skeleton_ground,
                                                     skeleton_segments, animated_head,
                                                     skeleton_chest, skeleton_pelvis);

        // Keep the ESP rectangle independent from the optional bone reader.
        // Box geometry is based on the stable player capsule, so a transient
        // bone/cache read can never collapse or stretch the rectangle.
        Vec3 body_bottom = render_feet;
        Vec3 body_top = {render_x, feet_y + head_height, render_z};
        if (transform_camera_mode) {
            body_bottom.y = feet.y - 1.60F;
            body_top.y = feet.y + 0.20F;
        }

        Vec2 sf{}, sh2{};
        bool bottom_visible = transform_camera_mode
            ? w2s_transform_camera(transform_camera_position, transform_camera_rotation, body_bottom, sw, sh, sf, false)
            : w2s(vp, body_bottom, sw, sh, sf, false);
        if (!bottom_visible) continue;
        bool top_visible = transform_camera_mode
            ? w2s_transform_camera(transform_camera_position, transform_camera_rotation, body_top, sw, sh, sh2, false)
            : w2s(vp, body_top, sw, sh, sh2, false);
        if (!top_visible) continue;

        float height = fabsf(sh2.y - sf.y);
        if (!std::isfinite(height) || height < 2.0F) continue;
        float cx = (sf.x + sh2.x) * 0.5F;
        float cy = (sf.y + sh2.y) * 0.5F;
        float half_w = height * PLAYER_BOX_WIDTH_RATIO * 0.5F;
        float half_h = height * 0.5F;

        constexpr float box_half_width = 0.35F, box_half_depth = 0.35F;
        const Vec3 world_corners[8] = {
            {render_x - box_half_width, body_bottom.y, render_z - box_half_depth},
            {render_x + box_half_width, body_bottom.y, render_z - box_half_depth},
            {render_x + box_half_width, body_bottom.y, render_z + box_half_depth},
            {render_x - box_half_width, body_bottom.y, render_z + box_half_depth},
            {render_x - box_half_width, body_top.y, render_z - box_half_depth},
            {render_x + box_half_width, body_top.y, render_z - box_half_depth},
            {render_x + box_half_width, body_top.y, render_z + box_half_depth},
            {render_x - box_half_width, body_top.y, render_z + box_half_depth}
        };
        EspBox box{};
        box.x1 = cx - half_w; box.y1 = cy - half_h;
        box.x2 = cx + half_w; box.y2 = cy + half_h;
        box.distance = distance;
        box.source = s_transforms[i];
        box.feet  = {render_x, feet_y, render_z};
        box.head  = {render_x, feet_y + head_height, render_z};
        box.vel   = vel;
        box.speed = sqrtf(vel.x * vel.x + vel.z * vel.z);
        box.crouched = crouched;
        box.skeleton_valid = have_skeleton;
        box.skeleton_aim_points_valid = false;
        box.skeleton_chest = skeleton_chest;
        box.skeleton_pelvis = skeleton_pelvis;
        box.skeleton_head_point = {0.f, 0.f, false};
        box.skeleton_chest_point = {0.f, 0.f, false};
        box.skeleton_pelvis_point = {0.f, 0.f, false};

        // Project the live bone graph in this same camera snapshot.  No
        // interpolation or previous-frame skeleton is used: if one transform
        // is unavailable, that segment is omitted rather than drawn detached.
        if (have_skeleton) {
            box.skeleton.reserve(skeleton_segments.size());
            for (const auto& segment : skeleton_segments) {
                Vec2 a{}, b{};
                bool a_projected = transform_camera_mode
                    ? w2s_transform_camera(transform_camera_position,
                                            transform_camera_rotation,
                                            segment.first, sw, sh, a, false)
                    : w2s(vp, segment.first, sw, sh, a, false);
                bool b_projected = transform_camera_mode
                    ? w2s_transform_camera(transform_camera_position,
                                            transform_camera_rotation,
                                            segment.second, sw, sh, b, false)
                    : w2s(vp, segment.second, sw, sh, b, false);
                if (!a_projected || !b_projected ||
                    !std::isfinite(a.x) || !std::isfinite(a.y) ||
                    !std::isfinite(b.x) || !std::isfinite(b.y)) continue;
                box.skeleton.push_back({a.x, a.y, b.x, b.y});
            }
            box.skeleton_valid = !box.skeleton.empty();

            if (box.skeleton_valid) {
                // The head screen point is the same live point used for the
                // box top. Chest and pelvis are projected from the selected
                // hierarchy anchors, so aim never falls back to rectangle
                // fractions when a real skeleton is available.
                box.skeleton_head_point = {sh2.x, sh2.y, true};
                Vec2 chest_screen{}, pelvis_screen{};
                bool chest_projected = transform_camera_mode
                    ? w2s_transform_camera(transform_camera_position,
                                            transform_camera_rotation,
                                            skeleton_chest, sw, sh,
                                            chest_screen, false)
                    : w2s(vp, skeleton_chest, sw, sh, chest_screen, false);
                bool pelvis_projected = transform_camera_mode
                    ? w2s_transform_camera(transform_camera_position,
                                            transform_camera_rotation,
                                            skeleton_pelvis, sw, sh,
                                            pelvis_screen, false)
                    : w2s(vp, skeleton_pelvis, sw, sh, pelvis_screen, false);
                if (chest_projected && std::isfinite(chest_screen.x) &&
                    std::isfinite(chest_screen.y)) {
                    box.skeleton_chest_point = {chest_screen.x, chest_screen.y, true};
                }
                if (pelvis_projected && std::isfinite(pelvis_screen.x) &&
                    std::isfinite(pelvis_screen.y)) {
                    box.skeleton_pelvis_point = {pelvis_screen.x, pelvis_screen.y, true};
                }
                box.skeleton_aim_points_valid = box.skeleton_head_point.valid &&
                    box.skeleton_chest_point.valid && box.skeleton_pelvis_point.valid;
            }
        }

        // Screen-space velocity of the head: project head and head + vel*dt.
        box.aim_vx = 0.f;
        box.aim_vy = 0.f;
        if (!transform_camera_mode) {
            Vec3 h  = box.head;
            Vec3 h2 = {h.x + vel.x * 0.2f, h.y, h.z + vel.z * 0.2f};
            Vec2 s0{}, s1{};
            if (w2s(vp, h, sw, sh, s0, false) && w2s(vp, h2, sw, sh, s1, false)) {
                box.aim_vx = (s1.x - s0.x) / 0.2f;
                box.aim_vy = (s1.y - s0.y) / 0.2f;
                if (!std::isfinite(box.aim_vx) || !std::isfinite(box.aim_vy) ||
                    fabsf(box.aim_vx) > 4000.f || fabsf(box.aim_vy) > 4000.f) {
                    box.aim_vx = 0.f; box.aim_vy = 0.f;
                }
            }
        }

        for (size_t corner = 0; corner < 8; ++corner) {
            Vec2 sc{};
            bool projected = transform_camera_mode
                ? w2s_transform_camera(transform_camera_position, transform_camera_rotation, world_corners[corner], sw, sh, sc, false)
                : w2s(vp, world_corners[corner], sw, sh, sc, false);
            box.corner_visible[corner] = projected && sc.x >= 0.0F && sc.x <= sw && sc.y >= 0.0F && sc.y <= sh;
            box.corners[corner][0] = projected ? sc.x : -1.0F;
            box.corners[corner][1] = projected ? sc.y : -1.0F;
        }
        result.push_back(box);
    }

    return result;
}

float esp_get_camera_fov() {
    std::lock_guard<std::mutex> game_lock(g_game_mutex);
    return g_last_camera_fov_deg;
}

bool esp_world_to_screen(Vec3 world, int screen_w, int screen_h, float& sx, float& sy) {
    std::lock_guard<std::mutex> game_lock(g_game_mutex);
    if (!g_last_vp_valid) return false;
    float sw = screen_w >= 100 ? (float)screen_w : 1080.f;
    float sh = screen_h >= 100 ? (float)screen_h : 2400.f;
    Vec2 out{};
    if (!w2s(g_last_vp, world, sw, sh, out, false)) return false;
    sx = out.x;
    sy = out.y;
    return std::isfinite(sx) && std::isfinite(sy);
}

// ---------------------------------------------------------------------------
// Local player aim state. The local PlayerManager carries its input state as
// `playerEventHandler` (huU, base class huc). huc holds:
//   Aim        (huT)  at 0x268  -> huT.TCz bool at 0x10 (ADS input)
//   AimRaycast (huW<huz>) at 0x168 -> huz (aim raycast result)
// huz holds Tun (bool hit) at 0x10 and TuV (PlayerManager hit) at 0x40.
// ---------------------------------------------------------------------------
static uint64_t local_player_event_handler() {
    uint64_t local = resolve_local_player();
    if (!local) return 0;
    return rd_ptr(local + PLAYER_EVENT_HANDLER);
}

// Any player's crouch state: huU.Crouch is a huT (button state), whose bool
// TCz lives at +0x10 — same pattern as the Aim flag used for ADS detection.
static bool player_crouching(uint64_t player) {
    if (!player) return false;
    uint64_t handler = rd_ptr(player + PLAYER_EVENT_HANDLER);
    if (!likely_native_pointer(handler)) return false;
    uint64_t crouch = rd_ptr(handler + HUC_CROUCH);
    if (!likely_native_pointer(crouch)) return false;
    return rd<uint8_t>(crouch + HUT_STATE) != 0;
}

bool esp_is_aiming() {
    std::lock_guard<std::mutex> game_lock(g_game_mutex);
    uint64_t handler = local_player_event_handler();
    if (!handler) return false;
    uint64_t aim = rd_ptr(handler + HUC_AIM);
    if (!likely_native_pointer(aim)) return false;
    return rd<uint8_t>(aim + HUT_STATE) != 0;
}

uint64_t esp_aim_hit_player() {
    std::lock_guard<std::mutex> game_lock(g_game_mutex);
    uint64_t handler = local_player_event_handler();
    if (!handler) return 0;
    // Both RaycastData (crosshair) and AimRaycast (aim assist) cache the most
    // recent raycast result as huW<huz>; the huz instance sits at huW+0x20
    // (current) / +0x28 (previous). Check both fields and both slots.
    for (uint64_t field : {HUC_RAYCAST_DATA, HUC_AIM_RAYCAST}) {
        uint64_t wrapped = rd_ptr(handler + field);
        if (!likely_native_pointer(wrapped)) continue;
        for (uint64_t off : {HUW_VALUE, HUW_PREV}) {
            uint64_t ray = rd_ptr(wrapped + off);
            if (!likely_native_pointer(ray)) continue;
            uint8_t hit = rd<uint8_t>(ray + HUZ_HIT);
            uint8_t hit2 = rd<uint8_t>(ray + HUZ_HIT + 1);
            if (hit > 1 || hit2 > 1) continue; // not a huz (bool fields)
            if (hit == 0) continue;            // ray hit nothing
            uint64_t player = rd_ptr(ray + HUZ_PLAYER);
            if (player && likely_native_pointer(player)) return player;
        }
    }
    return 0;
}


