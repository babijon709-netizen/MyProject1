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
// The network position is deliberately not used for the skeleton.  It only
// identifies the player and is updated on a different tick from the rendered
// Animator.  The KCC's head Transform belongs to the same Unity transform
// hierarchy as every animated body bone.  Reading that hierarchy gives us the
// exact current pose (including IK, crouch, reload and ragdoll), and not a
// collection of guessed offsets.
struct SkeletonHierarchyInfo {
    uint64_t matrices = 0;
    uint64_t indices = 0;
    int32_t count = 0;
};

static TransformHierarchyLayout g_skeleton_layout{};
static bool g_skeleton_layout_valid = false;
static std::unordered_map<uint64_t, SkeletonHierarchyInfo> g_skeleton_hierarchy_cache;

struct SkeletonFrameData {
    uint64_t matrices = 0;
    uint64_t indices = 0;
    std::vector<Matrix34> matrix_values;
    std::vector<int32_t> parent_values;
};
// All players normally share one TransformData allocation. Keep one immutable
// memory snapshot per allocation for the duration of esp_get_boxes(), so ten
// enemies cost one pair of bulk reads instead of ten pairwise bone walks.
static std::unordered_map<uint64_t, SkeletonFrameData> g_skeleton_frame_cache;
static std::mutex g_skeleton_mutex;
static std::chrono::steady_clock::time_point g_skeleton_layout_retry_after{};

static bool read_remote_block(uint64_t address, void* destination, size_t size) {
    if (!address || !destination || size == 0) return false;
    struct iovec local = {destination, size};
    struct iovec remote = {(void*)address, size};
    return process_vm_readv(g_pid, &local, 1, &remote, 1, 0) == (ssize_t)size;
}

static bool resolve_transform_hierarchy_storage(uint64_t native_transform,
                                                const TransformHierarchyLayout& layout,
                                                uint64_t& matrices,
                                                uint64_t& indices,
                                                int32_t& transform_index) {
    matrices = 0; indices = 0; transform_index = -1;
    if (!native_transform) return false;
    uint64_t transform_data = rd_ptr(native_transform + layout.data_offset);
    transform_index = rd<int32_t>(native_transform + layout.index_offset);
    if (!likely_native_pointer(transform_data) || transform_index < 0 || transform_index > 100000)
        return false;
    matrices = rd_ptr(transform_data + layout.matrices_offset);
    indices = rd_ptr(transform_data + layout.indices_offset);
    if (layout.matrices_indirect) matrices = rd_ptr(matrices);
    if (layout.indices_indirect) indices = rd_ptr(indices);
    return likely_native_pointer(matrices) && likely_native_pointer(indices);
}

static bool skeleton_head_matches_player(uint64_t native_head, const Vec3& feet,
                                         const TransformHierarchyLayout& layout,
                                         SkeletonHierarchyInfo& storage,
                                         Vec3& head_position,
                                         int32_t& head_index) {
    uint64_t matrices = 0, indices = 0;
    int32_t index = -1;
    if (!resolve_transform_hierarchy_storage(native_head, layout, matrices, indices, index)) return false;
    if (!read_transform_hierarchy_arrays(matrices, indices, index, head_position)) return false;
    float dx = head_position.x - feet.x;
    float dy = head_position.y - feet.y;
    float dz = head_position.z - feet.z;
    float horizontal = sqrtf(dx * dx + dz * dz);
    // A head may be displaced by a fall animation, but it cannot be tens of
    // metres from the player's network root.  This rejects false pointer/layout
    // candidates without rejecting crouching or ragdoll poses.
    if (!std::isfinite(horizontal) || !std::isfinite(dy) || horizontal > 8.0f || fabsf(dy) > 8.0f)
        return false;
    storage.matrices = matrices;
    storage.indices = indices;
    storage.count = 0;
    head_index = index;
    return true;
}

