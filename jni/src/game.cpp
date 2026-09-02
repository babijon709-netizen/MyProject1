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

const int ESP_BONE_LINKS[ESP_BONE_LINK_COUNT][2] = {
    {BONE_HIPS, BONE_SPINE}, {BONE_SPINE, BONE_SPINE1}, {BONE_SPINE1, BONE_SPINE2},
    {BONE_SPINE2, BONE_NECK}, {BONE_NECK, BONE_HEAD},
    {BONE_SPINE2, BONE_SHOULDER_L}, {BONE_SHOULDER_L, BONE_ARM_L},
    {BONE_ARM_L, BONE_FOREARM_L}, {BONE_FOREARM_L, BONE_HAND_L},
    {BONE_SPINE2, BONE_SHOULDER_R}, {BONE_SHOULDER_R, BONE_ARM_R},
    {BONE_ARM_R, BONE_FOREARM_R}, {BONE_FOREARM_R, BONE_HAND_R},
    {BONE_HIPS, BONE_UPLEG_L}, {BONE_UPLEG_L, BONE_LEG_L},
    {BONE_LEG_L, BONE_FOOT_L}, {BONE_FOOT_L, BONE_TOE_L},
    {BONE_HIPS, BONE_UPLEG_R}, {BONE_UPLEG_R, BONE_LEG_R},
    {BONE_LEG_R, BONE_FOOT_R}, {BONE_FOOT_R, BONE_TOE_R}
};

struct SkeletonBoneName { const char* name; int bone; };
static const SkeletonBoneName kSkeletonBoneNames[] = {
    {"Hips", BONE_HIPS}, {"Spine", BONE_SPINE}, {"Spine1", BONE_SPINE1},
    {"Spine2", BONE_SPINE2}, {"Neck", BONE_NECK}, {"Head", BONE_HEAD},
    {"Shoulder.L", BONE_SHOULDER_L}, {"Arm.L", BONE_ARM_L},
    {"ForeArm.L", BONE_FOREARM_L}, {"Hand.L", BONE_HAND_L},
    {"Shoulder.R", BONE_SHOULDER_R}, {"Arm.R", BONE_ARM_R},
    {"ForeArm.R", BONE_FOREARM_R}, {"Hand.R", BONE_HAND_R},
    {"UpLeg.L", BONE_UPLEG_L}, {"Leg.L", BONE_LEG_L},
    {"Foot.L", BONE_FOOT_L}, {"ToeBase.L", BONE_TOE_L},
    {"UpLeg.R", BONE_UPLEG_R}, {"Leg.R", BONE_LEG_R},
    {"Foot.R", BONE_FOOT_R}, {"ToeBase.R", BONE_TOE_R}
};

struct CachedSkeleton {
    uint64_t bone_transform[ESP_BONE_COUNT] = {};
    int32_t  bone_index[ESP_BONE_COUNT] = {};
    uint64_t hierarchy_data = 0;
    int32_t  hips_index = -1;
    int32_t  needed_count = 0;   // highest hierarchy index we must read + 1
    int      bone_count = 0;
    bool     valid = false;
    int      retry_cooldown = 0;
    int      fail_streak = 0;
};

static std::unordered_map<uint64_t, CachedSkeleton> g_skeletons;
static bool g_skeleton_enabled = false;

static TransformHierarchyLayout g_skeleton_layout{};
static bool g_skeleton_layout_valid = false;

static uint64_t g_go_name_offset = 0;
static bool     g_go_name_plain_pointer = false; // fallback: name stored as raw char*
static bool     g_go_name_offset_valid = false;
static int      g_go_name_retry_cooldown = 0;

