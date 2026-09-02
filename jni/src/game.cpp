#include "game.h"
#include "game_offsets.h"
#include "Vector.h"

#include <string.h>
#include <sys/uio.h>
#include <stdio.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <unistd.h>
#include <sys/syscall.h>

static ssize_t remote_vm_readv(pid_t pid, const struct iovec* local_iov, unsigned long liovcnt, const struct iovec* remote_iov, unsigned long riovcnt, unsigned long flags) {
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

// Camera state captured by the last esp_get_boxes() call (used by the aimbot
// to convert bone positions into yaw/pitch offsets from the crosshair).
static float     g_cam_fov_deg = 0.0F;
static bool      g_cam_pose_valid = false;
static Vec3      g_cam_pos{};
static Vec3      g_cam_right{}, g_cam_up{}, g_cam_forward{};

// The game fires along PlayerEventHandler.LookDirection (= MouseLook.m_LookRoot
// forward) from the KCC eye point, NOT along the camera transform (which carries
// visual sway/kick on top). Aim angles are therefore measured against this
// reference whenever it can be read, so the aimbot steers the actual firing
// direction onto the target instead of the camera.
static bool      g_aim_ref_valid = false;
static Vec3      g_aim_ref_origin{};
static Vec3      g_aim_ref_forward{}, g_aim_ref_right{}, g_aim_ref_up{};


static bool vec3_is_finite(const Vec3& value);

template<typename T>
static T rd(uint64_t addr) {
    T v{};
    struct iovec lv = { &v, sizeof(T) };
    struct iovec rv = { (void*)addr, sizeof(T) };
    remote_vm_readv(g_pid, &lv, 1, &rv, 1, 0);
    return v;
}
template<typename T>
static bool rd_exact(uint64_t addr, T& value) {
    value = {};
    if (!addr) return false;
    struct iovec local = {&value, sizeof(T)};
    struct iovec remote = {(void*)addr, sizeof(T)};
    return remote_vm_readv(g_pid, &local, 1, &remote, 1, 0) == (ssize_t)sizeof(T);
}
static uint64_t rd_ptr(uint64_t a) { return rd<uint64_t>(a); }
static Vec3     rd_v3 (uint64_t a) { return rd<Vec3>(a);     }
static Mat4     rd_m4 (uint64_t a) { return rd<Mat4>(a);     }

static bool rd_buf(uint64_t addr, void* out, size_t size) {
    if (!addr || !size) return false;
    struct iovec local = {out, size};
    struct iovec remote = {(void*)addr, size};
    return remote_vm_readv(g_pid, &local, 1, &remote, 1, 0) == (ssize_t)size;
}

static std::string read_remote_string(uint64_t address) {
    if (!address) return {};
    char buffer[96]{};
    struct iovec local = {buffer, sizeof(buffer) - 1};
    struct iovec remote = {(void*)address, sizeof(buffer) - 1};
    ssize_t count = remote_vm_readv(g_pid, &local, 1, &remote, 1, 0);
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
    // NOTE: translation.w and scale.w are SIMD padding lanes. The game's
    // animation/IK code leaves garbage (NaN/huge values) there for bones it
    // actively writes every frame (arms while aiming, legs while walking), so
    // those lanes must NOT be validated - only the meaningful components.
    const float values[] = {
        matrix.translation.x, matrix.translation.y, matrix.translation.z,
        matrix.rotation.x, matrix.rotation.y, matrix.rotation.z, matrix.rotation.w,
        matrix.scale.x, matrix.scale.y, matrix.scale.z
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
    bool saved_use_direct = g_use_direct_player_position;
    g_use_direct_player_position = true;
    const uint64_t known_offsets[] = {0x1D4, 0x1C8, 0x1E0, 0x2D0, 0x2DC, 0x1D0, 0x1DC, 0x1E8};
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
    g_use_direct_player_position = saved_use_direct;
    return false;
}

// Unity Matrix4x4 is column-major in memory: m[col*4 + row].
static float mat_get(const Mat4& matrix, int row, int column) {
    return matrix.m[(size_t)column * 4 + row];
}
static void mat_set(Mat4& matrix, int row, int column, float value) {
    matrix.m[(size_t)column * 4 + row] = value;
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
    // result = a * b (column-major, same as Unity Matrix4x4 operator*)
    Mat4 result{};
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row) {
            float value = 0.0F;
            for (int k = 0; k < 4; ++k) value += mat_get(a, row, k) * mat_get(b, k, column);
            mat_set(result, row, column, value);
        }
    return result;
}

static Mat4 mat_perspective(float fov_degrees, float aspect, float z_near, float z_far) {
    Mat4 result{};
    if (!(fov_degrees > 0.1F && fov_degrees < 179.0F) || !(aspect > 0.05F) || !(z_near > 0.0F) || !(z_far > z_near))
        return result;
    float fov_rad = fov_degrees * 0.01745329251F;
    float cotangent = 1.0F / tanf(fov_rad * 0.5F);
    mat_set(result, 0, 0, cotangent / aspect);
    mat_set(result, 1, 1, cotangent);
    mat_set(result, 2, 2, -(z_far + z_near) / (z_far - z_near));
    mat_set(result, 2, 3, -(2.0F * z_far * z_near) / (z_far - z_near));
    mat_set(result, 3, 2, -1.0F);
    return result;
}

// worldToCamera from camera world pose (Unity camera looks down -Z).
static Mat4 mat_world_to_camera(const Vec3& position, const Vec4& rotation) {
    Vec3 right = rotate_vector(rotation, {1.0F, 0.0F, 0.0F});
    Vec3 up = rotate_vector(rotation, {0.0F, 1.0F, 0.0F});
    Vec3 forward = rotate_vector(rotation, {0.0F, 0.0F, 1.0F});
    // View basis: rows = right, up, -forward (camera space).
    Mat4 view{};
    mat_set(view, 0, 0, right.x);   mat_set(view, 0, 1, right.y);   mat_set(view, 0, 2, right.z);
    mat_set(view, 1, 0, up.x);      mat_set(view, 1, 1, up.y);      mat_set(view, 1, 2, up.z);
    mat_set(view, 2, 0, -forward.x); mat_set(view, 2, 1, -forward.y); mat_set(view, 2, 2, -forward.z);
    mat_set(view, 0, 3, -(right.x * position.x + right.y * position.y + right.z * position.z));
    mat_set(view, 1, 3, -(up.x * position.x + up.y * position.y + up.z * position.z));
    mat_set(view, 2, 3, -(-forward.x * position.x + -forward.y * position.y + -forward.z * position.z));
    mat_set(view, 3, 3, 1.0F);
    return view;
}

static bool camera_position_from_view(const Mat4& view, Vec3& position) {
    // For orthonormal worldToCamera: cam_pos = -R^T * t
    float r00 = mat_get(view, 0, 0), r01 = mat_get(view, 0, 1), r02 = mat_get(view, 0, 2);
    float r10 = mat_get(view, 1, 0), r11 = mat_get(view, 1, 1), r12 = mat_get(view, 1, 2);
    float r20 = mat_get(view, 2, 0), r21 = mat_get(view, 2, 1), r22 = mat_get(view, 2, 2);
    float tx = mat_get(view, 0, 3), ty = mat_get(view, 1, 3), tz = mat_get(view, 2, 3);
    position = {
        -(r00 * tx + r10 * ty + r20 * tz),
        -(r01 * tx + r11 * ty + r21 * tz),
        -(r02 * tx + r12 * ty + r22 * tz)
    };
    return vec3_is_finite(position);
}

static bool w2s(const Mat4& vp, const Vec3& world, float sw, float sh, Vec2& out, bool clip_to_screen = true) {
    // clip = VP * float4(world, 1) with column-major VP
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

static bool read_camera_transform_pose(uint64_t native_transform, Vec3& position, Vec4& rotation) {
    if (!native_transform) return false;
    if (g_transform_hierarchy_layout_valid) {
        if (read_transform_hierarchy_layout(native_transform, g_transform_hierarchy_layout, position, &rotation))
            return true;
    }
    // Probe the common TransformAccess layouts used elsewhere in this file.
    uint64_t transform_data = rd_ptr(native_transform + 0x38);
    int32_t transform_index = rd<int32_t>(native_transform + 0x40);
    if (!transform_data || transform_index < 0 || transform_index > 100000) {
        transform_data = rd_ptr(native_transform + 0x18);
        transform_index = rd<int32_t>(native_transform + 0x20);
    }
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
                if (read_transform_hierarchy_arrays(matrices, indices_ptr, transform_index, position, &rotation))
                    return true;
            }
        }
    }
    return false;
}

