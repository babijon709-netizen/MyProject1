#include "game.h"
#include "game_offsets.h"
#include "Vector.h"

#include <string.h>
#include <sys/uio.h>
#include <stdio.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <string>
#include <chrono>
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
// PlayerManager pointers from the previous scene snapshot. A wholesale
// replacement (server switch / scene reload) resets per-scene caches.
static std::unordered_set<uint64_t> g_scene_players;

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

struct NativeTreeLayout {
    uint64_t children_offset;
    uint64_t child_count_offset;
    uint64_t parent_offset;
};

struct PlayerBoneCache {
    uint64_t animator = 0;
    uint64_t rig_root = 0;
    std::array<uint64_t, ESP_BONE_COUNT> joints{};
    double resolved_at = 0.0;
    bool named_rig = false;
};
static std::unordered_map<uint64_t, PlayerBoneCache> g_player_bones;

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

static double monotonic_seconds() {
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static bool read_remote_bytes(uint64_t address, void* output, size_t size) {
    if (!address || !output || size == 0) return false;
    struct iovec local = {output, size};
    struct iovec remote = {(void*)address, size};
    return process_vm_readv(g_pid, &local, 1, &remote, 1, 0) == (ssize_t)size;
}

// IL2CPP strings are UTF-16. Convert the small strings used by ESP directly to
// UTF-8 so player names are safe to pass to ImGui (including Cyrillic names).
static std::string read_il2cpp_string(uint64_t string_object, size_t max_chars = 63) {
    if (!likely_native_pointer(string_object)) return {};
    int32_t length = rd<int32_t>(string_object + 0x10);
    if (length <= 0 || length > 256) return {};
    size_t count = std::min<size_t>((size_t)length, max_chars);
    std::vector<uint16_t> text(count);
    if (!read_remote_bytes(string_object + 0x14, text.data(), count * sizeof(uint16_t))) return {};

    std::string result;
    result.reserve(count * 2);
    for (size_t i = 0; i < count; ++i) {
        uint32_t code = text[i];
        if (code >= 0xD800 && code <= 0xDBFF && i + 1 < count) {
            uint32_t low = text[i + 1];
            if (low >= 0xDC00 && low <= 0xDFFF) {
                code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                ++i;
            }
        }
        if (code == 0) break;
        if (code < 0x80) result.push_back((char)code);
        else if (code < 0x800) {
            result.push_back((char)(0xC0 | (code >> 6)));
            result.push_back((char)(0x80 | (code & 0x3F)));
        } else if (code < 0x10000) {
            result.push_back((char)(0xE0 | (code >> 12)));
            result.push_back((char)(0x80 | ((code >> 6) & 0x3F)));
            result.push_back((char)(0x80 | (code & 0x3F)));
        } else if (code <= 0x10FFFF) {
            result.push_back((char)(0xF0 | (code >> 18)));
            result.push_back((char)(0x80 | ((code >> 12) & 0x3F)));
            result.push_back((char)(0x80 | ((code >> 6) & 0x3F)));
            result.push_back((char)(0x80 | (code & 0x3F)));
        }
    }
    return result;
}

static bool native_transform_is_valid(uint64_t native_transform) {
    if (!likely_native_pointer(native_transform)) return false;
    for (const auto& offsets : {std::pair<uint64_t, uint64_t>{0x38, 0x40},
                                std::pair<uint64_t, uint64_t>{0x28, 0x30}}) {
        uint64_t transform_data = rd_ptr(native_transform + offsets.first);
        int32_t transform_index = rd<int32_t>(native_transform + offsets.second);
        if (likely_native_pointer(transform_data) && transform_index >= 0 && transform_index <= 100000)
            return true;
    }
    return false;
}

// Every native Component points to its GameObject at +0x30. GameObject's first
// component is always its Transform. Unity has used two component-pair layouts
// across the relevant releases, so both are validated before use.
static uint64_t native_transform_from_component(uint64_t managed_component) {
    if (!likely_native_pointer(managed_component)) return 0;
    uint64_t native_component = rd_ptr(managed_component + MANAGED_CACHED_PTR);
    if (!likely_native_pointer(native_component)) return 0;
    uint64_t game_object = rd_ptr(native_component + NATIVE_COMPONENT_GAME_OBJECT);
    if (!likely_native_pointer(game_object)) return 0;

    const uint64_t component_vector_offsets[] = {
        NATIVE_GAME_OBJECT_COMPONENTS, NATIVE_GAME_OBJECT_COMPONENTS + 8
    };
    for (uint64_t vector_offset : component_vector_offsets) {
        uint64_t components = rd_ptr(game_object + vector_offset);
        if (!likely_native_pointer(components)) continue;
        for (uint64_t entry_offset : {uint64_t(8), uint64_t(0), uint64_t(0x10)}) {
            uint64_t candidate = rd_ptr(components + entry_offset);
            if (native_transform_is_valid(candidate)) return candidate;
        }
    }
    return 0;
}

static bool read_native_transform_pose(uint64_t native_transform, Vec3& position, Vec4& rotation) {
    if (!native_transform_is_valid(native_transform)) return false;
    if (g_transform_hierarchy_layout_valid &&
        read_transform_hierarchy_layout(native_transform, g_transform_hierarchy_layout, position, &rotation))
        return true;

    const TransformHierarchyLayout common_layouts[] = {
        {0x38, 0x40, 0x18, 0x20, false, false},
        {0x38, 0x40, 0x18, 0x20, true,  true},
        {0x38, 0x40, 0x08, 0x10, false, false},
        {0x38, 0x40, 0x08, 0x10, true,  true},
        {0x28, 0x30, 0x18, 0x20, false, false},
        {0x28, 0x30, 0x18, 0x20, true,  true},
        {0x28, 0x30, 0x08, 0x10, false, false},
        {0x28, 0x30, 0x08, 0x10, true,  true}
    };
    for (const auto& layout : common_layouts)
        if (read_transform_hierarchy_layout(native_transform, layout, position, &rotation)) return true;
    return false;
}

static bool printable_native_name(const char* text, size_t capacity, std::string& output) {
    size_t length = 0;
    while (length < capacity && text[length]) ++length;
    if (length == 0 || length == capacity) return false;
    size_t printable = 0;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)text[i];
        if (c >= 0x20 && c < 0x7F) ++printable;
    }
    if (printable * 10 < length * 8) return false;
    output.assign(text, length);
    return true;
}

