#include "game.h"
#include "game_offsets.h"
#include "Vector.h"

#include <string.h>
#include <sys/uio.h>
#include <stdio.h>
#include <time.h>
#include <algorithm>
#include <cmath>
#include <string>
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

// ============================ Skeleton ESP ============================
// Внешняя адаптация внутреннего скелетона: внутренний вариант звал
// Transform::GetChild / Object::GetName и обходил иерархию костей по именам
// ("Hips", "Arm.L", ...). Снаружи вызывать icall нельзя, а эта сборка игры
// прячет таблицу icall, поэтому оффсеты (first-child / next-sibling /
// GameObject / имя) находятся один раз за сессию рантайм-дискавери и
// валидируются по набору имён гуманоидного рига. Мировые позиции костей
// каждый кадр берутся из уже проверенных массивов TransformData, поэтому
// скелет полностью динамичный и двигается вместе с конечностями.

struct SkeletonLayout {
    bool     valid = false;
    uint64_t data_off = 0x38;   // native Transform -> TransformData*
    uint64_t idx_off  = 0x40;   // native Transform -> индекс в батче
    uint64_t head_off = 0;      // native Transform -> первый ребёнок
    uint64_t next_off = 0;      // ребёнок -> следующий сиблинг
    uint64_t go_off   = 0;      // native Transform -> GameObject*
    uint64_t name_off = 0;      // GameObject -> хранилище имени
    bool     name_indirect = false; // имя лежит по char*, а не инлайном
};
static SkeletonLayout g_skel_layout;
static uint64_t g_skel_last_try_ms = 0;
// сид иерархии: 0 = worldCameraRoot игрока, 1 = transform модели персонажа
static int       g_skel_seed_mode = 0;
static uint64_t  g_skel_seed_go_off = 0;

struct BoneAlias { int bone; const char* names[4]; };
static const BoneAlias kSkelBoneAliases[] = {
    {BONE_HIPS,       {"Hips", "hips", "Pelvis"}},
    {BONE_SPINE,      {"Spine", "spine"}},
    {BONE_SPINE1,     {"Spine1", "Spine_1"}},
    {BONE_SPINE2,     {"Spine2", "Spine_2", "Chest"}},
    {BONE_NECK,       {"Neck", "neck"}},
    {BONE_HEAD,       {"Head", "head"}},
    {BONE_SHOULDER_L, {"Shoulder.L", "LeftShoulder"}},
    {BONE_ARM_L,      {"Arm.L", "UpperArm.L"}},
    {BONE_FOREARM_L,  {"ForeArm.L", "LowerArm.L"}},
    {BONE_HAND_L,     {"Hand.L", "LeftHand"}},
    {BONE_SHOULDER_R, {"Shoulder.R", "RightShoulder"}},
    {BONE_ARM_R,      {"Arm.R", "UpperArm.R"}},
    {BONE_FOREARM_R,  {"ForeArm.R", "LowerArm.R"}},
    {BONE_HAND_R,     {"Hand.R", "RightHand"}},
    {BONE_UPLEG_L,    {"UpLeg.L", "LeftUpLeg"}},
    {BONE_LEG_L,      {"Leg.L", "LeftLeg"}},
    {BONE_FOOT_L,     {"Foot.L", "LeftFoot"}},
    {BONE_TOEBASE_L,  {"ToeBase.L", "LeftToeBase"}},
    {BONE_UPLEG_R,    {"UpLeg.R", "RightUpLeg"}},
    {BONE_LEG_R,      {"Leg.R", "RightLeg"}},
    {BONE_FOOT_R,     {"Foot.R", "RightFoot"}},
    {BONE_TOEBASE_R,  {"ToeBase.R", "RightToeBase"}},
};

static int skel_bone_from_name(const std::string& name) {
    if (name.empty() || name.size() > 32) return -1;
    for (const BoneAlias& alias : kSkelBoneAliases)
        for (const char* candidate : alias.names) {
            if (!candidate) break;
            if (name == candidate) return alias.bone;
        }
    return -1;
}

static uint64_t skel_now_ms() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static bool skel_plausible_ptr(uint64_t v) {
    return v >= 0x10000 && v < 0x0001000000000000ULL && (v & 7) == 0;
}

static bool skel_read_buf(uint64_t addr, void* dst, size_t len) {
    if (!addr || !dst || !len) return false;
    struct iovec lv = { dst, len };
    struct iovec rv = { (void*)addr, len };
    return remote_vm_readv(g_pid, &lv, 1, &rv, 1, 0) == (ssize_t)len;
}

