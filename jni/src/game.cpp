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

struct PlayerCachedInfo {
    char name[64]{};
    char weapon[32]{};
    float max_health = 100.f;
    double updated_at = 0.0;
};
static std::unordered_map<uint64_t, PlayerCachedInfo> g_player_info_cache;

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
    if (!g_player_manager_class) return 0;
    if (!g_player_manager_static_fields)
        g_player_manager_static_fields = get_class_static_fields(g_player_manager_class);
    if (!g_player_manager_static_fields) return 0;

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
        fabsf(value.x) < 50000.0F && fabsf(value.y) < 50000.0F && fabsf(value.z) < 50000.0F;
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

static bool likely_native_pointer(uint64_t value) {
    return value >= 0x10000 && value < 0x0001000000000000ULL && (value & 0x7) == 0;
}

static float vec_distance(const Vec3& first, const Vec3& second) {
    float dx = first.x - second.x, dy = first.y - second.y, dz = first.z - second.z;
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

static bool read_entity_position(uint64_t source, Vec3& position) {
    if (!source) return false;
    position = rd_v3(source + g_player_position_offset);
    return vec3_is_finite(position);
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

static uint64_t resolve_kcc(uint64_t player) {
    if (!player) return 0;
    uint64_t reference = rd_ptr(player + PLAYER_KCC_REFERENCE);
    if (!reference) return 0;

    const uint64_t candidates[] = {
        rd_ptr(reference + 0x10),
        rd_ptr(reference + 0x18),
        reference
    };
    for (uint64_t candidate : candidates) {
        if (!likely_native_pointer(candidate)) continue;
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
    double now = monotonic_seconds();
    auto& cached = g_player_info_cache[player];
    bool refresh_strings = (now - cached.updated_at > 0.4);

    if (refresh_strings) {
        cached.name[0] = '\0';
        cached.weapon[0] = '\0';
        cached.updated_at = now;

        uint64_t nicklabel = rd_ptr(player + PLAYER_NICKLABEL);
        uint64_t text = nicklabel ? rd_ptr(nicklabel + NICKLABEL_TEXT) : 0;
        std::string name = text ? read_il2cpp_string(rd_ptr(text + UI_TEXT_VALUE), 48) : std::string();
        if (name.empty()) name = read_il2cpp_string(rd_ptr(player + PLAYER_USER_ID), 48);
        if (!name.empty()) snprintf(cached.name, sizeof(cached.name), "%s", name.c_str());

        uint64_t vitals = rd_ptr(player + PLAYER_VITALS);
        if (likely_native_pointer(vitals)) {
            float max_hp = rd<float>(vitals + PLAYER_VITALS_MAX_HP);
            if (std::isfinite(max_hp) && max_hp > 0.f && max_hp <= 10000.f)
                cached.max_health = max_hp;
        }

        uint64_t reference = rd_ptr(player + PLAYER_WEAPON_REFERENCE);
        uint64_t weapon = reference ? rd_ptr(reference + INTERFACE_REFERENCE_VALUE) : 0;
        if (likely_native_pointer(weapon)) {
            int16_t number = rd<int16_t>(weapon + PLAYER_WEAPON_NUMBER);
            if (number > 0 && number < 10000)
                snprintf(cached.weapon, sizeof(cached.weapon), "Weapon #%d", (int)number);
        }
    }

    snprintf(box.name, sizeof(box.name), "%s", cached.name);
    snprintf(box.weapon, sizeof(box.weapon), "%s", cached.weapon);
    box.max_health = cached.max_health;

    // Read live current health
    box.health = -1.f;
    uint64_t handler = rd_ptr(player + PLAYER_EVENT_HANDLER);
    uint64_t health_value = handler ? rd_ptr(handler + HUC_HEALTH) : 0;
    if (likely_native_pointer(health_value)) {
        float hp = rd<float>(health_value + HUO_CURRENT_FLOAT);
        if (std::isfinite(hp) && hp >= 0.f && hp <= 10000.f)
            box.health = hp;
    }
}

static bool mat_is_finite(const Mat4& matrix) {
    bool has_non_zero = false;
    for (float value : matrix.m) {
        if (!std::isfinite(value) || fabsf(value) > 1000000.0F) return false;
        if (fabsf(value) > 0.000001F) has_non_zero = true;
    }
    return has_non_zero;
}

static float mat_get(const Mat4& matrix, int row, int column) {
    return matrix.m[(size_t)column * 4 + row];
}
static void mat_set(Mat4& matrix, int row, int column, float value) {
    matrix.m[(size_t)column * 4 + row] = value;
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

static std::vector<uint64_t> read_configured_player_transforms() {
    std::vector<uint64_t> transforms;
    uint64_t list = resolve_runtime_player_list();
    if (!list) return transforms;

    uint64_t local_player = resolve_local_player();
    uint64_t items = rd_ptr(list + IL2CPP_LIST_ITEMS);
    int32_t count = rd<int32_t>(list + IL2CPP_LIST_SIZE);
    if (!items || count <= 0 || count > 512) return transforms;

    transforms.reserve((size_t)count + 1);
    if (local_player) transforms.push_back(local_player);

    for (int32_t index = 0; index < count; ++index) {
        uint64_t player = rd_ptr(items + IL2CPP_ARRAY_FIRST_ELEMENT + (uint64_t)index * sizeof(uint64_t));
        if (!player) continue;
        if (g_player_manager_class && rd_ptr(player) != g_player_manager_class) continue;
        if (player == local_player) continue;
        transforms.push_back(player);
    }
    return transforms;
}

static bool g_skeleton_enabled_from_ui = false;
void esp_set_skeleton_enabled(bool enabled) { g_skeleton_enabled_from_ui = enabled; }

bool esp_init(pid_t pid) {
    std::lock_guard<std::mutex> game_lock(g_game_mutex);
    g_pid = pid;
    g_il2cpp_base = get_base("libil2cpp.so");
    if (!g_il2cpp_base) return false;

    g_player_manager_class = 0;
    g_player_manager_static_fields = 0;
    g_game_controller_class = 0;
    g_local_player = 0;
    g_last_camera_fov_deg = -1.f;
    g_last_vp_valid = false;
    g_player_snapshot.clear();
    g_player_snapshot_stamp = {};
    g_player_track.clear();
    g_player_info_cache.clear();
    g_player_position_offset = PLAYER_POSITION;
    g_scene_players.clear();
    g_use_direct_player_position = true;
    g_player_position_validated = true;
    return true;
}

void esp_reset() {
    std::lock_guard<std::mutex> game_lock(g_game_mutex);
    g_pid = -1; g_il2cpp_base = 0;
    g_player_manager_class = 0; g_player_manager_static_fields = 0;
    g_game_controller_class = 0; g_local_player = 0;
    g_last_camera_fov_deg = -1.f;
    g_last_vp_valid = false;
    g_player_snapshot.clear();
    g_player_snapshot_stamp = {};
    g_player_track.clear();
    g_player_info_cache.clear();
    g_player_position_offset = PLAYER_POSITION;
    g_scene_players.clear();
    g_use_direct_player_position = true;
    g_player_position_validated = false;
}

std::vector<EspBox> esp_get_boxes(int overlay_width, int overlay_height) {
    std::lock_guard<std::mutex> game_lock(g_game_mutex);
    std::vector<EspBox> result;
    g_last_vp_valid = false;

    if (g_pid <= 0 || !g_il2cpp_base) return result;

    float sw = overlay_width >= 100 ? (float)overlay_width : 1080.0F;
    float sh = overlay_height >= 100 ? (float)overlay_height : 2400.0F;

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
                g_player_info_cache.clear();
                g_use_direct_player_position = true;
                g_player_position_offset = PLAYER_POSITION;
            }
        }
        g_scene_players.clear();
        for (uint64_t p : g_player_snapshot) g_scene_players.insert(p);
    }
    if (g_player_snapshot.empty()) return result;

    const std::vector<uint64_t>& s_transforms = g_player_snapshot;

    uint64_t native_cam = 0;
    Mat4 projection{}, view{}, vp{};
    {
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
        if (!mat_is_finite(projection) || !mat_is_finite(view)) return result;

        vp = mat_mul(projection, view);
        g_last_vp = vp;
        g_last_vp_valid = true;

        float cot_half = mat_get(projection, 1, 1);
        if (std::isfinite(cot_half) && cot_half > 1e-6f) {
            g_last_camera_fov_deg = 2.0f * atanf(1.0f / cot_half) * (180.0f / 3.14159265358979f);
            if (g_last_camera_fov_deg > 179.f) g_last_camera_fov_deg = 60.f;
        } else {
            g_last_camera_fov_deg = -1.f;
        }
    }

    bool has_local_position = false;
    Vec3 local{};
    size_t local_entity_index = s_transforms.size();
    uint64_t resolved_local_player = resolve_local_player();
    {
        for (size_t index = 0; index < s_transforms.size(); ++index) {
            Vec3 candidate{};
            if (!read_entity_position(s_transforms[index], candidate)) continue;
            if (resolved_local_player && s_transforms[index] == resolved_local_player) {
                local_entity_index = index;
                local = candidate;
                break;
            }
        }
        if (local_entity_index == s_transforms.size() && !s_transforms.empty()) {
            local_entity_index = 0;
            read_entity_position(s_transforms[0], local);
        }
        has_local_position = local_entity_index != s_transforms.size();
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

        Vec3 vel{};
        float render_x = read_pos.x, render_z = read_pos.z;
        float age = 0.f;
        {
            double now = monotonic_seconds();
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

        // In Oxide, lastTickPosition is head/eye level (~1.65m above ground)
        float ground_y = read_pos.y - head_height;
        float top_y = read_pos.y + 0.15f;

        Vec3 body_bottom = {render_x, ground_y, render_z};
        Vec3 body_top = {render_x, top_y, render_z};
        float body_x = render_x, body_z = render_z;

        Vec2 sf{}, sh2{};
        if (!w2s(vp, body_bottom, sw, sh, sf, false) || !w2s(vp, body_top, sw, sh, sh2, false)) continue;

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
        box.skeleton_valid = false;

        read_player_labels(s_transforms[i], box);

        // Screen-space velocity of the head
        box.aim_vx = 0.f;
        box.aim_vy = 0.f;
        {
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
            bool proj = w2s(vp, world_corners[corner], sw, sh, sc, false);
            box.corner_visible[corner] = proj && sc.x >= 0.0F && sc.x <= sw && sc.y >= 0.0F && sc.y <= sh;
            box.corners[corner][0] = proj ? sc.x : -1.0F;
            box.corners[corner][1] = proj ? sc.y : -1.0F;
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