static bool resolve_skeleton_layout(uint64_t native_head, const Vec3& feet,
                                    SkeletonHierarchyInfo& storage,
                                    int32_t& head_index, Vec3& head_position) {
    if (!native_head) return false;

    if (g_skeleton_layout_valid && skeleton_head_matches_player(native_head, feet,
                                                                  g_skeleton_layout,
                                                                  storage, head_position,
                                                                  head_index))
        return true;

    // A valid layout discovered while resolving a player position is shared by
    // Unity's TransformAccess hierarchy, so try it before probing.
    if (g_transform_hierarchy_layout_valid &&
        skeleton_head_matches_player(native_head, feet, g_transform_hierarchy_layout,
                                     storage, head_position, head_index)) {
        g_skeleton_layout = g_transform_hierarchy_layout;
        g_skeleton_layout_valid = true;
        return true;
    }

    // This is the layout already used by the working world-position reader.
    // Try it directly before the compatibility probe; the latter is deliberately
    // kept as a fallback because probing remote memory for every player/frame
    // would make the overlay stutter.
    TransformHierarchyLayout standard_layout{};
    if (skeleton_head_matches_player(native_head, feet, standard_layout,
                                     storage, head_position, head_index)) {
        g_skeleton_layout = standard_layout;
        g_skeleton_layout_valid = true;
        return true;
    }

    auto now = std::chrono::steady_clock::now();
    if (now < g_skeleton_layout_retry_after) return false;

    // Unity revisions used by the game have moved the two small hierarchy
    // arrays, but retain the same TransformData/index pairing. Probe only the
    // known ABI shapes and accept a candidate only if it projects the actual
    // head near the actual player root.
    const uint64_t data_offsets[] = {0x38, 0x30, 0x40};
    const int64_t index_deltas[] = {8, 16, -8, 24};
    const uint64_t array_offsets[][2] = {{0x18, 0x20}, {0x08, 0x10}, {0x20, 0x28}};
    for (uint64_t data_offset : data_offsets) {
        for (int64_t delta : index_deltas) {
            int64_t index_offset_signed = (int64_t)data_offset + delta;
            if (index_offset_signed < 0x10 || index_offset_signed > 0x220) continue;
            for (const auto& offsets : array_offsets) {
                for (int matrices_indirect = 0; matrices_indirect < 2; ++matrices_indirect) {
                    for (int indices_indirect = 0; indices_indirect < 2; ++indices_indirect) {
                        TransformHierarchyLayout candidate{};
                        candidate.data_offset = data_offset;
                        candidate.index_offset = (uint64_t)index_offset_signed;
                        candidate.matrices_offset = offsets[0];
                        candidate.indices_offset = offsets[1];
                        candidate.matrices_indirect = matrices_indirect != 0;
                        candidate.indices_indirect = indices_indirect != 0;
                        SkeletonHierarchyInfo candidate_storage{};
                        int32_t candidate_index = -1;
                        Vec3 candidate_head{};
                        if (!skeleton_head_matches_player(native_head, feet, candidate,
                                                           candidate_storage, candidate_head,
                                                           candidate_index)) continue;
                        g_skeleton_layout = candidate;
                        g_skeleton_layout_valid = true;
                        storage = candidate_storage;
                        head_index = candidate_index;
                        head_position = candidate_head;
                        return true;
                    }
                }
            }
        }
    }
    g_skeleton_layout_retry_after = now + std::chrono::milliseconds(750);
    return false;
}

