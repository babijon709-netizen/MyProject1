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
// Внешняя адаптация внутреннего скелетона. Внутренний вариант обходил
// кости через Transform::GetChild + Object::GetName по именам ("Hips",
// "Arm.L", ...). Снаружи icall вызвать нельзя, а эта сборка игры прячет
// таблицу icall в рантайме протектора, поэтому кости берутся напрямую из
// управляемых полей игры (dump.cs, точные оффсеты):
//   PlayerManager + 0xB0 -> SingleKcc (kccReference)
//   SingleKcc     + 0x90 -> head (Transform), + 0xC0 -> CharacterAnimation
//   CharAnim      + 0x30 -> PlayerModelInfo
//   ModelInfo     + 0x20 head, +0x28 rightWeaponHolder, +0x30 leftWeaponHolder,
//                  +0x40 body  (все UnityEngine.Transform)
// Managed Transform -> +0x10 (m_CachedPtr) -> нативный Transform -> индекс
// батча (0x40) и TransformData (0x38) — те же массивы, что уже использует
// позиционный ESP. От якорей (голова/корпус/руки) весь остальной риг
// размечается по дереву родительских индексов батча: позвоночник — путь
// между якорями, ноги — нисходящие цепочки от таза, руки — цепочки от
// держателей оружия. Никакого перебора оффсетов и чтения имён GameObject.
// Мировые позиции каждый кадр считаются из локальных матриц, поэтому
// скелет полностью динамичный и двигается с конечностями.

struct SkelRig {
    uint64_t player = 0;
    uint64_t transform_data = 0;
    int32_t  anchor[4] = {-1, -1, -1, -1}; // HEAD, BODY, RHAND, LHAND (индексы батча)
    int32_t  slice_lo = 0, slice_hi = 0;   // окно среза батча
    int32_t  entry = -1;                   // точка входа в риг (голова или корень)
    int32_t  bone[ESP_BONE_COUNT] {};
    bool     labeled = false;
    bool     kcc_anchored = false; // якоря из SingleKcc; false = структурная разметка
    uint64_t next_try_ms = 0;
    uint64_t last_ok_ms = 0;
    // буферы кадра (срез батча)
    std::vector<Matrix34> mats;
    std::vector<int32_t>  idxs;
    std::vector<Matrix34> anc_mat;
    std::vector<int32_t>  anc_idx, anc_par;
};
static std::vector<SkelRig> g_skel_rigs;

enum { SK_HEAD = 0, SK_BODY = 1, SK_RHAND = 2, SK_LHAND = 3 };

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

// managed Transform -> индекс батча; заодно возвращает TransformData
static bool skel_managed_to_index(uint64_t managed, uint64_t& data, int32_t& index) {
    if (!skel_plausible_ptr(managed)) return false;
    uint64_t native = rd_ptr(managed + MANAGED_CACHED_PTR);
    if (!skel_plausible_ptr(native)) return false;
    // приоритет — уже валидированный рантайм-дискавери layout
    uint64_t pairs[3][2] = {{0x38, 0x40}, {0x38, 0x40}, {0x18, 0x20}};
    if (g_transform_hierarchy_layout_valid) {
        pairs[0][0] = g_transform_hierarchy_layout.data_offset;
        pairs[0][1] = g_transform_hierarchy_layout.index_offset;
    }
    for (const auto& pair : pairs) {
        uint64_t d = rd_ptr(native + pair[0]);
        int32_t ix = rd<int32_t>(native + pair[1]);
        if (skel_plausible_ptr(d) && ix >= 0 && ix <= 100000) {
            data = d;
            index = ix;
            return true;
        }
    }
    return false;
}