static bool read_native_camera_matrices(uint64_t native_cam, float screen_aspect, Mat4& projection, Mat4& view) {
    if (!native_cam) return false;

    static Mat4 s_last_view{};
    static Mat4 s_last_proj{};
    static bool s_last_ok = false;

    // Fresh view from the Camera's live Transform (cam+0x20). The +0x70 cache is
    // only rebuilt inside Unity getters when dirty-flag +0x502 is set — we never
    // run those getters, so raw +0x70 drifts while the camera moves.
    bool have_live_view = false;
    uint64_t native_transform = rd_ptr(native_cam + CAMERA_NATIVE_TRANSFORM);
    if (native_transform) {
        Vec3 cam_pos{};
        Vec4 cam_rot{};
        if (read_camera_transform_pose(native_transform, cam_pos, cam_rot) &&
            vec3_is_finite(cam_pos) && normalize_quaternion(cam_rot)) {
            view = mat_world_to_camera(cam_pos, cam_rot);
            have_live_view = matrix_is_finite(view);
            if (have_live_view) {
                g_cam_pos = cam_pos;
                g_cam_right = rotate_vector(cam_rot, {1.0F, 0.0F, 0.0F});
                g_cam_up = rotate_vector(cam_rot, {0.0F, 1.0F, 0.0F});
                g_cam_forward = rotate_vector(cam_rot, {0.0F, 0.0F, 1.0F});
                g_cam_pose_valid = true;
            }
        }
    }
    if (!have_live_view) {
        if (s_last_ok && matrix_is_finite(s_last_view)) {
            view = s_last_view;
        } else {
            view = rd_m4(native_cam + CAMERA_VIEW_MATRIX);
            if (!matrix_is_finite(view)) return false;
        }
    } else {
        s_last_view = view;
    }

    // Projection params (FOV/aspect/clip) are stored as plain floats and stay hot.
    float fov = rd<float>(native_cam + CAMERA_FOV_DEGREES);
    float aspect = rd<float>(native_cam + CAMERA_ASPECT);
    float z_near = rd<float>(native_cam + CAMERA_NEAR_CLIP);
    float z_far = rd<float>(native_cam + CAMERA_FAR_CLIP);
    if (!(aspect > 0.1F && aspect < 10.0F))
        aspect = (screen_aspect > 0.1F && screen_aspect < 10.0F) ? screen_aspect : (9.0F / 16.0F);
    if (!(z_near > 0.001F && z_near < 100.0F)) z_near = 0.1F;
    if (!(z_far > z_near && z_far < 100000.0F)) z_far = 1000.0F;
    if (std::isfinite(fov) && fov > 1.0F && fov < 179.0F) g_cam_fov_deg = fov;
    projection = mat_perspective(fov, aspect, z_near, z_far);
    if (!matrix_is_finite(projection)) {
        if (s_last_ok && matrix_is_finite(s_last_proj)) {
            projection = s_last_proj;
        } else {
            projection = rd_m4(native_cam + CAMERA_PROJECTION_MATRIX);
            if (!matrix_is_finite(projection)) return false;
        }
    }
    if (matrix_is_finite(projection)) s_last_proj = projection;
    if (have_live_view && matrix_is_finite(view)) {
        s_last_view = view;
        s_last_ok = true;
    }
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

    Mat4 validated_projection{}, validated_view{};
    if (!read_native_camera_matrices(native_camera, 0.0F, validated_projection, validated_view))
        return false;
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

// ===================== Skeleton ESP =====================
//
// External bone resolution: PlayerManager.characterModel (managed GameObject)
// -> m_CachedPtr -> native GameObject -> component[0] = native Transform (model
// root). The model subtree is walked through the native Transform children
// array (t+0x48 / count t+0x58) and bones are identified by their GameObject
// names ("Hips", "Spine", ..., "ToeBase.R"). World positions are then computed
// each frame from the shared TransformHierarchy arrays (local TRS matrices +
// parent indices) with two bulk reads per player.

enum SkeletonBone {
    BONE_HIPS = 0, BONE_SPINE, BONE_SPINE1, BONE_SPINE2, BONE_NECK, BONE_HEAD,
    BONE_SHOULDER_L, BONE_ARM_L, BONE_FOREARM_L, BONE_HAND_L,
    BONE_SHOULDER_R, BONE_ARM_R, BONE_FOREARM_R, BONE_HAND_R,
    BONE_UPLEG_L, BONE_LEG_L, BONE_FOOT_L, BONE_TOE_L,
    BONE_UPLEG_R, BONE_LEG_R, BONE_FOOT_R, BONE_TOE_R
};


struct SkeletonBoneName { const char* name; int bone; };
// Names are matched after normalization: lowercase, prefix up to the last ':'
// stripped, spaces removed, '_' turned into '.'. Aliases cover the Blender-style
// rig from the game plus common Unity/Mixamo/UE naming, just in case.
static const SkeletonBoneName kSkeletonBoneNames[] = {
    {"hips", BONE_HIPS}, {"pelvis", BONE_HIPS},
    {"spine", BONE_SPINE}, {"spine.01", BONE_SPINE},
    {"spine1", BONE_SPINE1}, {"chest", BONE_SPINE1}, {"spine.02", BONE_SPINE1},
    {"spine2", BONE_SPINE2}, {"upperchest", BONE_SPINE2}, {"spine.03", BONE_SPINE2},
    {"neck", BONE_NECK}, {"neck.01", BONE_NECK},
    {"head", BONE_HEAD},
    {"shoulder.l", BONE_SHOULDER_L}, {"leftshoulder", BONE_SHOULDER_L}, {"clavicle.l", BONE_SHOULDER_L},
    {"arm.l", BONE_ARM_L}, {"leftarm", BONE_ARM_L}, {"upperarm.l", BONE_ARM_L},
    {"forearm.l", BONE_FOREARM_L}, {"leftforearm", BONE_FOREARM_L}, {"lowerarm.l", BONE_FOREARM_L},
    {"hand.l", BONE_HAND_L}, {"lefthand", BONE_HAND_L},
    {"shoulder.r", BONE_SHOULDER_R}, {"rightshoulder", BONE_SHOULDER_R}, {"clavicle.r", BONE_SHOULDER_R},
    {"arm.r", BONE_ARM_R}, {"rightarm", BONE_ARM_R}, {"upperarm.r", BONE_ARM_R},
    {"forearm.r", BONE_FOREARM_R}, {"rightforearm", BONE_FOREARM_R}, {"lowerarm.r", BONE_FOREARM_R},
    {"hand.r", BONE_HAND_R}, {"righthand", BONE_HAND_R},
    {"upleg.l", BONE_UPLEG_L}, {"leftupleg", BONE_UPLEG_L}, {"thigh.l", BONE_UPLEG_L},
    {"leg.l", BONE_LEG_L}, {"leftleg", BONE_LEG_L}, {"calf.l", BONE_LEG_L},
    {"foot.l", BONE_FOOT_L}, {"leftfoot", BONE_FOOT_L},
    {"toebase.l", BONE_TOE_L}, {"lefttoebase", BONE_TOE_L}, {"toe.l", BONE_TOE_L}, {"ball.l", BONE_TOE_L},
    {"upleg.r", BONE_UPLEG_R}, {"rightupleg", BONE_UPLEG_R}, {"thigh.r", BONE_UPLEG_R},
    {"leg.r", BONE_LEG_R}, {"rightleg", BONE_LEG_R}, {"calf.r", BONE_LEG_R},
    {"foot.r", BONE_FOOT_R}, {"rightfoot", BONE_FOOT_R},
    {"toebase.r", BONE_TOE_R}, {"righttoebase", BONE_TOE_R}, {"toe.r", BONE_TOE_R}, {"ball.r", BONE_TOE_R}
};

struct CachedSkeleton {
    uint64_t bone_transform[ESP_BONE_COUNT] = {};
    Vec3     bone_world[ESP_BONE_COUNT] = {};
    uint8_t  bone_world_age[ESP_BONE_COUNT] = {}; // 0 = none, 1 = fresh, grows on reuse
    uint64_t model_root = 0;
    int      bone_count = 0;
    bool     valid = false;
    int      retry_cooldown = 0;
    int      fail_streak = 0;
    int      revalidate_timer = 0;
};

static std::unordered_map<uint64_t, CachedSkeleton> g_skeletons;
static bool g_skeleton_enabled = false;
// The aimbot needs bone positions regardless of the skeleton ESP toggle. Bone
// resolution is therefore driven by (skeleton ESP || aim requested).
static bool g_aim_bones_requested = false;

struct LocalAimState {
    bool     aiming = false;
    int      source = 0;          // 1 = event handler Aim activity, 2 = weapon isAiming
    uint64_t event_handler = 0;
    uint64_t aim_activity = 0;
    uint64_t fp_manager = 0;
    uint64_t weapon = 0;
    int      revalidate = 0;
};
static LocalAimState g_aim_state{};

// Per-player auxiliary data resolved through the KCC (character controller):
// the game-maintained head transform and the current pose. Used for
// crouch-aware boxes and as the aim target when rig bones are unavailable.
struct PlayerAux {
    uint64_t kcc = 0;
    uint64_t head_native = 0;   // native Transform of KCC.head
    uint64_t head_hitbox_transform = 0; // native Transform of the Head HitBox
    Vec3     head_hitbox_center{};      // HitBox.center (local to that transform)
    bool     head_hitbox_valid = false;
    float    normal_height = 1.8F;
    float    crouch_height = 1.1F;
    int      retry_cooldown = 0;
    int      revalidate = 0;
};
static std::unordered_map<uint64_t, PlayerAux> g_player_aux;

static TransformHierarchyLayout g_skeleton_layout{};
static bool g_skeleton_layout_valid = false;

static uint64_t g_go_name_offset = 0;
static bool     g_go_name_plain_pointer = false; // fallback: name stored as raw char*
static bool     g_go_name_offset_valid = false;
static int      g_go_name_retry_cooldown = 0;
static int      g_skeleton_builds_this_frame = 0; // heavy rescans: max 1 per frame

void esp_set_skeleton_enabled(bool enabled) { g_skeleton_enabled = enabled; }
void esp_set_aim_bones_enabled(bool enabled) { g_aim_bones_requested = enabled; }

// characterModel (managed GameObject) -> native GameObject -> its Transform.
static uint64_t skeleton_model_root(uint64_t player) {
    if (!player) return 0;
    uint64_t managed_go = rd_ptr(player + PLAYER_CHARACTER_MODEL);
    if (!managed_go) return 0;
    uint64_t native_go = rd_ptr(managed_go + MANAGED_CACHED_PTR);
    if (!native_go) return 0;
    uint64_t pairs = rd_ptr(native_go + GAMEOBJECT_COMPONENT_ARRAY);
    if (!pairs) return 0;
    uint64_t transform = rd_ptr(pairs + COMPONENT_PAIR_PTR);
    if (!transform) return 0;
    // Sanity: the transform must point back at the same GameObject.
    if (rd_ptr(transform + COMPONENT_GAMEOBJECT) != native_go) return 0;
    return transform;
}

static int read_transform_children(uint64_t transform, uint64_t* out, int max_children) {
    if (!transform) return 0;
    int32_t count = rd<int32_t>(transform + TRANSFORM_CHILD_COUNT);
    if (count <= 0 || count > 128) return 0;
    if (count > max_children) count = max_children;
    uint64_t array = rd_ptr(transform + TRANSFORM_CHILDREN_ARRAY);
    if (!array) return 0;
    if (!rd_buf(array, out, (size_t)count * sizeof(uint64_t))) return 0;
    return count;
}

static void collect_transform_subtree(uint64_t root, std::vector<uint64_t>& nodes, size_t max_nodes) {
    nodes.clear();
    if (!root) return;
    nodes.push_back(root);
    size_t cursor = 0;
    uint64_t children[128];
    while (cursor < nodes.size() && nodes.size() < max_nodes) {
        uint64_t current = nodes[cursor++];
        int count = read_transform_children(current, children, 128);
        for (int i = 0; i < count && nodes.size() < max_nodes; ++i) {
            if (children[i]) nodes.push_back(children[i]);
        }
    }
}

static bool string_is_reasonable_name(const char* value) {
    size_t length = strnlen(value, 48);
    if (length == 0 || length >= 48) return false;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)value[i];
        if (c < 0x20 || c > 0x7E) return false;
    }
    return true;
}

