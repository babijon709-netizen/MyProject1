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
#include <functional>
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

static bool read_entity_position(uint64_t source, Vec3& position);

static bool read_tick_position(uint64_t source, Vec3& position) {
    if (!source || g_player_position_offset == 0) return false;
    position = rd_v3(source + g_player_position_offset);
    return vec3_is_finite(position);
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
        g_player_position_offset = best_offset;
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

static std::unordered_map<uint64_t, uint64_t> g_player_model_info_cache;

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

static float vec_dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static float vec_len(const Vec3& a) { return sqrtf(a.x * a.x + a.y * a.y + a.z * a.z); }
static Vec3  vec_sub(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static Vec3  vec_add(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
static Vec3  vec_mul(const Vec3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
static Vec3  vec_norm(const Vec3& a) {
    float length = vec_len(a);
    if (length < 0.0001F) return {0.0F, 1.0F, 0.0F};
    return {a.x / length, a.y / length, a.z / length};
}

static uint64_t resolve_player_model_info(uint64_t player) {
    if (!player) return 0;
    auto cached = g_player_model_info_cache.find(player);
    if (cached != g_player_model_info_cache.end() && cached->second) return cached->second;
    uint64_t view = rd_ptr(player + PLAYER_VIEW_QL);
    uint64_t from_view = view ? rd_ptr(view + QL_PLAYER_MODEL_INFO) : 0;
    if (from_view) {
        g_player_model_info_cache[player] = from_view;
        return from_view;
    }
    return 0;
}

enum class HumanBoneId : int {
    Hips = 0,
    LeftUpperLeg = 1,
    RightUpperLeg = 2,
    LeftLowerLeg = 3,
    RightLowerLeg = 4,
    LeftFoot = 5,
    RightFoot = 6,
    Spine = 7,
    Chest = 8,
    Neck = 9,
    Head = 10,
    LeftShoulder = 11,
    RightShoulder = 12,
    LeftUpperArm = 13,
    RightUpperArm = 14,
    LeftLowerArm = 15,
    RightLowerArm = 16,
    LeftHand = 17,
    RightHand = 18,
    UpperChest = 54,
    LastBone = 55
};

struct BoneSlot {
    uint64_t native = 0;
    int32_t index = -1;
};

struct PlayerVisualCache {
    uint64_t player = 0;
    BoneSlot bones[kEspBoneCount]{};
    uint64_t matrices = 0;
    uint64_t indices = 0;
    bool ready = false;
};

static std::unordered_map<uint64_t, PlayerVisualCache> g_visual_cache;

static bool native_to_index(uint64_t native, int32_t& index, uint64_t& matrices, uint64_t& parent_indices) {
    if (!native) return false;
    const TransformHierarchyLayout& layout = g_transform_hierarchy_layout_valid ? g_transform_hierarchy_layout : TransformHierarchyLayout{};
    uint64_t transform_data = rd_ptr(native + layout.data_offset);
    int32_t transform_index = rd<int32_t>(native + layout.index_offset);
    if (!transform_data || transform_index < 0 || transform_index > 100000) return false;
    uint64_t local_matrices = rd_ptr(transform_data + layout.matrices_offset);
    uint64_t local_indices = rd_ptr(transform_data + layout.indices_offset);
    if (layout.matrices_indirect) local_matrices = rd_ptr(local_matrices);
    if (layout.indices_indirect) local_indices = rd_ptr(local_indices);
    Vec3 probe{};
    if (!read_transform_hierarchy_arrays(local_matrices, local_indices, transform_index, probe)) return false;
    index = transform_index;
    matrices = local_matrices;
    parent_indices = local_indices;
    return true;
}

static bool read_managed_transform_pos(uint64_t transform, Vec3& position) {
    uint64_t native = resolve_native_transform(transform);
    return native && read_transform_hierarchy_position(native, position);
}

static bool read_cached_bone(const PlayerVisualCache& cache, EspBone bone, Vec3& position) {
    const BoneSlot& slot = cache.bones[(int)bone];
    if (slot.index >= 0 && cache.matrices && cache.indices) {
        if (read_transform_hierarchy_arrays(cache.matrices, cache.indices, slot.index, position))
            return true;
    }
    if (slot.native) return read_transform_hierarchy_position(slot.native, position);
    return false;
}

static void set_slot_native(PlayerVisualCache& cache, EspBone bone, uint64_t native) {
    if (!native || bone == EspBone::Count) return;
    BoneSlot& slot = cache.bones[(int)bone];
    if (slot.native) return;
    slot.native = native;
    int32_t index = -1;
    uint64_t matrices = 0, indices = 0;
    if (native_to_index(native, index, matrices, indices)) {
        slot.index = index;
        if (!cache.matrices) {
            cache.matrices = matrices;
            cache.indices = indices;
        }
    }
}

static EspBone human_to_esp(int human) {
    switch (human) {
        case (int)HumanBoneId::Hips: return EspBone::Hip;
        case (int)HumanBoneId::Spine: return EspBone::Spine;
        case (int)HumanBoneId::Chest:
        case (int)HumanBoneId::UpperChest: return EspBone::Chest;
        case (int)HumanBoneId::Neck: return EspBone::Neck;
        case (int)HumanBoneId::Head: return EspBone::Head;
        case (int)HumanBoneId::LeftShoulder: return EspBone::LeftShoulder;
        case (int)HumanBoneId::RightShoulder: return EspBone::RightShoulder;
        case (int)HumanBoneId::LeftUpperArm: return EspBone::LeftUpperArm;
        case (int)HumanBoneId::RightUpperArm: return EspBone::RightUpperArm;
        case (int)HumanBoneId::LeftLowerArm: return EspBone::LeftLowerArm;
        case (int)HumanBoneId::RightLowerArm: return EspBone::RightLowerArm;
        case (int)HumanBoneId::LeftHand: return EspBone::LeftHand;
        case (int)HumanBoneId::RightHand: return EspBone::RightHand;
        case (int)HumanBoneId::LeftUpperLeg: return EspBone::LeftThigh;
        case (int)HumanBoneId::RightUpperLeg: return EspBone::RightThigh;
        case (int)HumanBoneId::LeftLowerLeg: return EspBone::LeftShin;
        case (int)HumanBoneId::RightLowerLeg: return EspBone::RightShin;
        case (int)HumanBoneId::LeftFoot: return EspBone::LeftFoot;
        case (int)HumanBoneId::RightFoot: return EspBone::RightFoot;
        default: return EspBone::Count;
    }
}

static bool resolve_bone_native(uint64_t object, uint64_t& native, Vec3& position) {
    if (!likely_native_pointer(object)) return false;
    uint64_t as_native = resolve_native_transform(object);
    if (as_native && read_transform_hierarchy_position(as_native, position)) {
        native = as_native;
        return true;
    }
    if (read_transform_hierarchy_position(object, position)) {
        native = object;
        return true;
    }
    return false;
}

static int score_humanoid_table(uint64_t table, bool il2cpp_array, const Vec3& origin, uint64_t natives[19], Vec3 positions[19], bool valid[19]) {
    for (int i = 0; i < 19; ++i) {
        natives[i] = 0;
        valid[i] = false;
    }
    int readable = 0;
    for (int i = 0; i < 19; ++i) {
        uint64_t object = il2cpp_array
            ? rd_ptr(table + IL2CPP_ARRAY_FIRST_ELEMENT + (uint64_t)i * sizeof(uint64_t))
            : rd_ptr(table + (uint64_t)i * sizeof(uint64_t));
        Vec3 pos{};
        uint64_t native = 0;
        if (!resolve_bone_native(object, native, pos)) continue;
        if (vec_len(vec_sub(pos, origin)) > 2.4F) continue;
        natives[i] = native;
        positions[i] = pos;
        valid[i] = true;
        ++readable;
    }
    if (!valid[0] || !valid[10] || readable < 8) return 0;
    float torso = vec_len(vec_sub(positions[10], positions[0]));
    if (torso < 0.35F || torso > 1.35F) return 0;
    if (positions[10].y < positions[0].y + 0.25F) return 0;
    if (valid[1] && positions[1].y > positions[0].y + 0.15F) return 0;
    if (valid[2] && positions[2].y > positions[0].y + 0.15F) return 0;
    if (valid[5] && positions[5].y > positions[0].y + 0.05F) return 0;
    if (valid[6] && positions[6].y > positions[0].y + 0.05F) return 0;
    int score = readable * 10;
    if (valid[1] && valid[2]) score += 20;
    if (valid[5] && valid[6]) score += 20;
    if (valid[17] && valid[18]) score += 15;
    if (valid[13] && valid[14]) score += 15;
    if (valid[7] || valid[8]) score += 10;
    return score;
}

static bool bind_humanoid_natives(PlayerVisualCache& cache, uint64_t natives[19], bool valid[19]) {
    int bound = 0;
    for (int i = 0; i < 19; ++i) {
        if (!valid[i] || !natives[i]) continue;
        EspBone bone = human_to_esp(i);
        if (bone == EspBone::Count) continue;
        set_slot_native(cache, bone, natives[i]);
        ++bound;
    }
    return bound >= 6;
}

static bool try_il2cpp_humanoid_array(uint64_t array, const Vec3& origin, PlayerVisualCache& cache) {
    if (!likely_native_pointer(array)) return false;
    int32_t count = 0;
    if (!rd_exact(array + IL2CPP_LIST_SIZE, count) || count < 19 || count > 80) return false;
    uint64_t natives[19]{};
    Vec3 positions[19]{};
    bool valid[19]{};
    if (score_humanoid_table(array, true, origin, natives, positions, valid) < 80) return false;
    return bind_humanoid_natives(cache, natives, valid);
}

static bool try_raw_humanoid_table(uint64_t table, const Vec3& origin, PlayerVisualCache& cache) {
    if (!likely_native_pointer(table)) return false;
    uint64_t natives[19]{};
    Vec3 positions[19]{};
    bool valid[19]{};
    if (score_humanoid_table(table, false, origin, natives, positions, valid) < 80) return false;
    return bind_humanoid_natives(cache, natives, valid);
}

static bool scan_object_for_humanoid(uint64_t object, const Vec3& origin, PlayerVisualCache& cache) {
    if (!likely_native_pointer(object)) return false;
    for (uint64_t offset = 0x10; offset <= 0x280; offset += 8) {
        uint64_t field = rd_ptr(object + offset);
        if (try_il2cpp_humanoid_array(field, origin, cache)) return true;
        if (try_raw_humanoid_table(field, origin, cache)) return true;
    }
    uint64_t native = rd_ptr(object + MANAGED_CACHED_PTR);
    if (!likely_native_pointer(native) || native == object) return false;
    for (uint64_t offset = 0x10; offset <= 0x300; offset += 8) {
        uint64_t field = rd_ptr(native + offset);
        if (try_il2cpp_humanoid_array(field, origin, cache)) return true;
        if (try_raw_humanoid_table(field, origin, cache)) return true;
        int32_t maybe_count = rd<int32_t>(native + offset);
        if (maybe_count == 55 || maybe_count == 54 || maybe_count == 19) {
            uint64_t table = rd_ptr(native + offset + 8);
            if (try_raw_humanoid_table(table, origin, cache)) return true;
        }
    }
    return false;
}

static bool build_player_visual_cache(uint64_t player, PlayerVisualCache& cache) {
    cache = {};
    cache.player = player;
    Vec3 origin{};
    if (!read_tick_position(player, origin)) return false;

    uint64_t animator = rd_ptr(player + PLAYER_ANIMATOR);
    if (scan_object_for_humanoid(animator, origin, cache)) {
        cache.ready = true;
        return true;
    }

    uint64_t model = resolve_player_model_info(player);
    if (model) {
        uint64_t character_anim = rd_ptr(model + PMI_CHARACTER_ANIM);
        if (character_anim) {
            uint64_t anim2 = rd_ptr(character_anim + CHAR_ANIM_ANIMATOR);
            if (scan_object_for_humanoid(anim2, origin, cache)) {
                cache.ready = true;
                return true;
            }
            if (scan_object_for_humanoid(character_anim, origin, cache)) {
                cache.ready = true;
                return true;
            }
        }
        uint64_t smr = rd_ptr(model + PMI_SKINNED_MESH);
        if (scan_object_for_humanoid(smr, origin, cache)) {
            cache.ready = true;
            return true;
        }
    }
    return false;
}

static PlayerVisualCache* visual_cache_for(uint64_t player) {
    auto it = g_visual_cache.find(player);
    if (it != g_visual_cache.end() && it->second.ready) {
        Vec3 probe{};
        if (read_cached_bone(it->second, EspBone::Hip, probe) ||
            read_cached_bone(it->second, EspBone::Head, probe))
            return &it->second;
        g_visual_cache.erase(it);
    }
    PlayerVisualCache built{};
    if (!build_player_visual_cache(player, built)) return nullptr;
    g_visual_cache[player] = built;
    return &g_visual_cache[player];
}

static bool read_entity_position(uint64_t source, Vec3& position) {
    return read_tick_position(source, position);
}

static bool read_visual_head(uint64_t source, Vec3& position) {
    uint64_t model = resolve_player_model_info(source);
    if (!model) return false;
    if (read_managed_transform_pos(rd_ptr(model + PMI_HEAD), position)) return true;
    return read_managed_transform_pos(rd_ptr(model + PMI_HEAD_MODEL), position);
}

static int fill_skeleton_world(uint64_t player, const Vec3& hip_fallback, Vec3 out_bones[kEspBoneCount], bool out_valid[kEspBoneCount]) {
    for (int i = 0; i < kEspBoneCount; ++i) out_valid[i] = false;
    PlayerVisualCache* cache = visual_cache_for(player);
    if (!cache) return 0;
    int count = 0;
    for (int i = 0; i < kEspBoneCount; ++i) {
        Vec3 pos{};
        if (!read_cached_bone(*cache, (EspBone)i, pos)) continue;
        out_bones[i] = pos;
        out_valid[i] = true;
        ++count;
    }
    if (!out_valid[(int)EspBone::Hip]) {
        if (out_valid[(int)EspBone::LeftThigh] && out_valid[(int)EspBone::RightThigh]) {
            out_bones[(int)EspBone::Hip] = vec_mul(vec_add(out_bones[(int)EspBone::LeftThigh], out_bones[(int)EspBone::RightThigh]), 0.5F);
            out_valid[(int)EspBone::Hip] = true;
            ++count;
        } else if (vec3_is_finite(hip_fallback)) {
            out_bones[(int)EspBone::Hip] = hip_fallback;
            out_valid[(int)EspBone::Hip] = true;
            ++count;
        }
    }
    return count;
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
    g_player_model_info_cache.clear();
    g_visual_cache.clear();
}

std::vector<EspBox> esp_get_boxes(int overlay_width, int overlay_height, EspQuery query) {
    std::vector<EspBox> result;
    if (g_pid <= 0 || !g_il2cpp_base) return result;

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
    if (!g_transform_hierarchy_layout_valid) {
        size_t discovered_position_count = 0, hierarchy_candidate_count = 0;
        discover_transform_hierarchy_layout(s_transforms, discovered_position_count, hierarchy_candidate_count);
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
        if (!managed_cam) return result;
        native_cam = rd_ptr(managed_cam + MANAGED_CACHED_PTR);
        if (!native_cam) return result;
        projection = rd_m4(native_cam + CAMERA_PROJECTION_MATRIX);
        view = rd_m4(native_cam + CAMERA_VIEW_MATRIX);
        if (!matrix_is_finite(projection) || !matrix_is_finite(view)) return result;
        if (!g_matrix_configuration_validated) {
            if (!optimize_matrix_configuration(native_cam, s_transforms)) return result;
            projection = rd_m4(native_cam + CAMERA_PROJECTION_MATRIX);
            view = rd_m4(native_cam + CAMERA_VIEW_MATRIX);
        }
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

    auto project = [&](const Vec3& world, Vec2& screen) -> bool {
        return transform_camera_mode
            ? w2s_transform_camera(transform_camera_position, transform_camera_rotation, world, sw, sh, screen, false)
            : w2s(vp, world, sw, sh, screen, false);
    };

    for (size_t i = 0; i < s_transforms.size(); ++i) {
        if (i == local_entity_index || !s_transforms[i]) continue;
        Vec3 hip{};
        if (!read_entity_position(s_transforms[i], hip)) continue;

        float distance = -1.0F;
        if (has_local_position) {
            float dx = hip.x - local.x, dy = hip.y - local.y, dz = hip.z - local.z;
            distance = sqrtf(dx * dx + dy * dy + dz * dz);
            if (!std::isfinite(distance) || distance < MIN_PLAYER_DISTANCE || distance > MAX_PLAYER_DISTANCE) continue;
        }

        Vec3 head{};
        if (!read_visual_head(s_transforms[i], head))
            head = {hip.x, hip.y + PLAYER_HEIGHT, hip.z};

        Vec3 body_bottom = hip;
        Vec3 body_top = head;
        Vec3 up = vec_norm(vec_sub(head, hip));
        body_bottom = vec_sub(hip, vec_mul(up, 0.12F));
        body_top = vec_add(head, vec_mul(up, 0.12F));

        Vec2 sf{}, sh2{};
        if (!project(body_bottom, sf) || !project(body_top, sh2)) continue;
        float height = fabsf(sh2.y - sf.y);
        if (!std::isfinite(height) || height < 2.0F) continue;
        float cx = (sf.x + sh2.x) * 0.5F;
        float cy = (sf.y + sh2.y) * 0.5F;
        float half_w = height * PLAYER_BOX_WIDTH_RATIO * 0.5F;
        float half_h = height * 0.5F;

        EspBox box{};
        box.x1 = cx - half_w; box.y1 = cy - half_h;
        box.x2 = cx + half_w; box.y2 = cy + half_h;
        box.distance = distance;
        box.has_skeleton = false;

        if (query.chams) {
            constexpr float box_half_width = 0.28F, box_half_depth = 0.28F;
            const Vec3 world_corners[8] = {
                {hip.x - box_half_width, body_bottom.y, hip.z - box_half_depth},
                {hip.x + box_half_width, body_bottom.y, hip.z - box_half_depth},
                {hip.x + box_half_width, body_bottom.y, hip.z + box_half_depth},
                {hip.x - box_half_width, body_bottom.y, hip.z + box_half_depth},
                {hip.x - box_half_width, body_top.y, hip.z - box_half_depth},
                {hip.x + box_half_width, body_top.y, hip.z - box_half_depth},
                {hip.x + box_half_width, body_top.y, hip.z + box_half_depth},
                {hip.x - box_half_width, body_top.y, hip.z + box_half_depth}
            };
            for (size_t corner = 0; corner < 8; ++corner) {
                Vec2 sc{};
                bool projected = project(world_corners[corner], sc);
                box.corner_visible[corner] = projected && sc.x >= 0.0F && sc.x <= sw && sc.y >= 0.0F && sc.y <= sh;
                box.corners[corner][0] = projected ? sc.x : -1.0F;
                box.corners[corner][1] = projected ? sc.y : -1.0F;
            }
        }

        if (query.skeleton) {
            Vec3 world_bones[kEspBoneCount]{};
            bool world_valid[kEspBoneCount]{};
            int projected = 0;
            fill_skeleton_world(s_transforms[i], hip, world_bones, world_valid);
            for (int bone = 0; bone < kEspBoneCount; ++bone) {
                if (!world_valid[bone]) continue;
                Vec2 screen{};
                if (!project(world_bones[bone], screen) || !std::isfinite(screen.x) || !std::isfinite(screen.y)) continue;
                box.bones[bone][0] = screen.x;
                box.bones[bone][1] = screen.y;
                box.bone_visible[bone] = true;
                ++projected;
            }
            box.has_skeleton = projected >= 2;
        }
        result.push_back(box);
    }
    return result;
}