static bool skel_resolve_arrays(uint64_t data, uint64_t& matrices, uint64_t& indices) {
    if (!skel_plausible_ptr(data)) return false;
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

// Чтение среза батча [lo, hi] + цепочка предков от start (якорь головы):
// все кости рига сходятся к общему корню, поэтому предков любой кости,
// выходящих за срез, достаточно брать из цепочки головы.
static bool skel_read_slice(SkelRig& s, uint64_t matrices, uint64_t indices,
                            int32_t lo, int32_t& hi, int32_t start) {
    if (lo < 0 || hi < lo || hi - lo > 2048) return false;
    // чтение чанками: за концом массива батча память может быть не замаплена.
    // размер чанка деградирует 64->16->4->1, чтобы конец массива, не лежащий
    // на границе чанка, не выбрасывал до 63 валидных элементов (включая риг)
    constexpr int32_t CHUNK = 64;
    s.idxs.clear();
    s.mats.clear();
    s.idxs.reserve((size_t)(hi - lo + 1));
    s.mats.reserve((size_t)(hi - lo + 1));
    int32_t got = lo - 1;
    int32_t c = lo;
    bool unmapped = false;
    while (c <= hi && !unmapped) {
        int32_t step = std::min(CHUNK, hi - c + 1);
        while (step > 0) {
            std::vector<int32_t> idx_chunk((size_t)step);
            std::vector<Matrix34> mat_chunk((size_t)step);
            bool ok = skel_read_buf(indices + (uint64_t)c * 4, idx_chunk.data(), (size_t)step * 4)
                   && skel_read_buf(matrices + (uint64_t)c * sizeof(Matrix34), mat_chunk.data(), (size_t)step * sizeof(Matrix34));
            if (ok) {
                s.idxs.insert(s.idxs.end(), idx_chunk.begin(), idx_chunk.end());
                s.mats.insert(s.mats.end(), mat_chunk.begin(), mat_chunk.end());
                got = c + step - 1;
                c += step;
                break;
            }
            step /= 4; // 64 -> 16 -> 4 -> 1 -> стоп
        }
        if (step <= 0) unmapped = true; // не читается даже 1 элемент — дальше памяти нет
    }
    hi = got;
    if (hi < lo) return false;

    s.anc_idx.clear(); s.anc_par.clear(); s.anc_mat.clear();
    if (start < lo || start > hi) return false;
    int32_t parent = s.idxs[(size_t)(start - lo)];
    int guard = 0;
    while (parent >= 0 && parent < lo && guard++ < 128) {
        Matrix34 m{};
        if (!rd_exact(matrices + (uint64_t)parent * sizeof(Matrix34), m) || !matrix34_is_valid(m)) break;
        int32_t pp = rd<int32_t>(indices + (uint64_t)parent * 4);
        if (pp < -1 || pp > 100000) break;
        s.anc_idx.push_back(parent);
        s.anc_mat.push_back(m);
        s.anc_par.push_back(pp);
        parent = pp;
    }
    return true;
}

struct SkelSliceView {
    const std::vector<Matrix34>* mats;
    const std::vector<int32_t>* idxs;
    const std::vector<Matrix34>* anc_mat;
    const std::vector<int32_t>* anc_idx;
    const std::vector<int32_t>* anc_par;
    int32_t lo, hi;

    bool inside(int32_t i) const { return i >= lo && i <= hi; }
    bool known(int32_t i) const {
        if (inside(i)) return true;
        for (size_t k = 0; k < anc_idx->size(); ++k)
            if ((*anc_idx)[k] == i) return true;
        return false;
    }
    int32_t parent_of(int32_t i) const {
        if (inside(i)) return (*idxs)[(size_t)(i - lo)];
        for (size_t k = 0; k < anc_idx->size(); ++k)
            if ((*anc_idx)[k] == i) return (*anc_par)[k];
        return -2; // неизвестный узел
    }
    bool world_at(int32_t index, Vec3& out) const {
        if (!inside(index)) return false;
        Matrix34 current = (*mats)[(size_t)(index - lo)];
        if (!matrix34_is_valid(current)) return false;
        Vec3 result = {current.translation.x, current.translation.y, current.translation.z};
        if (!vec3_is_finite(result)) return false;
        int32_t par = parent_of(index);
        int depth = 0;
        while (par != -1) {
            // порядок индексов в батче не гарантирует parent < child
            // (аллокация при загрузке модели); от циклов защищает лимит глубины
            if (par < 0 || ++depth > 160) return false;
            if (!inside(par) && !known(par)) return false;
            const Matrix34* m = nullptr;
            if (inside(par)) m = &(*mats)[(size_t)(par - lo)];
            else
                for (size_t k = 0; k < anc_idx->size(); ++k)
                    if ((*anc_idx)[k] == par) { m = &(*anc_mat)[k]; break; }
            if (!m || !matrix34_is_valid(*m)) return false;
            Vec3 scaled = {result.x * m->scale.x, result.y * m->scale.y, result.z * m->scale.z};
            Vec3 rotated = rotate_vector(m->rotation, scaled);
            result = {m->translation.x + rotated.x, m->translation.y + rotated.y, m->translation.z + rotated.z};
            if (!vec3_is_finite(result)) return false;
            par = parent_of(par);
        }
        out = result;
        return true;
    }
};

// Цепочка индексов от x вверх до корня (включая x).
static bool skel_chain_up(const SkelSliceView& v, int32_t x, std::vector<int32_t>& chain) {
    chain.clear();
    if (!v.known(x)) return false;
    int32_t cur = x;
    int guard = 0;
    while (cur >= 0 && guard++ < 160) {
        chain.push_back(cur);
        int32_t p = v.parent_of(cur);
        if (p == -2) break; // дальше цепочка не отслеживается
        cur = p;
    }
    return true;
}

static bool skel_chain_contains(const std::vector<int32_t>& chain, int32_t x) {
    for (int32_t c : chain) if (c == x) return true;
    return false;
}

// Самый длинный нисходящий путь от node (по детям внутри среза).
static void skel_deepest_path(const SkelSliceView& v, std::vector<std::vector<int32_t>>& children,
                              int32_t node, std::vector<int32_t>& best) {
    best.clear();
    best.push_back(node);
    // итеративный DFS с текущим путём (children индексируется относительно v.lo)
    std::vector<std::pair<int32_t, size_t>> work;
    work.push_back({node, 0});
    std::vector<int32_t> path;
    path.push_back(node);
    while (!work.empty()) {
        auto& [cur, ci] = work.back();
        const std::vector<int32_t>& kids = children[(size_t)(cur - v.lo)];
        if (ci < kids.size()) {
            int32_t kid = kids[ci++];
            work.push_back({kid, 0});
            path.push_back(kid);
        } else {
            if (path.size() > best.size()) best = path;
            work.pop_back();
            path.pop_back();
        }
    }
}

// Разметка костей по якорям. Возвращает true, если размечено ядро
// (позвоночник + голова); руки/ноги размечаются по мере возможности.
static bool skel_label(SkelRig& s, const SkelSliceView& v) {
    for (int b = 0; b < ESP_BONE_COUNT; ++b) s.bone[b] = -1;
    const int32_t head = s.anchor[SK_HEAD];
    const int32_t body = s.anchor[SK_BODY];
    const int32_t rhand = s.anchor[SK_RHAND];
    const int32_t lhand = s.anchor[SK_LHAND];
    if (head < 0) return false;

    std::vector<int32_t> chain_head, chain_body, chain_r, chain_l;
    skel_chain_up(v, head, chain_head);
    if (body >= 0) skel_chain_up(v, body, chain_body);

    // плечи: поднимаемся от держателей оружия до пересечения с цепочкой головы
    auto arm_chain = [&](int32_t hand, std::vector<int32_t>& out_chain, int32_t& attach) {
        out_chain.clear();
        attach = -1;
        if (hand < 0) return false;
        std::vector<int32_t> up;
        if (!skel_chain_up(v, hand, up)) return false;
        if (up.size() > 12) return false;
        for (size_t k = 0; k < up.size(); ++k) {
            if (skel_chain_contains(chain_head, up[k]) || (body >= 0 && skel_chain_contains(chain_body, up[k]))) {
                attach = up[k];
                // цепочка от точки крепления вниз до кисти (сам узел крепления не входит)
                for (size_t j = k; j > 0; --j) out_chain.push_back(up[j - 1]);
                return out_chain.size() >= 3;
            }
        }
        return false;
    };
    std::vector<int32_t> arm_r, arm_l;
    int32_t attach_r = -1, attach_l = -1;
    bool have_arm_r = arm_chain(rhand, arm_r, attach_r);
    bool have_arm_l = arm_chain(lhand, arm_l, attach_l);
    (void)have_arm_r; (void)have_arm_l;

    // точка крепления рук = верх позвоночника (Spine2/Chest)
    int32_t spine_top = -1;
    if (attach_r >= 0) spine_top = attach_r;
    else if (attach_l >= 0) spine_top = attach_l;
    else if (body >= 0 && chain_body.size() > 1) spine_top = chain_body[0]; // body сам верх, если плеч нет

    s.bone[BONE_HEAD] = head;
    {
        int32_t neck = v.parent_of(head);
        if (neck >= 0 && neck != head) s.bone[BONE_NECK] = neck;
    }

    // hips: идём вверх от spine_top/body, пока не встретим узел с >=3 детьми,
    // из которых хотя бы два не на пути головы и ведут вниз (ноги)
    auto build_children = [&](std::vector<std::vector<int32_t>>& children) {
        children.assign((size_t)(v.hi - v.lo + 1), {});
        for (int32_t i = v.lo; i <= v.hi; ++i) {
            int32_t p = v.parent_of(i);
            if (p >= v.lo && p <= v.hi) children[(size_t)(p - v.lo)].push_back(i);
        }
    };
    std::vector<std::vector<int32_t>> children;
    build_children(children);

    Vec3 head_world{};
    bool have_head_world = v.world_at(head, head_world);

    // полная разметка (ноги/таз) требует якоря корпуса или кистей —
    // без них рисуем только якоря, чтобы не угадывать
    if (body < 0 && rhand < 0 && lhand < 0) return false;

    int32_t hips = -1;
    int32_t scan_from = spine_top >= 0 ? spine_top : (body >= 0 ? body : head);
    {
        std::vector<int32_t> up;
        skel_chain_up(v, scan_from, up);
        for (int32_t candidate : up) {
            if (candidate < v.lo || candidate > v.hi) continue;
            const std::vector<int32_t>& kids = children[(size_t)(candidate - v.lo)];
            if ((int)kids.size() < 3) continue;
            // дети не на пути головы/плеч и с глубиной >= 3 = ноги
            int legs = 0;
            std::vector<int32_t> leg_roots;
            for (int32_t kid : kids) {
                if (skel_chain_contains(chain_head, kid)) continue;
                bool on_arm = skel_chain_contains(arm_r, kid) || skel_chain_contains(arm_l, kid);
                if (on_arm) continue;
                std::vector<int32_t> deep;
                skel_deepest_path(v, children, kid, deep);
                if ((int)deep.size() >= 4) { ++legs; leg_roots.push_back(kid); }
            }
            if (legs >= 2) { hips = candidate; break; }
        }
    }
    if (hips < 0) {
        // без ног: рисуем то, что есть
        return s.bone[BONE_HEAD] >= 0;
    }
    s.bone[BONE_HIPS] = hips;

    // позвоночник: сегмент пути головы от hips вверх до spine_top.
    // chain_head = [head, neck, spine2, ..., spine, hips, ...root] —
    // разворачиваем часть до hips и берём низ сегмента
    {
        std::vector<int32_t> rev; // [hips, spine, ..., neck, head] снизу вверх
        for (int32_t c : chain_head) {
            rev.push_back(c);
            if (c == hips) break;
        }
        std::reverse(rev.begin(), rev.end());
        std::vector<int32_t> spine_seg; // [hips, spine, spine1, spine2]
        for (int32_t c : rev) {
            spine_seg.push_back(c);
            if (spine_seg.size() >= 4) break;
            if (spine_top >= 0 && c == spine_top) break;
            if (spine_top < 0 && c == body) break;
        }
        if (spine_seg.size() >= 1) s.bone[BONE_HIPS]  = spine_seg[0];
        if (spine_seg.size() >= 2) s.bone[BONE_SPINE] = spine_seg[1];
        if (spine_seg.size() >= 3) s.bone[BONE_SPINE1] = spine_seg[2];
        if (spine_seg.size() >= 4) s.bone[BONE_SPINE2] = spine_seg[3];
    }

    // руки: цепочка от держателя = [holder .. shoulder] (снизу вверх)
    auto label_arm = [&](std::vector<int32_t>& chain, int sh, int ar, int fa, int ha) {
        // цепочка сверху вниз [shoulder, arm, forearm, hand, (holder...)] —
        // лишние узлы снизу (держатель оружия) просто не размечаем
        if (chain.size() >= 4) {
            s.bone[sh] = chain[0];
            s.bone[ar] = chain[1];
            s.bone[fa] = chain[2];
            s.bone[ha] = chain[3];
        } else if (chain.size() == 3) {
            s.bone[sh] = -1;
            s.bone[ar] = chain[0];
            s.bone[fa] = chain[1];
            s.bone[ha] = chain[2];
        } else if (chain.size() == 2) {
            s.bone[ar] = chain[0];
            s.bone[fa] = chain[1];
        }
    };
    label_arm(arm_r, BONE_SHOULDER_R, BONE_ARM_R, BONE_FOREARM_R, BONE_HAND_R);
    label_arm(arm_l, BONE_SHOULDER_L, BONE_ARM_L, BONE_FOREARM_L, BONE_HAND_L);

    // ноги: дети hips, не позвоночник и не руки
    Vec3 hips_world{};
    bool have_hips_world = v.world_at(hips, hips_world);
    std::vector<int32_t> leg_roots;
    for (int32_t kid : children[(size_t)(hips - v.lo)]) {
        if (kid == s.bone[BONE_SPINE]) continue;
        if (skel_chain_contains(chain_head, kid)) continue;
        if (skel_chain_contains(arm_r, kid) || skel_chain_contains(arm_l, kid)) continue;
        std::vector<int32_t> deep;
        skel_deepest_path(v, children, kid, deep);
        if ((int)deep.size() >= 4) leg_roots.push_back(kid);
    }
    if ((int)leg_roots.size() >= 2) {
        // определить лево/право по мировой X относительно направления "вперёд"
        Vec3 w0{}, w1{};
        bool ok0 = v.world_at(leg_roots[0], w0);
        bool ok1 = v.world_at(leg_roots[1], w1);
        int right_first = 0;
        Vec3 fwd = {0.0F, 0.0F, 0.0F};
        bool have_fwd = false;
        if (have_hips_world && v.inside(hips)) {
            // вперёд из поворота таза — работает и когда игрок стоит
            const Matrix34& hm = (*v.mats)[(size_t)(hips - v.lo)];
            Vec3 f = rotate_vector(hm.rotation, Vec3{0.0F, 0.0F, 1.0F});
            fwd = {f.x, 0.0F, f.z};
            float fl = sqrtf(fwd.x * fwd.x + fwd.z * fwd.z);
            if (fl > 0.3F) { fwd.x /= fl; fwd.z /= fl; have_fwd = true; }
        }
        if (!have_fwd && have_head_world && have_hips_world) {
            Vec3 f = {head_world.x - hips_world.x, 0.0F, head_world.z - hips_world.z};
            float fl = sqrtf(f.x * f.x + f.z * f.z);
            if (fl > 0.3F) { fwd = {f.x / fl, 0.0F, f.z / fl}; have_fwd = true; }
        }
        if (ok0 && ok1 && have_fwd) {
            Vec3 right = {fwd.z, 0.0F, -fwd.x};
            float side0 = (w0.x - hips_world.x) * right.x + (w0.z - hips_world.z) * right.z;
            float side1 = (w1.x - hips_world.x) * right.x + (w1.z - hips_world.z) * right.z;
            right_first = side0 >= side1 ? 0 : 1;
        }
        int32_t root_r = leg_roots[(size_t)right_first];
        int32_t root_l = leg_roots[(size_t)(right_first == 0 ? 1 : 0)];
        auto label_leg = [&](int32_t root, int up, int lo2, int ft, int toe) {
            std::vector<int32_t> deep;
            skel_deepest_path(v, children, root, deep);
            // deep[0]=root(UpLeg), далее Leg, Foot, ToeBase...
            s.bone[up] = deep.size() > 0 ? deep[0] : -1;
            s.bone[lo2] = deep.size() > 1 ? deep[1] : -1;
            s.bone[ft] = deep.size() > 2 ? deep[2] : -1;
            s.bone[toe] = deep.size() > 3 ? deep[3] : -1;
        };
        label_leg(root_r, BONE_UPLEG_R, BONE_LEG_R, BONE_FOOT_R, BONE_TOEBASE_R);
        label_leg(root_l, BONE_UPLEG_L, BONE_LEG_L, BONE_FOOT_L, BONE_TOEBASE_L);
    }

    int total = 0;
    for (int b = 0; b < ESP_BONE_COUNT; ++b)
        if (s.bone[b] >= 0) ++total;
    return total >= 6;
}

// Структурная разметка рига: слои с независимыми предположениями.
// Слой 1 (цепочка): таз на цепочке предков опорного узла (голова якорем или
// worldCameraRoot у шеи) — даёт точный позвоночник сегментом цепочки.
// Слой 2 (глобальный скан): таз = ЛЮБОЙ узел среза с двумя ветвями,
// физически падающими вниз на >= 0.35 м (device-проверенный путь v4:
// не требует, чтобы таз был предком точки входа). Позвоночник — подъём
// от таза по максимальному Y.
// Частичный проход не отклоняет кандидатов: пустой скелет невозможен,
// если риг читается. Никаких полей игры — только дерево и мировые Y.
static bool skel_label_structural(SkelRig& s, const SkelSliceView& v, int32_t entry, int32_t head_hint) {
    for (int b = 0; b < ESP_BONE_COUNT; ++b) s.bone[b] = -1;
    const int32_t count = v.hi - v.lo + 1;
    if (!v.inside(entry) || count <= 0 || count > 2048) return false;

    std::vector<std::vector<int32_t>> children((size_t)count);
    for (int32_t i = v.lo; i <= v.hi; ++i) {
        int32_t p = v.parent_of(i);
        if (p != i && p >= v.lo && p <= v.hi) children[(size_t)(p - v.lo)].push_back(i - v.lo);
    }
    std::vector<int32_t> height((size_t)count, 1);
    {
        std::vector<int32_t> order;
        order.reserve((size_t)count);
        std::vector<int32_t> queue;
        std::vector<uint8_t> seen((size_t)count, 0);
        for (int32_t i = 0; i < count; ++i) {
            int32_t p = v.parent_of(v.lo + i);
            if (p < v.lo || p > v.hi) queue.push_back(i);
        }
        for (size_t qi = 0; qi < queue.size(); ++qi) {
            int32_t cur = queue[qi];
            if (seen[(size_t)cur]) continue;
            seen[(size_t)cur] = 1;
            order.push_back(cur);
            for (int32_t kid : children[(size_t)cur])
                if (!seen[(size_t)kid]) queue.push_back(kid);
        }
        for (size_t qi = order.size(); qi-- > 0;) {
            int32_t cur = order[qi];
            int32_t best = 0;
            for (int32_t kid : children[(size_t)cur])
                if (height[(size_t)kid] > best) best = height[(size_t)kid];
            height[(size_t)cur] = best + 1;
            if (height[(size_t)cur] > 400) return false;
        }
    }

    int32_t anchor_node = (head_hint >= v.lo && head_hint <= v.hi && v.known(head_hint)) ? head_hint : entry;
    std::vector<int32_t> chain;
    if (!skel_chain_up(v, anchor_node, chain)) chain.assign(1, anchor_node);
    auto in_chain = [&](int32_t abs_node) {
        for (int32_t c : chain) if (c == abs_node) return true;
        return false;
    };

    auto node_y = [&](int32_t abs_node, float& out_y) {
        Vec3 w{};
        if (!v.world_at(abs_node, w)) return false;
        out_y = w.y;
        return true;
    };
    auto branch_lowest_y = [&](int32_t rel_root, float& out_y) {
        bool any = false;
        float lowest = 0.0F;
        std::vector<int32_t> stack{rel_root};
        int guard = 0;
        while (!stack.empty() && guard++ < 600) {
            int32_t cur = stack.back();
            stack.pop_back();
            Vec3 w{};
            if (v.world_at(v.lo + cur, w)) {
                if (!any || w.y < lowest) { lowest = w.y; any = true; }
            }
            for (int32_t kid : children[(size_t)cur]) stack.push_back(kid);
        }
        if (!any) return false;
        out_y = lowest;
        return true;
    };

    struct Cand {
        int32_t rel_hips;
        int32_t leg_r, leg_l; // относительные корни ног
        bool on_chain;
        float drop;           // насколько ноги падают ниже кандидата
    };
    std::vector<Cand> cands;
    std::vector<uint8_t> used((size_t)count, 0);
    auto try_add = [&](int32_t rel_node, bool chain_only_context) {
        if (used[(size_t)rel_node]) return;
        int32_t abs_node = v.lo + rel_node;
        float nodey = 0.0F;
        if (!node_y(abs_node, nodey)) return;
        std::vector<std::pair<float, int32_t>> falling;
        int deep_branches = 0;
        for (int32_t kid : children[(size_t)rel_node]) {
            if (height[(size_t)kid] < 2) continue;
            ++deep_branches;
            float y;
            if (branch_lowest_y(kid, y) && (nodey - y) >= 0.35F)
                falling.push_back({y, kid});
        }
        if (deep_branches < 2 || falling.size() < 2) return;
        std::sort(falling.begin(), falling.end());
        used[(size_t)rel_node] = 1;
        Cand c;
        c.rel_hips = rel_node;
        c.leg_r = falling[0].second;
        c.leg_l = falling[1].second;
        c.on_chain = chain_only_context;
        c.drop = nodey - falling[0].first;
        cands.push_back(c);
    };
    auto collect = [&](float min_drop_unused) {
        (void)min_drop_unused;
        for (size_t cp = 1; cp + 1 < chain.size(); ++cp) try_add(chain[cp] - v.lo, true);
    };
    collect(0.35F);
    // глобальный скан: любой узел среза с двумя падающими ветвями
    for (int32_t i = 0; i < count; ++i) try_add(i, false);
    if (cands.empty()) {
        // аварийный допуск по падению — только цепочка, затем глобально
        std::fill(used.begin(), used.end(), 0);
        for (size_t cp = 1; cp + 1 < chain.size(); ++cp) {
            int32_t rel = chain[cp] - v.lo;
            if (used[(size_t)rel]) continue;
            int32_t abs_node = v.lo + rel;
            float nodey = 0.0F;
            if (!node_y(abs_node, nodey)) continue;
            std::vector<std::pair<float, int32_t>> falling;
            for (int32_t kid : children[(size_t)rel]) {
                if (height[(size_t)kid] < 2) continue;
                float y;
                if (branch_lowest_y(kid, y) && (nodey - y) >= 0.15F)
                    falling.push_back({y, kid});
            }
            if (falling.size() < 2) continue;
            std::sort(falling.begin(), falling.end());
            used[(size_t)rel] = 1;
            Cand c; c.rel_hips = rel; c.leg_r = falling[0].second; c.leg_l = falling[1].second; c.on_chain = true;
            c.drop = nodey - falling[0].first;
            cands.push_back(c);
        }
        for (int32_t i = 0; i < count; ++i) {
            if (used[(size_t)i]) continue;
            int32_t abs_node = v.lo + i;
            float nodey = 0.0F;
            if (!node_y(abs_node, nodey)) continue;
            std::vector<std::pair<float, int32_t>> falling;
            for (int32_t kid : children[(size_t)i]) {
                if (height[(size_t)kid] < 2) continue;
                float y;
                if (branch_lowest_y(kid, y) && (nodey - y) >= 0.15F)
                    falling.push_back({y, kid});
            }
            if (falling.size() < 2) continue;
            std::sort(falling.begin(), falling.end());
            used[(size_t)i] = 1;
            Cand c; c.rel_hips = i; c.leg_r = falling[0].second; c.leg_l = falling[1].second; c.on_chain = false;
            c.drop = nodey - falling[0].first;
            cands.push_back(c);
        }
    }
    if (cands.empty()) return false;

    Vec3 right_dir = {1.0F, 0.0F, 0.0F};
    Vec3 hw{};
    bool have_hw = false;
    auto setup_right = [&](int32_t hips_abs) {
        have_hw = v.world_at(hips_abs, hw);
        if (!have_hw) return;
        Vec3 f = {0.0F, 0.0F, 1.0F};
        if (v.inside(hips_abs)) {
            const Matrix34& hm = (*v.mats)[(size_t)(hips_abs - v.lo)];
            Vec3 fr = rotate_vector(hm.rotation, Vec3{0.0F, 0.0F, 1.0F});
            float fl = sqrtf(fr.x * fr.x + fr.z * fr.z);
            if (fl > 0.3F) f = {fr.x / fl, 0.0F, fr.z / fl};
        }
        right_dir = {f.z, 0.0F, -f.x};
    };
    auto side_of = [&](int32_t abs_node) {
        Vec3 w{};
        if (!have_hw || !v.world_at(abs_node, w)) return 0.0F;
        return (w.x - hw.x) * right_dir.x + (w.z - hw.z) * right_dir.z;
    };
    auto deepest4 = [&](int32_t rel_root, int32_t out[4]) {
        std::vector<int32_t> best{rel_root};
        std::vector<std::pair<int32_t, size_t>> work{{rel_root, 0}};
        std::vector<int32_t> path{rel_root};
        while (!work.empty()) {
            auto& [cur, ci] = work.back();
            const std::vector<int32_t>& kids = children[(size_t)cur];
            if (ci < kids.size()) {
                int32_t kid = kids[ci++];
                work.push_back({kid, 0});
                path.push_back(kid);
            } else {
                if (path.size() > best.size()) best = path;
                work.pop_back();
                path.pop_back();
            }
        }
        for (int i = 0; i < 4; ++i) out[i] = (i < (int)best.size()) ? v.lo + best[(size_t)i] : -1;
    };

    auto try_candidate = [&](Cand& cand, bool allow_partial) {
        for (int b = 0; b < ESP_BONE_COUNT; ++b) s.bone[b] = -1;
        const int32_t hips_abs = v.lo + cand.rel_hips;

        // позвоночник
        int32_t neck_abs = -1;
        if (cand.on_chain) {
            int hips_cp = -1;
            for (size_t cp = 0; cp < chain.size(); ++cp)
                if (chain[cp] == hips_abs) { hips_cp = (int)cp; break; }
            if (hips_cp >= 0) {
                s.bone[BONE_HIPS]  = chain[(size_t)hips_cp];
                if (hips_cp >= 1) s.bone[BONE_SPINE]  = chain[(size_t)hips_cp - 1];
                if (hips_cp >= 2) s.bone[BONE_SPINE1] = chain[(size_t)hips_cp - 2];
                if (hips_cp >= 3) s.bone[BONE_SPINE2] = chain[(size_t)hips_cp - 3];
                neck_abs = chain.size() >= 2 ? chain[1] : hips_abs;
            }
        }
        if (s.bone[BONE_HIPS] < 0) {
            // глобальный кандидат: подъём от таза по максимальному Y
            s.bone[BONE_HIPS] = hips_abs;
            int32_t cur = cand.rel_hips;
            float cur_y = 0.0F;
            node_y(hips_abs, cur_y);
            const int spine_bones[3] = {BONE_SPINE, BONE_SPINE1, BONE_SPINE2};
            for (int si = 0; si < 3; ++si) {
                int32_t best = -1;
                float best_y = cur_y;
                for (int32_t kid : children[(size_t)cur]) {
                    float ky = 0.0F;
                    if (!node_y(v.lo + kid, ky)) continue;
                    if (ky > best_y + 0.01F) { best_y = ky; best = kid; }
                }
                if (best < 0) break;
                s.bone[spine_bones[si]] = v.lo + best;
                cur = best;
                cur_y = best_y;
            }
            // шея = самый высокий ребёнок верхнего узла (анатомически выше плеч)
            int32_t top = cur;
            float top_y = cur_y;
            int32_t neck_rel = -1;
            float neck_y = top_y;
            for (int32_t kid : children[(size_t)top]) {
                float ky = 0.0F;
                if (!node_y(v.lo + kid, ky)) continue;
                if (ky > neck_y + 0.01F) { neck_y = ky; neck_rel = kid; }
            }
            neck_abs = neck_rel >= 0 ? v.lo + neck_rel : (s.bone[BONE_SPINE2] >= 0 ? s.bone[BONE_SPINE2] : hips_abs);
        }
        s.bone[BONE_NECK] = neck_abs;

        // голова
        if (anchor_node == head_hint) {
            s.bone[BONE_HEAD] = head_hint;
        } else if (neck_abs >= v.lo) {
            int32_t head = -1;
            float best_y = -1.0e9F;
            float nw_y = 0.0F;
            bool have_nw = node_y(neck_abs, nw_y);
            for (int32_t kid : children[(size_t)(neck_abs - v.lo)]) {
                int32_t abs_kid = v.lo + kid;
                if (abs_kid == anchor_node) continue;
                float ky = 0.0F;
                if (!have_nw || !node_y(abs_kid, ky)) continue;
                if (ky > nw_y - 0.05F && ky > best_y) { best_y = ky; head = abs_kid; }
            }
            s.bone[BONE_HEAD] = head;
        }

        // ноги
        setup_right(hips_abs);
        int32_t leg_r = cand.leg_r, leg_l = cand.leg_l;
        float sr = side_of(v.lo + leg_r), sl = side_of(v.lo + leg_l);
        if (sr < sl) std::swap(leg_r, leg_l);
        {
            int out[4];
            deepest4(leg_r, out);
            s.bone[BONE_UPLEG_R] = out[0]; s.bone[BONE_LEG_R] = out[1]; s.bone[BONE_FOOT_R] = out[2]; s.bone[BONE_TOEBASE_R] = out[3];
            deepest4(leg_l, out);
            s.bone[BONE_UPLEG_L] = out[0]; s.bone[BONE_LEG_L] = out[1]; s.bone[BONE_FOOT_L] = out[2]; s.bone[BONE_TOEBASE_L] = out[3];
        }

        // руки: дети груди (spine2/верх подъёма), кроме шеи и нисходящего ствола
        int32_t chest = s.bone[BONE_SPINE2];
        if (chest < 0) chest = s.bone[BONE_SPINE1];
        if (chest < 0) chest = s.bone[BONE_SPINE];
        if (chest >= 0) {
            struct Arm { int32_t nodes[4]; float side; };
            std::vector<Arm> arms;
            for (int32_t kid : children[(size_t)(chest - v.lo)]) {
                int32_t abs_kid = v.lo + kid;
                if (abs_kid == neck_abs) continue;
                if (cand.on_chain && in_chain(abs_kid)) continue;
                if (height[(size_t)kid] < 3) continue;
                Arm a;
                deepest4(kid, a.nodes);
                a.side = side_of(abs_kid);
                arms.push_back(a);
            }
            if (arms.size() >= 2) {
                std::sort(arms.begin(), arms.end(), [](const Arm& a, const Arm& b) { return a.side > b.side; });
                s.bone[BONE_SHOULDER_R] = arms[0].nodes[0]; s.bone[BONE_ARM_R] = arms[0].nodes[1];
                s.bone[BONE_FOREARM_R] = arms[0].nodes[2]; s.bone[BONE_HAND_R] = arms[0].nodes[3];
                s.bone[BONE_SHOULDER_L] = arms[1].nodes[0]; s.bone[BONE_ARM_L] = arms[1].nodes[1];
                s.bone[BONE_FOREARM_L] = arms[1].nodes[2]; s.bone[BONE_HAND_L] = arms[1].nodes[3];
            }
        }

        // частичный проход ничего не отклоняет: лучше ноги, чем пусто
        int total = 0;
        for (int b = 0; b < ESP_BONE_COUNT; ++b)
            if (s.bone[b] >= 0) ++total;
        if (allow_partial) return total >= 4;

        // полный: физическая валидация Y
        auto bone_y = [&](int b, float& y) { return s.bone[b] >= 0 && node_y(s.bone[b], y); };
        float y_hips = 0, y_top = 0, y_foot = 0;
        bool ok_hips = bone_y(BONE_HIPS, y_hips);
        bool ok_top = bone_y(BONE_NECK, y_top) || bone_y(BONE_SPINE2, y_top) || bone_y(BONE_SPINE, y_top);
        bool ok_foot = bone_y(BONE_FOOT_R, y_foot) || bone_y(BONE_FOOT_L, y_foot);
        if (ok_hips && ok_foot && y_hips - y_foot < 0.45F) return false; // руки-под-ноги отбраковка
        if (ok_hips && ok_top && y_top < y_hips) return false;
        if (have_hw) {
            for (int b = 0; b < ESP_BONE_COUNT; ++b) {
                if (s.bone[b] < 0) continue;
                Vec3 w{};
                if (!v.world_at(s.bone[b], w)) continue;
                float dx = w.x - hw.x, dy = w.y - hw.y, dz = w.z - hw.z;
                if (fabsf(dx) > 3.0F || fabsf(dy) > 3.0F || fabsf(dz) > 3.0F) return false;
            }
        }
        return total >= 8;
    };

    // порядок: цепочечные (точный позвоночник), затем глобальные (device-путь v4);
    // внутри группы — по величине падения: ноги (0.9м) всегда падают сильнее
    // рук с пальцами (0.45-0.7м), поэтому настоящий таз пробуется первым
    std::stable_sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
        if (a.on_chain != b.on_chain) return a.on_chain > b.on_chain;
        return a.drop > b.drop;
    });
    for (auto& cand : cands)
        if (try_candidate(cand, false)) return true;
    for (auto& cand : cands)
        if (try_candidate(cand, true)) return true;
    return false;
}