void esp_set_skeleton_enabled(bool enabled) { g_skeleton_enabled = enabled; }

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
    static const char* kProbeNames[] = {"Hips", "Spine", "Spine1", "Spine2", "Neck", "Head", "Armature", "Root"};

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
                for (const char* probe : kProbeNames) {
                    if (strcmp(name, probe) == 0) { ++matches; break; }
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

static bool build_skeleton(uint64_t player, CachedSkeleton& skeleton) {
    skeleton = CachedSkeleton{};
    for (int bone = 0; bone < ESP_BONE_COUNT; ++bone) skeleton.bone_index[bone] = -1;

    uint64_t root = skeleton_model_root(player);
    if (!root) return false;

    static std::vector<uint64_t> nodes;
    collect_transform_subtree(root, nodes, 384);
    if (nodes.size() < 16) return false;

    if (!g_go_name_offset_valid && !discover_gameobject_name_offset(nodes)) return false;

    int found = 0;
    for (uint64_t node : nodes) {
        char name[48];
        if (!read_transform_name(node, name, sizeof(name))) continue;
        for (const SkeletonBoneName& entry : kSkeletonBoneNames) {
            if (strcmp(name, entry.name) != 0) continue;
            if (!skeleton.bone_transform[entry.bone]) {
                skeleton.bone_transform[entry.bone] = node;
                ++found;
            }
            break;
        }
        if (found >= ESP_BONE_COUNT) break;
    }
    if (!skeleton.bone_transform[BONE_HIPS] || !skeleton.bone_transform[BONE_HEAD] || found < 10) return false;
    if (!resolve_skeleton_layout(skeleton.bone_transform[BONE_HIPS])) return false;

    uint64_t data = rd_ptr(skeleton.bone_transform[BONE_HIPS] + g_skeleton_layout.data_offset);
    if (!data) return false;
    uint64_t indices = rd_ptr(data + g_skeleton_layout.indices_offset);
    if (g_skeleton_layout.indices_indirect) indices = rd_ptr(indices);
    if (!indices) return false;

    int32_t max_needed = -1;
    int valid_bones = 0;
    for (int bone = 0; bone < ESP_BONE_COUNT; ++bone) {
        uint64_t transform = skeleton.bone_transform[bone];
        if (!transform) continue;
        uint64_t bone_data = rd_ptr(transform + g_skeleton_layout.data_offset);
        int32_t index = rd<int32_t>(transform + g_skeleton_layout.index_offset);
        if (bone_data != data || index < 0 || index > 100000) {
            skeleton.bone_transform[bone] = 0;
            continue;
        }
        // Walk the parent chain once to learn how much of the arrays we need.
        int32_t walker = index, local_max = index;
        int depth = 0;
        bool chain_ok = false;
        while (depth++ < 128) {
            int32_t parent = -2;
            if (!rd_exact(indices + (uint64_t)walker * sizeof(int32_t), parent)) break;
            if (parent < 0) { chain_ok = parent == -1; break; }
            if (parent > 100000 || parent == walker) break;
            if (parent > local_max) local_max = parent;
            walker = parent;
        }
        if (!chain_ok) {
            skeleton.bone_transform[bone] = 0;
            continue;
        }
        skeleton.bone_index[bone] = index;
        if (local_max > max_needed) max_needed = local_max;
        ++valid_bones;
    }
    if (valid_bones < 8 || max_needed < 0 || max_needed > 8192) return false;

    skeleton.hierarchy_data = data;
    skeleton.hips_index = skeleton.bone_index[BONE_HIPS];
    skeleton.needed_count = max_needed + 1;
    skeleton.bone_count = valid_bones;
    skeleton.valid = true;
    return true;
}

// Same math as read_transform_hierarchy_arrays, but on locally buffered arrays.
static bool skeleton_local_walk(const Matrix34* matrices, const int32_t* parents, int32_t count, int32_t index, Vec3& out) {
    if (index < 0 || index >= count) return false;
    const Matrix34& current = matrices[index];
    if (!matrix34_is_valid(current)) return false;
    Vec3 result = {current.translation.x, current.translation.y, current.translation.z};
    if (!vec3_is_finite(result)) return false;
    int32_t parent = parents[index];
    int32_t previous = index;
    int depth = 0;
    while (parent >= 0 && depth++ < 128) {
        if (parent >= count || parent == previous) return false;
        const Matrix34& matrix = matrices[parent];
        if (!matrix34_is_valid(matrix)) return false;
        Vec3 scaled = {result.x * matrix.scale.x, result.y * matrix.scale.y, result.z * matrix.scale.z};
        Vec3 rotated = rotate_vector(matrix.rotation, scaled);
        result = {matrix.translation.x + rotated.x, matrix.translation.y + rotated.y, matrix.translation.z + rotated.z};
        if (!vec3_is_finite(result)) return false;
        previous = parent;
        parent = parents[parent];
    }
    if (parent != -1 || depth >= 128) return false;
    out = result;
    return true;
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

static bool fill_skeleton_box(uint64_t player, const Mat4& view_projection, float sw, float sh, EspBox& box) {
    box.has_skeleton = false;
    for (int bone = 0; bone < ESP_BONE_COUNT; ++bone) box.bone_valid[bone] = false;

    CachedSkeleton& skeleton = g_skeletons[player];
    if (!skeleton.valid) {
        if (skeleton.retry_cooldown > 0) { --skeleton.retry_cooldown; return false; }
        if (!build_skeleton(player, skeleton)) {
            skeleton.valid = false;
            skeleton.retry_cooldown = 90;
            return false;
        }
    }

    // Cheap liveness check: the hips transform must still live in the same
    // hierarchy at the same index (model swaps / respawns move it).
    uint64_t data = rd_ptr(skeleton.bone_transform[BONE_HIPS] + g_skeleton_layout.data_offset);
    int32_t hips_index = rd<int32_t>(skeleton.bone_transform[BONE_HIPS] + g_skeleton_layout.index_offset);
    if (data != skeleton.hierarchy_data || hips_index != skeleton.hips_index) {
        skeleton = CachedSkeleton{};
        skeleton.retry_cooldown = 2;
        return false;
    }

    uint64_t matrices = rd_ptr(data + g_skeleton_layout.matrices_offset);
    uint64_t indices = rd_ptr(data + g_skeleton_layout.indices_offset);
    if (g_skeleton_layout.matrices_indirect) matrices = rd_ptr(matrices);
    if (g_skeleton_layout.indices_indirect) indices = rd_ptr(indices);
    if (!matrices || !indices) return false;

    // Two bulk reads cover every bone (shared TRS + parent-index arrays).
    static std::vector<Matrix34> local_matrices;
    static std::vector<int32_t> local_parents;
    local_matrices.resize((size_t)skeleton.needed_count);
    local_parents.resize((size_t)skeleton.needed_count);
    if (!rd_buf(matrices, local_matrices.data(), local_matrices.size() * sizeof(Matrix34)) ||
        !rd_buf(indices, local_parents.data(), local_parents.size() * sizeof(int32_t))) {
        if (++skeleton.fail_streak > 30) { skeleton = CachedSkeleton{}; skeleton.retry_cooldown = 30; }
        return false;
    }

    int projected = 0;
    for (int bone = 0; bone < ESP_BONE_COUNT; ++bone) {
        if (!skeleton.bone_transform[bone] || skeleton.bone_index[bone] < 0) continue;
        Vec3 world{};
        if (!skeleton_local_walk(local_matrices.data(), local_parents.data(),
                                 skeleton.needed_count, skeleton.bone_index[bone], world))
            continue;
        Vec2 screen{};
        if (!w2s(view_projection, world, sw, sh, screen, false)) continue;
        if (fabsf(screen.x) > sw * 4.0F || fabsf(screen.y) > sh * 4.0F) continue;
        box.bones[bone][0] = screen.x;
        box.bones[bone][1] = screen.y;
        box.bone_valid[bone] = true;
        ++projected;
    }
    if (projected < 4) {
        if (++skeleton.fail_streak > 30) { skeleton = CachedSkeleton{}; skeleton.retry_cooldown = 30; }
        return false;
    }
    skeleton.fail_streak = 0;
    box.has_skeleton = true;
    return true;
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

    static std::vector<uint64_t> s_transforms;
    std::vector<uint64_t> refreshed = read_configured_player_transforms();
    if (!refreshed.empty()) s_transforms = std::move(refreshed);
    if (s_transforms.empty()) return result;

    if (g_skeleton_enabled) prune_skeleton_cache(s_transforms);
    else if (!g_skeletons.empty()) g_skeletons.clear();

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
        if (g_skeleton_enabled && !transform_camera_mode)
            fill_skeleton_box(s_transforms[i], vp, sw, sh, box);
        result.push_back(box);
    }

    return result;
}