static std::string read_native_transform_name(uint64_t native_transform) {
    uint64_t game_object = rd_ptr(native_transform + NATIVE_COMPONENT_GAME_OBJECT);
    if (!likely_native_pointer(game_object)) return {};
    for (uint64_t name_offset : {NATIVE_GAME_OBJECT_NAME, uint64_t(0x58), uint64_t(0x68)}) {
        uint64_t name = rd_ptr(game_object + name_offset);
        if (!likely_native_pointer(name)) continue;
        for (int indirect = 0; indirect < 2; ++indirect) {
            uint64_t candidate = indirect ? rd_ptr(name) : name;
            if (!likely_native_pointer(candidate)) continue;
            char buffer[96]{};
            if (!read_remote_bytes(candidate, buffer, sizeof(buffer) - 1)) continue;
            std::string result;
            if (printable_native_name(buffer, sizeof(buffer) - 1, result)) return result;
        }
    }
    return {};
}

static std::string lower_ascii(std::string value) {
    for (char& c : value) c = (char)std::tolower((unsigned char)c);
    return value;
}

static std::string compact_bone_name(const std::string& value) {
    std::string compact;
    compact.reserve(value.size());
    for (unsigned char c : value)
        if (std::isalnum(c)) compact.push_back((char)std::tolower(c));
    return compact;
}

static bool contains_one(const std::string& value, std::initializer_list<const char*> words) {
    for (const char* word : words)
        if (value.find(word) != std::string::npos) return true;
    return false;
}

enum class BoneSide { None, Left, Right };

static BoneSide bone_side(const std::string& original, const std::string& compact) {
    if (compact.find("left") != std::string::npos) return BoneSide::Left;
    if (compact.find("right") != std::string::npos) return BoneSide::Right;
    std::string padded = " " + lower_ascii(original) + " ";
    const bool explicit_left = contains_one(padded, {" l ", "_l ", ".l ", "-l ", " l_", " l.", " l-"});
    const bool explicit_right = contains_one(padded, {" r ", "_r ", ".r ", "-r ", " r_", " r.", " r-"});
    if (explicit_left && !explicit_right) return BoneSide::Left;
    if (explicit_right && !explicit_left) return BoneSide::Right;

    // Compact prefixes are only accepted for forms such as LUpperArm. Do not
    // interpret the first letter of LowerArm/LowerLeg as the left-side marker.
    if ((!compact.empty() && compact.back() == 'l') ||
        (compact.size() > 1 && compact.front() == 'l' && compact.rfind("lower", 0) != 0))
        return BoneSide::Left;
    if ((!compact.empty() && compact.back() == 'r') ||
        (compact.size() > 1 && compact.front() == 'r'))
        return BoneSide::Right;
    return BoneSide::None;
}

static int bone_name_score(const std::string& original, EspBone role) {
    const std::string compact = compact_bone_name(original);
    if (compact.empty()) return 0;
    const BoneSide side = bone_side(original, compact);
    const bool finger = contains_one(compact, {"finger", "thumb", "index", "middle", "pinky", "ring", "weapon", "socket", "ik"});

    switch (role) {
        case ESP_BONE_HEAD:
            if (compact == "head" || compact.find("mixamorighead") != std::string::npos) return 180;
            if (compact.find("head") != std::string::npos && !contains_one(compact, {"hair", "eye", "end", "top"})) return 130;
            break;
        case ESP_BONE_NECK:
            if (compact.find("neck") != std::string::npos && !contains_one(compact, {"lace", "socket"})) return 150;
            break;
        case ESP_BONE_CHEST:
            if (compact.find("upperchest") != std::string::npos) return 180;
            if (compact.find("chest") != std::string::npos) return 160;
            if (contains_one(compact, {"spine3", "spine03", "spine2", "spine02"})) return 135;
            if (compact.find("spine") != std::string::npos) return 80;
            break;
        case ESP_BONE_PELVIS:
            if (compact.find("pelvis") != std::string::npos) return 180;
            if (compact.find("hips") != std::string::npos) return 170;
            if (side == BoneSide::None && compact.find("hip") != std::string::npos) return 100;
            break;
        default: break;
    }

    bool left_role = role == ESP_BONE_LEFT_SHOULDER || role == ESP_BONE_LEFT_ELBOW ||
        role == ESP_BONE_LEFT_HAND || role == ESP_BONE_LEFT_HIP ||
        role == ESP_BONE_LEFT_KNEE || role == ESP_BONE_LEFT_FOOT;
    bool right_role = role == ESP_BONE_RIGHT_SHOULDER || role == ESP_BONE_RIGHT_ELBOW ||
        role == ESP_BONE_RIGHT_HAND || role == ESP_BONE_RIGHT_HIP ||
        role == ESP_BONE_RIGHT_KNEE || role == ESP_BONE_RIGHT_FOOT;
    if ((left_role && side != BoneSide::Left) || (right_role && side != BoneSide::Right)) return 0;

    switch (role) {
        case ESP_BONE_LEFT_SHOULDER: case ESP_BONE_RIGHT_SHOULDER:
            if (contains_one(compact, {"shoulder", "clavicle", "collar"})) return 170;
            if (contains_one(compact, {"upperarm", "uparm"})) return 120;
            if (compact.find("arm") != std::string::npos && !contains_one(compact, {"lower", "fore"})) return 85;
            break;
        case ESP_BONE_LEFT_ELBOW: case ESP_BONE_RIGHT_ELBOW:
            if (contains_one(compact, {"lowerarm", "forearm", "elbow"})) return 170;
            break;
        case ESP_BONE_LEFT_HAND: case ESP_BONE_RIGHT_HAND:
            if (!finger && contains_one(compact, {"hand", "wrist"})) return 170;
            break;
        case ESP_BONE_LEFT_HIP: case ESP_BONE_RIGHT_HIP:
            if (contains_one(compact, {"upperleg", "upleg", "thigh"})) return 170;
            if (compact.find("hip") != std::string::npos) return 120;
            break;
        case ESP_BONE_LEFT_KNEE: case ESP_BONE_RIGHT_KNEE:
            if (contains_one(compact, {"lowerleg", "lowleg", "calf", "shin", "knee"})) return 170;
            break;
        case ESP_BONE_LEFT_FOOT: case ESP_BONE_RIGHT_FOOT:
            if (!contains_one(compact, {"toe", "end", "ik"}) && contains_one(compact, {"foot", "ankle"})) return 170;
            break;
        default: break;
    }
    return 0;
}

