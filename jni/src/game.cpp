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
#include <functional>
#include <iterator>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <unistd.h>
#include <sys/syscall.h>

static ssize_t pvm_readv(pid_t pid, const struct iovec* local_iov, unsigned long liovcnt, const struct iovec* remote_iov, unsigned long riovcnt, unsigned long flags) {
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
static std::unordered_set<uint64_t> g_scene_players;

struct PlayerTrack {
    Vec3 pos;
    double t;
    Vec3 vel;
};
static std::unordered_map<uint64_t, PlayerTrack> g_player_track;

struct TransformHierarchyLayout {
    uint64_t data_offset = 0x38;
    uint64_t index_offset = 0x40;
    uint64_t matrices_offset = 0x18;
    uint64_t indices_offset = 0x20;
    bool matrices_indirect = false;
    bool indices_indirect = false;
    uint32_t stride = 48;
    bool world_direct = false;
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
    pvm_readv(g_pid, &lv, 1, &rv, 1, 0);
    return v;
}
template<typename T>
static bool rd_exact(uint64_t addr, T& value) {
    value = {};
    if (!addr) return false;
    struct iovec local = {&value, sizeof(T)};
    struct iovec remote = {(void*)addr, sizeof(T)};
    return pvm_readv(g_pid, &local, 1, &remote, 1, 0) == (ssize_t)sizeof(T);
}
static uint64_t rd_ptr(uint64_t a) { return rd<uint64_t>(a); }
static Vec3     rd_v3 (uint64_t a) { return rd<Vec3>(a);     }
static Mat4     rd_m4 (uint64_t a) { return rd<Mat4>(a);     }

static std::string read_remote_string(uint64_t address) {
    if (!address) return {};
    char buffer[96]{};
    struct iovec local = {buffer, sizeof(buffer) - 1};
    struct iovec remote = {(void*)address, sizeof(buffer) - 1};
    ssize_t count = pvm_readv(g_pid, &local, 1, &remote, 1, 0);
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

static bool likely_native_pointer(uint64_t value) {
    return value >= 0x10000 && value < 0x0001000000000000ULL && (value & 0x7) == 0;
}

static float vec_distance(const Vec3& first, const Vec3& second) {
    float dx = first.x - second.x, dy = first.y - second.y, dz = first.z - second.z;
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

// ---------------------------------------------------------------------------
// Per-layout transform readers.
// ---------------------------------------------------------------------------
static bool read_layout_entry(uint64_t matrices, int32_t index, uint32_t stride,
                              Vec3& pos, Vec4& rot, Vec3& scale) {
    uint64_t base = matrices + (uint64_t)index * (uint64_t)stride;
    if (stride >= 48) {
        Matrix34 m{};
        if (!rd_exact(base, m) || !matrix34_is_valid(m)) return false;
        pos = {m.translation.x, m.translation.y, m.translation.z};
        rot = m.rotation;
        scale = {m.scale.x, m.scale.y, m.scale.z};
    } else {
        if (!rd_exact(base, pos) || !vec3_is_finite(pos)) return false;
        if (!rd_exact(base + 12, rot)) return false;
        if (!rd_exact(base + 28, scale)) return false;
    }
    return vec3_is_finite(pos);
}

static bool read_world_from_arrays(uint64_t matrices, uint64_t indices, int32_t index,
                                   const TransformHierarchyLayout& L, Vec3& position, Vec4* world_rotation = nullptr) {
    if (!likely_native_pointer(matrices) || index < 0 || index > 100000) return false;
    if (L.world_direct) {
        Vec3 pos{};
        if (!rd_exact(matrices + (uint64_t)index * (uint64_t)L.stride, pos) || !vec3_is_finite(pos)) return false;
        position = pos;
        if (world_rotation) *world_rotation = {0, 0, 0, 1};
        return true;
    }
    Vec3 pos{}, cpos{}, cscale{};
    Vec4 crot{};
    if (!read_layout_entry(matrices, index, L.stride, pos, crot, cscale)) return false;
    Vec3 result = pos;
    Vec4 result_rot = crot;
    int32_t parent = -2, previous = index;
    if (!likely_native_pointer(indices) || !rd_exact(indices + (uint64_t)index * sizeof(int32_t), parent)) return false;
    int depth = 0;
    while (parent >= 0 && depth++ < 128) {
        if (parent > 100000 || parent == previous) return false;
        if (!read_layout_entry(matrices, parent, L.stride, cpos, crot, cscale)) return false;
        Vec3 scaled = {result.x * cscale.x, result.y * cscale.y, result.z * cscale.z};
        Vec3 rotated = rotate_vector(crot, scaled);
        result = {cpos.x + rotated.x, cpos.y + rotated.y, cpos.z + rotated.z};
        result_rot = multiply_quaternion(crot, result_rot);
        if (!vec3_is_finite(result)) return false;
        previous = parent;
        if (!rd_exact(indices + (uint64_t)parent * sizeof(int32_t), parent)) return false;
    }
    if (parent != -1 || !vec3_is_finite(result)) return false;
    if (world_rotation) {
        normalize_quaternion(result_rot);
        *world_rotation = result_rot;
    }
    position = result;
    return true;
}

static bool read_transform_world_skel(uint64_t native_transform, const TransformHierarchyLayout& L, Vec3& position) {
    if (!native_transform) return false;
    uint64_t data = rd_ptr(native_transform + L.data_offset);
    int32_t index = rd<int32_t>(native_transform + L.index_offset);
    if (!likely_native_pointer(data) || index < 0 || index > 100000) return false;
    uint64_t matrices = rd_ptr(data + L.matrices_offset);
    if (L.matrices_indirect) matrices = rd_ptr(matrices);
    uint64_t indices = rd_ptr(data + L.indices_offset);
    if (L.indices_indirect) indices = rd_ptr(indices);
    return read_world_from_arrays(matrices, indices, index, L, position);
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
    return read_world_from_arrays(matrices, indices, transform_index, layout, position, world_rotation);
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
                TransformHierarchyLayout L{};
                L.data_offset = 0x38; L.index_offset = 0x40;
                L.matrices_offset = offsets[0]; L.indices_offset = offsets[1];
                L.stride = 48;
                if (read_world_from_arrays(matrices, indices_ptr, transform_index, L, position)) return true;
            }
        }
    }
    return false;
}

static uint64_t resolve_player_native_transform(uint64_t player) {
    if (!player) return 0;
    return resolve_native_transform(rd_ptr(player + PLAYER_TRANSFORM));
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
    return pvm_readv(g_pid, &local, 1, &remote, 1, 0) == (ssize_t)size;
}

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