static bool skel_build(SkelRig& s, uint64_t player) {
    SkelRig tmp;
    tmp.player = player;

    // --- вход в риг, два независимых пути ---
    // основной: SingleKcc (dump.cs), валидируется backref-ом или кросс-проверкой
    // головы через CharacterAnimation -> PlayerModelInfo
    uint64_t data = 0;
    int32_t entry = -1;
    bool kcc_ok = false;
    uint64_t kcc = rd_ptr(player + PLAYER_KCC_REFERENCE);
    if (skel_plausible_ptr(kcc)) {
        uint64_t head = rd_ptr(kcc + KCC_HEAD_TRANSFORM);
        if (skel_plausible_ptr(head)) {
            bool backref_ok = rd_ptr(kcc + KCC_PLAYER_BACKREF) == player;
            bool cross_ok = false;
            uint64_t model_info = 0;
            uint64_t char_anim = rd_ptr(kcc + KCC_CHARACTER_ANIMATION);
            if (skel_plausible_ptr(char_anim)) {
                model_info = rd_ptr(char_anim + CHAR_ANIM_PLAYER_MODEL_INFO);
                if (skel_plausible_ptr(model_info))
                    cross_ok = rd_ptr(model_info + MODEL_INFO_HEAD) == head;
            }
            if (backref_ok || cross_ok) {
                uint64_t managed[4] = {head, 0, 0, 0};
                if (skel_plausible_ptr(model_info)) {
                    managed[SK_BODY]  = rd_ptr(model_info + MODEL_INFO_BODY);
                    managed[SK_RHAND] = rd_ptr(model_info + MODEL_INFO_RIGHT_HAND);
                    managed[SK_LHAND] = rd_ptr(model_info + MODEL_INFO_LEFT_HAND);
                }
                for (int a = 0; a < 4; ++a) {
                    if (!skel_plausible_ptr(managed[a])) continue;
                    uint64_t d = 0;
                    int32_t ix = -1;
                    if (!skel_managed_to_index(managed[a], d, ix)) continue;
                    if (data && d != data) continue; // якорь из чужого батча
                    data = d;
                    tmp.anchor[a] = ix;
                }
                if (tmp.anchor[SK_HEAD] >= 0 && skel_plausible_ptr(data)) {
                    entry = tmp.anchor[SK_HEAD];
                    kcc_ok = true;
                }
            }
        }
    }
    if (entry < 0) {
        // запасной вход: worldCameraRoot (+0x68) — ровно тот transform, по
        // которому уже работает позиционный ESP; дальше только топология дерева
        uint64_t root_managed = rd_ptr(player + PLAYER_TRANSFORM);
        uint64_t d = 0;
        int32_t ix = -1;
        if (!skel_plausible_ptr(root_managed) || !skel_managed_to_index(root_managed, d, ix))
            return false;
        data = d;
        entry = ix;
        tmp.anchor[SK_BODY] = ix;
    }

    uint64_t matrices = 0, indices = 0;
    if (!skel_resolve_arrays(data, matrices, indices)) return false;

    int32_t lo, hi;
    if (kcc_ok) {
        lo = entry; hi = entry;
        for (int a = 0; a < 4; ++a) {
            if (tmp.anchor[a] >= 0 && tmp.anchor[a] < lo) lo = tmp.anchor[a];
            if (tmp.anchor[a] > hi) hi = tmp.anchor[a];
        }
        lo -= 96;
        hi += 160;
    } else {
        lo = entry - 128; // индексы батча не обязаны быть упорядочены по дереву
        hi = entry + 288;
    }
    if (lo < 0) lo = 0;
    if (hi - lo > 1024) hi = lo + 1024;

    if (!skel_read_slice(tmp, matrices, indices, lo, hi, entry)) return false;
    for (int a = 0; a < 4; ++a)
        if (tmp.anchor[a] > hi) tmp.anchor[a] = -1;
    if (entry > hi) return false;
    tmp.slice_lo = lo;
    tmp.slice_hi = hi;
    tmp.entry = entry;

    SkelSliceView view{&tmp.mats, &tmp.idxs, &tmp.anc_mat, &tmp.anc_idx, &tmp.anc_par, lo, hi};

    tmp.transform_data = data;
    tmp.kcc_anchored = kcc_ok;
    tmp.labeled = false;
    if (kcc_ok) tmp.labeled = skel_label(tmp, view);
    if (!tmp.labeled)
        tmp.labeled = skel_label_structural(tmp, view, entry, kcc_ok ? tmp.anchor[SK_HEAD] : -1);
    tmp.last_ok_ms = skel_now_ms();
    s = std::move(tmp); // фиксируем только успешную сборку целиком
    return true;
}