static void collect_native_transforms(uint64_t root, const NativeTreeLayout& layout,
                                      std::vector<uint64_t>& output,
                                      std::unordered_set<uint64_t>& visited,
                                      int depth = 0) {
    if (!native_transform_is_valid(root) || depth > 48 || output.size() >= 384 || !visited.insert(root).second) return;
    output.push_back(root);
    int32_t count = rd<int32_t>(root + layout.child_count_offset);
    uint64_t children = rd_ptr(root + layout.children_offset);
    if (count <= 0 || count > 512 || !likely_native_pointer(children)) return;
    for (int32_t index = 0; index < count && output.size() < 384; ++index) {
        uint64_t child = rd_ptr(children + (uint64_t)index * sizeof(uint64_t));
        if (!native_transform_is_valid(child)) continue;
        uint64_t parent = rd_ptr(child + layout.parent_offset);
        if (likely_native_pointer(parent) && parent != root) continue;
        collect_native_transforms(child, layout, output, visited, depth + 1);
    }
}

static int map_named_bones(const std::vector<uint64_t>& transforms,
                           std::array<uint64_t, ESP_BONE_COUNT>& joints) {
    std::array<int, ESP_BONE_COUNT> scores{};
    joints.fill(0);
    for (uint64_t transform : transforms) {
        std::string name = read_native_transform_name(transform);
        if (name.empty()) continue;
        for (int role = 0; role < ESP_BONE_COUNT; ++role) {
            int score = bone_name_score(name, (EspBone)role);
            if (score > scores[(size_t)role]) {
                scores[(size_t)role] = score;
                joints[(size_t)role] = transform;
            }
        }
    }
    int result = 0;
    for (int role = 0; role < ESP_BONE_COUNT; ++role)
        if (joints[(size_t)role]) result += 1000 + scores[(size_t)role];
    return result;
}

static PlayerBoneCache resolve_player_bones(uint64_t player) {
    PlayerBoneCache cache{};
    cache.animator = rd_ptr(player + PLAYER_ANIMATOR);
    cache.resolved_at = monotonic_seconds();

    uint64_t animator_transform = native_transform_from_component(cache.animator);
    uint64_t camera_root = resolve_player_native_transform(player);
    if (!animator_transform && !camera_root) return cache;

    const NativeTreeLayout layouts[] = {
        {NATIVE_TRANSFORM_CHILDREN, NATIVE_TRANSFORM_CHILD_COUNT, NATIVE_TRANSFORM_PARENT},
        {0x68, 0x78, 0x88},
        {0x78, 0x88, 0x98}
    };

    int best_score = 0;
    for (const auto& layout : layouts) {
        for (uint64_t seed : {animator_transform, camera_root}) {
            if (!native_transform_is_valid(seed)) continue;
            std::vector<uint64_t> roots;
            roots.push_back(seed);
            uint64_t highest = seed;
            for (int depth = 0; depth < 12; ++depth) {
                uint64_t parent = rd_ptr(highest + layout.parent_offset);
                if (!native_transform_is_valid(parent) || parent == highest) break;
                highest = parent;
            }
            if (highest != seed) roots.push_back(highest);

            for (uint64_t root : roots) {
                std::vector<uint64_t> transforms;
                std::unordered_set<uint64_t> visited;
                collect_native_transforms(root, layout, transforms, visited);
                std::array<uint64_t, ESP_BONE_COUNT> mapped{};
                int score = map_named_bones(transforms, mapped);
                if (score > best_score) {
                    best_score = score;
                    cache.rig_root = animator_transform ? animator_transform : root;
                    cache.joints = mapped;
                }
            }
        }
    }

    int found = 0;
    for (uint64_t joint : cache.joints) if (joint) ++found;
    cache.named_rig = found >= 11 && cache.joints[ESP_BONE_HEAD] &&
        cache.joints[ESP_BONE_CHEST] && cache.joints[ESP_BONE_PELVIS] &&
        cache.joints[ESP_BONE_LEFT_ELBOW] && cache.joints[ESP_BONE_RIGHT_ELBOW] &&
        cache.joints[ESP_BONE_LEFT_KNEE] && cache.joints[ESP_BONE_RIGHT_KNEE] &&
        cache.joints[ESP_BONE_LEFT_FOOT] && cache.joints[ESP_BONE_RIGHT_FOOT];
    if (!cache.rig_root) cache.rig_root = animator_transform ? animator_transform : camera_root;
    return cache;
}

static Vec3 vec_lerp(const Vec3& from, const Vec3& to, float amount) {
    return {from.x + (to.x - from.x) * amount,
            from.y + (to.y - from.y) * amount,
            from.z + (to.z - from.z) * amount};
}

static Vec3 vec_add_scaled(const Vec3& base, const Vec3& direction, float amount) {
    return {base.x + direction.x * amount, base.y + direction.y * amount, base.z + direction.z * amount};
}