// Кандидат имени: 24 байта на go+off — либо инлайн-символы, либо char* в первом qword.
static bool skel_read_name(uint64_t go, uint64_t off, bool indirect, std::string& out) {
    if (!go || off > 0x400) return false;
    uint8_t buf[24]{};
    if (!skel_read_buf(go + off, buf, sizeof(buf))) return false;
    const uint8_t* p = buf;
    if (indirect) {
        uint64_t ptr = 0;
        memcpy(&ptr, buf, sizeof(ptr));
        if (!skel_plausible_ptr(ptr)) return false;
        if (!skel_read_buf(ptr, buf, sizeof(buf))) return false;
    }
    int len = 0;
    while (len < (int)sizeof(buf) && p[len] != 0) ++len;
    if (len < 2 || len >= (int)sizeof(buf)) return false;
    for (int i = 0; i < len; ++i)
        if (p[i] < 0x20 || p[i] > 0x7E) return false;
    out.assign((const char*)p, (size_t)len);
    return true;
}

struct SkelNode { uint64_t transform; int32_t index; };

// Дети трансформа через связный список (head у родителя, next у ребёнка).
// Оракул корректности: все трансформы поддерева живут в одном TransformData,
// и индекс ребёнка всегда больше индекса родителя (инвариант Unity).
static int skel_collect_children(uint64_t parent, uint64_t head_off, uint64_t next_off,
                                 uint64_t data_off, uint64_t idx_off, uint64_t expect_data,
                                 int32_t parent_index, SkelNode* out, int max_out) {
    uint64_t child = rd_ptr(parent + head_off);
    const uint64_t sentinel_lo = parent + head_off;
    const uint64_t sentinel_hi = sentinel_lo + 16;
    int count = 0, guard = 0;
    while (skel_plausible_ptr(child) && child != parent &&
           !(child >= sentinel_lo && child < sentinel_hi) && guard++ < 256) {
        if (expect_data) {
            uint64_t data = rd_ptr(child + data_off);
            if (data != expect_data) return -1;
            int32_t ci = rd<int32_t>(child + idx_off);
            if (ci < 0 || ci > 100000 || ci <= parent_index) return -1;
            if (count < max_out) { out[count].transform = child; out[count].index = ci; }
        } else if (count < max_out) {
            out[count].transform = child;
            out[count].index = rd<int32_t>(child + idx_off);
        }
        ++count;
        if (count > 96) return -1;
        uint64_t next = rd_ptr(child + next_off);
        if (next == child) return -1;
        child = next;
    }
    return count;
}

static bool skel_collect_subtree(uint64_t root, uint64_t head_off, uint64_t next_off,
                                 uint64_t data_off, uint64_t idx_off, uint64_t expect_data,
                                 std::vector<SkelNode>& out, int max_nodes) {
    out.clear();
    if (!skel_plausible_ptr(root)) return false;
    int32_t root_index = rd<int32_t>(root + idx_off);
    if (root_index < 0 || root_index > 100000) return false;
    std::vector<SkelNode> stack;
    std::unordered_set<uint64_t> visited;
    stack.push_back({root, root_index});
    visited.insert(root);
    SkelNode buffer[96];
    while (!stack.empty() && (int)out.size() < max_nodes) {
        SkelNode node = stack.back();
        stack.pop_back();
        out.push_back(node);
        int children = skel_collect_children(node.transform, head_off, next_off, data_off, idx_off,
                                             expect_data, node.index, buffer, 96);
        if (children < 0) return false;
        for (int i = 0; i < children; ++i) {
            if (!visited.insert(buffer[i].transform).second) return false; // цикл — неверные оффсеты
            if (visited.size() > 1024) return false;
            stack.push_back(buffer[i]);
        }
    }
    return !stack.empty() || (int)out.size() >= 2;
}

static int skel_score_names(const std::vector<SkelNode>& nodes, uint64_t go_off, uint64_t name_off,
                            bool indirect, int max_check, int& bone_hits) {
    int ascii_ok = 0;
    bone_hits = 0;
    int checked = 0;
    for (const SkelNode& node : nodes) {
        if (checked >= max_check) break;
        ++checked;
        uint64_t go = rd_ptr(node.transform + go_off);
        if (!skel_plausible_ptr(go)) continue;
        std::string name;
        if (!skel_read_name(go, name_off, indirect, name)) continue;
        ++ascii_ok;
        if (skel_bone_from_name(name) >= 0) ++bone_hits;
    }
    return ascii_ok;
}