// Lowercase, strip "prefix:" namespaces, drop spaces, unify '_' -> '.'.
static void normalize_bone_name(const char* in, char* out, size_t cap) {
    const char* start = in;
    for (const char* p = in; *p; ++p) {
        if (*p == ':') start = p + 1;
    }
    size_t n = 0;
    for (const char* p = start; *p && n + 1 < cap; ++p) {
        char c = *p;
        if (c == ' ') continue;
        if (c == '_') c = '.';
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        out[n++] = c;
    }
    out[n] = '\0';
}

static int match_bone_name(const char* raw_name) {
    char normalized[48];
    normalize_bone_name(raw_name, normalized, sizeof(normalized));
    if (!normalized[0]) return -1;
    for (const SkeletonBoneName& entry : kSkeletonBoneNames) {
        if (strcmp(normalized, entry.name) == 0) return entry.bone;
    }
    return -1;
}


// Read a GameObject name. Unity stores it as a 32-byte core::string with SSO:
// flags byte at +0x1F; when (flags >= 0x40) the first 8 bytes are a heap char*,
// otherwise the characters live inline at +0x0.
static bool read_gameobject_name_at(uint64_t native_go, uint64_t name_offset, bool plain_pointer, char* out, size_t cap) {
    if (!native_go || cap < 2) return false;
    char buffer[48] = {};
    if (plain_pointer) {
        uint64_t ptr = rd_ptr(native_go + name_offset);
        if (ptr < 0x10000 || ptr >= 0x0001000000000000ULL) return false;
        if (!rd_buf(ptr, buffer, 32)) return false;
        buffer[32] = '\0';
    } else {
        uint8_t raw[32];
        if (!rd_buf(native_go + name_offset, raw, sizeof(raw))) return false;
        uint8_t flags = raw[31];
        if (flags >= 0x40) {
            uint64_t heap = 0;
            memcpy(&heap, raw, sizeof(heap));
            if (heap < 0x10000 || heap >= 0x0001000000000000ULL) return false;
            if (!rd_buf(heap, buffer, 32)) return false;
            buffer[32] = '\0';
        } else {
            memcpy(buffer, raw, 23);
            buffer[23] = '\0';
        }
    }
    if (!string_is_reasonable_name(buffer)) return false;
    strncpy(out, buffer, cap - 1);
    out[cap - 1] = '\0';
    return true;
}

static bool read_transform_name(uint64_t transform, char* out, size_t cap) {
    if (!g_go_name_offset_valid || !transform) return false;
    uint64_t native_go = rd_ptr(transform + COMPONENT_GAMEOBJECT);
    if (!native_go) return false;
    return read_gameobject_name_at(native_go, g_go_name_offset, g_go_name_plain_pointer, out, cap);
}

// Find the GameObject name field offset by probing candidates against the
// character model subtree until known bone names show up.
static bool discover_gameobject_name_offset(const std::vector<uint64_t>& nodes) {
    static const uint64_t kCandidates[] = {GAMEOBJECT_NAME_GUESS, 0x40, 0x50, 0x38, 0x30, 0x28, 0x58, 0x20, 0x60};
    static const char* kProbeNames[] = {"hips", "spine", "spine1", "spine2", "neck", "head", "armature", "root", "pelvis"};

    std::vector<uint64_t> gameobjects;
    gameobjects.reserve(nodes.size());
    for (uint64_t node : nodes) {
        uint64_t go = rd_ptr(node + COMPONENT_GAMEOBJECT);
        if (go) gameobjects.push_back(go);
        if (gameobjects.size() >= 192) break;
    }
    if (gameobjects.size() < 8) return false;

    for (int plain_pointer = 0; plain_pointer < 2; ++plain_pointer) {
        for (uint64_t offset : kCandidates) {
            int matches = 0;
            for (uint64_t go : gameobjects) {
                char name[48];
                if (!read_gameobject_name_at(go, offset, plain_pointer != 0, name, sizeof(name))) continue;
                char normalized[48];
                normalize_bone_name(name, normalized, sizeof(normalized));
                for (const char* probe : kProbeNames) {
                    if (strcmp(normalized, probe) == 0) { ++matches; break; }
                }
                if (matches >= 3) break;
            }
            if (matches >= 3) {
                g_go_name_offset = offset;
                g_go_name_plain_pointer = plain_pointer != 0;
                g_go_name_offset_valid = true;
                return true;
            }
        }
    }
    return false;
}