static bool read_animated_skeleton(PlayerBoneCache& cache, const Vec3& reference_feet,
                                   std::array<Vec3, ESP_BONE_COUNT>& bones) {
    if (!cache.named_rig) return false;
    std::array<bool, ESP_BONE_COUNT> valid{};
    for (int role = 0; role < ESP_BONE_COUNT; ++role) {
        uint64_t transform = cache.joints[(size_t)role];
        if (!transform) continue;
        Vec3 position{};
        if (read_transform_hierarchy_position(transform, position)) {
            bones[(size_t)role] = position;
            valid[(size_t)role] = true;
        }
    }

    // Some rigs omit explicit neck/clavicle nodes. Keep all endpoints attached
    // to real animated joints and derive only the missing connector.
    if (!valid[ESP_BONE_NECK] && valid[ESP_BONE_HEAD] && valid[ESP_BONE_CHEST]) {
        bones[ESP_BONE_NECK] = vec_lerp(bones[ESP_BONE_CHEST], bones[ESP_BONE_HEAD], 0.68f);
        valid[ESP_BONE_NECK] = true;
    }
    if (!valid[ESP_BONE_LEFT_SHOULDER] && valid[ESP_BONE_CHEST] && valid[ESP_BONE_LEFT_ELBOW]) {
        bones[ESP_BONE_LEFT_SHOULDER] = vec_lerp(bones[ESP_BONE_CHEST], bones[ESP_BONE_LEFT_ELBOW], 0.28f);
        valid[ESP_BONE_LEFT_SHOULDER] = true;
    }
    if (!valid[ESP_BONE_RIGHT_SHOULDER] && valid[ESP_BONE_CHEST] && valid[ESP_BONE_RIGHT_ELBOW]) {
        bones[ESP_BONE_RIGHT_SHOULDER] = vec_lerp(bones[ESP_BONE_CHEST], bones[ESP_BONE_RIGHT_ELBOW], 0.28f);
        valid[ESP_BONE_RIGHT_SHOULDER] = true;
    }
    if (!valid[ESP_BONE_LEFT_HIP] && valid[ESP_BONE_PELVIS] && valid[ESP_BONE_LEFT_KNEE]) {
        bones[ESP_BONE_LEFT_HIP] = vec_lerp(bones[ESP_BONE_PELVIS], bones[ESP_BONE_LEFT_KNEE], 0.18f);
        valid[ESP_BONE_LEFT_HIP] = true;
    }
    if (!valid[ESP_BONE_RIGHT_HIP] && valid[ESP_BONE_PELVIS] && valid[ESP_BONE_RIGHT_KNEE]) {
        bones[ESP_BONE_RIGHT_HIP] = vec_lerp(bones[ESP_BONE_PELVIS], bones[ESP_BONE_RIGHT_KNEE], 0.18f);
        valid[ESP_BONE_RIGHT_HIP] = true;
    }
    for (bool value : valid) if (!value) return false;

    float feet_y = std::min(bones[ESP_BONE_LEFT_FOOT].y, bones[ESP_BONE_RIGHT_FOOT].y);
    float height = bones[ESP_BONE_HEAD].y - feet_y;
    if (!std::isfinite(height) || height < 0.55f || height > 2.7f) return false;
    for (const Vec3& point : bones) {
        float dx = point.x - reference_feet.x;
        float dy = point.y - reference_feet.y;
        float dz = point.z - reference_feet.z;
        if (!vec3_is_finite(point) || dx * dx + dy * dy + dz * dz > 16.0f) return false;
    }
    return true;
}

static void build_procedural_skeleton(const PlayerBoneCache& cache, const Vec3& feet,
                                      bool crouched, std::array<Vec3, ESP_BONE_COUNT>& bones) {
    Vec3 right{1.f, 0.f, 0.f}, forward{0.f, 0.f, 1.f};
    Vec3 pose_position{};
    Vec4 pose_rotation{};
    if (read_native_transform_pose(cache.rig_root, pose_position, pose_rotation)) {
        right = rotate_vector(pose_rotation, {1.f, 0.f, 0.f});
        forward = rotate_vector(pose_rotation, {0.f, 0.f, 1.f});
        right.y = 0.f; forward.y = 0.f;
        float rl = sqrtf(right.x * right.x + right.z * right.z);
        float fl = sqrtf(forward.x * forward.x + forward.z * forward.z);
        if (rl > 0.001f) { right.x /= rl; right.z /= rl; } else right = {1.f, 0.f, 0.f};
        if (fl > 0.001f) { forward.x /= fl; forward.z /= fl; } else forward = {0.f, 0.f, 1.f};
    }

    const float pelvis_y = crouched ? 0.64f : 0.92f;
    const float chest_y  = crouched ? 0.94f : 1.34f;
    const float neck_y   = crouched ? 1.08f : 1.55f;
    const float head_y   = crouched ? 1.20f : 1.73f;
    Vec3 pelvis = {feet.x, feet.y + pelvis_y, feet.z};
    if (crouched) pelvis = vec_add_scaled(pelvis, forward, -0.08f);
    Vec3 chest = {pelvis.x, feet.y + chest_y, pelvis.z};
    chest = vec_add_scaled(chest, forward, crouched ? 0.10f : 0.f);

    bones[ESP_BONE_PELVIS] = pelvis;
    bones[ESP_BONE_CHEST] = chest;
    bones[ESP_BONE_NECK] = {chest.x, feet.y + neck_y, chest.z};
    bones[ESP_BONE_HEAD] = {chest.x, feet.y + head_y, chest.z};

    bones[ESP_BONE_LEFT_SHOULDER]  = vec_add_scaled(chest, right, -0.23f);
    bones[ESP_BONE_RIGHT_SHOULDER] = vec_add_scaled(chest, right,  0.23f);
    bones[ESP_BONE_LEFT_ELBOW]  = vec_add_scaled(bones[ESP_BONE_LEFT_SHOULDER], right, -0.16f);
    bones[ESP_BONE_RIGHT_ELBOW] = vec_add_scaled(bones[ESP_BONE_RIGHT_SHOULDER], right, 0.16f);
    bones[ESP_BONE_LEFT_ELBOW].y  -= crouched ? 0.17f : 0.23f;
    bones[ESP_BONE_RIGHT_ELBOW].y -= crouched ? 0.17f : 0.23f;
    bones[ESP_BONE_LEFT_HAND]  = vec_add_scaled(bones[ESP_BONE_LEFT_ELBOW], forward, 0.22f);
    bones[ESP_BONE_RIGHT_HAND] = vec_add_scaled(bones[ESP_BONE_RIGHT_ELBOW], forward, 0.22f);
    bones[ESP_BONE_LEFT_HAND].y  -= 0.20f;
    bones[ESP_BONE_RIGHT_HAND].y -= 0.20f;

    bones[ESP_BONE_LEFT_HIP]  = vec_add_scaled(pelvis, right, -0.12f);
    bones[ESP_BONE_RIGHT_HIP] = vec_add_scaled(pelvis, right,  0.12f);
    bones[ESP_BONE_LEFT_KNEE]  = vec_add_scaled({feet.x, feet.y + (crouched ? 0.34f : 0.48f), feet.z}, right, -0.13f);
    bones[ESP_BONE_RIGHT_KNEE] = vec_add_scaled({feet.x, feet.y + (crouched ? 0.34f : 0.48f), feet.z}, right,  0.13f);
    if (crouched) {
        bones[ESP_BONE_LEFT_KNEE]  = vec_add_scaled(bones[ESP_BONE_LEFT_KNEE], forward, 0.28f);
        bones[ESP_BONE_RIGHT_KNEE] = vec_add_scaled(bones[ESP_BONE_RIGHT_KNEE], forward, 0.28f);
    }
    bones[ESP_BONE_LEFT_FOOT]  = vec_add_scaled({feet.x, feet.y + 0.03f, feet.z}, right, -0.14f);
    bones[ESP_BONE_RIGHT_FOOT] = vec_add_scaled({feet.x, feet.y + 0.03f, feet.z}, right,  0.14f);
    bones[ESP_BONE_LEFT_FOOT]  = vec_add_scaled(bones[ESP_BONE_LEFT_FOOT], forward, 0.08f);
    bones[ESP_BONE_RIGHT_FOOT] = vec_add_scaled(bones[ESP_BONE_RIGHT_FOOT], forward, 0.08f);
}