// Одноразовый рантайм-дискавери оффсетов скелета (стиль discover_transform_hierarchy_layout).
static bool skel_discover_layout(uint64_t root_native) {
    if (!skel_plausible_ptr(root_native)) return false;
    const uint64_t data_idx_pairs[2][2] = {{0x38, 0x40}, {0x18, 0x20}};
    uint64_t data_off = 0, idx_off = 0, data0 = 0;
    for (const auto& pair : data_idx_pairs) {
        uint64_t d = rd_ptr(root_native + pair[0]);
        int32_t ix = rd<int32_t>(root_native + pair[1]);
        if (skel_plausible_ptr(d) && ix >= 0 && ix <= 100000) {
            data_off = pair[0]; idx_off = pair[1]; data0 = d;
            break;
        }
    }
    if (!data0 || !idx_off) return false;

    struct TravCandidate { uint64_t head, next; };
    TravCandidate candidates[8];
    int candidate_count = 0;
    static std::vector<SkelNode> nodes;
    for (uint64_t head = 0x28; head <= 0x98 && candidate_count < 8; head += 8) {
        for (uint64_t next = 0x28; next <= 0xA0 && candidate_count < 8; next += 8) {
            if (next == head) continue;
            if (!skel_collect_subtree(root_native, head, next, data_off, idx_off, data0, nodes, 384)) continue;
            if (nodes.size() < 16 || nodes.size() > 320) continue;
            candidates[candidate_count++] = {head, next};
            break;
        }
    }
    if (!candidate_count) return false;

    for (int ci = 0; ci < candidate_count; ++ci) {
        if (!skel_collect_subtree(root_native, candidates[ci].head, candidates[ci].next,
                                  data_off, idx_off, data0, nodes, 384)) continue;
        for (uint64_t go_off = 0x18; go_off <= 0x38; go_off += 8) {
            // быстрый префильтр: GameObject-указатель должен быть валиден у большинства узлов
            int plausible = 0, sampled = 0;
            for (const SkelNode& node : nodes) {
                if (sampled >= 16) break;
                ++sampled;
                if (skel_plausible_ptr(rd_ptr(node.transform + go_off))) ++plausible;
            }
            if (sampled > 0 && plausible < sampled * 3 / 4) continue;
            for (uint64_t name_off = 0x08; name_off <= 0x78; name_off += 8) {
                for (int indirect = 0; indirect < 2; ++indirect) {
                    int bone_hits = 0;
                    int ascii_ok = skel_score_names(nodes, go_off, name_off, indirect != 0, 32, bone_hits);
                    if (bone_hits >= 8 && ascii_ok >= 10) {
                        g_skel_layout.data_off = data_off;
                        g_skel_layout.idx_off = idx_off;
                        g_skel_layout.head_off = candidates[ci].head;
                        g_skel_layout.next_off = candidates[ci].next;
                        g_skel_layout.go_off = go_off;
                        g_skel_layout.name_off = name_off;
                        g_skel_layout.name_indirect = indirect != 0;
                        g_skel_layout.valid = true;
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

struct PlayerSkel {
    uint64_t player = 0;
    uint64_t root_native = 0;
    uint64_t transform_data = 0;
    int32_t  root_index = -1;
    int32_t  bone_index[ESP_BONE_COUNT];
    int      found = 0;
    int32_t  span = 0;
    uint64_t next_build_ms = 0;
    uint64_t last_ok_ms = 0;
    // буферы кадра
    std::vector<Matrix34> mats;
    std::vector<int32_t>  idxs;
    std::vector<int32_t>  anc_index, anc_parent;
    std::vector<Matrix34> anc_mat;
};
static std::vector<PlayerSkel> g_skel_players;

// Корень иерархии костей конкретного игрока (по найденному при дискавери сиду).
static uint64_t skel_root_for_player(uint64_t player) {
    uint64_t t = resolve_player_native_transform(player);
    if (!t || g_skel_seed_mode != 1) return t;
    uint64_t managed_go = rd_ptr(player + PLAYER_CHARACTER_MODEL);
    if (!skel_plausible_ptr(managed_go)) return t;
    uint64_t native_go = rd_ptr(managed_go + MANAGED_CACHED_PTR);
    if (!skel_plausible_ptr(native_go)) return t;
    uint64_t alt = rd_ptr(native_go + g_skel_seed_go_off);
    return skel_plausible_ptr(alt) ? alt : t;
}

// Дискавери: сначала worldCameraRoot игрока, затем transform модели персонажа
// (GameObject -> Transform через перебор оффсетов, validated по именам костей).
static bool skel_try_discover(uint64_t player) {
    if (skel_discover_layout(resolve_player_native_transform(player))) {
        g_skel_seed_mode = 0;
        return true;
    }
    uint64_t managed_go = rd_ptr(player + PLAYER_CHARACTER_MODEL);
    if (!skel_plausible_ptr(managed_go)) return false;
    uint64_t native_go = rd_ptr(managed_go + MANAGED_CACHED_PTR);
    if (!skel_plausible_ptr(native_go)) return false;
    static const uint64_t kGoToTransformOffsets[] = {0x28, 0x30, 0x38, 0x40, 0x48};
    for (uint64_t off : kGoToTransformOffsets) {
        uint64_t seed = rd_ptr(native_go + off);
        if (!skel_plausible_ptr(seed)) continue;
        if (skel_discover_layout(seed)) {
            g_skel_seed_mode = 1;
            g_skel_seed_go_off = off;
            return true;
        }
    }
    return false;
}

static bool skel_build_player(PlayerSkel& s, uint64_t player) {
    uint64_t root = skel_root_for_player(player);
    if (!skel_plausible_ptr(root)) return false;
    uint64_t data = rd_ptr(root + g_skel_layout.data_off);
    int32_t ridx = rd<int32_t>(root + g_skel_layout.idx_off);
    if (!skel_plausible_ptr(data) || ridx < 0 || ridx > 100000) return false;

    static std::vector<SkelNode> nodes;
    if (!skel_collect_subtree(root, g_skel_layout.head_off, g_skel_layout.next_off,
                              g_skel_layout.data_off, g_skel_layout.idx_off, data, nodes, 384))
        return false;

    for (int b = 0; b < ESP_BONE_COUNT; ++b) s.bone_index[b] = -1;
    int found = 0;
    int32_t max_idx = ridx;
    for (const SkelNode& node : nodes) {
        uint64_t go = rd_ptr(node.transform + g_skel_layout.go_off);
        if (!skel_plausible_ptr(go)) continue;
        std::string name;
        if (!skel_read_name(go, g_skel_layout.name_off, g_skel_layout.name_indirect, name)) continue;
        int bone = skel_bone_from_name(name);
        if (bone < 0 || s.bone_index[bone] >= 0) continue;
        if (node.index > ridx + 2048) continue;
        s.bone_index[bone] = node.index;
        ++found;
        if (node.index > max_idx) max_idx = node.index;
    }
    // обязательные кости: таз/спина и голова/шея, иначе это не риг персонажа
    bool core = (s.bone_index[BONE_HIPS] >= 0 || s.bone_index[BONE_SPINE] >= 0) &&
                (s.bone_index[BONE_HEAD] >= 0 || s.bone_index[BONE_NECK] >= 0);
    if (!core || found < 10) return false;

    s.player = player;
    s.root_native = root;
    s.transform_data = data;
    s.root_index = ridx;
    s.span = max_idx - ridx;
    if (s.span < 1) s.span = 1;
    if (s.span > 2048) s.span = 2048;
    s.found = found;
    s.last_ok_ms = skel_now_ms();
    return true;
}

static bool skel_resolve_arrays(uint64_t data, uint64_t& matrices, uint64_t& indices) {
    if (g_transform_hierarchy_layout_valid) {
        const TransformHierarchyLayout& layout = g_transform_hierarchy_layout;
        matrices = rd_ptr(data + layout.matrices_offset);
        indices  = rd_ptr(data + layout.indices_offset);
        if (layout.matrices_indirect) matrices = rd_ptr(matrices);
        if (layout.indices_indirect) indices = rd_ptr(indices);
        if (skel_plausible_ptr(matrices) && skel_plausible_ptr(indices)) return true;
    }
    const uint64_t pairs[2][2] = {{0x18, 0x20}, {0x08, 0x10}};
    for (const auto& pair : pairs) {
        uint64_t m = rd_ptr(data + pair[0]);
        uint64_t i = rd_ptr(data + pair[1]);
        if (!skel_plausible_ptr(m) || !skel_plausible_ptr(i)) continue;
        uint64_t m2 = rd_ptr(m), i2 = rd_ptr(i);
        if (skel_plausible_ptr(m2) && skel_plausible_ptr(i2)) { matrices = m2; indices = i2; return true; }
        matrices = m; indices = i;
        return true;
    }
    return false;
}

static void skel_fill_box(PlayerSkel& s, const Mat4& vp, float sw, float sh, EspBox& box) {
    box.skel_valid = false;
    if (s.found < 10 || !s.root_native) return;

    uint64_t data = rd_ptr(s.root_native + g_skel_layout.data_off);
    int32_t ridx = rd<int32_t>(s.root_native + g_skel_layout.idx_off);
    if (data != s.transform_data || ridx != s.root_index) { s.found = 0; s.next_build_ms = skel_now_ms(); return; }

    uint64_t matrices = 0, indices = 0;
    if (!skel_resolve_arrays(data, matrices, indices)) return;

    const int32_t span = s.span;
    s.idxs.resize((size_t)span + 1);
    s.mats.resize((size_t)span + 1);
    if (!skel_read_buf(indices + (uint64_t)s.root_index * 4, s.idxs.data(), (size_t)(span + 1) * 4)) return;
    if (!skel_read_buf(matrices + (uint64_t)s.root_index * sizeof(Matrix34), s.mats.data(), (size_t)(span + 1) * sizeof(Matrix34))) return;

    // предки рута (над слайсом): цепочка родителей до -1
    s.anc_index.clear(); s.anc_parent.clear(); s.anc_mat.clear();
    int32_t parent = s.idxs[0];
    int guard = 0;
    while (parent >= 0 && parent < s.root_index && guard++ < 128) {
        Matrix34 m{};
        if (!rd_exact(matrices + (uint64_t)parent * sizeof(Matrix34), m) || !matrix34_is_valid(m)) break;
        int32_t pp = rd<int32_t>(indices + (uint64_t)parent * 4);
        if (pp < -1 || pp > 100000) break;
        s.anc_index.push_back(parent);
        s.anc_mat.push_back(m);
        s.anc_parent.push_back(pp);
        parent = pp;
    }
    const int32_t anc_count = (int32_t)s.anc_index.size();
    auto anc_parent_of = [&](int32_t index) -> int32_t {
        for (int32_t k = 0; k < anc_count; ++k)
            if (s.anc_index[k] == index) return s.anc_parent[k];
        return -1;
    };

    // мировая позиция по локальным матрицам (та же математика, что и в иерархии выше)
    auto world_at = [&](int32_t index, Vec3& out) -> bool {
        if (index < s.root_index || index > s.root_index + span) return false;
        Matrix34 current = s.mats[index - s.root_index];
        if (!matrix34_is_valid(current)) return false;
        Vec3 result = {current.translation.x, current.translation.y, current.translation.z};
        if (!vec3_is_finite(result)) return false;
        int32_t par = s.idxs[index - s.root_index];
        int32_t cur = index;
        int depth = 0;
        while (par != -1) {
            if (par < 0 || par >= cur || ++depth > 128) return false;
            const Matrix34* m = nullptr;
            if (par >= s.root_index) m = &s.mats[par - s.root_index];
            else
                for (int32_t k = 0; k < anc_count; ++k)
                    if (s.anc_index[k] == par) { m = &s.anc_mat[k]; break; }
            if (!m || !matrix34_is_valid(*m)) return false;
            Vec3 scaled = {result.x * m->scale.x, result.y * m->scale.y, result.z * m->scale.z};
            Vec3 rotated = rotate_vector(m->rotation, scaled);
            result = {m->translation.x + rotated.x, m->translation.y + rotated.y, m->translation.z + rotated.z};
            if (!vec3_is_finite(result)) return false;
            cur = par;
            par = (par >= s.root_index) ? s.idxs[par - s.root_index] : anc_parent_of(par);
        }
        out = result;
        return true;
    };

    int projected = 0;
    for (int b = 0; b < ESP_BONE_COUNT; ++b) {
        box.skel_visible[b] = false;
        box.skel_x[b] = -1.0F;
        box.skel_y[b] = -1.0F;
        if (s.bone_index[b] < 0) continue;
        Vec3 world{};
        if (!world_at(s.bone_index[b], world)) continue;
        Vec2 screen{};
        if (!w2s(vp, world, sw, sh, screen, false)) continue;
        box.skel_x[b] = screen.x;
        box.skel_y[b] = screen.y;
        box.skel_visible[b] = true;
        ++projected;
    }
    box.skel_valid = projected >= 6;
    if (box.skel_valid) s.last_ok_ms = skel_now_ms();
}

static void skel_update_player(uint64_t player, const Mat4& vp, float sw, float sh, EspBox& box) {
    box.skel_valid = false;
    if (!g_skel_layout.valid) return;
    PlayerSkel* s = nullptr;
    for (PlayerSkel& entry : g_skel_players)
        if (entry.player == player) { s = &entry; break; }
    if (!s) {
        g_skel_players.push_back(PlayerSkel{});
        s = &g_skel_players.back();
        s->player = player;
    }
    const uint64_t now = skel_now_ms();
    if (s->found <= 0) {
        if (now < s->next_build_ms) return;
        s->next_build_ms = now + 1500;
        if (!skel_build_player(*s, player)) return;
    } else if (s->last_ok_ms && now - s->last_ok_ms > 30000) {
        // периодический ребилд (модель могли пересоздать)
        if (now < s->next_build_ms) return;
        s->next_build_ms = now + 1500;
        s->found = 0;
        if (!skel_build_player(*s, player)) return;
    }
    skel_fill_box(*s, vp, sw, sh, box);
    if (!box.skel_valid && s->last_ok_ms && now - s->last_ok_ms > 5000) {
        s->found = 0;
        s->next_build_ms = now + 1500;
    }
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

bool esp_init(pid_t pid) {
    g_pid = pid;
    g_il2cpp_base = get_base("libil2cpp.so");
    if (!g_il2cpp_base) return false;
    return true;
}

void esp_reset() {
    g_pid = -1; g_il2cpp_base = 0;
    g_player_manager_class = 0; g_player_manager_static_fields = 0;
    g_game_controller_class = 0; g_local_player = 0;
    g_matrix_configuration_validated = false; g_camera_matrix_physical_match = false;
    g_player_position_offset = PLAYER_POSITION;
    g_transform_hierarchy_layout = {}; g_transform_hierarchy_layout_valid = false;
    g_use_direct_player_position = true;
    g_player_position_validated = false;
    g_skel_layout = SkeletonLayout{};
    g_skel_players.clear();
    g_skel_last_try_ms = 0;
    g_skel_seed_mode = 0;
    g_skel_seed_go_off = 0;
}


std::vector<EspBox> esp_get_boxes(int overlay_width, int overlay_height, bool with_skeleton) {
    std::vector<EspBox> result;

    if (g_pid <= 0 || !g_il2cpp_base) { return result; }

    uint64_t native_cam = 0;
    Mat4 projection{}, view{}, vp{};
    float sw = overlay_width >= 100 ? (float)overlay_width : 1080.0F;
    float sh = overlay_height >= 100 ? (float)overlay_height : 2400.0F;
    if (!std::isfinite(sw) || sw < 100.0F || sw > 10000.0F) sw = 1080.0F;
    if (!std::isfinite(sh) || sh < 100.0F || sh > 10000.0F) sh = 2400.0F;

    static std::vector<uint64_t> s_transforms;
    std::vector<uint64_t> refreshed = read_configured_player_transforms();
    if (!refreshed.empty()) s_transforms = std::move(refreshed);
    if (s_transforms.empty()) return result;

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

    // Skeleton ESP: одноразовый рантайм-дискавери оффсетов (не чаще раза в 2с).
    if (with_skeleton && !g_skel_layout.valid && s_transforms.size() > 0) {
        uint64_t now = skel_now_ms();
        if (now - g_skel_last_try_ms > 2000) {
            g_skel_last_try_ms = now;
            skel_try_discover(s_transforms[local_entity_index < s_transforms.size() ? local_entity_index : 0]);
        }
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

        Vec3 body_bottom = {feet.x, feet.y, feet.z};
        Vec3 body_top = {feet.x, feet.y + PLAYER_HEIGHT, feet.z};
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
        if (with_skeleton)
            skel_update_player(s_transforms[i], vp, sw, sh, box);
        result.push_back(box);
    }

    // подчистка кэша скелетов умерших/ушедших игроков
    if (with_skeleton && !g_skel_players.empty()) {
        size_t write = 0;
        for (size_t read = 0; read < g_skel_players.size(); ++read) {
            bool alive = false;
            for (uint64_t t : s_transforms)
                if (t == g_skel_players[read].player) { alive = true; break; }
            if (alive) g_skel_players[write++] = g_skel_players[read];
        }
        g_skel_players.resize(write);
    }

    return result;
}