static void skel_fill_box(SkelRig& s, const Mat4& vp, float sw, float sh, EspBox& box) {
    box.skel_valid = false;
    if (!s.transform_data || s.slice_hi < s.slice_lo) return;

    // модель пересоздали (переключение оружия/скина)? — якорь уходит в другой батч
    if (s.kcc_anchored) {
        uint64_t kcc = rd_ptr(s.player + PLAYER_KCC_REFERENCE);
        if (!skel_plausible_ptr(kcc)) { s.last_ok_ms = 0; return; }
        uint64_t head_managed = rd_ptr(kcc + KCC_HEAD_TRANSFORM);
        uint64_t d = 0;
        int32_t ix = -1;
        if (!skel_plausible_ptr(head_managed) || !skel_managed_to_index(head_managed, d, ix) || d != s.transform_data) {
            s.last_ok_ms = 0; // заставит skel_update перестроить риг
            return;
        }
        if (ix != s.anchor[SK_HEAD]) { s.last_ok_ms = 0; return; }
    }

    uint64_t matrices = 0, indices = 0;
    if (!skel_resolve_arrays(s.transform_data, matrices, indices)) return;
    if (s.entry < s.slice_lo || s.entry > s.slice_hi) { s.last_ok_ms = 0; return; }
    if (!skel_read_slice(s, matrices, indices, s.slice_lo, s.slice_hi, s.entry)) return;

    SkelSliceView view{&s.mats, &s.idxs, &s.anc_mat, &s.anc_idx, &s.anc_par, s.slice_lo, s.slice_hi};

    int projected = 0;
    for (int b = 0; b < ESP_BONE_COUNT; ++b) {
        box.skel_visible[b] = false;
        box.skel_x[b] = -1.0F;
        box.skel_y[b] = -1.0F;
        int32_t index = s.bone[b];
        if (!s.labeled) {
            // без полной разметки рисуем якоря и шею: видно минимум голову/корпус/кисти
            if (b == BONE_HEAD) index = s.anchor[SK_HEAD];
            else if (b == BONE_SPINE1 && s.anchor[SK_BODY] >= 0) index = s.anchor[SK_BODY];
            else if (b == BONE_HAND_R && s.anchor[SK_RHAND] >= 0) index = s.anchor[SK_RHAND];
            else if (b == BONE_HAND_L && s.anchor[SK_LHAND] >= 0) index = s.anchor[SK_LHAND];
            else if (b == BONE_NECK) {
                int32_t from = s.anchor[SK_HEAD] >= 0 ? s.anchor[SK_HEAD] : s.entry;
                index = from >= 0 ? view.parent_of(from) : -1;
            } else continue;
            if (index < view.lo || index > view.hi) continue;
        } else if (index < 0) {
            continue;
        }
        if (index < view.lo || index > view.hi) continue;
        Vec3 world{};
        if (!view.world_at(index, world)) continue;
        Vec2 screen{};
        if (!w2s(vp, world, sw, sh, screen, false)) continue;
        box.skel_x[b] = screen.x;
        box.skel_y[b] = screen.y;
        box.skel_visible[b] = true;
        ++projected;
    }
    box.skel_valid = projected >= 2;
    if (box.skel_valid) s.last_ok_ms = skel_now_ms();
}