// ---------------------------------------------------------------------------
// Resolve KCC and related components for a player
// ---------------------------------------------------------------------------
static uint64_t resolve_kcc(uint64_t player) {
    if (!player) return 0;
    uint64_t reference = rd_ptr(player + PLAYER_KCC_REFERENCE);
    if (!reference) return 0;

    const uint64_t candidates[] = {
        rd_ptr(reference + 0x10), // InterfaceReference._component
        rd_ptr(reference + 0x18), // InterfaceReference.Cxa
        reference
    };
    for (uint64_t candidate : candidates) {
        if (!likely_native_pointer(candidate)) continue;
        // Verify candidate looks like a KCC
        uint64_t anim = rd_ptr(candidate + KCC_CHARACTER_ANIMATION);
        uint64_t hroot = rd_ptr(candidate + KCC_HITBOX_ROOT);
        uint64_t head = rd_ptr(candidate + KCC_HEAD_TRANSFORM);
        if (likely_native_pointer(anim) || likely_native_pointer(hroot) || likely_native_pointer(head)) {
            return candidate;
        }
    }
    return 0;
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

// ===========================================================================
// Animated skeleton ESP
// ===========================================================================
enum HumanBoneIndex {
    HB_HIPS = 0,
    HB_LEFT_UPPER_LEG = 1,  HB_RIGHT_UPPER_LEG = 2,
    HB_LEFT_LOWER_LEG = 3,  HB_RIGHT_LOWER_LEG = 4,
    HB_LEFT_FOOT = 5,       HB_RIGHT_FOOT = 6,
    HB_SPINE = 7,           HB_CHEST = 8,
    HB_NECK = 9,            HB_HEAD = 10,
    HB_LEFT_SHOULDER = 11,  HB_RIGHT_SHOULDER = 12,
    HB_LEFT_UPPER_ARM = 13, HB_RIGHT_UPPER_ARM = 14,
    HB_LEFT_LOWER_ARM = 15, HB_RIGHT_LOWER_ARM = 16,
    HB_LEFT_HAND = 17,      HB_RIGHT_HAND = 18,
    HB_UPPER_CHEST = 54
};

struct BoneRoleMapping { EspBone role; int first; int second; };
static constexpr BoneRoleMapping kBoneRoleMap[] = {
    {ESP_BONE_HEAD,           HB_HEAD,            -1},
    {ESP_BONE_NECK,           HB_NECK,            HB_HEAD},
    {ESP_BONE_CHEST,          HB_UPPER_CHEST,     HB_CHEST},
    {ESP_BONE_PELVIS,         HB_HIPS,            -1},
    {ESP_BONE_LEFT_SHOULDER,  HB_LEFT_SHOULDER,   HB_LEFT_UPPER_ARM},
    {ESP_BONE_LEFT_ELBOW,     HB_LEFT_LOWER_ARM,  -1},
    {ESP_BONE_LEFT_HAND,      HB_LEFT_HAND,       -1},
    {ESP_BONE_RIGHT_SHOULDER, HB_RIGHT_SHOULDER,  HB_RIGHT_UPPER_ARM},
    {ESP_BONE_RIGHT_ELBOW,    HB_RIGHT_LOWER_ARM, -1},
    {ESP_BONE_RIGHT_HAND,     HB_RIGHT_HAND,      -1},
    {ESP_BONE_LEFT_HIP,       HB_LEFT_UPPER_LEG,  -1},
    {ESP_BONE_LEFT_KNEE,      HB_LEFT_LOWER_LEG,  -1},
    {ESP_BONE_LEFT_FOOT,      HB_LEFT_FOOT,       -1},
    {ESP_BONE_RIGHT_HIP,      HB_RIGHT_UPPER_LEG, -1},
    {ESP_BONE_RIGHT_KNEE,     HB_RIGHT_LOWER_LEG, -1},
    {ESP_BONE_RIGHT_FOOT,     HB_RIGHT_FOOT,      -1}
};

static bool g_skeleton_enabled_from_ui = false;
void esp_set_skeleton_enabled(bool enabled) { g_skeleton_enabled_from_ui = enabled; }

struct RigJoint {
    uint64_t hierarchy = 0;
    int32_t  index = -1;
    bool valid() const { return hierarchy != 0 && index >= 0; }
};

enum RigSource : int {
    RIG_SOURCE_NONE = 0,
    RIG_SOURCE_BONE_CACHE = 1,
    RIG_SOURCE_HITBOXES = 2,
    RIG_SOURCE_MODEL_INFO = 3
};

struct PlayerRig {
    std::array<RigJoint, ESP_BONE_COUNT> joints{};
    uint64_t signature = 0;
    double   resolved_at = 0.0;
    uint64_t head_anchor = 0;
    int      source = RIG_SOURCE_NONE;
    int      failures = 0;
};
static std::unordered_map<uint64_t, PlayerRig> g_player_rigs;
static TransformHierarchyLayout g_bone_layout{};
static bool g_bone_layout_valid = false;

struct LayoutSample { uint64_t transform; Vec3 world; };

static int score_bone_layout(const std::vector<LayoutSample>& samples, const TransformHierarchyLayout& L) {
    int matches = 0;
    for (const auto& s : samples) {
        Vec3 pos{};
        if (read_transform_world_skel(s.transform, L, pos) && vec_distance(pos, s.world) < 3.5f)
            ++matches;
    }
    return matches;
}

static bool calibrate_bone_layout(const std::vector<LayoutSample>& samples, TransformHierarchyLayout& out) {
    if (samples.empty()) return false;

    if (g_transform_hierarchy_layout_valid) {
        TransformHierarchyLayout L = g_transform_hierarchy_layout;
        L.stride = 48; L.world_direct = false;
        if (score_bone_layout(samples, L) >= (int)std::min<size_t>(2, samples.size())) {
            out = L;
            return true;
        }
        L.stride = 40;
        if (score_bone_layout(samples, L) >= (int)std::min<size_t>(2, samples.size())) {
            out = L;
            return true;
        }
    }

    const uint64_t data_offsets[] = {0x38, 0x28, 0x48, 0x30, 0x20, 0x40};
    const uint64_t matrix_offsets[] = {0x18, 0x08, 0x10, 0x20, 0x28, 0x30, 0x00};
    struct Probe { uint32_t stride; bool world_direct; };
    const Probe probes[] = { {48, false}, {40, false}, {16, true}, {12, true} };

    int best_matches = 0;
    TransformHierarchyLayout best{};
    for (uint64_t data_offset : data_offsets) {
        uint64_t index_offset = data_offset + 8;
        for (uint64_t matrices_offset : matrix_offsets) {
            uint64_t indices_offset = matrices_offset + 8;
            for (int indirect = 0; indirect < 2; ++indirect) {
                for (const Probe& p : probes) {
                    TransformHierarchyLayout L{};
                    L.data_offset = data_offset; L.index_offset = index_offset;
                    L.matrices_offset = matrices_offset; L.indices_offset = indices_offset;
                    L.matrices_indirect = indirect != 0; L.indices_indirect = indirect != 0;
                    L.stride = p.stride; L.world_direct = p.world_direct;
                    int matches = score_bone_layout(samples, L);
                    if (matches > best_matches) { best_matches = matches; best = L; }
                }
            }
        }
    }

    size_t needed = samples.size() >= 2 ? 2 : 1;
    if (best_matches < (int)needed) return false;
    out = best;
    return true;
}

static bool native_transform_is_valid(uint64_t native_transform) {
    if (!likely_native_pointer(native_transform)) return false;
    if (g_bone_layout_valid) {
        uint64_t data = rd_ptr(native_transform + g_bone_layout.data_offset);
        int32_t index = rd<int32_t>(native_transform + g_bone_layout.index_offset);
        return likely_native_pointer(data) && index >= 0 && index <= 100000;
    }
    for (const auto& offsets : {std::pair<uint64_t, uint64_t>{0x38, 0x40},
                                std::pair<uint64_t, uint64_t>{0x28, 0x30},
                                std::pair<uint64_t, uint64_t>{0x48, 0x50},
                                std::pair<uint64_t, uint64_t>{0x30, 0x38},
                                std::pair<uint64_t, uint64_t>{0x20, 0x28}}) {
        uint64_t transform_data = rd_ptr(native_transform + offsets.first);
        int32_t transform_index = rd<int32_t>(native_transform + offsets.second);
        if (likely_native_pointer(transform_data) && transform_index >= 0 && transform_index <= 100000)
            return true;
    }
    return false;
}

static uint64_t native_transform_from_component(uint64_t managed_component) {
    if (!likely_native_pointer(managed_component)) return 0;
    uint64_t native_component = rd_ptr(managed_component + MANAGED_CACHED_PTR);
    if (!likely_native_pointer(native_component)) return 0;
    uint64_t game_object = rd_ptr(native_component + NATIVE_COMPONENT_GAME_OBJECT);
    if (!likely_native_pointer(game_object)) return 0;

    for (uint64_t vector_offset : {NATIVE_GAME_OBJECT_COMPONENTS, NATIVE_GAME_OBJECT_COMPONENTS + 8}) {
        uint64_t components = rd_ptr(game_object + vector_offset);
        if (!likely_native_pointer(components)) continue;
        for (uint64_t entry_offset : {uint64_t(8), uint64_t(0), uint64_t(0x10)}) {
            uint64_t candidate = rd_ptr(components + entry_offset);
            if (native_transform_is_valid(candidate)) return candidate;
        }
    }
    return 0;
}

static uint64_t native_transform_from_game_object(uint64_t managed_game_object) {
    if (!likely_native_pointer(managed_game_object)) return 0;
    uint64_t game_object = rd_ptr(managed_game_object + MANAGED_CACHED_PTR);
    if (!likely_native_pointer(game_object)) return 0;
    for (uint64_t vector_offset : {NATIVE_GAME_OBJECT_COMPONENTS, NATIVE_GAME_OBJECT_COMPONENTS + 8}) {
        uint64_t components = rd_ptr(game_object + vector_offset);
        if (!likely_native_pointer(components)) continue;
        for (uint64_t entry_offset : {uint64_t(8), uint64_t(0), uint64_t(0x10)}) {
            uint64_t candidate = rd_ptr(components + entry_offset);
            if (native_transform_is_valid(candidate)) return candidate;
        }
    }
    return 0;
}

static uint64_t resolve_character_animation(uint64_t player) {
    uint64_t kcc = resolve_kcc(player);
    if (kcc) {
        uint64_t animation = rd_ptr(kcc + KCC_CHARACTER_ANIMATION);
        if (likely_native_pointer(animation)) return animation;
    }
    return 0;
}

static uint64_t resolve_player_model_info(uint64_t player) {
    uint64_t animation = resolve_character_animation(player);
    if (animation) {
        uint64_t model_info = rd_ptr(animation + CHARACTER_ANIMATION_MODEL_INFO);
        if (likely_native_pointer(model_info)) return model_info;
    }
    return 0;
}

static uint64_t resolve_model_head_transform(uint64_t player) {
    uint64_t model_info = resolve_player_model_info(player);
    if (model_info) {
        uint64_t head = resolve_native_transform(rd_ptr(model_info + MODEL_INFO_HEAD));
        if (native_transform_is_valid(head)) return head;
    }
    uint64_t kcc = resolve_kcc(player);
    if (kcc) {
        uint64_t head = resolve_native_transform(rd_ptr(kcc + KCC_HEAD_TRANSFORM));
        if (native_transform_is_valid(head)) return head;
    }
    return 0;
}

static uint64_t resolve_model_root_transform(uint64_t player) {
    uint64_t model_info = resolve_player_model_info(player);
    if (model_info) {
        uint64_t body = resolve_native_transform(rd_ptr(model_info + MODEL_INFO_BODY));
        if (native_transform_is_valid(body)) return body;
    }
    uint64_t model = native_transform_from_game_object(rd_ptr(player + PLAYER_CHARACTER_MODEL));
    if (native_transform_is_valid(model)) return model;
    return resolve_player_native_transform(player);
}

// ---------------------------------------------------------------------------
// Per-frame hierarchy snapshot
// ---------------------------------------------------------------------------
struct HierarchyWorldTransform {
    Vec3 position{};
    Vec4 rotation{};
    Vec3 scale{1.f, 1.f, 1.f};
};

struct HierarchySnapshot {
    std::vector<Matrix34> locals;
    std::vector<int32_t> parents;
    std::unordered_map<int32_t, HierarchyWorldTransform> world;
    uint32_t stride = 48;
    bool world_direct = false;
};
static std::unordered_map<uint64_t, HierarchySnapshot> g_hierarchy_frame;

static void clear_hierarchy_frame_cache() { g_hierarchy_frame.clear(); }

static bool hierarchy_arrays_of(uint64_t hierarchy, uint64_t& matrices, uint64_t& indices) {
    if (!likely_native_pointer(hierarchy)) return false;
    TransformHierarchyLayout L = g_bone_layout_valid ? g_bone_layout : TransformHierarchyLayout{};
    matrices = rd_ptr(hierarchy + L.matrices_offset);
    if (L.matrices_indirect) matrices = rd_ptr(matrices);
    indices = rd_ptr(hierarchy + L.indices_offset);
    if (L.indices_indirect) indices = rd_ptr(indices);
    if (likely_native_pointer(matrices) && likely_native_pointer(indices)) return true;

    const uint64_t cand_offsets[][2] = {{0x18, 0x20}, {0x08, 0x10}, {0x10, 0x18}, {0x20, 0x28}, {0x28, 0x30}, {0x00, 0x08}};
    for (const auto& pair : cand_offsets) {
        uint64_t m = rd_ptr(hierarchy + pair[0]);
        uint64_t ind = rd_ptr(hierarchy + pair[1]);
        if (likely_native_pointer(m) && likely_native_pointer(ind)) {
            matrices = m; indices = ind; return true;
        }
        if (likely_native_pointer(rd_ptr(m)) && likely_native_pointer(rd_ptr(ind))) {
            matrices = rd_ptr(m); indices = rd_ptr(ind); return true;
        }
    }
    return false;
}

static HierarchySnapshot* prepare_hierarchy_snapshot(uint64_t hierarchy, int32_t max_index) {
    if (!likely_native_pointer(hierarchy) || max_index < 0 || max_index > 4096) return nullptr;
    uint32_t stride = g_bone_layout_valid ? (g_bone_layout.stride ? g_bone_layout.stride : 48) : 48;
    bool world_direct = g_bone_layout_valid ? g_bone_layout.world_direct : false;
    auto existing = g_hierarchy_frame.find(hierarchy);
    if (existing != g_hierarchy_frame.end() &&
        (int32_t)existing->second.locals.size() > max_index &&
        existing->second.stride == stride &&
        existing->second.world_direct == world_direct) {
        return &existing->second;
    }
    if (existing != g_hierarchy_frame.end()) g_hierarchy_frame.erase(existing);

    uint64_t matrices = 0, indices = 0;
    if (!hierarchy_arrays_of(hierarchy, matrices, indices)) return nullptr;
    size_t count = (size_t)max_index + 1;
    HierarchySnapshot snapshot;
    snapshot.stride = stride;
    snapshot.world_direct = world_direct;
    snapshot.locals.resize(count);

    std::vector<uint8_t> raw(count * stride);
    if (!read_remote_bytes(matrices, raw.data(), raw.size())) return nullptr;
    for (size_t i = 0; i < count; ++i) {
        const uint8_t* base = raw.data() + i * stride;
        if (world_direct) {
            Vec3 pos{};
            memcpy(&pos, base, sizeof(Vec3));
            if (!vec3_is_finite(pos)) continue;
            snapshot.locals[i].translation = {pos.x, pos.y, pos.z, 1.f};
            snapshot.locals[i].rotation = {0, 0, 0, 1};
            snapshot.locals[i].scale = {1, 1, 1, 1};
        } else if (stride >= 48) {
            Matrix34 m{};
            memcpy(&m, base, sizeof(Matrix34));
            if (!matrix34_is_valid(m)) continue;
            snapshot.locals[i] = m;
        } else {
            Vec3 pos{}, scale{1, 1, 1};
            Vec4 rot{0, 0, 0, 1};
            memcpy(&pos, base, sizeof(Vec3));
            memcpy(&rot, base + 12, sizeof(Vec4));
            memcpy(&scale, base + 28, sizeof(Vec3));
            if (!vec3_is_finite(pos)) continue;
            snapshot.locals[i].translation = {pos.x, pos.y, pos.z, 1.f};
            snapshot.locals[i].rotation = rot;
            snapshot.locals[i].scale = {scale.x, scale.y, scale.z, 1.f};
        }
    }
    snapshot.parents.resize(count);
    if (!read_remote_bytes(indices, snapshot.parents.data(), count * sizeof(int32_t)))
        return nullptr;
    auto inserted = g_hierarchy_frame.emplace(hierarchy, std::move(snapshot));
    return &inserted.first->second;
}

static bool snapshot_world_transform(HierarchySnapshot& snapshot, int32_t index,
                                     HierarchyWorldTransform& result) {
    if (index < 0 || index >= (int32_t)snapshot.locals.size()) return false;
    auto cached = snapshot.world.find(index);
    if (cached != snapshot.world.end()) { result = cached->second; return true; }

    if (snapshot.world_direct) {
        const Matrix34& m = snapshot.locals[(size_t)index];
        if (m.translation.w == 0.f) return false;
        result.position = {m.translation.x, m.translation.y, m.translation.z};
        if (!vec3_is_finite(result.position)) return false;
        snapshot.world[index] = result;
        return true;
    }

    std::vector<int32_t> chain;
    int32_t current = index;
    HierarchyWorldTransform base{};
    bool have_base = false;
    for (int guard = 0; guard < 256; ++guard) {
        if (current < 0 || current >= (int32_t)snapshot.parents.size()) return false;
        auto found = snapshot.world.find(current);
        if (found != snapshot.world.end()) { base = found->second; have_base = true; break; }
        chain.push_back(current);
        int32_t parent = snapshot.parents[(size_t)current];
        if (parent == -1) break;
        if (parent < 0 || parent == current) return false;
        current = parent;
    }
    if (chain.empty()) return have_base ? (result = base, true) : false;

    HierarchyWorldTransform accumulated = base;
    if (!have_base) {
        const Matrix34& root = snapshot.locals[(size_t)chain.back()];
        if (!matrix34_is_valid(root)) return false;
        accumulated.position = {root.translation.x, root.translation.y, root.translation.z};
        accumulated.rotation = root.rotation;
        accumulated.scale = {root.scale.x, root.scale.y, root.scale.z};
        snapshot.world[chain.back()] = accumulated;
        chain.pop_back();
    }
    for (size_t step = chain.size(); step-- > 0;) {
        const Matrix34& local = snapshot.locals[(size_t)chain[step]];
        if (!matrix34_is_valid(local)) return false;
        Vec3 scaled = {local.translation.x * accumulated.scale.x,
                       local.translation.y * accumulated.scale.y,
                       local.translation.z * accumulated.scale.z};
        Vec3 rotated = rotate_vector(accumulated.rotation, scaled);
        HierarchyWorldTransform next{};
        next.position = {accumulated.position.x + rotated.x,
                         accumulated.position.y + rotated.y,
                         accumulated.position.z + rotated.z};
        next.rotation = multiply_quaternion(accumulated.rotation, local.rotation);
        next.scale = {accumulated.scale.x * local.scale.x,
                      accumulated.scale.y * local.scale.y,
                      accumulated.scale.z * local.scale.z};
        if (!vec3_is_finite(next.position)) return false;
        accumulated = next;
        snapshot.world[chain[step]] = accumulated;
    }
    result = accumulated;
    return true;
}

static bool joint_from_transform(uint64_t native_transform, RigJoint& joint) {
    if (!likely_native_pointer(native_transform)) return false;
    if (g_bone_layout_valid) {
        uint64_t hierarchy = rd_ptr(native_transform + g_bone_layout.data_offset);
        int32_t index = rd<int32_t>(native_transform + g_bone_layout.index_offset);
        if (likely_native_pointer(hierarchy) && index >= 0 && index <= 100000) {
            joint.hierarchy = hierarchy;
            joint.index = index;
            return true;
        }
    }
    for (const auto& offsets : {std::pair<uint64_t, uint64_t>{0x38, 0x40},
                                std::pair<uint64_t, uint64_t>{0x28, 0x30},
                                std::pair<uint64_t, uint64_t>{0x48, 0x50},
                                std::pair<uint64_t, uint64_t>{0x30, 0x38},
                                std::pair<uint64_t, uint64_t>{0x20, 0x28}}) {
        uint64_t hierarchy = rd_ptr(native_transform + offsets.first);
        int32_t index = rd<int32_t>(native_transform + offsets.second);
        if (likely_native_pointer(hierarchy) && index >= 0 && index <= 100000) {
            joint.hierarchy = hierarchy;
            joint.index = index;
            return true;
        }
    }
    return false;
}

static bool joint_position(const RigJoint& joint, Vec3& position) {
    if (!joint.valid()) return false;
    auto snapshot = g_hierarchy_frame.find(joint.hierarchy);
    if (snapshot != g_hierarchy_frame.end() &&
        joint.index < (int32_t)snapshot->second.locals.size()) {
        HierarchyWorldTransform world{};
        if (snapshot_world_transform(snapshot->second, joint.index, world)) {
            position = world.position;
            return vec3_is_finite(position);
        }
    }
    uint64_t matrices = 0, indices = 0;
    if (!hierarchy_arrays_of(joint.hierarchy, matrices, indices)) return false;
    TransformHierarchyLayout L = g_bone_layout_valid ? g_bone_layout : TransformHierarchyLayout{};
    return read_world_from_arrays(matrices, indices, joint.index, L, position);
}

static bool joint_parent(const RigJoint& joint, RigJoint& parent) {
    uint64_t matrices = 0, indices = 0;
    if (!joint.valid() || !hierarchy_arrays_of(joint.hierarchy, matrices, indices)) return false;
    int32_t parent_index = -1;
    if (!rd_exact(indices + (uint64_t)joint.index * sizeof(int32_t), parent_index)) return false;
    if (parent_index < 0 || parent_index > 100000) return false;
    parent.hierarchy = joint.hierarchy;
    parent.index = parent_index;
    return true;
}

static void joint_chain(const RigJoint& start, const Vec3& start_position,
                        std::vector<RigJoint>& chain, std::vector<Vec3>& positions,
                        size_t max_links) {
    RigJoint current = start;
    Vec3 previous = start_position;
    for (int step = 0; step < 12 && chain.size() < max_links; ++step) {
        RigJoint parent{};
        if (!joint_parent(current, parent)) break;
        Vec3 position{};
        if (!joint_position(parent, position) || !vec3_is_finite(position)) break;
        current = parent;
        if (vec_distance(position, previous) < 0.035f) continue;
        chain.push_back(parent);
        positions.push_back(position);
        previous = position;
    }
}

// ---------------------------------------------------------------------------
// Primary rig: humanoid bone cache (CharacterAnimation.pvi / tk)
// ---------------------------------------------------------------------------
static int fill_rig_from_cache(uint64_t transforms, int32_t count,
                               const std::function<int(int)>& slot_for_human_bone,
                               PlayerRig& rig) {
    rig.joints.fill(RigJoint{});
    auto native_transform_at_slot = [&](int slot) -> uint64_t {
        if (slot < 0 || slot >= count) return 0;
        uint64_t managed = rd_ptr(transforms + IL2CPP_ARRAY_FIRST_ELEMENT + (uint64_t)slot * sizeof(uint64_t));
        uint64_t native = resolve_native_transform(managed);
        return native_transform_is_valid(native) ? native : 0;
    };

    int resolved = 0;
    uint64_t majority_hierarchy = 0;
    for (const BoneRoleMapping& entry : kBoneRoleMap) {
        uint64_t native = native_transform_at_slot(slot_for_human_bone(entry.first));
        if (!native && entry.second >= 0)
            native = native_transform_at_slot(slot_for_human_bone(entry.second));
        if (!native) continue;
        RigJoint joint{};
        if (!joint_from_transform(native, joint)) continue;
        if (!majority_hierarchy) majority_hierarchy = joint.hierarchy;
        rig.joints[(size_t)entry.role] = joint;
        ++resolved;
    }
    if (resolved == 0 || !majority_hierarchy) return 0;

    int kept = 0;
    for (RigJoint& joint : rig.joints) {
        if (!joint.valid()) continue;
        if (joint.hierarchy != majority_hierarchy) { joint = RigJoint{}; continue; }
        ++kept;
    }
    return kept;
}

static bool rig_pose_plausible(const PlayerRig& rig, const Vec3& center_pos) {
    Vec3 head{}, pelvis{};
    int valid = 0;
    for (const RigJoint& joint : rig.joints) {
        if (!joint.valid()) continue;
        Vec3 position{};
        if (!joint_position(joint, position)) return false;
        if (vec_distance(position, center_pos) > 6.5f) return false;
        ++valid;
    }
    if (valid < 4) return false;
    const RigJoint& head_joint = rig.joints[ESP_BONE_HEAD];
    const RigJoint& pelvis_joint = rig.joints[ESP_BONE_PELVIS];
    if (head_joint.valid() && pelvis_joint.valid()) {
        if (joint_position(head_joint, head) && joint_position(pelvis_joint, pelvis)) {
            float height = head.y - pelvis.y;
            if (height < 0.15f || height > 2.5f) return false;
        }
    }
    return true;
}

static bool build_bone_cache_rig(uint64_t player, const Vec3& center_pos, PlayerRig& rig) {
    uint64_t animation = resolve_character_animation(player);
    if (!likely_native_pointer(animation)) return false;
    uint64_t cache = rd_ptr(animation + CHARACTER_ANIMATION_BONE_CACHE);
    if (!likely_native_pointer(cache)) return false;
    uint64_t transforms = rd_ptr(cache + BONE_CACHE_TRANSFORMS);
    if (!likely_native_pointer(transforms)) return false;

    int32_t count = rd<int32_t>(transforms + IL2CPP_LIST_SIZE);
    if (count <= 0 || count > 128) return false;

    uint64_t head_anchor = rig.head_anchor ? rig.head_anchor : resolve_model_head_transform(player);

    std::vector<int32_t> map;
    uint64_t mapping = rd_ptr(cache + BONE_CACHE_MAPPING);
    int32_t map_count = likely_native_pointer(mapping) ? rd<int32_t>(mapping + IL2CPP_LIST_SIZE) : 0;
    if (map_count > 0 && map_count <= 128) {
        map.resize((size_t)map_count);
        if (!read_remote_bytes(mapping + IL2CPP_ARRAY_FIRST_ELEMENT, map.data(), map.size() * sizeof(int32_t)))
            map.clear();
    }

    auto resolver_direct = [&](int human_bone) -> int { return human_bone; };
    auto resolver_map_forward = [&](int human_bone) -> int {
        if (human_bone >= 0 && human_bone < map_count) return map[(size_t)human_bone];
        return -1;
    };
    auto resolver_map_reverse = [&](int human_bone) -> int {
        for (int32_t slot = 0; slot < map_count && slot < count; ++slot)
            if (map[(size_t)slot] == human_bone) return (int)slot;
        return -1;
    };
    const std::function<int(int)> resolvers[] = {resolver_direct, resolver_map_forward, resolver_map_reverse};

    for (const auto& resolver : resolvers) {
        PlayerRig candidate{};
        candidate.head_anchor = head_anchor;
        candidate.source = RIG_SOURCE_BONE_CACHE;
        candidate.signature = rig.signature;
        candidate.resolved_at = rig.resolved_at;

        int kept = fill_rig_from_cache(transforms, count, resolver, candidate);
        if (kept < 4) continue;

        const RigJoint& head_joint = candidate.joints[ESP_BONE_HEAD];
        if (!head_joint.valid()) continue;

        bool accepted = false;
        if (head_anchor && g_bone_layout_valid) {
            uint64_t anchor_hierarchy = rd_ptr(head_anchor + g_bone_layout.data_offset);
            int32_t anchor_index = rd<int32_t>(head_anchor + g_bone_layout.index_offset);
            accepted = (anchor_hierarchy == head_joint.hierarchy && anchor_index == head_joint.index);
        }
        if (!accepted) accepted = rig_pose_plausible(candidate, center_pos);
        if (!accepted) continue;

        candidate.resolved_at = rig.resolved_at;
        rig = candidate;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Fallback rig: HitBox components on character bones
// ---------------------------------------------------------------------------
struct HitboxSample {
    RigJoint joint{};
    int area = 0;
    Vec3 position{};
};

static bool collect_player_hitboxes(uint64_t player, std::vector<HitboxSample>& samples) {
    uint64_t kcc = resolve_kcc(player);
    if (!kcc) return false;
    uint64_t root = rd_ptr(kcc + KCC_HITBOX_ROOT);
    if (!likely_native_pointer(root)) return false;
    uint64_t boxes = rd_ptr(root + HITBOX_ROOT_BOXES);
    if (!likely_native_pointer(boxes)) return false;
    int32_t count = rd<int32_t>(boxes + IL2CPP_LIST_SIZE);
    if (count <= 0 || count > 64) return false;

    for (int32_t index = 0; index < count; ++index) {
        uint64_t hitbox = rd_ptr(boxes + IL2CPP_ARRAY_FIRST_ELEMENT + (uint64_t)index * sizeof(uint64_t));
        if (!likely_native_pointer(hitbox)) continue;
        int32_t area = rd<int32_t>(hitbox + HITBOX_HIT_AREA);
        if (area < HIT_AREA_HEAD || area > HIT_AREA_HAND) continue;
        HitboxSample sample{};
        sample.area = (int)area;
        uint64_t native = native_transform_from_component(hitbox);
        if (!joint_from_transform(native, sample.joint)) continue;
        if (!joint_position(sample.joint, sample.position) || !vec3_is_finite(sample.position)) continue;
        samples.push_back(sample);
    }
    return samples.size() >= 3;
}

struct LimbLinks {
    RigJoint links[6];
    Vec3 positions[6];
    int count = 0;
};

static LimbLinks limb_links_of(const HitboxSample& sample) {
    std::vector<RigJoint> chain;
    std::vector<Vec3> positions;
    joint_chain(sample.joint, sample.position, chain, positions, 6);
    LimbLinks links{};
    links.count = (int)std::min<size_t>(chain.size(), 6);
    for (int i = 0; i < links.count; ++i) {
        links.links[i] = chain[(size_t)i];
        links.positions[i] = positions[(size_t)i];
    }
    return links;
}

static bool build_hitbox_rig(uint64_t player, const Vec3& center_pos, PlayerRig& rig) {
    std::vector<HitboxSample> samples;
    if (!collect_player_hitboxes(player, samples)) return false;

    samples.erase(std::remove_if(samples.begin(), samples.end(), [&](const HitboxSample& sample) {
        return vec_distance(sample.position, center_pos) > 4.5f;
    }), samples.end());
    if (samples.size() < 3) return false;

    rig.joints.fill(RigJoint{});
    auto assign = [&](EspBone role, const RigJoint& joint) {
        if (joint.valid() && !rig.joints[(size_t)role].valid())
            rig.joints[(size_t)role] = joint;
    };

    const HitboxSample* head_sample = nullptr;
    for (const auto& sample : samples)
        if (sample.area == HIT_AREA_HEAD && (!head_sample || sample.position.y > head_sample->position.y))
            head_sample = &sample;
    if (head_sample) {
        assign(ESP_BONE_HEAD, head_sample->joint);
        LimbLinks links = limb_links_of(*head_sample);
        if (links.count > 0) assign(ESP_BONE_NECK, links.links[0]);
        if (links.count > 1) assign(ESP_BONE_CHEST, links.links[1]);
        if (links.count > 2 && !rig.joints[ESP_BONE_CHEST].valid()) assign(ESP_BONE_CHEST, links.links[2]);
    }

    for (const auto& sample : samples) {
        if (sample.area != HIT_AREA_CHEST) continue;
        assign(ESP_BONE_CHEST, sample.joint);
        LimbLinks links = limb_links_of(sample);
        if (links.count > 1) assign(ESP_BONE_PELVIS, links.links[1]);
        else if (links.count > 0) assign(ESP_BONE_PELVIS, links.links[0]);
    }

    std::vector<const HitboxSample*> leg_ends;
    for (const auto& sample : samples) if (sample.area == HIT_AREA_FOOT) leg_ends.push_back(&sample);
    if (leg_ends.size() < 2)
        for (const auto& sample : samples) if (sample.area == HIT_AREA_LEG) leg_ends.push_back(&sample);
    std::sort(leg_ends.begin(), leg_ends.end(), [](const HitboxSample* a, const HitboxSample* b) {
        return a->position.y < b->position.y;
    });
    if (leg_ends.size() > 2) leg_ends.erase(leg_ends.begin() + 2, leg_ends.end());
    const EspBone foot_roles[2] = {ESP_BONE_LEFT_FOOT, ESP_BONE_RIGHT_FOOT};
    const EspBone knee_roles[2] = {ESP_BONE_LEFT_KNEE, ESP_BONE_RIGHT_KNEE};
    const EspBone hip_roles[2]  = {ESP_BONE_LEFT_HIP,  ESP_BONE_RIGHT_HIP};
    for (size_t side = 0; side < leg_ends.size(); ++side) {
        assign(foot_roles[side], leg_ends[side]->joint);
        LimbLinks links = limb_links_of(*leg_ends[side]);
        if (links.count > 0) assign(knee_roles[side], links.links[0]);
        if (links.count > 1) assign(hip_roles[side], links.links[1]);
        if (links.count > 2 && !rig.joints[ESP_BONE_PELVIS].valid()) assign(ESP_BONE_PELVIS, links.links[2]);
    }

    std::vector<const HitboxSample*> hands;
    for (const auto& sample : samples) if (sample.area == HIT_AREA_HAND) hands.push_back(&sample);
    if (hands.size() > 2) hands.resize(2);
    const EspBone hand_roles[2]     = {ESP_BONE_LEFT_HAND, ESP_BONE_RIGHT_HAND};
    const EspBone elbow_roles[2]    = {ESP_BONE_LEFT_ELBOW, ESP_BONE_RIGHT_ELBOW};
    const EspBone shoulder_roles[2] = {ESP_BONE_LEFT_SHOULDER, ESP_BONE_RIGHT_SHOULDER};
    for (size_t side = 0; side < hands.size(); ++side) {
        assign(hand_roles[side], hands[side]->joint);
        LimbLinks links = limb_links_of(*hands[side]);
        if (links.count > 0) assign(elbow_roles[side], links.links[0]);
        if (links.count > 1) assign(shoulder_roles[side], links.links[1]);
    }

    int resolved = 0;
    for (const RigJoint& joint : rig.joints) if (joint.valid()) ++resolved;
    bool torso = rig.joints[ESP_BONE_HEAD].valid() || rig.joints[ESP_BONE_CHEST].valid() || rig.joints[ESP_BONE_PELVIS].valid();
    if (resolved < 4 || !torso) { rig.joints.fill(RigJoint{}); return false; }
    rig.source = RIG_SOURCE_HITBOXES;
    return true;
}

static PlayerRig resolve_player_rig(uint64_t player, const Vec3& center_pos) {
    PlayerRig rig{};
    rig.resolved_at = monotonic_seconds();
    rig.head_anchor = resolve_model_head_transform(player);
    rig.signature = rd_ptr(player + PLAYER_CHARACTER_MODEL);

    rig.joints.fill(RigJoint{});
    if (build_bone_cache_rig(player, center_pos, rig)) return rig;

    PlayerRig hitbox_rig = rig;
    if (build_hitbox_rig(player, center_pos, hitbox_rig) && rig_pose_plausible(hitbox_rig, center_pos))
        return hitbox_rig;

    rig.joints.fill(RigJoint{});
    rig.source = RIG_SOURCE_NONE;
    return rig;
}

static void complete_missing_bones(std::array<Vec3, ESP_BONE_COUNT>& bones,
                                   std::array<bool, ESP_BONE_COUNT>& valid) {
    if (valid[ESP_BONE_HEAD] && valid[ESP_BONE_CHEST] && !valid[ESP_BONE_NECK]) {
        bones[ESP_BONE_NECK] = {
            (bones[ESP_BONE_HEAD].x + bones[ESP_BONE_CHEST].x) * 0.5f,
            (bones[ESP_BONE_HEAD].y + bones[ESP_BONE_CHEST].y) * 0.5f,
            (bones[ESP_BONE_HEAD].z + bones[ESP_BONE_CHEST].z) * 0.5f
        };
        valid[ESP_BONE_NECK] = true;
    }
    if (valid[ESP_BONE_NECK] && valid[ESP_BONE_CHEST] && !valid[ESP_BONE_HEAD]) {
        bones[ESP_BONE_HEAD] = {
            bones[ESP_BONE_NECK].x + (bones[ESP_BONE_NECK].x - bones[ESP_BONE_CHEST].x) * 0.8f,
            bones[ESP_BONE_NECK].y + (bones[ESP_BONE_NECK].y - bones[ESP_BONE_CHEST].y) * 0.8f,
            bones[ESP_BONE_NECK].z + (bones[ESP_BONE_NECK].z - bones[ESP_BONE_CHEST].z) * 0.8f
        };
        valid[ESP_BONE_HEAD] = true;
    }
    if (!valid[ESP_BONE_CHEST] && valid[ESP_BONE_PELVIS]) {
        Vec3 upper = valid[ESP_BONE_NECK] ? bones[ESP_BONE_NECK] : bones[ESP_BONE_HEAD];
        if (valid[ESP_BONE_NECK] || valid[ESP_BONE_HEAD]) {
            bones[ESP_BONE_CHEST] = {
                upper.x * 0.6f + bones[ESP_BONE_PELVIS].x * 0.4f,
                upper.y * 0.6f + bones[ESP_BONE_PELVIS].y * 0.4f,
                upper.z * 0.6f + bones[ESP_BONE_PELVIS].z * 0.4f
            };
            valid[ESP_BONE_CHEST] = true;
        }
    }
    if (!valid[ESP_BONE_PELVIS] && valid[ESP_BONE_CHEST]) {
        float leg_y = 0.f; int leg_cnt = 0;
        if (valid[ESP_BONE_LEFT_KNEE]) { leg_y += bones[ESP_BONE_LEFT_KNEE].y; ++leg_cnt; }
        if (valid[ESP_BONE_RIGHT_KNEE]) { leg_y += bones[ESP_BONE_RIGHT_KNEE].y; ++leg_cnt; }
        if (leg_cnt > 0) {
            float avg_leg_y = leg_y / (float)leg_cnt;
            bones[ESP_BONE_PELVIS] = {
                bones[ESP_BONE_CHEST].x,
                (bones[ESP_BONE_CHEST].y + avg_leg_y) * 0.5f,
                bones[ESP_BONE_CHEST].z
            };
            valid[ESP_BONE_PELVIS] = true;
        }
    }
    if (valid[ESP_BONE_CHEST]) {
        if (!valid[ESP_BONE_LEFT_SHOULDER] && valid[ESP_BONE_LEFT_ELBOW]) {
            bones[ESP_BONE_LEFT_SHOULDER] = {
                bones[ESP_BONE_CHEST].x * 0.6f + bones[ESP_BONE_LEFT_ELBOW].x * 0.4f,
                bones[ESP_BONE_CHEST].y * 0.8f + bones[ESP_BONE_LEFT_ELBOW].y * 0.2f,
                bones[ESP_BONE_CHEST].z * 0.6f + bones[ESP_BONE_LEFT_ELBOW].z * 0.4f
            };
            valid[ESP_BONE_LEFT_SHOULDER] = true;
        }
        if (!valid[ESP_BONE_RIGHT_SHOULDER] && valid[ESP_BONE_RIGHT_ELBOW]) {
            bones[ESP_BONE_RIGHT_SHOULDER] = {
                bones[ESP_BONE_CHEST].x * 0.6f + bones[ESP_BONE_RIGHT_ELBOW].x * 0.4f,
                bones[ESP_BONE_CHEST].y * 0.8f + bones[ESP_BONE_RIGHT_ELBOW].y * 0.2f,
                bones[ESP_BONE_CHEST].z * 0.6f + bones[ESP_BONE_RIGHT_ELBOW].z * 0.4f
            };
            valid[ESP_BONE_RIGHT_SHOULDER] = true;
        }
    }
    if (valid[ESP_BONE_PELVIS]) {
        if (!valid[ESP_BONE_LEFT_HIP] && valid[ESP_BONE_LEFT_KNEE]) {
            bones[ESP_BONE_LEFT_HIP] = {
                bones[ESP_BONE_PELVIS].x * 0.6f + bones[ESP_BONE_LEFT_KNEE].x * 0.4f,
                bones[ESP_BONE_PELVIS].y * 0.8f + bones[ESP_BONE_LEFT_KNEE].y * 0.2f,
                bones[ESP_BONE_PELVIS].z * 0.6f + bones[ESP_BONE_LEFT_KNEE].z * 0.4f
            };
            valid[ESP_BONE_LEFT_HIP] = true;
        }
        if (!valid[ESP_BONE_RIGHT_HIP] && valid[ESP_BONE_RIGHT_KNEE]) {
            bones[ESP_BONE_RIGHT_HIP] = {
                bones[ESP_BONE_PELVIS].x * 0.6f + bones[ESP_BONE_RIGHT_KNEE].x * 0.4f,
                bones[ESP_BONE_PELVIS].y * 0.8f + bones[ESP_BONE_RIGHT_KNEE].y * 0.2f,
                bones[ESP_BONE_PELVIS].z * 0.6f + bones[ESP_BONE_RIGHT_KNEE].z * 0.4f
            };
            valid[ESP_BONE_RIGHT_HIP] = true;
        }
    }
}

static bool read_rig_skeleton(PlayerRig& rig, const Vec3& center_pos,
                              std::array<Vec3, ESP_BONE_COUNT>& bones,
                              std::array<bool, ESP_BONE_COUNT>& valid) {
    valid.fill(false);
    if (rig.source == RIG_SOURCE_NONE) return false;

    std::unordered_map<uint64_t, int32_t> max_indices;
    for (const RigJoint& joint : rig.joints) {
        if (!joint.valid()) continue;
        max_indices[joint.hierarchy] = std::max(max_indices[joint.hierarchy], joint.index);
    }
    for (const auto& kv : max_indices) {
        prepare_hierarchy_snapshot(kv.first, kv.second);
    }

    int valid_count = 0;
    for (size_t role = 0; role < (size_t)ESP_BONE_COUNT; ++role) {
        const RigJoint& joint = rig.joints[role];
        if (!joint.valid()) continue;
        Vec3 position{};
        if (!joint_position(joint, position) || !vec3_is_finite(position)) continue;
        if (vec_distance(position, center_pos) > 6.5f) continue;
        bones[role] = position;
        valid[role] = true;
        ++valid_count;
    }

    if (valid_count < 4) return false;

    complete_missing_bones(bones, valid);
    return true;
}

static bool read_player_skeleton(uint64_t player, const Vec3& center_pos,
                                 std::array<Vec3, ESP_BONE_COUNT>& bones,
                                 std::array<bool, ESP_BONE_COUNT>& valid) {
    valid.fill(false);
    double now = monotonic_seconds();
    uint64_t signature = rd_ptr(player + PLAYER_CHARACTER_MODEL);

    auto iterator = g_player_rigs.find(player);
    bool refresh = iterator == g_player_rigs.end() ||
        iterator->second.signature != signature ||
        (iterator->second.source == RIG_SOURCE_NONE
             ? now - iterator->second.resolved_at > 0.5
             : now - iterator->second.resolved_at > 10.0);
    if (refresh) {
        g_player_rigs[player] = resolve_player_rig(player, center_pos);
        iterator = g_player_rigs.find(player);
    }
    if (iterator == g_player_rigs.end() || iterator->second.source == RIG_SOURCE_NONE) return false;
    PlayerRig& rig = iterator->second;

    if (read_rig_skeleton(rig, center_pos, bones, valid)) {
        rig.failures = 0;
        return true;
    }

    if (++rig.failures >= 2) {
        g_player_rigs.erase(iterator);
    }
    return false;
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
    if (!initialized || valid < 1 || non_zero < 1) return false;
    double extent = fabs((double)maximum.x - minimum.x) + fabs((double)maximum.y - minimum.y) + fabs((double)maximum.z - minimum.z);
    if (!std::isfinite(extent) || extent > 1000000.0) return false;
    if (valid >= 2 && extent < 0.1) return false;
    score = (double)valid * 1000000.0 + std::min(extent, 999999.0);
    return true;
}

static bool discover_player_position_offset(const std::vector<uint64_t>& players) {
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
    g_player_rigs.clear();
    g_hierarchy_frame.clear();
    g_bone_layout = {};
    g_bone_layout_valid = false;
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
    g_player_rigs.clear();
    g_hierarchy_frame.clear();
    g_bone_layout = {}; g_bone_layout_valid = false;
    g_player_position_offset = PLAYER_POSITION;
    g_transform_hierarchy_layout = {}; g_transform_hierarchy_layout_valid = false;
    g_scene_players.clear();
    g_use_direct_player_position = true;
    g_player_position_validated = false;
}

std::vector<EspBox> esp_get_boxes(int overlay_width, int overlay_height) {
    std::lock_guard<std::mutex> game_lock(g_game_mutex);
    std::vector<EspBox> result;
    g_last_vp_valid = false;
    clear_hierarchy_frame_cache();

    if (g_pid <= 0 || !g_il2cpp_base) {
        return result;
    }

    uint64_t native_cam = 0;
    Mat4 projection{}, view{}, vp{};
    float sw = overlay_width >= 100 ? (float)overlay_width : 1080.0F;
    float sh = overlay_height >= 100 ? (float)overlay_height : 2400.0F;
    if (!std::isfinite(sw) || sw < 100.0F || sw > 10000.0F) sw = 1080.0F;
    if (!std::isfinite(sh) || sh < 100.0F || sh > 10000.0F) sh = 2400.0F;

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

        if (!g_player_snapshot.empty() && !g_scene_players.empty()) {
            size_t overlap = 0;
            for (uint64_t p : g_player_snapshot)
                if (g_scene_players.count(p)) ++overlap;
            if (overlap == 0) {
                g_player_track.clear();
                g_player_rigs.clear();
                g_bone_layout = {};
                g_bone_layout_valid = false;
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
             rd_ptr(resolved_local_player + PLAYER_OBSERVED) != 0);
    }

    bool camera_transition = local_player_transition;
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

    // Calibrate the native TransformData layout
    if (g_skeleton_enabled_from_ui && !g_bone_layout_valid) {
        std::vector<LayoutSample> samples;
        std::unordered_set<uint64_t> sampled_transforms;
        for (int pass = 0; pass < 2 && samples.size() < 8; ++pass) {
            for (size_t i = 0; i < s_transforms.size() && samples.size() < 8; ++i) {
                if ((i == local_entity_index) != (pass == 1)) continue;
                Vec3 pos{};
                if (!read_entity_position(s_transforms[i], pos)) continue;
                uint64_t nt = resolve_model_root_transform(s_transforms[i]);
                if (nt && sampled_transforms.insert(nt).second)
                    samples.push_back({nt, pos});
                uint64_t head = resolve_model_head_transform(s_transforms[i]);
                if (head && sampled_transforms.insert(head).second)
                    samples.push_back({head, pos});
            }
        }
        TransformHierarchyLayout layout{};
        if (calibrate_bone_layout(samples, layout)) {
            g_bone_layout = layout;
            g_bone_layout_valid = true;
        }
    }

    for (size_t i = 0; i < s_transforms.size(); ++i) {
        if (i == local_entity_index) continue;
        if (!s_transforms[i]) continue;
        Vec3 read_pos{};
        if (!read_entity_position(s_transforms[i], read_pos)) continue;

        float distance = -1.0F;
        if (has_local_position) {
            float dx = read_pos.x - local.x, dy = read_pos.y - local.y, dz = read_pos.z - local.z;
            distance = sqrtf(dx * dx + dy * dy + dz * dz);
            if (!std::isfinite(distance) || distance < MIN_PLAYER_DISTANCE || distance > MAX_PLAYER_DISTANCE) continue;
        }

        bool crouched = player_crouching(s_transforms[i]);
        float head_height = crouched ? 1.15f : PLAYER_HEIGHT;

        // --- velocity + position extrapolation ---
        Vec3 vel{};
        float render_x = read_pos.x, render_z = read_pos.z;
        float age = 0.f;
        {
            double now = std::chrono::duration<double>(
                std::chrono::steady_clock::now().time_since_epoch()).count();

            auto it = g_player_track.find(s_transforms[i]);
            if (it != g_player_track.end()) {
                double dt = now - it->second.t;
                float ddx = read_pos.x - it->second.pos.x;
                float ddz = read_pos.z - it->second.pos.z;
                float moved = fabsf(ddx) + fabsf(ddz);
                vel = it->second.vel;

                if (moved > 0.005f) {
                    if (dt > 0.03) {
                        double cdt = dt > 0.5 ? 0.5 : dt;
                        float ivx = ddx / (float)cdt;
                        float ivz = ddz / (float)cdt;
                        float sp = sqrtf(ivx * ivx + ivz * ivz);
                        if (sp < 15.f) {
                            vel.x += (ivx - vel.x) * 0.6f;
                            vel.z += (ivz - vel.z) * 0.6f;
                        }
                    }
                    it->second.pos = read_pos;
                    it->second.t = now;
                } else if (dt > 0.5) {
                    vel.x *= 0.8f; vel.z *= 0.8f;
                    if (fabsf(vel.x) < 0.05f && fabsf(vel.z) < 0.05f) { vel.x = 0.f; vel.z = 0.f; }
                    it->second.pos = read_pos;
                    it->second.t = now;
                }
                it->second.vel = vel;

                age = (float)(now - it->second.t);
                if (age > 0.f && age < 0.12f) {
                    render_x = it->second.pos.x + vel.x * age;
                    render_z = it->second.pos.z + vel.z * age;
                }
            } else {
                g_player_track[s_transforms[i]] = {read_pos, now, vel};
            }
        }

        // In Oxide, lastTickPosition (0x1D0) and worldCameraRoot (0x68) are at head/eye height (~1.65m above ground).
        // Ground/feet level is at position.y - head_height, and top of head is at position.y + 0.15m.
        float ground_y = read_pos.y - head_height;
        float top_y = read_pos.y + 0.15f;

        Vec3 body_bottom = {render_x, ground_y, render_z};
        Vec3 body_top = {render_x, top_y, render_z};
        float body_x = render_x, body_z = render_z;

        auto project_world = [&](const Vec3& world, Vec2& screen) {
            return transform_camera_mode
                ? w2s_transform_camera(transform_camera_position, transform_camera_rotation, world, sw, sh, screen, false)
                : w2s(vp, world, sw, sh, screen, false);
        };

        Vec2 sf{}, sh2{};
        if (!project_world(body_bottom, sf) || !project_world(body_top, sh2)) continue;

        float screen_top_y = std::min(sf.y, sh2.y);
        float screen_bot_y = std::max(sf.y, sh2.y);
        float height = screen_bot_y - screen_top_y;
        if (!std::isfinite(height) || height < 2.0F) continue;

        float cx = (sf.x + sh2.x) * 0.5F;
        float half_w = height * PLAYER_BOX_WIDTH_RATIO * 0.5F;

        EspBox box{};
        box.x1 = cx - half_w;
        box.y1 = screen_top_y;
        box.x2 = cx + half_w;
        box.y2 = screen_bot_y;
        box.distance = distance;
        box.source = s_transforms[i];
        box.feet = body_bottom;
        box.head = {body_x, top_y, body_z};
        box.vel = vel;
        box.speed = sqrtf(vel.x * vel.x + vel.z * vel.z);
        box.crouched = crouched;

        read_player_labels(s_transforms[i], box);

        // --- animated skeleton ---
        std::array<Vec3, ESP_BONE_COUNT> player_bones{};
        std::array<bool, ESP_BONE_COUNT> player_bones_valid{};
        if (g_skeleton_enabled_from_ui &&
            read_player_skeleton(s_transforms[i], read_pos, player_bones, player_bones_valid)) {
            int projected = 0;
            float bone_min_x = 1e9f, bone_max_x = -1e9f;
            float bone_min_y = 1e9f, bone_max_y = -1e9f;

            for (size_t bone = 0; bone < (size_t)ESP_BONE_COUNT; ++bone) {
                box.bone_visible[bone] = false;
                if (!player_bones_valid[bone]) continue;
                Vec3 bone_world = player_bones[bone];
                // Extrapolate horizontal position along with model velocity
                if (age > 0.f && age < 0.12f && (fabsf(vel.x) > 0.05f || fabsf(vel.z) > 0.05f)) {
                    bone_world.x += vel.x * age;
                    bone_world.z += vel.z * age;
                }
                box.bones[bone] = bone_world;
                Vec2 bone_screen{};
                bool on_screen = project_world(bone_world, bone_screen) &&
                    bone_screen.x >= 0.0F && bone_screen.x <= sw &&
                    bone_screen.y >= 0.0F && bone_screen.y <= sh;
                box.bone_screen[bone][0] = on_screen ? bone_screen.x : -1.0F;
                box.bone_screen[bone][1] = on_screen ? bone_screen.y : -1.0F;
                box.bone_visible[bone] = on_screen;
                if (on_screen) {
                    ++projected;
                    bone_min_x = std::min(bone_min_x, bone_screen.x);
                    bone_max_x = std::max(bone_max_x, bone_screen.x);
                    bone_min_y = std::min(bone_min_y, bone_screen.y);
                    bone_max_y = std::max(bone_max_y, bone_screen.y);
                }
            }
            box.skeleton_valid = projected >= 4;

            if (player_bones_valid[ESP_BONE_HEAD]) {
                box.head = player_bones[ESP_BONE_HEAD];
                if (age > 0.f && age < 0.12f && (fabsf(vel.x) > 0.05f || fabsf(vel.z) > 0.05f)) {
                    box.head.x += vel.x * age;
                    box.head.z += vel.z * age;
                }
            }

            // Fit 2D ESP box precisely to animated bone coordinates
            if (box.skeleton_valid && projected >= 4) {
                float head_r = 12.f;
                if (box.bone_visible[ESP_BONE_HEAD] && box.bone_visible[ESP_BONE_NECK]) {
                    float hdx = box.bone_screen[ESP_BONE_HEAD][0] - box.bone_screen[ESP_BONE_NECK][0];
                    float hdy = box.bone_screen[ESP_BONE_HEAD][1] - box.bone_screen[ESP_BONE_NECK][1];
                    head_r = sqrtf(hdx * hdx + hdy * hdy) * 0.9f;
                } else if (box.bone_visible[ESP_BONE_HEAD]) {
                    head_r = height * 0.08f;
                }
                head_r = std::max(4.f, std::min(head_r, 40.f));

                float skel_top = bone_min_y - head_r;
                float skel_bot = bone_max_y + head_r * 0.5f;
                float skel_h = skel_bot - skel_top;
                if (skel_h > 4.f) {
                    float skel_cx = (bone_min_x + bone_max_x) * 0.5f;
                    float skel_w = std::max((bone_max_x - bone_min_x) + head_r * 1.5f, skel_h * PLAYER_BOX_WIDTH_RATIO);
                    box.y1 = skel_top;
                    box.y2 = skel_bot;
                    box.x1 = skel_cx - skel_w * 0.5f;
                    box.x2 = skel_cx + skel_w * 0.5f;
                }
            }
        } else {
            box.skeleton_valid = false;
        }

        // Screen-space velocity of the animated head
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

static uint64_t local_player_event_handler() {
    uint64_t local = resolve_local_player();
    if (!local) return 0;
    return rd_ptr(local + PLAYER_EVENT_HANDLER);
}

static bool player_crouching(uint64_t player) {
    if (!player) return false;
    uint64_t kcc = resolve_kcc(player);
    if (kcc) {
        int32_t pose = rd<int32_t>(kcc + KCC_POSE);
        if (pose == 1) return true;
        uint8_t move_state = rd<uint8_t>(kcc + KCC_MOVE_STATE);
        if (move_state == (uint8_t)MOVE_STATE_CROUCHING) return true;
        if (pose == 0 && move_state <= 8) return false;
    }
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
    for (uint64_t field : {HUC_RAYCAST_DATA, HUC_AIM_RAYCAST}) {
        uint64_t wrapped = rd_ptr(handler + field);
        if (!likely_native_pointer(wrapped)) continue;
        for (uint64_t off : {HUW_VALUE, HUW_PREV}) {
            uint64_t ray = rd_ptr(wrapped + off);
            if (!likely_native_pointer(ray)) continue;
            uint8_t hit = rd<uint8_t>(ray + HUZ_HIT);
            uint8_t hit2 = rd<uint8_t>(ray + HUZ_HIT + 1);
            if (hit > 1 || hit2 > 1) continue;
            if (hit == 0) continue;
            uint64_t player = rd_ptr(ray + HUZ_PLAYER);
            if (player && likely_native_pointer(player)) return player;
        }
    }
    return 0;
}