enum DirectHumanBone : int {
    DIRECT_HIPS = 0,
    DIRECT_LEFT_UPPER_LEG = 1,
    DIRECT_RIGHT_UPPER_LEG = 2,
    DIRECT_LEFT_LOWER_LEG = 3,
    DIRECT_RIGHT_LOWER_LEG = 4,
    DIRECT_LEFT_FOOT = 5,
    DIRECT_RIGHT_FOOT = 6,
    DIRECT_SPINE = 7,
    DIRECT_CHEST = 8,
    DIRECT_UPPER_CHEST = 9,
    DIRECT_NECK = 10,
    DIRECT_HEAD = 11,
    DIRECT_LEFT_SHOULDER = 12,
    DIRECT_RIGHT_SHOULDER = 13,
    DIRECT_LEFT_UPPER_ARM = 14,
    DIRECT_RIGHT_UPPER_ARM = 15,
    DIRECT_LEFT_LOWER_ARM = 16,
    DIRECT_RIGHT_LOWER_ARM = 17,
    DIRECT_LEFT_HAND = 18,
    DIRECT_RIGHT_HAND = 19,
    DIRECT_BONE_COUNT = 22
};

struct DirectBoneSet {
    std::array<Vec3, DIRECT_BONE_COUNT> position{};
    std::array<bool, DIRECT_BONE_COUNT> valid{};
    int score = -1;
};

static uint64_t resolve_character_animation(uint64_t player) {
    uint64_t reference = rd_ptr(player + PLAYER_KCC_REFERENCE);
    const uint64_t candidates[] = {
        reference,
        reference ? rd_ptr(reference + 0x10) : 0,
        reference ? rd_ptr(reference + 0x18) : 0,
        reference ? rd_ptr(reference + 0x20) : 0
    };
    for (uint64_t candidate : candidates) {
        if (!likely_native_pointer(candidate)) continue;
        if (rd_ptr(candidate + KCC_PLAYER) == player) {
            uint64_t animation = rd_ptr(candidate + KCC_CHARACTER_ANIMATION);
            if (likely_native_pointer(animation)) return animation;
        }
        // Tutorial/legacy KCC has inherited fields shifted by eight bytes.
        if (rd_ptr(candidate + 0x80) == player) {
            uint64_t animation = rd_ptr(candidate + 0xC0);
            if (likely_native_pointer(animation)) return animation;
        }
    }
    return 0;
}

static DirectBoneSet map_direct_bones(const std::vector<Vec3>& positions,
                                      const std::vector<uint8_t>& position_valid,
                                      const std::vector<int32_t>& mapping,
                                      int mapping_mode) {
    DirectBoneSet result{};
    result.score = 0;
    int iterations = mapping_mode == 2 ? DIRECT_BONE_COUNT : (int)positions.size();
    for (int index = 0; index < iterations; ++index) {
        int bone_id = index;
        int transform_index = index;
        if (mapping_mode == 1) {
            if (index >= (int)mapping.size()) continue;
            bone_id = mapping[(size_t)index];
        } else if (mapping_mode == 2) {
            if (index >= (int)mapping.size()) continue;
            transform_index = mapping[(size_t)index];
        }
        if (bone_id < 0 || bone_id >= DIRECT_BONE_COUNT ||
            transform_index < 0 || transform_index >= (int)positions.size() ||
            !position_valid[(size_t)transform_index] || result.valid[(size_t)bone_id]) continue;
        result.position[(size_t)bone_id] = positions[(size_t)transform_index];
        result.valid[(size_t)bone_id] = true;
        ++result.score;
    }

    if (result.valid[DIRECT_HIPS]) result.score += 100;
    if (result.valid[DIRECT_HEAD]) result.score += 120;
    if (result.valid[DIRECT_NECK]) result.score += 60;
    if (result.valid[DIRECT_CHEST] || result.valid[DIRECT_UPPER_CHEST] || result.valid[DIRECT_SPINE]) result.score += 80;
    if (result.valid[DIRECT_LEFT_LOWER_ARM] && result.valid[DIRECT_RIGHT_LOWER_ARM]) result.score += 70;
    if (result.valid[DIRECT_LEFT_UPPER_LEG] && result.valid[DIRECT_RIGHT_UPPER_LEG]) result.score += 50;

    if (result.valid[DIRECT_HEAD] && result.valid[DIRECT_HIPS]) {
        float dy = result.position[DIRECT_HEAD].y - result.position[DIRECT_HIPS].y;
        if (std::isfinite(dy) && dy > 0.25f && dy < 1.8f) result.score += 100;
        else result.score -= 250;
    }
    if (result.valid[DIRECT_LEFT_FOOT] && result.valid[DIRECT_RIGHT_FOOT] && result.valid[DIRECT_HIPS]) {
        float feet_y = std::min(result.position[DIRECT_LEFT_FOOT].y, result.position[DIRECT_RIGHT_FOOT].y);
        float dy = result.position[DIRECT_HIPS].y - feet_y;
        if (std::isfinite(dy) && dy > 0.15f && dy < 1.4f) result.score += 80;
        else result.score -= 200;
    }
    return result;
}