static int32_t discover_skeleton_hierarchy_count(uint64_t matrices, uint64_t indices,
                                                 int32_t seed_index) {
    if (!matrices || !indices || seed_index < 0) return 0;

    // Humanoid models in this title are well below 512 transforms. Reading a
    // block once is considerably cheaper than issuing one process_vm_readv per
    // bone on every frame. If a platform maps a shorter block, retry with a
    // smaller one instead of treating a partial read as a valid pose.
    constexpr int kProbeCount = 512;
    std::vector<Matrix34> matrix_probe(kProbeCount);
    std::vector<int32_t> index_probe(kProbeCount);
    int probe_count = kProbeCount;
    while (probe_count >= 32) {
        matrix_probe.resize((size_t)probe_count);
        index_probe.resize((size_t)probe_count);
        if (read_remote_block(matrices, matrix_probe.data(),
                              (size_t)probe_count * sizeof(Matrix34)) &&
            read_remote_block(indices, index_probe.data(),
                              (size_t)probe_count * sizeof(int32_t)))
            break;
        probe_count /= 2;
    }
    if (probe_count < 32) return 0;

    int32_t highest = -1;
    int invalid_tail = 0;
    for (int32_t index = 0; index < probe_count; ++index) {
        int32_t parent = index_probe[(size_t)index];
        bool valid = matrix34_is_valid(matrix_probe[(size_t)index]) &&
                     parent >= -1 && parent < probe_count;
        if (valid) {
            highest = index;
            invalid_tail = 0;
        } else if (index > seed_index && ++invalid_tail >= 24) {
            // There normally are no holes in Unity's transform arrays. This
            // keeps stale allocator data after the live array out of the graph.
            break;
        }
    }
    if (highest < seed_index) return 0;
    return highest + 1;
}