static bool resolve_skeleton_layout(uint64_t sample_transform) {
    Vec3 probe{};
    if (g_skeleton_layout_valid) {
        if (read_transform_hierarchy_layout(sample_transform, g_skeleton_layout, probe)) return true;
        g_skeleton_layout_valid = false;
    }
    if (g_transform_hierarchy_layout_valid &&
        read_transform_hierarchy_layout(sample_transform, g_transform_hierarchy_layout, probe)) {
        g_skeleton_layout = g_transform_hierarchy_layout;
        g_skeleton_layout_valid = true;
        return true;
    }
    const uint64_t base_offsets[][2] = {{0x38, 0x40}, {0x18, 0x20}};
    const uint64_t data_offsets[][2] = {{0x18, 0x20}, {0x08, 0x10}};
    for (const auto& base : base_offsets) {
        for (const auto& offsets : data_offsets) {
            for (int matrices_indirect = 0; matrices_indirect < 2; ++matrices_indirect) {
                for (int indices_indirect = 0; indices_indirect < 2; ++indices_indirect) {
                    TransformHierarchyLayout layout{};
                    layout.data_offset = base[0];
                    layout.index_offset = base[1];
                    layout.matrices_offset = offsets[0];
                    layout.indices_offset = offsets[1];
                    layout.matrices_indirect = matrices_indirect != 0;
                    layout.indices_indirect = indices_indirect != 0;
                    if (read_transform_hierarchy_layout(sample_transform, layout, probe)) {
                        g_skeleton_layout = layout;
                        g_skeleton_layout_valid = true;
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

// ===================== Ragdoll bone list (no name matching) =====================
//
// The game itself keeps the rig bone transforms in Ragdoll.m_Bones (BodyPart[])
// plus m_Pelvis. Resolving bones from there is immune to rig naming, duplicate
// meshes and BFS depth issues. Intermediate bones (spine chain, neck, shoulders,
// hands, feet) are recovered structurally from the transform hierarchy.

static uint64_t managed_object_native(uint64_t managed) {
    if (!managed) return 0;
    uint64_t native = rd_ptr(managed + MANAGED_CACHED_PTR);
    if (native < 0x10000 || native >= 0x0001000000000000ULL) return 0;
    return native;
}

static bool skeleton_transform_ptr_valid(uint64_t transform) {
    if (!transform) return false;
    uint64_t go = rd_ptr(transform + COMPONENT_GAMEOBJECT);
    if (!go) return false;
    uint64_t pairs = rd_ptr(go + GAMEOBJECT_COMPONENT_ARRAY);
    return pairs && rd_ptr(pairs + COMPONENT_PAIR_PTR) == transform;
}

// Any native Component -> the Transform of its GameObject.
static uint64_t native_component_transform(uint64_t native_component) {
    if (!native_component) return 0;
    uint64_t go = rd_ptr(native_component + COMPONENT_GAMEOBJECT);
    if (!go) return 0;
    uint64_t pairs = rd_ptr(go + GAMEOBJECT_COMPONENT_ARRAY);
    if (!pairs) return 0;
    uint64_t transform = rd_ptr(pairs + COMPONENT_PAIR_PTR);
    if (!transform || rd_ptr(transform + COMPONENT_GAMEOBJECT) != go) return 0;
    return transform;
}

// player -> KCC. kccReference (0xB0) may be an obfuscated wrapper, and its
// internal layout is unknown, so probe it and — as a last resort — scan the
// PlayerManager fields. A real KCC is recognized by its back-reference
// (KCC.player @0x78 == player); KCC.head (@0x88, managed Transform) and the
// CharacterAnimation slot (@0x108) strengthen the match.
static bool kcc_head_transform_valid(uint64_t kcc) {
    uint64_t head = managed_object_native(rd_ptr(kcc + KCC_HEAD_TRANSFORM));
    return skeleton_transform_ptr_valid(head);
}

static bool looks_like_kcc(uint64_t candidate, uint64_t player) {
    if (candidate < 0x10000 || candidate >= 0x0001000000000000ULL) return false;
    return rd_ptr(candidate + KCC_PLAYER_BACKREF) == player;
}

static uint64_t resolve_player_kcc(uint64_t player) {
    uint64_t reference = rd_ptr(player + PLAYER_KCC_REFERENCE);
    if (reference) {
        if (looks_like_kcc(reference, player)) return reference;
        for (uint64_t offset = 0x08; offset <= 0x60; offset += 8) {
            uint64_t candidate = rd_ptr(reference + offset);
            if (looks_like_kcc(candidate, player)) return candidate;
        }
    }
    // Field scan: prefer candidates whose head transform checks out, then
    // any with a plausible CharacterAnimation pointer.
    uint64_t weak = 0;
    for (uint64_t offset = 0x68; offset <= 0x2C8; offset += 8) {
        uint64_t candidate = rd_ptr(player + offset);
        if (!looks_like_kcc(candidate, player)) continue;
        if (kcc_head_transform_valid(candidate)) return candidate;
        if (!weak && rd_ptr(candidate + KCC_CHARACTER_ANIMATION)) weak = candidate;
    }
    return weak;
}

static uint64_t resolve_player_ragdoll(uint64_t player, uint64_t& kcc_out) {
    kcc_out = 0;
    uint64_t kcc = resolve_player_kcc(player);
    if (!kcc) return 0;
    kcc_out = kcc;
    uint64_t anim = rd_ptr(kcc + KCC_CHARACTER_ANIMATION);
    if (!anim) return 0;
    uint64_t back = rd_ptr(anim + CHAR_ANIM_PLAYER_BACKREF);
    if (back && back != player) return 0;
    return rd_ptr(anim + CHAR_ANIM_RAGDOLL);
}

// Ancestor index chain (excluding the start index) via remote reads.
static int read_parent_chain_remote(uint64_t indices, int32_t index, int32_t* chain, int cap) {
    int length = 0;
    int32_t current = index;
    while (length < cap) {
        int32_t parent = rd<int32_t>(indices + (uint64_t)current * 4);
        if (parent < 0 || parent > 100000 || parent == current) break;
        chain[length++] = parent;
        current = parent;
    }
    return length;
}

// Child of `parent_transform` whose hierarchy index lies on `chain`.
static uint64_t skeleton_child_on_chain(uint64_t parent_transform, uint64_t data,
                                        const int32_t* chain, int chain_length) {
    if (!parent_transform) return 0;
    uint64_t children[64];
    int count = read_transform_children(parent_transform, children, 64);
    for (int i = 0; i < count; ++i) {
        if (rd_ptr(children[i] + g_skeleton_layout.data_offset) != data) continue;
        int32_t child_index = rd<int32_t>(children[i] + g_skeleton_layout.index_offset);
        for (int j = 0; j < chain_length; ++j)
            if (chain[j] == child_index) return children[i];
    }
    return 0;
}

static bool build_skeleton_from_ragdoll(uint64_t player, CachedSkeleton& skeleton) {
    uint64_t kcc = 0;
    uint64_t ragdoll = resolve_player_ragdoll(player, kcc);
    if (!ragdoll) return false;

    uint64_t bones_array = rd_ptr(ragdoll + RAGDOLL_BONES_ARRAY);
    if (!bones_array) return false;
    int32_t element_count = rd<int32_t>(bones_array + IL2CPP_ARRAY_LENGTH);
    if (element_count < 4 || element_count > 64) return false;

    uint64_t pelvis = native_component_transform(
        managed_object_native(rd_ptr(ragdoll + RAGDOLL_PELVIS_RIGIDBODY)));
    if (pelvis && !skeleton_transform_ptr_valid(pelvis)) pelvis = 0;

    // Collect the ragdoll bone transforms. Elements are BodyPart objects
    // (transform at +0x10) but tolerate a plain Component[] as well.
    constexpr int kMaxSet = 24;
    uint64_t set_transform[kMaxSet];
    int set_count = 0;
    for (int32_t i = 0; i < element_count && set_count < kMaxSet; ++i) {
        uint64_t element = rd_ptr(bones_array + IL2CPP_ARRAY_FIRST_ELEMENT + (uint64_t)i * 8);
        if (!element) continue;
        uint64_t transform = managed_object_native(rd_ptr(element + RAGDOLL_BODYPART_TRANSFORM));
        if (!skeleton_transform_ptr_valid(transform))
            transform = native_component_transform(managed_object_native(element));
        if (!skeleton_transform_ptr_valid(transform)) continue;
        bool duplicate = false;
        for (int j = 0; j < set_count; ++j)
            if (set_transform[j] == transform) { duplicate = true; break; }
        if (!duplicate) set_transform[set_count++] = transform;
    }
    if (pelvis) {
        bool present = false;
        for (int j = 0; j < set_count; ++j)
            if (set_transform[j] == pelvis) { present = true; break; }
        if (!present && set_count < kMaxSet) set_transform[set_count++] = pelvis;
    }
    if (set_count < 5) return false;

    if (!resolve_skeleton_layout(pelvis ? pelvis : set_transform[0])) return false;

    // Bones may live in different TransformHierarchies (the game re-parents
    // parts of the rig at runtime), so resolve arrays per hierarchy.
    auto hierarchy_arrays = [&](uint64_t hierarchy, uint64_t& matrices, uint64_t& indices) -> bool {
        matrices = rd_ptr(hierarchy + g_skeleton_layout.matrices_offset);
        indices = rd_ptr(hierarchy + g_skeleton_layout.indices_offset);
        if (g_skeleton_layout.matrices_indirect) matrices = rd_ptr(matrices);
        if (g_skeleton_layout.indices_indirect) indices = rd_ptr(indices);
        return matrices && indices;
    };

    // Hierarchy, index, ancestor chain and world position per ragdoll bone.
    uint64_t set_data[kMaxSet];
    int32_t set_index[kMaxSet];
    int32_t chains[kMaxSet][48];
    int     chain_length[kMaxSet];
    Vec3    world[kMaxSet];
    {
        int write = 0;
        for (int i = 0; i < set_count; ++i) {
            uint64_t bone_data = rd_ptr(set_transform[i] + g_skeleton_layout.data_offset);
            if (!bone_data) continue;
            int32_t index = rd<int32_t>(set_transform[i] + g_skeleton_layout.index_offset);
            if (index < 0 || index > 100000) continue;
            uint64_t matrices = 0, indices = 0;
            if (!hierarchy_arrays(bone_data, matrices, indices)) continue;
            Vec3 position{};
            if (!read_transform_hierarchy_arrays(matrices, indices, index, position)) continue;
            set_transform[write] = set_transform[i];
            set_data[write] = bone_data;
            set_index[write] = index;
            world[write] = position;
            ++write;
        }
        set_count = write;
    }
    if (set_count < 5) return false;
    for (int i = 0; i < set_count; ++i) {
        uint64_t matrices = 0, indices = 0;
        chain_length[i] = hierarchy_arrays(set_data[i], matrices, indices)
            ? read_parent_chain_remote(indices, set_index[i], chains[i], 48) : 0;
    }

    // Ancestor relations only make sense inside one hierarchy, so slot lookup
    // matches both the index and the hierarchy the chain belongs to.
    auto set_slot_of_index = [&](int32_t index, uint64_t hierarchy) -> int {
        for (int i = 0; i < set_count; ++i)
            if (set_index[i] == index && set_data[i] == hierarchy) return i;
        return -1;
    };

    // Nearest set ancestor + number of set descendants for every bone.
    int nearest[kMaxSet];
    int descendants[kMaxSet];
    for (int i = 0; i < set_count; ++i) { nearest[i] = -1; descendants[i] = 0; }
    for (int i = 0; i < set_count; ++i) {
        for (int step = 0; step < chain_length[i]; ++step) {
            int ancestor = set_slot_of_index(chains[i][step], set_data[i]);
            if (ancestor < 0) continue;
            if (nearest[i] < 0) nearest[i] = ancestor;
            ++descendants[ancestor];
        }
    }

    // Pelvis: prefer the game's own m_Pelvis, else the widest ancestor.
    int pelvis_slot = -1;
    if (pelvis) {
        for (int i = 0; i < set_count; ++i)
            if (set_transform[i] == pelvis) { pelvis_slot = i; break; }
    }
    if (pelvis_slot < 0) {
        for (int i = 0; i < set_count; ++i)
            if (pelvis_slot < 0 || descendants[i] > descendants[pelvis_slot]) pelvis_slot = i;
    }
    if (pelvis_slot < 0 || descendants[pelvis_slot] < 2) return false;

    // Chest: pelvis branch with the most descendants; walk down while the
    // branch still splits (handles an intermediate spine rigidbody).
    int chest_slot = -1;
    for (int i = 0; i < set_count; ++i) {
        if (i == pelvis_slot || nearest[i] != pelvis_slot) continue;
        if (descendants[i] >= 2 && (chest_slot < 0 || descendants[i] > descendants[chest_slot]))
            chest_slot = i;
    }
    if (chest_slot < 0) {
        // Upper body re-parented into another hierarchy: its subtree root has
        // no set ancestor. Pick the rootless bone with the widest subtree.
        for (int i = 0; i < set_count; ++i) {
            if (i == pelvis_slot || nearest[i] >= 0) continue;
            if (descendants[i] >= 2 && (chest_slot < 0 || descendants[i] > descendants[chest_slot]))
                chest_slot = i;
        }
    }
    while (chest_slot >= 0) {
        int next = -1;
        for (int i = 0; i < set_count; ++i)
            if (nearest[i] == chest_slot && descendants[i] >= 2) { next = i; break; }
        if (next < 0) break;
        chest_slot = next;
    }
    if (chest_slot < 0) return false;

    bool on_spine[kMaxSet] = {};
    on_spine[chest_slot] = true;
    for (int step = 0; step < chain_length[chest_slot]; ++step) {
        int slot = set_slot_of_index(chains[chest_slot][step], set_data[chest_slot]);
        if (slot >= 0) on_spine[slot] = true;
    }

    // Legs: pelvis branches outside the spine, starting at/below the pelvis.
    int thigh_slot[2] = {-1, -1};
    for (int i = 0; i < set_count; ++i) {
        if (i == pelvis_slot || on_spine[i] || nearest[i] != pelvis_slot) continue;
        if (world[i].y > world[pelvis_slot].y + 0.2F) continue;
        if (thigh_slot[0] < 0) thigh_slot[0] = i;
        else if (thigh_slot[1] < 0) thigh_slot[1] = i;
    }

    // Head: leaf hanging off the chest (highest one); arms: chest branches
    // that continue (forearm below them).
    int head_slot = -1;
    int upperarm_slot[2] = {-1, -1};
    for (int i = 0; i < set_count; ++i) {
        if (i == pelvis_slot || on_spine[i] || nearest[i] != chest_slot) continue;
        if (descendants[i] == 0) {
            if (head_slot < 0 || world[i].y > world[head_slot].y) head_slot = i;
        } else if (upperarm_slot[0] < 0) {
            upperarm_slot[0] = i;
        } else if (upperarm_slot[1] < 0) {
            upperarm_slot[1] = i;
        }
    }
    int forearm_slot[2] = {-1, -1};
    for (int side = 0; side < 2; ++side) {
        if (upperarm_slot[side] < 0) continue;
        for (int i = 0; i < set_count; ++i)
            if (nearest[i] == upperarm_slot[side]) { forearm_slot[side] = i; break; }
    }

    // Consistent left/right split by local-space X (siblings share a parent).
    auto local_x = [&](int slot) -> float {
        uint64_t matrices = 0, indices = 0;
        if (!hierarchy_arrays(set_data[slot], matrices, indices)) return 0.0F;
        return rd<float>(matrices + (uint64_t)set_index[slot] * sizeof(Matrix34));
    };
    if (thigh_slot[0] >= 0 && thigh_slot[1] >= 0 && local_x(thigh_slot[0]) < local_x(thigh_slot[1])) {
        int swap = thigh_slot[0]; thigh_slot[0] = thigh_slot[1]; thigh_slot[1] = swap;
    }
    if (upperarm_slot[0] >= 0 && upperarm_slot[1] >= 0 && local_x(upperarm_slot[0]) < local_x(upperarm_slot[1])) {
        int swap = upperarm_slot[0]; upperarm_slot[0] = upperarm_slot[1]; upperarm_slot[1] = swap;
        swap = forearm_slot[0]; forearm_slot[0] = forearm_slot[1]; forearm_slot[1] = swap;
    }

    auto assign = [&](int bone, uint64_t transform) {
        if (bone >= 0 && bone < ESP_BONE_COUNT && transform && !skeleton.bone_transform[bone])
            skeleton.bone_transform[bone] = transform;
    };
    // Child pick for chain ends (hand/foot/toe): prefer a name match when the
    // name offset is known, otherwise the first child in the same hierarchy.
    auto pick_child = [&](uint64_t parent, int bone_hint) -> uint64_t {
        if (!parent) return 0;
        uint64_t parent_data = rd_ptr(parent + g_skeleton_layout.data_offset);
        uint64_t children[16];
        int count = read_transform_children(parent, children, 16);
        uint64_t fallback = 0;
        for (int i = 0; i < count; ++i) {
            if (rd_ptr(children[i] + g_skeleton_layout.data_offset) != parent_data) continue;
            if (!fallback) fallback = children[i];
            if (g_go_name_offset_valid) {
                char name[48];
                if (read_transform_name(children[i], name, sizeof(name)) &&
                    match_bone_name(name) == bone_hint) return children[i];
            }
        }
        return fallback;
    };

    assign(BONE_HIPS, set_transform[pelvis_slot]);
    assign(BONE_SPINE2, set_transform[chest_slot]);

    // Spine chain: pelvis -> ... -> chest along the chest's ancestor chain.
    {
        uint64_t cursor = set_transform[pelvis_slot];
        const int spine_bones[2] = {BONE_SPINE, BONE_SPINE1};
        for (int step = 0; step < 2 && cursor; ++step) {
            uint64_t next = skeleton_child_on_chain(cursor, set_data[chest_slot], chains[chest_slot], chain_length[chest_slot]);
            if (!next || next == set_transform[chest_slot]) break;
            assign(spine_bones[step], next);
            cursor = next;
        }
    }

    if (head_slot >= 0) {
        assign(BONE_HEAD, set_transform[head_slot]);
        uint64_t neck = skeleton_child_on_chain(set_transform[chest_slot], set_data[head_slot],
                                                chains[head_slot], chain_length[head_slot]);
        if (neck && neck != set_transform[head_slot]) assign(BONE_NECK, neck);
    }

    const int arm_bones[2][4] = {
        {BONE_SHOULDER_L, BONE_ARM_L, BONE_FOREARM_L, BONE_HAND_L},
        {BONE_SHOULDER_R, BONE_ARM_R, BONE_FOREARM_R, BONE_HAND_R}
    };
    for (int side = 0; side < 2; ++side) {
        int arm = upperarm_slot[side];
        if (arm < 0) continue;
        uint64_t shoulder = skeleton_child_on_chain(set_transform[chest_slot], set_data[arm],
                                                    chains[arm], chain_length[arm]);
        if (shoulder && shoulder != set_transform[arm]) assign(arm_bones[side][0], shoulder);
        assign(arm_bones[side][1], set_transform[arm]);
        if (forearm_slot[side] >= 0) {
            assign(arm_bones[side][2], set_transform[forearm_slot[side]]);
            assign(arm_bones[side][3], pick_child(set_transform[forearm_slot[side]], arm_bones[side][3]));
        }
    }

    const int leg_bones[2][4] = {
        {BONE_UPLEG_L, BONE_LEG_L, BONE_FOOT_L, BONE_TOE_L},
        {BONE_UPLEG_R, BONE_LEG_R, BONE_FOOT_R, BONE_TOE_R}
    };
    for (int side = 0; side < 2; ++side) {
        if (thigh_slot[side] < 0) continue;
        assign(leg_bones[side][0], set_transform[thigh_slot[side]]);
        int calf = -1;
        for (int i = 0; i < set_count; ++i)
            if (nearest[i] == thigh_slot[side]) { calf = i; break; }
        if (calf < 0) continue;
        assign(leg_bones[side][1], set_transform[calf]);
        uint64_t foot = pick_child(set_transform[calf], leg_bones[side][2]);
        assign(leg_bones[side][2], foot);
        assign(leg_bones[side][3], pick_child(foot, leg_bones[side][3]));
    }

    // Bonus: the game exposes the head transform directly on the KCC.
    if (kcc && !skeleton.bone_transform[BONE_HEAD]) {
        uint64_t head = managed_object_native(rd_ptr(kcc + KCC_HEAD_TRANSFORM));
        if (skeleton_transform_ptr_valid(head)) assign(BONE_HEAD, head);
    }

    // Final filter: keep bones with a resolvable hierarchy (any hierarchy).
    int valid_bones = 0;
    for (int bone = 0; bone < ESP_BONE_COUNT; ++bone) {
        uint64_t transform = skeleton.bone_transform[bone];
        if (!transform) continue;
        uint64_t bone_data = rd_ptr(transform + g_skeleton_layout.data_offset);
        int32_t index = rd<int32_t>(transform + g_skeleton_layout.index_offset);
        if (!bone_data || index < 0 || index > 100000) {
            skeleton.bone_transform[bone] = 0;
            continue;
        }
        ++valid_bones;
    }
    if (valid_bones < 6 || !skeleton.bone_transform[BONE_HIPS]) return false;

    skeleton.model_root = skeleton_model_root(player); // may be 0; revalidation compares equal
    skeleton.bone_count = valid_bones;
    skeleton.valid = true;
    return true;
}

static bool build_skeleton_from_names(uint64_t player, CachedSkeleton& skeleton) {
    skeleton = CachedSkeleton{};

    uint64_t root = skeleton_model_root(player);
    if (!root) return false;

    // Wide scan of the whole model to locate "Hips" candidates. Deep bones
    // (arms/head) may be far down, so the cap here is generous.
    static std::vector<uint64_t> nodes;
    collect_transform_subtree(root, nodes, 1024);
    if (nodes.size() < 8) return false;

    if (!g_go_name_offset_valid && !discover_gameobject_name_offset(nodes)) return false;

    // Collect up to a few Hips candidates (render rig, ragdoll copies, ...).
    uint64_t hips_candidates[4] = {};
    int hips_candidate_count = 0;
    for (uint64_t node : nodes) {
        char name[48];
        if (!read_transform_name(node, name, sizeof(name))) continue;
        if (match_bone_name(name) != BONE_HIPS) continue;
        hips_candidates[hips_candidate_count++] = node;
        if (hips_candidate_count >= 4) break;
    }
    if (!hips_candidate_count) return false;

    // Targeted parent->child descent along the known rig structure. Unlike a
    // breadth-first subtree scan this cannot starve on wide hierarchies (bone
    // attachments, hitboxes, gear), because it only ever looks at the children
    // of already-identified bones. A missing middle bone is tolerated: the
    // search for the next slot simply continues from the last found bone.
    auto find_child_bone = [&](uint64_t parent, int bone_id) -> uint64_t {
        if (!parent) return 0;
        uint64_t children[64];
        int count = read_transform_children(parent, children, 64);
        for (int i = 0; i < count; ++i) {
            char name[48];
            if (!read_transform_name(children[i], name, sizeof(name))) continue;
            if (match_bone_name(name) == bone_id) return children[i];
        }
        return 0;
    };

    uint64_t best_bones[ESP_BONE_COUNT] = {};
    int best_count = 0;
    for (int candidate = 0; candidate < hips_candidate_count; ++candidate) {
        uint64_t hips = hips_candidates[candidate];
        uint64_t bones[ESP_BONE_COUNT] = {};
        bones[BONE_HIPS] = hips;
        int found = 1;

        // Spine chain (cursor only advances on hits, so gaps are skipped).
        uint64_t cursor = hips;
        for (int slot = BONE_SPINE; slot <= BONE_SPINE2; ++slot) {
            uint64_t next = find_child_bone(cursor, slot);
            if (next) { bones[slot] = next; ++found; cursor = next; }
        }
        uint64_t chest = cursor; // deepest spine bone found (or hips)
        uint64_t neck = find_child_bone(chest, BONE_NECK);
        if (neck) { bones[BONE_NECK] = neck; ++found; }
        uint64_t head = find_child_bone(neck ? neck : chest, BONE_HEAD);
        if (head) { bones[BONE_HEAD] = head; ++found; }

        const int arm_chain[2][4] = {
            {BONE_SHOULDER_L, BONE_ARM_L, BONE_FOREARM_L, BONE_HAND_L},
            {BONE_SHOULDER_R, BONE_ARM_R, BONE_FOREARM_R, BONE_HAND_R}
        };
        for (int side = 0; side < 2; ++side) {
            // Shoulders may hang off any spine bone.
            uint64_t link = 0;
            const uint64_t roots[4] = {chest, bones[BONE_SPINE1], bones[BONE_SPINE], hips};
            for (uint64_t r : roots) {
                if (!r) continue;
                link = find_child_bone(r, arm_chain[side][0]);
                if (link) break;
            }
            if (link) { bones[arm_chain[side][0]] = link; ++found; }
            else link = chest;
            for (int i = 1; i < 4; ++i) {
                uint64_t next = find_child_bone(link, arm_chain[side][i]);
                if (next) { bones[arm_chain[side][i]] = next; ++found; link = next; }
            }
        }

        const int leg_chain[2][4] = {
            {BONE_UPLEG_L, BONE_LEG_L, BONE_FOOT_L, BONE_TOE_L},
            {BONE_UPLEG_R, BONE_LEG_R, BONE_FOOT_R, BONE_TOE_R}
        };
        for (int side = 0; side < 2; ++side) {
            uint64_t link = hips;
            for (int i = 0; i < 4; ++i) {
                uint64_t next = find_child_bone(link, leg_chain[side][i]);
                if (next) { bones[leg_chain[side][i]] = next; ++found; link = next; }
            }
        }

        if (found > best_count) {
            best_count = found;
            memcpy(best_bones, bones, sizeof(bones));
            if (found >= ESP_BONE_COUNT) break;
        }
    }
    if (best_count < 6 || !best_bones[BONE_HIPS]) return false;
    if (!resolve_skeleton_layout(best_bones[BONE_HIPS])) return false;

    uint64_t data = rd_ptr(best_bones[BONE_HIPS] + g_skeleton_layout.data_offset);
    if (!data) return false;

    // Keep bones with a resolvable hierarchy (re-parented bones included).
    int valid_bones = 0;
    for (int bone = 0; bone < ESP_BONE_COUNT; ++bone) {
        uint64_t transform = best_bones[bone];
        if (!transform) continue;
        uint64_t bone_data = rd_ptr(transform + g_skeleton_layout.data_offset);
        int32_t index = rd<int32_t>(transform + g_skeleton_layout.index_offset);
        if (!bone_data || index < 0 || index > 100000) continue;
        skeleton.bone_transform[bone] = transform;
        ++valid_bones;
    }
    if (valid_bones < 6 || !skeleton.bone_transform[BONE_HIPS]) return false;

    skeleton.model_root = root;
    skeleton.bone_count = valid_bones;
    skeleton.valid = true;
    return true;
}

// Build with both strategies and keep the richer skeleton. The name path
// identifies bones on the render rig directly, so it wins ties: the ragdoll
// list can reference physics/hitbox proxy transforms whose upper-body
// positions do not follow the animated model.
static bool build_skeleton(uint64_t player, CachedSkeleton& skeleton) {
    skeleton = CachedSkeleton{};

    CachedSkeleton from_ragdoll{};
    bool ragdoll_ok = build_skeleton_from_ragdoll(player, from_ragdoll);

    CachedSkeleton from_names{};
    bool names_ok = build_skeleton_from_names(player, from_names);

    if (names_ok && (!ragdoll_ok || from_names.bone_count >= from_ragdoll.bone_count)) {
        skeleton = from_names;
    } else if (ragdoll_ok) {
        skeleton = from_ragdoll;
    } else {
        return false;
    }
    return true;
}

// Same math as read_transform_hierarchy_arrays, but on locally buffered arrays.
// Returns 1 on success, 0 on failure, -1 when the parent chain leaves the
// buffered range (caller may retry with a remote walk).
static int skeleton_local_walk(const Matrix34* matrices, const int32_t* parents, int32_t count, int32_t index, Vec3& out) {
    if (index < 0 || index >= count) return -1;
    const Matrix34& current = matrices[index];
    if (!matrix34_is_valid(current)) return 0;
    Vec3 result = {current.translation.x, current.translation.y, current.translation.z};
    if (!vec3_is_finite(result)) return 0;
    int32_t parent = parents[index];
    int32_t previous = index;
    int depth = 0;
    while (parent >= 0 && depth++ < 128) {
        if (parent >= count) return -1;
        if (parent == previous) return 0;
        const Matrix34& matrix = matrices[parent];
        if (!matrix34_is_valid(matrix)) return 0;
        Vec3 scaled = {result.x * matrix.scale.x, result.y * matrix.scale.y, result.z * matrix.scale.z};
        Vec3 rotated = rotate_vector(matrix.rotation, scaled);
        result = {matrix.translation.x + rotated.x, matrix.translation.y + rotated.y, matrix.translation.z + rotated.z};
        if (!vec3_is_finite(result)) return 0;
        previous = parent;
        parent = parents[parent];
    }
    if (parent != -1 || depth >= 128) return 0;
    out = result;
    return 1;
}

static void prune_skeleton_cache(const std::vector<uint64_t>& players) {
    for (auto it = g_skeletons.begin(); it != g_skeletons.end();) {
        bool present = false;
        for (uint64_t player : players) {
            if (player == it->first) { present = true; break; }
        }
        if (!present) it = g_skeletons.erase(it);
        else ++it;
    }
}

static uint64_t resolve_player_kcc(uint64_t player);

static PlayerAux& player_aux(uint64_t player) {
    PlayerAux& aux = g_player_aux[player];
    if (aux.kcc) {
        // Cheap liveness check every frame; full re-resolve occasionally.
        if (rd_ptr(aux.kcc + KCC_PLAYER_BACKREF) != player || ++aux.revalidate >= 300) {
            aux = PlayerAux{};
        }
    }
    if (!aux.kcc) {
        if (aux.retry_cooldown > 0) { --aux.retry_cooldown; return aux; }
        uint64_t kcc = resolve_player_kcc(player);
        if (!kcc) { aux.retry_cooldown = 30; return aux; }
        aux.kcc = kcc;
        aux.revalidate = 0;
        float nh = rd<float>(kcc + KCC_NORMAL_HEIGHT);
        float ch = rd<float>(kcc + KCC_CROUCH_HEIGHT);
        if (std::isfinite(nh) && nh > 1.2F && nh < 2.6F) aux.normal_height = nh;
        if (std::isfinite(ch) && ch > 0.6F && ch < aux.normal_height) aux.crouch_height = ch;
        uint64_t head = rd_ptr(kcc + KCC_HEAD_TRANSFORM);
        aux.head_native = head ? rd_ptr(head + MANAGED_CACHED_PTR) : 0;
        if (aux.head_native && (aux.head_native < 0x10000 || aux.head_native >= 0x0001000000000000ULL))
            aux.head_native = 0;
        // Head hit volume (what the server actually tests shots against).
        aux.head_hitbox_valid = false;
        uint64_t hb_root = rd_ptr(kcc + KCC_HITBOX_ROOT);
        uint64_t hb_array = hb_root ? rd_ptr(hb_root + HITBOX_ROOT_ARRAY) : 0;
        int32_t hb_count = hb_array ? rd<int32_t>(hb_array + IL2CPP_ARRAY_LENGTH) : 0;
        if (hb_count > 0 && hb_count <= 64) {
            for (int32_t i = 0; i < hb_count; ++i) {
                uint64_t hb = rd_ptr(hb_array + IL2CPP_ARRAY_FIRST_ELEMENT + (uint64_t)i * 8);
                if (!hb || rd<int32_t>(hb + HITBOX_AREA) != 0) continue;
                Vec3 center = rd_v3(hb + HITBOX_CENTER);
                Vec3 size = rd_v3(hb + HITBOX_SIZE);
                if (!vec3_is_finite(center) || !vec3_is_finite(size)) continue;
                if (fabsf(center.x) > 1.0F || fabsf(center.y) > 1.0F || fabsf(center.z) > 1.0F) continue;
                if (!(size.x > 0.02F && size.x < 1.0F && size.y > 0.02F && size.y < 1.0F)) continue;
                uint64_t transform = native_component_transform(managed_object_native(hb));
                if (!skeleton_transform_ptr_valid(transform)) continue;
                aux.head_hitbox_transform = transform;
                aux.head_hitbox_center = center;
                aux.head_hitbox_valid = true;
                break;
            }
        }
    }
    return aux;
}

// Pose from KCC.Move: true when crouched (Pose == Crouch or State == CROUCHING).
static bool player_is_crouched(const PlayerAux& aux) {
    if (!aux.kcc) return false;
    int32_t state = rd<int32_t>(aux.kcc + KCC_MOVE + 0x00);
    int32_t pose  = rd<int32_t>(aux.kcc + KCC_MOVE + 0x04);
    return pose == 1 || state == 3;
}

static bool player_head_world(const PlayerAux& aux, Vec3& out) {
    if (!aux.head_native) return false;
    if (g_skeleton_layout_valid &&
        read_transform_hierarchy_layout(aux.head_native, g_skeleton_layout, out)) return vec3_is_finite(out);
    return read_transform_hierarchy_position(aux.head_native, out) && vec3_is_finite(out);
}

// World-space centre of the Head hit volume: transform TRS applied to the
// local centre (HitBox.transform.TransformPoint(center)).
static bool player_head_hitbox_world(const PlayerAux& aux, Vec3& out) {
    if (!aux.head_hitbox_valid || !aux.head_hitbox_transform) return false;
    Vec3 pos{}; Vec4 rot{};
    const TransformHierarchyLayout& layout = g_skeleton_layout_valid ? g_skeleton_layout : g_transform_hierarchy_layout;
    if (!(g_skeleton_layout_valid || g_transform_hierarchy_layout_valid)) return false;
    if (!read_transform_hierarchy_layout(aux.head_hitbox_transform, layout, pos, &rot)) return false;
    // Lossy scale is ~1 on character rigs; rotate the local centre and offset.
    Vec3 offset = rotate_vector(rot, aux.head_hitbox_center);
    out = {pos.x + offset.x, pos.y + offset.y, pos.z + offset.z};
    return vec3_is_finite(out);
}

static void prune_player_aux(const std::vector<uint64_t>& players) {
    for (auto it = g_player_aux.begin(); it != g_player_aux.end();) {
        bool present = false;
        for (uint64_t player : players) if (player == it->first) { present = true; break; }
        if (!present) it = g_player_aux.erase(it); else ++it;
    }
}

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

static bool set_aim_point(EspBox& box, int slot, const Vec3& world, const Mat4& vp, float sw, float sh) {
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

static bool fill_skeleton_box(uint64_t player, const Mat4& view_projection, float sw, float sh, EspBox& box) {
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

// ===================== Local player aim (ADS) state =====================

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

float esp_camera_fov_deg() { return g_cam_fov_deg; }

bool esp_camera_angles(float& yaw_deg, float& pitch_deg) {
    if (!g_cam_pose_valid && !g_aim_ref_valid) return false;
    // Same reference the aim angles are measured against (firing direction
    // when available), so finger-gain learning and target lead stay consistent.
    const Vec3& f = g_aim_ref_valid ? g_aim_ref_forward : g_cam_forward;
    constexpr float rad2deg = 57.29577951F;
    float yaw = atan2f(f.x, f.z) * rad2deg;
    float horiz = sqrtf(f.x * f.x + f.z * f.z);
    float pitch = atan2f(f.y, horiz) * rad2deg;
    if (!std::isfinite(yaw) || !std::isfinite(pitch)) return false;
    yaw_deg = yaw; pitch_deg = pitch;
    return true;
}


// Pipeline status for the on-screen debug line:
//   R  = ragdoll build stage (0 ok; 2 no KCC, 3 no anim, 4 anim backref,
//        5 no ragdoll, 6 no array, 7 bad count, 8/11 few bones, 9 layout,
//        10 arrays, 12 pelvis, 13 chest, 14 final; -1 never ran)
//   H  = hips candidates (name path), N = best name-path bone count
//   P  = build path used (1 ragdoll, 2 names), B = cached bones
//   F  = fill failure (0 ok, 1 cooldown, 2 build, 3 root, 4 hips gone,
//        7 projected<4)
//   C  = players with a valid cached skeleton
//   D  = distinct transform hierarchies used by the bones (re-parenting),
//        then the per-bone mask torso|armL|armR|legL|legR.
bool esp_init(pid_t pid) {
    g_pid = pid;
    g_il2cpp_base = get_base("libil2cpp.so");
    if (!g_il2cpp_base) return false;
    return true;
}

// KCC.Move.Position: the simulated character position (capsule bottom) the
// game itself moves the character with. Independent of the discovered
// PlayerManager position field, and validated by the KCC back-reference.
static bool player_kcc_position(const PlayerAux& aux, Vec3& out) {
    if (!aux.kcc) return false;
    Vec3 p = rd_v3(aux.kcc + KCC_MOVE + 0x0C);
    if (!vec3_is_finite(p)) return false;
    float magnitude = fabsf(p.x) + fabsf(p.y) + fabsf(p.z);
    if (magnitude < 0.01F || magnitude > 100000.0F) return false;
    out = p;
    return true;
}

// Local firing reference: LookDirection from the event handler plus the eye
// point the hitscan ray starts from. Falls back to the camera when unavailable
// or implausible (must stay within ~20 deg of the camera forward).
static void read_local_aim_reference(uint64_t local_player, const PlayerAux* local_aux, bool local_crouched) {
    g_aim_ref_valid = false;
    if (!local_player || !g_cam_pose_valid) return;
    uint64_t handler = rd_ptr(local_player + PLAYER_EVENT_HANDLER);
    if (!handler || rd_ptr(handler + EVENT_HANDLER_MANAGER_BACKREF) != local_player) return;
    uint64_t look = rd_ptr(handler + EVENT_HANDLER_LOOK_DIRECTION);
    if (!look) return;
    Vec3 dir = rd_v3(look + SYNC_VALUE_OFFSET);
    if (!vec3_is_finite(dir)) return;
    float len = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (!(len > 0.5F && len < 2.0F)) return;
    dir = {dir.x / len, dir.y / len, dir.z / len};
    float dot = dir.x * g_cam_forward.x + dir.y * g_cam_forward.y + dir.z * g_cam_forward.z;
    if (!(dot > 0.94F)) return; // > ~20 deg away from the camera: not the look root
    Vec3 world_up = {0.0F, 1.0F, 0.0F};
    Vec3 right = cross_product(world_up, dir);
    float rl = sqrtf(right.x * right.x + right.y * right.y + right.z * right.z);
    if (!(rl > 0.01F)) return; // looking straight up/down: keep camera basis
    right = {right.x / rl, right.y / rl, right.z / rl};
    Vec3 up = cross_product(dir, right);

    // Eye point: KCC position + (capsule height + lookHeightOffset) * up. Only
    // trusted when it lands close to the camera; otherwise use the camera.
    Vec3 origin = g_cam_pos;
    if (local_aux && local_aux->kcc) {
        Vec3 kcc_pos{};
        if (player_kcc_position(*local_aux, kcc_pos)) {
            float h = local_crouched ? local_aux->crouch_height : local_aux->normal_height;
            float look_offset = rd<float>(local_aux->kcc + KCC_LOOK_HEIGHT_OFFSET);
            if (!std::isfinite(look_offset) || fabsf(look_offset) > 1.0F) look_offset = 0.0F;
            Vec3 eye = {kcc_pos.x, kcc_pos.y + h + look_offset, kcc_pos.z};
            float dx = eye.x - g_cam_pos.x, dy = eye.y - g_cam_pos.y, dz = eye.z - g_cam_pos.z;
            if (dx * dx + dy * dy + dz * dz < 0.5F * 0.5F) origin = eye;
        }
    }
    g_aim_ref_origin = origin;
    g_aim_ref_forward = dir;
    g_aim_ref_right = right;
    g_aim_ref_up = up;
    g_aim_ref_valid = true;
}

// Per-frame player list, kept across frames so a transient empty read does
// not blank the overlay, but dropped on a real world change.
static std::vector<uint64_t> g_frame_transforms;
static int      g_frame_transforms_empty_streak = 0;

// Everything derived from a particular world/session. Called when the whole
// player population is replaced (scene reload / new session) or the player
// list disappears for a while, so no stale pointers survive into the next
// world. Deliberately NOT tied to the camera object: the game swaps cameras
// while aiming, which must not disturb boxes or skeletons.
static void reset_world_caches() {
    g_matrix_configuration_validated = false; g_camera_matrix_physical_match = false;
    g_player_position_validated = false;
    g_local_player = 0;
    g_aim_state = {};
    g_aim_ref_valid = false;
    g_player_aux.clear();
    g_skeletons.clear();
}

void esp_reset() {
    g_pid = -1; g_il2cpp_base = 0;
    g_frame_transforms.clear(); g_frame_transforms_empty_streak = 0;
    g_aim_ref_valid = false;
    g_player_manager_class = 0; g_player_manager_static_fields = 0;
    g_game_controller_class = 0; g_local_player = 0;
    g_matrix_configuration_validated = false; g_camera_matrix_physical_match = false;
    g_player_position_offset = PLAYER_POSITION;
    g_transform_hierarchy_layout = {}; g_transform_hierarchy_layout_valid = false;
    g_use_direct_player_position = true;
    g_player_position_validated = false;
    g_cam_fov_deg = 0.0F; g_cam_pose_valid = false;
    g_aim_state = {};
    g_player_aux.clear();
    g_skeletons.clear();
    g_skeleton_layout = {}; g_skeleton_layout_valid = false;
    g_go_name_offset = 0; g_go_name_plain_pointer = false;
    g_go_name_offset_valid = false; g_go_name_retry_cooldown = 0;
}


std::vector<EspBox> esp_get_boxes(int overlay_width, int overlay_height) {
    std::vector<EspBox> result;

    if (g_pid <= 0 || !g_il2cpp_base) { return result; }

    uint64_t native_cam = 0;
    Mat4 projection{}, view{}, vp{};
    float sw = overlay_width >= 100 ? (float)overlay_width : 1080.0F;
    float sh = overlay_height >= 100 ? (float)overlay_height : 2400.0F;
    if (!std::isfinite(sw) || sw < 100.0F || sw > 10000.0F) sw = 1080.0F;
    if (!std::isfinite(sh) || sh < 100.0F || sh > 10000.0F) sh = 2400.0F;

    std::vector<uint64_t>& s_transforms = g_frame_transforms;
    std::vector<uint64_t> refreshed = read_configured_player_transforms();
    if (!refreshed.empty()) {
        // World reload: every PlayerManager object is new (no overlap with
        // the previous population, which was more than just ourselves).
        if (s_transforms.size() >= 2) {
            bool overlap = false;
            for (uint64_t previous : s_transforms) {
                for (uint64_t current : refreshed) if (previous == current) { overlap = true; break; }
                if (overlap) break;
            }
            if (!overlap) reset_world_caches();
        }
        s_transforms = std::move(refreshed);
        g_frame_transforms_empty_streak = 0;
    } else if (++g_frame_transforms_empty_streak > 10) {
        // List gone for a while: the world is being torn down / reloaded.
        if (!s_transforms.empty()) { s_transforms.clear(); reset_world_caches(); }
        return result;
    }
    if (s_transforms.empty()) return result;

    const bool want_bones = g_skeleton_enabled || g_aim_bones_requested;
    prune_player_aux(s_transforms);
    if (want_bones) prune_skeleton_cache(s_transforms);
    else if (!g_skeletons.empty()) g_skeletons.clear();
    g_skeleton_builds_this_frame = 0;

    if (!g_player_position_validated) {
        if (!discover_player_position_offset(s_transforms)) return result;
    }

    bool transform_camera_mode = false; // light fix: always use native cam matrices, avoid dead-body-as-camera on death
    if (!transform_camera_mode) {
        uint64_t managed_cam = 0;
        if (g_game_controller_class) {
            uint64_t gcb_sf = get_class_static_fields(g_game_controller_class);
            if (gcb_sf) {
                uint64_t cam_mgr = rd_ptr(gcb_sf + GAME_CONTROLLER_CAMERA_MANAGER_FIELD);
                if (cam_mgr) managed_cam = rd_ptr(cam_mgr + CAMERA_MANAGER_CAMERA_FIELD);
            }
        }
        if (!managed_cam) { return result; }
        native_cam = rd_ptr(managed_cam + MANAGED_CACHED_PTR);
        if (!native_cam) return result;
        if (!read_native_camera_matrices(native_cam, sw / sh, projection, view)) return result;
        if (!g_matrix_configuration_validated) {
            if (!optimize_matrix_configuration(native_cam, s_transforms)) return result;
            if (!read_native_camera_matrices(native_cam, sw / sh, projection, view)) return result;
        }
        // Unity worldToClip = projection * worldToCamera (same order as native 0xe2b90c).
        vp = mat_mul(projection, view);
    }

    bool has_local_position = false;
    Vec3 local{};
    size_t local_entity_index = s_transforms.size();

    {
        Vec3 camera_position{};
        bool has_camera_position = g_camera_matrix_physical_match && camera_position_from_view(view, camera_position);
        double nearest_distance_squared = INFINITY;
        size_t first_valid_index = s_transforms.size();
        Vec3 first_valid_position{};
        for (size_t index = 0; index < s_transforms.size(); ++index) {
            Vec3 candidate{};
            if (!read_entity_position(s_transforms[index], candidate)) continue;
            if (first_valid_index == s_transforms.size()) { first_valid_index = index; first_valid_position = candidate; }
            if (!has_camera_position) continue;
            double dx = (double)candidate.x - camera_position.x, dy = (double)candidate.y - camera_position.y, dz = (double)candidate.z - camera_position.z;
            double distance_squared = dx * dx + dy * dy + dz * dz;
            if (std::isfinite(distance_squared) && distance_squared < nearest_distance_squared) {
                nearest_distance_squared = distance_squared; local_entity_index = index; local = candidate;
            }
        }
        if (local_entity_index == s_transforms.size() && first_valid_index != s_transforms.size()) {
            local_entity_index = first_valid_index; local = first_valid_position;
        }
        has_local_position = local_entity_index != s_transforms.size();
        if (!has_local_position) {
            g_player_position_validated = false;
            return result;
        }
    }

    Vec3 transform_camera_position{};
    Vec4 transform_camera_rotation{};
    if (transform_camera_mode) {
        if (local_entity_index >= s_transforms.size() || !read_entity_pose(s_transforms[local_entity_index], transform_camera_position, transform_camera_rotation)) {
            g_player_position_validated = false;
            return result;
        }
        local = transform_camera_position; has_local_position = true;
    }

    // Firing reference for the aimbot (local look direction + eye point).
    {
        uint64_t local_player = (local_entity_index < s_transforms.size()) ? s_transforms[local_entity_index] : 0;
        const PlayerAux* local_aux = nullptr;
        bool local_crouched = false;
        if (local_player && g_aim_bones_requested) {
            PlayerAux& la = player_aux(local_player);
            local_aux = &la;
            local_crouched = player_is_crouched(la);
        }
        read_local_aim_reference(local_player, local_aux, local_crouched);
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

        // Crouch-aware body height from the character controller.
        PlayerAux& aux = player_aux(s_transforms[i]);
        const bool crouched = player_is_crouched(aux);
        float body_height = crouched ? aux.crouch_height : aux.normal_height;
        if (!(body_height > 0.6F && body_height < 2.6F)) body_height = PLAYER_HEIGHT;

        Vec3 body_bottom = {feet.x, feet.y, feet.z};
        Vec3 body_top = {feet.x, feet.y + body_height, feet.z};
        if (transform_camera_mode || !g_use_direct_player_position) { body_bottom.y = feet.y - 1.60F; body_top.y = feet.y + 0.20F; }

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
            {feet.x - box_half_width, body_bottom.y, feet.z - box_half_depth},
            {feet.x + box_half_width, body_bottom.y, feet.z - box_half_depth},
            {feet.x + box_half_width, body_bottom.y, feet.z + box_half_depth},
            {feet.x - box_half_width, body_bottom.y, feet.z + box_half_depth},
            {feet.x - box_half_width, body_top.y, feet.z - box_half_depth},
            {feet.x + box_half_width, body_top.y, feet.z - box_half_depth},
            {feet.x + box_half_width, body_top.y, feet.z + box_half_depth},
            {feet.x - box_half_width, body_top.y, feet.z + box_half_depth}
        };
        EspBox box{};
        box.id = s_transforms[i];
        box.crouched = crouched;
        box.aim_source = 0;
        box.x1 = cx - half_w; box.y1 = cy - half_h;
        box.x2 = cx + half_w; box.y2 = cy + half_h;
        box.distance = distance;
        for (size_t corner = 0; corner < 8; ++corner) {
            Vec2 sc{};
            bool projected = transform_camera_mode
                ? w2s_transform_camera(transform_camera_position, transform_camera_rotation, world_corners[corner], sw, sh, sc, false)
                : w2s(vp, world_corners[corner], sw, sh, sc, false);
            box.corner_visible[corner] = projected && sc.x >= 0.0F && sc.x <= sw && sc.y >= 0.0F && sc.y <= sh;
            box.corners[corner][0] = projected ? sc.x : -1.0F;
            box.corners[corner][1] = projected ? sc.y : -1.0F;
        }
        if (want_bones && !transform_camera_mode)
            fill_skeleton_box(s_transforms[i], vp, sw, sh, box);

        // Head slot: prefer the centre of the server-side Head hit volume over
        // the rig-derived estimate. This is the exact volume the shot is tested
        // against, so it removes the constant model/hitbox offset that makes
        // long-range head shots land low. Only accepted when it lies within
        // the body (plausibility vs feet) so a stale transform cannot hijack it.
        if (g_aim_bones_requested && !transform_camera_mode) {
            Vec3 hb{};
            if (player_head_hitbox_world(aux, hb)) {
                float dy = hb.y - feet.y;
                float dx = hb.x - feet.x, dz = hb.z - feet.z;
                if (dy > 0.5F && dy < 2.4F && (dx * dx + dz * dz) < 1.0F) {
                    bool agree = true;
                    if (box.aim_valid[0]) {
                        // Compare against the rig head point in screen space:
                        // reject if wildly different (different body).
                        Vec2 hs{};
                        if (w2s(vp, hb, sw, sh, hs, false)) {
                            float ex = hs.x - box.aim_pts[0][0], ey = hs.y - box.aim_pts[0][1];
                            float bh = fabsf(box.y2 - box.y1);
                            agree = (ex * ex + ey * ey) < (bh * 0.25F) * (bh * 0.25F) + 4.0F;
                        }
                    }
                    if (agree && set_aim_point(box, 0, hb, vp, sw, sh) && box.aim_source == 0) box.aim_source = 1;
                }
            }
        }

        // Aim fallbacks when rig bones are not (yet) available, so the aimbot
        // always has a crouch-aware target instead of a fixed-height guess.
        if (g_aim_bones_requested && !transform_camera_mode &&
            !(box.aim_valid[0] && box.aim_valid[1] && box.aim_valid[2])) {
            Vec3 head{};
            bool have_head = false;
            // (2) game-maintained head transform (moves with crouch/animation)
            if (player_head_world(aux, head)) {
                float dy = head.y - feet.y;
                have_head = dy > 0.4F && dy < 2.4F;
            }
            const float n_down = 0.12F;                    // head -> neck
            const float c_down = crouched ? 0.26F : 0.34F; // head -> chest
            if (have_head) {
                if (!box.aim_valid[0]) { Vec3 t = head; t.y += 0.03F; if (set_aim_point(box, 0, t, vp, sw, sh) && box.aim_source == 0) box.aim_source = 2; }
                if (!box.aim_valid[1]) { Vec3 t = head; t.y -= n_down; if (set_aim_point(box, 1, t, vp, sw, sh) && box.aim_source == 0) box.aim_source = 2; }
                if (!box.aim_valid[2]) { Vec3 t = head; t.y -= c_down; if (set_aim_point(box, 2, t, vp, sw, sh) && box.aim_source == 0) box.aim_source = 2; }
            }
            // (3) feet + pose height estimate
            if (!box.aim_valid[0]) { Vec3 t = feet; t.y += body_height - 0.12F; if (set_aim_point(box, 0, t, vp, sw, sh) && box.aim_source == 0) box.aim_source = 3; }
            if (!box.aim_valid[1]) { Vec3 t = feet; t.y += body_height - 0.26F; if (set_aim_point(box, 1, t, vp, sw, sh) && box.aim_source == 0) box.aim_source = 3; }
            if (!box.aim_valid[2]) { Vec3 t = feet; t.y += body_height * 0.72F; if (set_aim_point(box, 2, t, vp, sw, sh) && box.aim_source == 0) box.aim_source = 3; }
        }
        result.push_back(box);
    }

    return result;
}