// CharacterAnimation.tk.pjh is the game's own HumanBodyBones Transform array.
// Reading it avoids guessing transform names and follows Animator/IK/ragdoll
// updates exactly. Name-based hierarchy discovery below remains a compatibility
// fallback for skins that temporarily rebuild this cache.
static bool read_direct_humanoid_skeleton(uint64_t player, const Vec3& feet,
                                          std::array<Vec3, ESP_BONE_COUNT>& bones) {
    uint64_t animation = resolve_character_animation(player);
    if (!animation) return false;
    uint64_t bone_cache = rd_ptr(animation + CHARACTER_ANIMATION_BONE_CACHE);
    if (!likely_native_pointer(bone_cache)) return false;
    uint64_t transform_array = rd_ptr(bone_cache + BONE_CACHE_TRANSFORMS);
    uint64_t mapping_array = rd_ptr(bone_cache + BONE_CACHE_MAPPING);
    if (!likely_native_pointer(transform_array)) return false;

    int32_t count = rd<int32_t>(transform_array + IL2CPP_LIST_SIZE);
    if (count <= 0 || count > 96) return false;
    std::vector<Vec3> positions((size_t)count);
    std::vector<uint8_t> position_valid((size_t)count, 0);
    for (int32_t index = 0; index < count; ++index) {
        uint64_t managed_transform = rd_ptr(transform_array + IL2CPP_ARRAY_FIRST_ELEMENT +
                                            (uint64_t)index * sizeof(uint64_t));
        uint64_t native_transform = resolve_native_transform(managed_transform);
        Vec3 position{};
        Vec4 rotation{};
        if (!read_native_transform_pose(native_transform, position, rotation)) continue;
        float dx = position.x - feet.x, dy = position.y - feet.y, dz = position.z - feet.z;
        if (!vec3_is_finite(position) || dx * dx + dy * dy + dz * dz > 100.f) continue;
        positions[(size_t)index] = position;
        position_valid[(size_t)index] = 1;
    }

    std::vector<int32_t> mapping;
    if (likely_native_pointer(mapping_array)) {
        int32_t mapping_count = rd<int32_t>(mapping_array + IL2CPP_LIST_SIZE);
        if (mapping_count > 0 && mapping_count <= 96) {
            mapping.resize((size_t)mapping_count);
            read_remote_bytes(mapping_array + IL2CPP_ARRAY_FIRST_ELEMENT,
                              mapping.data(), mapping.size() * sizeof(int32_t));
        }
    }

    DirectBoneSet best{};
    best.score = -1;
    for (int mode = 0; mode < 3; ++mode) {
        if (mode != 0 && mapping.empty()) continue;
        DirectBoneSet candidate = map_direct_bones(positions, position_valid, mapping, mode);
        if (candidate.score > best.score) best = candidate;
    }
    if (best.score < 250 || !best.valid[DIRECT_HIPS] ||
        !best.valid[DIRECT_LEFT_LOWER_ARM] || !best.valid[DIRECT_RIGHT_LOWER_ARM] ||
        !best.valid[DIRECT_LEFT_HAND] || !best.valid[DIRECT_RIGHT_HAND] ||
        !best.valid[DIRECT_LEFT_UPPER_LEG] || !best.valid[DIRECT_RIGHT_UPPER_LEG] ||
        !best.valid[DIRECT_LEFT_LOWER_LEG] || !best.valid[DIRECT_RIGHT_LOWER_LEG] ||
        !best.valid[DIRECT_LEFT_FOOT] || !best.valid[DIRECT_RIGHT_FOOT]) return false;

    Vec3 head{};
    if (best.valid[DIRECT_HEAD]) head = best.position[DIRECT_HEAD];
    else if (best.valid[DIRECT_NECK]) {
        head = best.position[DIRECT_NECK];
        head.y += 0.18f;
    } else return false;
    Vec3 chest = best.valid[DIRECT_UPPER_CHEST] ? best.position[DIRECT_UPPER_CHEST]
               : best.valid[DIRECT_CHEST] ? best.position[DIRECT_CHEST]
               : best.valid[DIRECT_SPINE] ? best.position[DIRECT_SPINE] : Vec3{};
    if (!vec3_is_finite(chest) || (!best.valid[DIRECT_UPPER_CHEST] &&
        !best.valid[DIRECT_CHEST] && !best.valid[DIRECT_SPINE])) return false;
    Vec3 neck = best.valid[DIRECT_NECK] ? best.position[DIRECT_NECK]
                                       : vec_lerp(chest, head, 0.68f);

    bones[ESP_BONE_HEAD] = head;
    bones[ESP_BONE_NECK] = neck;
    bones[ESP_BONE_CHEST] = chest;
    bones[ESP_BONE_PELVIS] = best.position[DIRECT_HIPS];
    bones[ESP_BONE_LEFT_SHOULDER] = best.valid[DIRECT_LEFT_SHOULDER]
        ? best.position[DIRECT_LEFT_SHOULDER] : best.position[DIRECT_LEFT_UPPER_ARM];
    bones[ESP_BONE_RIGHT_SHOULDER] = best.valid[DIRECT_RIGHT_SHOULDER]
        ? best.position[DIRECT_RIGHT_SHOULDER] : best.position[DIRECT_RIGHT_UPPER_ARM];
    bones[ESP_BONE_LEFT_ELBOW] = best.position[DIRECT_LEFT_LOWER_ARM];
    bones[ESP_BONE_RIGHT_ELBOW] = best.position[DIRECT_RIGHT_LOWER_ARM];
    bones[ESP_BONE_LEFT_HAND] = best.position[DIRECT_LEFT_HAND];
    bones[ESP_BONE_RIGHT_HAND] = best.position[DIRECT_RIGHT_HAND];
    bones[ESP_BONE_LEFT_HIP] = best.position[DIRECT_LEFT_UPPER_LEG];
    bones[ESP_BONE_RIGHT_HIP] = best.position[DIRECT_RIGHT_UPPER_LEG];
    bones[ESP_BONE_LEFT_KNEE] = best.position[DIRECT_LEFT_LOWER_LEG];
    bones[ESP_BONE_RIGHT_KNEE] = best.position[DIRECT_RIGHT_LOWER_LEG];
    bones[ESP_BONE_LEFT_FOOT] = best.position[DIRECT_LEFT_FOOT];
    bones[ESP_BONE_RIGHT_FOOT] = best.position[DIRECT_RIGHT_FOOT];

    // Shoulder bones are optional in Unity avatars; upper-arm roots occupy the
    // same anatomical joint and are a correct connector when they are absent.
    if (!best.valid[DIRECT_LEFT_SHOULDER] && !best.valid[DIRECT_LEFT_UPPER_ARM]) return false;
    if (!best.valid[DIRECT_RIGHT_SHOULDER] && !best.valid[DIRECT_RIGHT_UPPER_ARM]) return false;

    float raw_feet_y = std::min(bones[ESP_BONE_LEFT_FOOT].y, bones[ESP_BONE_RIGHT_FOOT].y);
    float height = bones[ESP_BONE_HEAD].y - raw_feet_y;
    if (!std::isfinite(height) || height < 0.55f || height > 2.7f) return false;

    // Bone roots are ankle pivots. Align their lowest level to the player's
    // capsule by a small common translation; relative animation stays exact.
    Vec3 visual_feet = vec_lerp(bones[ESP_BONE_LEFT_FOOT], bones[ESP_BONE_RIGHT_FOOT], 0.5f);
    Vec3 shift = {feet.x - visual_feet.x, feet.y - raw_feet_y, feet.z - visual_feet.z};
    float shift_length2 = shift.x * shift.x + shift.y * shift.y + shift.z * shift.z;
    if (!vec3_is_finite(shift) || shift_length2 > 16.f) return false;
    for (Vec3& point : bones) {
        point.x += shift.x; point.y += shift.y; point.z += shift.z;
        if (!vec3_is_finite(point)) return false;
    }
    return true;
}