static void skel_update_player(uint64_t player, const Mat4& vp, float sw, float sh, EspBox& box) {
    box.skel_valid = false;
    SkelRig* s = nullptr;
    for (SkelRig& entry : g_skel_rigs)
        if (entry.player == player) { s = &entry; break; }
    if (!s) {
        g_skel_rigs.push_back(SkelRig{});
        s = &g_skel_rigs.back();
        s->player = player;
    }
    const uint64_t now = skel_now_ms();
    if (s->last_ok_ms == 0) {
        if (now < s->next_try_ms) return;
        s->next_try_ms = now + (s->transform_data ? 300 : 1500);
        if (!skel_build(*s, player)) return;
    } else if (now - s->last_ok_ms > 15000) {
        // периодический ребилд на случай пересоздания модели
        if (now < s->next_try_ms) return;
        s->next_try_ms = now + 1500;
        s->last_ok_ms = 0;
        if (!skel_build(*s, player)) return;
    }
    skel_fill_box(*s, vp, sw, sh, box);
    if (!box.skel_valid && s->last_ok_ms && now - s->last_ok_ms > 4000) {
        s->last_ok_ms = 0;
        s->next_try_ms = now + 1500;
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
    g_skel_rigs.clear();
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
    if (with_skeleton && !g_skel_rigs.empty()) {
        size_t write = 0;
        for (size_t read = 0; read < g_skel_rigs.size(); ++read) {
            bool alive = false;
            for (uint64_t t : s_transforms)
                if (t == g_skel_rigs[read].player) { alive = true; break; }
            if (alive) g_skel_rigs[write++] = g_skel_rigs[read];
        }
        g_skel_rigs.resize(write);
    }

    return result;
}