static bool read_skeleton_segments(uint64_t player, const Vec3& feet,
                                   std::vector<std::pair<Vec3, Vec3>>& segments,
                                   Vec3& animated_head) {
    std::lock_guard<std::mutex> skeleton_lock(g_skeleton_mutex);
    segments.clear();
    animated_head = {};
    if (!player) return false;

    // InterfaceReference<T> stores the selected MonoBehaviour component. Some
    // IL2CPP builds expose the component directly, while others keep the
    // wrapper object; accept both representations after validating KCC.player.
    uint64_t reference = rd_ptr(player + PLAYER_KCC_REFERENCE);
    uint64_t kcc = 0;
    const uint64_t candidates[] = {
        reference,
        reference ? rd_ptr(reference + 0x10) : 0,
        reference ? rd_ptr(reference + 0x18) : 0,
        reference ? rd_ptr(reference + 0x20) : 0
    };
    for (uint64_t candidate : candidates) {
        if (!likely_native_pointer(candidate)) continue;
        if (rd_ptr(candidate + KCC_PLAYER) == player) {
            kcc = candidate;
            break;
        }
    }
    if (!kcc) return false;

    uint64_t head_transform = rd_ptr(kcc + KCC_HEAD);
    if (!likely_native_pointer(head_transform)) {
        // Fallback for a brief KCC rebuild: CharacterAnimation keeps the same
        // PlayerModelInfo reference while the controller is being recreated.
        uint64_t character_animation = rd_ptr(kcc + KCC_CHARACTER_ANIMATION);
        uint64_t model_info = character_animation
            ? rd_ptr(character_animation + CHARACTER_ANIMATION_MODEL_INFO) : 0;
        head_transform = model_info ? rd_ptr(model_info + PLAYER_MODEL_HEAD) : 0;
    }
    uint64_t native_head = resolve_native_transform(head_transform);
    if (!likely_native_pointer(native_head)) return false;

    SkeletonHierarchyInfo layout_storage{};
    int32_t head_index = -1;
    if (!resolve_skeleton_layout(native_head, feet, layout_storage,
                                 head_index, animated_head)) return false;

    uint64_t cache_key = layout_storage.matrices ^ (layout_storage.indices * 0x9E3779B97F4A7C15ULL);
    auto cached = g_skeleton_hierarchy_cache.find(cache_key);
    if (cached != g_skeleton_hierarchy_cache.end() &&
        cached->second.matrices == layout_storage.matrices &&
        cached->second.indices == layout_storage.indices) {
        layout_storage.count = cached->second.count;
    } else {
        layout_storage.count = discover_skeleton_hierarchy_count(layout_storage.matrices,
                                                                  layout_storage.indices,
                                                                  head_index);
        if (layout_storage.count <= head_index || layout_storage.count <= 0) return false;
        g_skeleton_hierarchy_cache[cache_key] = layout_storage;
    }
    if (layout_storage.count <= head_index || layout_storage.count > 512) return false;

    auto frame_it = g_skeleton_frame_cache.find(cache_key);
    if (frame_it == g_skeleton_frame_cache.end()) {
        SkeletonFrameData frame{};
        frame.matrices = layout_storage.matrices;
        frame.indices = layout_storage.indices;
        frame.matrix_values.resize((size_t)layout_storage.count);
        frame.parent_values.resize((size_t)layout_storage.count);
        if (!read_remote_block(frame.matrices, frame.matrix_values.data(),
                               frame.matrix_values.size() * sizeof(Matrix34)) ||
            !read_remote_block(frame.indices, frame.parent_values.data(),
                               frame.parent_values.size() * sizeof(int32_t))) return false;
        frame_it = g_skeleton_frame_cache.emplace(cache_key, std::move(frame)).first;
    }
    const std::vector<Matrix34>& matrices = frame_it->second.matrix_values;
    const std::vector<int32_t>& parents = frame_it->second.parent_values;

    std::vector<Vec3> world((size_t)layout_storage.count);
    std::vector<uint8_t> state((size_t)layout_storage.count, 0);
    std::function<bool(int32_t)> calculate_world = [&](int32_t index) -> bool {
        if (index < 0 || index >= layout_storage.count) return false;
        uint8_t& node_state = state[(size_t)index];
        if (node_state == 2) return true;
        if (node_state == 1) return false; // malformed/cyclic hierarchy
        node_state = 1;
        const Matrix34& local = matrices[(size_t)index];
        if (!matrix34_is_valid(local)) { node_state = 0; return false; }

        // Apply ancestors in the same order as Unity's Transform hierarchy:
        // local position -> parent scale/rotation/translation -> grandparent.
        // This is exact even for non-uniformly scaled ragdoll bones and avoids
        // any approximation from composing a single Euler/body rotation.
        Vec3 result = {local.translation.x, local.translation.y, local.translation.z};
        int32_t parent = parents[(size_t)index];
        int depth = 0;
        for (; parent >= 0 && depth < 128; ++depth) {
            if (parent >= layout_storage.count || !matrix34_is_valid(matrices[(size_t)parent])) {
                node_state = 0;
                return false;
            }
            const Matrix34& parent_local = matrices[(size_t)parent];
            Vec3 scaled = {result.x * parent_local.scale.x,
                           result.y * parent_local.scale.y,
                           result.z * parent_local.scale.z};
            Vec3 rotated = rotate_vector(parent_local.rotation, scaled);
            result = {parent_local.translation.x + rotated.x,
                      parent_local.translation.y + rotated.y,
                      parent_local.translation.z + rotated.z};
            parent = parents[(size_t)parent];
        }
        if ((depth >= 128 && parent >= 0) || parent < -1 ||
            parent >= layout_storage.count || !vec3_is_finite(result)) {
            node_state = 0;
            return false;
        }
        world[(size_t)index] = result;
        node_state = 2;
        return true;
    };

    // Resolve the root and validate that the head is actually part of it.
    int32_t root = head_index;
    for (int depth = 0; depth < 128; ++depth) {
        if (root < 0 || root >= layout_storage.count) return false;
        int32_t parent = parents[(size_t)root];
        if (parent == -1) break;
        if (parent < 0 || parent >= layout_storage.count) return false;
        root = parent;
        if (depth == 127) return false;
    }
    if (!calculate_world(head_index)) return false;
    animated_head = world[(size_t)head_index];

    std::vector<int8_t> relation((size_t)layout_storage.count, -1);
    std::function<bool(int32_t)> belongs_to_root = [&](int32_t index) -> bool {
        if (index < 0 || index >= layout_storage.count) return false;
        int8_t& relation_state = relation[(size_t)index];
        if (relation_state >= 0) return relation_state != 0;
        int32_t current = index;
        for (int depth = 0; depth < 128; ++depth) {
            if (current == root) {
                relation_state = 1;
                return true;
            }
            if (current < 0 || current >= layout_storage.count) break;
            current = parents[(size_t)current];
            if (current == -1) break;
        }
        relation_state = 0;
        return false;
    };

    constexpr float kMaxBoneDistanceFromRoot = 10.0f;
    constexpr float kMaxBoneSegmentLength = 4.0f;
    for (int32_t index = 0; index < layout_storage.count; ++index) {
        if (index == root || !belongs_to_root(index)) continue;
        int32_t parent = parents[(size_t)index];
        if (parent < 0 || !belongs_to_root(parent)) continue;
        if (!calculate_world(index) || !calculate_world(parent)) continue;
        const Vec3& a = world[(size_t)parent];
        const Vec3& b = world[(size_t)index];
        float ax = a.x - feet.x, ay = a.y - feet.y, az = a.z - feet.z;
        float bx = b.x - feet.x, by = b.y - feet.y, bz = b.z - feet.z;
        float length = sqrtf((a.x - b.x) * (a.x - b.x) +
                             (a.y - b.y) * (a.y - b.y) +
                             (a.z - b.z) * (a.z - b.z));
        if (!std::isfinite(length) || length > kMaxBoneSegmentLength ||
            !std::isfinite(ax + ay + az + bx + by + bz) ||
            sqrtf(ax * ax + ay * ay + az * az) > kMaxBoneDistanceFromRoot ||
            sqrtf(bx * bx + by * by + bz * bz) > kMaxBoneDistanceFromRoot) continue;
        segments.emplace_back(a, b);
        if (segments.size() >= 160) break;
    }
    return !segments.empty() && vec3_is_finite(animated_head);
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
    if (!initialized || valid < 2 || non_zero < 2) return false;
    double extent = fabs((double)maximum.x - minimum.x) + fabs((double)maximum.y - minimum.y) + fabs((double)maximum.z - minimum.z);
    if (!std::isfinite(extent) || extent < 0.1 || extent > 1000000.0) return false;
    score = (double)valid * 1000000.0 + std::min(extent, 999999.0);
    return true;
}