static bool read_player_skeleton(uint64_t player, const Vec3& feet, bool crouched,
                                 std::array<Vec3, ESP_BONE_COUNT>& bones) {
    if (read_direct_humanoid_skeleton(player, feet, bones)) return true;

    uint64_t animator = rd_ptr(player + PLAYER_ANIMATOR);
    double now = monotonic_seconds();
    auto iterator = g_player_bones.find(player);
    bool refresh = iterator == g_player_bones.end() || iterator->second.animator != animator ||
        now - iterator->second.resolved_at > (iterator->second.named_rig ? 5.0 : 1.0);
    if (refresh) {
        g_player_bones[player] = resolve_player_bones(player);
        iterator = g_player_bones.find(player);
    }
    if (iterator == g_player_bones.end()) return false;

    if (read_animated_skeleton(iterator->second, feet, bones)) return true;
    // A temporary LOD swap can replace every bone Transform at once. Force a
    // quick remap next time rather than holding stale pointers for five seconds.
    iterator->second.named_rig = false;
    iterator->second.resolved_at = now - 0.75;
    build_procedural_skeleton(iterator->second, feet, crouched, bones);
    return true;
}

static void read_player_labels(uint64_t player, EspBox& box) {
    box.name[0] = '\0';
    box.weapon[0] = '\0';
    box.health = -1.f;
    box.max_health = -1.f;

    uint64_t nicklabel = rd_ptr(player + PLAYER_NICKLABEL);
    uint64_t text = nicklabel ? rd_ptr(nicklabel + NICKLABEL_TEXT) : 0;
    std::string name = text ? read_il2cpp_string(rd_ptr(text + UI_TEXT_VALUE), 48) : std::string();
    if (name.empty()) name = read_il2cpp_string(rd_ptr(player + PLAYER_USER_ID), 48);
    if (!name.empty()) snprintf(box.name, sizeof(box.name), "%s", name.c_str());

    uint64_t handler = rd_ptr(player + PLAYER_EVENT_HANDLER);
    uint64_t health_value = handler ? rd_ptr(handler + HUC_HEALTH) : 0;
    uint64_t vitals = rd_ptr(player + PLAYER_VITALS);
    if (likely_native_pointer(health_value)) box.health = rd<float>(health_value + HUO_CURRENT_FLOAT);
    if (likely_native_pointer(vitals)) box.max_health = rd<float>(vitals + PLAYER_VITALS_MAX_HP);
    if (!std::isfinite(box.health) || box.health < 0.f || box.health > 100000.f) box.health = -1.f;
    if (!std::isfinite(box.max_health) || box.max_health <= 0.f || box.max_health > 100000.f) box.max_health = -1.f;

    uint64_t reference = rd_ptr(player + PLAYER_WEAPON_REFERENCE);
    uint64_t weapon = reference ? rd_ptr(reference + INTERFACE_REFERENCE_VALUE) : 0;
    if (likely_native_pointer(weapon)) {
        int16_t number = rd<int16_t>(weapon + PLAYER_WEAPON_NUMBER);
        if (number > 0 && number < 10000)
            snprintf(box.weapon, sizeof(box.weapon), "Weapon #%d", (int)number);
    }
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
    // UnityEngine.Matrix4x4 is laid out by columns in memory:
    // m00,m10,m20,m30,m01,m11,...  Treating it as row-major transposed the
    // camera matrices and made every ESP projection fail.
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
    g_player_bones.clear();
    g_player_position_offset = PLAYER_POSITION;
    g_transform_hierarchy_layout = {};
    g_transform_hierarchy_layout_valid = false;
    g_scene_players.clear();
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
    g_player_bones.clear();
    g_player_position_offset = PLAYER_POSITION;
    g_transform_hierarchy_layout = {}; g_transform_hierarchy_layout_valid = false;
    g_scene_players.clear();
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
        // switch. Re-derive the position/camera state for the new one.
        if (!g_player_snapshot.empty() && !g_scene_players.empty()) {
            size_t overlap = 0;
            for (uint64_t p : g_player_snapshot)
                if (g_scene_players.count(p)) ++overlap;
            if (overlap == 0) {
                g_player_track.clear();
                g_player_bones.clear();
                g_player_position_validated = false;
                g_use_direct_player_position = true;
                g_player_position_offset = PLAYER_POSITION;
                g_matrix_configuration_validated = false;
                g_camera_matrix_physical_match = false;
            }
        }
        g_scene_players.clear();
        for (uint64_t p : g_player_snapshot) g_scene_players.insert(p);
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

        Vec3 render_feet = {render_x, feet_y, render_z};
        Vec3 skeleton_feet = render_feet;
        if (transform_camera_mode) skeleton_feet.y = feet.y - 1.60F;

        // Read the enemy Animator hierarchy before constructing the box. The
        // actual animated head/feet make every ESP element follow crouching,
        // leaning and weapon animations. A crouch-aware procedural rig is kept
        // only as a safe fallback for an unexpected asset/LOD bone naming set.
        std::array<Vec3, ESP_BONE_COUNT> skeleton{};
        bool have_skeleton = read_player_skeleton(s_transforms[i], skeleton_feet, crouched, skeleton);

        Vec3 body_bottom = skeleton_feet;
        Vec3 body_top = {render_x, skeleton_feet.y + head_height, render_z};
        float body_x = render_x, body_z = render_z;
        if (have_skeleton) {
            body_bottom = vec_lerp(skeleton[ESP_BONE_LEFT_FOOT], skeleton[ESP_BONE_RIGHT_FOOT], 0.5f);
            body_bottom.y = std::min(skeleton[ESP_BONE_LEFT_FOOT].y, skeleton[ESP_BONE_RIGHT_FOOT].y) - 0.03f;
            body_top = skeleton[ESP_BONE_HEAD];
            body_top.y += 0.12f;
            body_x = skeleton[ESP_BONE_PELVIS].x;
            body_z = skeleton[ESP_BONE_PELVIS].z;
        } else if (transform_camera_mode) {
            body_top.y = feet.y + 0.20F;
        }

        auto project_world = [&](const Vec3& world, Vec2& screen) {
            return transform_camera_mode
                ? w2s_transform_camera(transform_camera_position, transform_camera_rotation, world, sw, sh, screen, false)
                : w2s(vp, world, sw, sh, screen, false);
        };

        Vec2 sf{}, sh2{};
        if (!project_world(body_bottom, sf) || !project_world(body_top, sh2)) continue;

        float height = fabsf(sh2.y - sf.y);
        if (!std::isfinite(height) || height < 2.0F) continue;
        float cx = (sf.x + sh2.x) * 0.5F;
        float cy = (sf.y + sh2.y) * 0.5F;
        float half_w = height * PLAYER_BOX_WIDTH_RATIO * 0.5F;
        float half_h = height * 0.5F;

        constexpr float box_half_width = 0.35F, box_half_depth = 0.35F;
        const Vec3 world_corners[8] = {
            {body_x - box_half_width, body_bottom.y, body_z - box_half_depth},
            {body_x + box_half_width, body_bottom.y, body_z - box_half_depth},
            {body_x + box_half_width, body_bottom.y, body_z + box_half_depth},
            {body_x - box_half_width, body_bottom.y, body_z + box_half_depth},
            {body_x - box_half_width, body_top.y, body_z - box_half_depth},
            {body_x + box_half_width, body_top.y, body_z - box_half_depth},
            {body_x + box_half_width, body_top.y, body_z + box_half_depth},
            {body_x - box_half_width, body_top.y, body_z + box_half_depth}
        };

        EspBox box{};
        box.x1 = cx - half_w; box.y1 = cy - half_h;
        box.x2 = cx + half_w; box.y2 = cy + half_h;
        box.distance = distance;
        box.source = s_transforms[i];
        box.feet = body_bottom;
        box.head = have_skeleton ? skeleton[ESP_BONE_HEAD]
                                 : Vec3{body_x, body_top.y, body_z};
        box.vel = vel;
        box.speed = sqrtf(vel.x * vel.x + vel.z * vel.z);
        box.crouched = crouched;
        box.skeleton_valid = have_skeleton;

        if (have_skeleton) {
            float min_x = INFINITY, min_y = INFINITY, max_x = -INFINITY, max_y = -INFINITY;
            int projected_count = 0;
            for (int role = 0; role < ESP_BONE_COUNT; ++role) {
                box.bones[role] = skeleton[(size_t)role];
                Vec2 screen{};
                bool projected = project_world(skeleton[(size_t)role], screen);
                box.bone_visible[role] = projected;
                box.bone_screen[role][0] = projected ? screen.x : -1.f;
                box.bone_screen[role][1] = projected ? screen.y : -1.f;
                if (projected) {
                    min_x = std::min(min_x, screen.x); max_x = std::max(max_x, screen.x);
                    min_y = std::min(min_y, screen.y); max_y = std::max(max_y, screen.y);
                    ++projected_count;
                }
            }
            // Make the regular 2D ESP share the animated skeleton bounds. This
            // removes the old fixed-height box lag and keeps it tight on crouch.
            if (projected_count >= 8 && std::isfinite(min_x) && std::isfinite(min_y) &&
                max_x > min_x && max_y > min_y) {
                float animated_h = max_y - min_y;
                float margin_x = std::max(2.f, animated_h * 0.055f);
                float margin_y = std::max(2.f, animated_h * 0.035f);
                box.x1 = min_x - margin_x; box.x2 = max_x + margin_x;
                box.y1 = min_y - margin_y; box.y2 = max_y + margin_y;
            }
        }

        read_player_labels(s_transforms[i], box);

        // Screen-space velocity of the animated head: project head and
        // head+velocity so aim feed-forward uses the same target as ESP.
        box.aim_vx = 0.f;
        box.aim_vy = 0.f;
        if (!transform_camera_mode) {
            Vec3 h = box.head;
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
            bool projected = project_world(world_corners[corner], sc);
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