static bool discover_player_position_offset(const std::vector<uint64_t>& players) {
    const uint64_t known_offsets[] = {0x1D0, 0x1DC, 0x1E8, 0x2D8, 0x2E4, 0x338};
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
    if (samples.size() < 2) {
        g_player_position_validated = false;
        return false;
    }

    Vec3 minimum = samples[0], maximum = samples[0];
    for (const Vec3& position : samples) {
        minimum.x = std::min(minimum.x, position.x); minimum.y = std::min(minimum.y, position.y); minimum.z = std::min(minimum.z, position.z);
        maximum.x = std::max(maximum.x, position.x); maximum.y = std::max(maximum.y, position.y); maximum.z = std::max(maximum.z, position.z);
    }
    float extent = fabsf(maximum.x - minimum.x) + fabsf(maximum.y - minimum.y) + fabsf(maximum.z - minimum.z);
    if (extent < 0.1F) {
        g_player_position_validated = false;
        return false;
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
    g_player_position_offset = PLAYER_POSITION;
    g_transform_hierarchy_layout = {}; g_transform_hierarchy_layout_valid = false;
    {
        std::lock_guard<std::mutex> skeleton_lock(g_skeleton_mutex);
        g_skeleton_layout = {}; g_skeleton_layout_valid = false;
        g_skeleton_layout_retry_after = {};
        g_skeleton_hierarchy_cache.clear();
        g_skeleton_frame_cache.clear();
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
    {
        std::lock_guard<std::mutex> skeleton_lock(g_skeleton_mutex);
        g_skeleton_frame_cache.clear();
    }

    if (g_pid <= 0 || !g_il2cpp_base) { return result; }

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
    }
    if (g_player_snapshot.empty()) return result;

    const std::vector<uint64_t>& s_transforms = g_player_snapshot;
    if (!g_player_position_validated) {
        if (!discover_player_position_offset(s_transforms)) return result;
    }

    bool transform_camera_mode = !g_use_direct_player_position && g_transform_hierarchy_layout_valid;
    bool camera_roll_detected = false;
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
        projection = rd_m4(native_cam + CAMERA_PROJECTION_MATRIX);
        view = rd_m4(native_cam + CAMERA_VIEW_MATRIX);
        if (!matrix_is_finite(projection) || !matrix_is_finite(view)) {
            g_last_vp_valid = false;
            return result;
        }

        // Row 0 is the camera right vector. Its Y component measures roll
        // around the look direction, while the old up_y check also fired during
        // ordinary vertical looking/pitch. A large right_y is the distinctive
        // death-camera tilt and is safe to use as a transition signal.
        float camera_right_y = mat_get(view, 0, 1);
        camera_roll_detected = std::isfinite(camera_right_y) &&
                               fabsf(camera_right_y) > 0.35f;

        // Do not reject the camera merely because the player is looking up or
        // down: view[1][1] changes with pitch during normal play.  Death-camera
        // handling is performed after the local player is identified below,
        // where the respawn/spectate state and camera height can be checked
        // together without hiding ESP during an ordinary look movement.

        if (!g_matrix_configuration_validated) {
            if (!optimize_matrix_configuration(native_cam, s_transforms)) {
                g_last_vp_valid = false;
                return result;
            }
            projection = rd_m4(native_cam + CAMERA_PROJECTION_MATRIX);
            view = rd_m4(native_cam + CAMERA_VIEW_MATRIX);
            if (!matrix_is_finite(projection) || !matrix_is_finite(view)) {
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
        local_player_transition = resolved_local_player &&
            (rd<uint8_t>(resolved_local_player + PLAYER_RESPAWNING) != 0 ||
             (rd<uint32_t>(resolved_local_player + PLAYER_FLAGS) & PLAYER_FLAG_SPECTATING) != 0);
    }

    // During the death/fall animation the game camera is moved down and tilted
    // before the local player is respawned. Never project a frame from that
    // camera: keeping the previous VP matrix is what made ESP lines appear to
    // fly independently from models after respawn.
    bool camera_transition = local_player_transition || camera_roll_detected;
    if (!transform_camera_mode && g_camera_matrix_physical_match &&
        have_world_camera && has_local_position) {
        float camera_height_over_feet = world_camera_position.y - local.y;
        camera_transition = camera_transition ||
            (std::isfinite(camera_height_over_feet) && camera_height_over_feet < 0.55f);
    }
    if (camera_transition) {
        g_matrix_configuration_validated = false;
        g_camera_matrix_physical_match = false;
        g_last_camera_fov_deg = -1.f;
        g_last_vp_valid = false;
        return result;
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
            struct Track { Vec3 pos; double t; Vec3 vel; };
            static std::unordered_map<uint64_t, Track> s_track;
            double now = std::chrono::duration<double>(
                std::chrono::steady_clock::now().time_since_epoch()).count();

            auto it = s_track.find(s_transforms[i]);
            if (it != s_track.end()) {
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
                s_track[s_transforms[i]] = {feet, now, vel};
            }
        }

        // Read the animated hierarchy before building the box.  The box remains
        // a safe fallback when a model is temporarily being rebuilt, but when
        // the hierarchy is available its head and skeleton are taken from the
        // exact same pose sample.
        std::vector<std::pair<Vec3, Vec3>> skeleton_segments;
        Vec3 animated_head{};
        bool have_skeleton = read_skeleton_segments(s_transforms[i], feet,
                                                     skeleton_segments, animated_head);

        Vec3 body_bottom = {render_x, feet_y, render_z};
        Vec3 body_top = have_skeleton ? animated_head
                                      : Vec3{render_x, feet_y + head_height, render_z};
        if (transform_camera_mode && !have_skeleton) {
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
        box.head  = have_skeleton ? animated_head
                                  : Vec3{render_x, feet_y + head_height, render_z};
        box.vel   = vel;
        box.speed = sqrtf(vel.x * vel.x + vel.z * vel.z);
        box.crouched = crouched;
        box.skeleton_valid = have_skeleton;

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


