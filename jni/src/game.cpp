#include "game.h"
#include "game_offsets.h"
#include "Vector.h"

#include <string.h>
#include <sys/uio.h>
#include <stdio.h>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <string>
#include <unordered_map>
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

// Aim targets computed by esp_get_boxes for the current frame.
static std::vector<EspAimTarget> g_aim_targets;
static std::vector<EspBox> esp_get_boxes_impl(int overlay_width, int overlay_height);

// Dead reckoning: the managed position field (lastTickPosition) is a tick
// snapshot and can lag running players by meters. Per entity we estimate
// velocity from consecutive value changes and extrapolate by the time elapsed
// since the last change. Everything is bounded: the horizon is a fraction of
// the observed update interval (<=0.4s), speed is capped, and teleports zero
// the velocity - so the worst case is the raw snapshot, never garbage.
struct EntityMotion {
    Vec3   pos{};
    Vec3   velocity{};
    double last_change_time = 0;
    double interval = 0.1; // EMA of seconds between observed value changes
    bool   has_prev = false;
};
static std::unordered_map<uint64_t, EntityMotion> g_entity_motion;

static double monotonic_seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static void apply_dead_reckoning(uint64_t source, Vec3& position) {
    constexpr double kChangeEpsilonSq = 0.0001F; // 1cm
    constexpr float  kMaxSpeed = 25.0F;
    constexpr float  kTeleportDistSq = 225.0F;   // >15m in one step
    constexpr float  kMinHorizon = 0.05F;
    constexpr float  kMaxHorizon = 0.4F;

    double now = monotonic_seconds();
    if (g_entity_motion.size() > 1024) g_entity_motion.clear();
    EntityMotion& m = g_entity_motion[source];

    if (!m.has_prev) {
        m.pos = position;
        m.last_change_time = now;
        m.has_prev = true;
        return;
    }

    Vec3 delta = {position.x - m.pos.x, position.y - m.pos.y, position.z - m.pos.z};
    float moved_sq = delta.x * delta.x + delta.y * delta.y + delta.z * delta.z;
    if (moved_sq > kChangeEpsilonSq) {
        double dt = now - m.last_change_time;
        if (moved_sq >= kTeleportDistSq) {
            m.velocity = {};
        } else if (dt > 0.001) {
            Vec3 vel = {(float)(delta.x / dt), (float)(delta.y / dt), (float)(delta.z / dt)};
            float speed_sq = vel.x * vel.x + vel.y * vel.y + vel.z * vel.z;
            if (speed_sq > kMaxSpeed * kMaxSpeed) {
                float scale = kMaxSpeed / sqrtf(speed_sq);
                vel = {vel.x * scale, vel.y * scale, vel.z * scale};
            }
            m.velocity = vel;
        }
        double clamped_dt = dt < 0.02 ? 0.02 : (dt > 2.0 ? 2.0 : dt);
        m.interval = (float)(m.interval * 0.8 + clamped_dt * 0.2);
        m.pos = position;
        m.last_change_time = now;
    } else {
        m.pos = position; // same snapshot, keep timing
    }

    double since_change = now - m.last_change_time;
    float horizon = m.interval * 0.8F;
    if (horizon > kMaxHorizon) horizon = kMaxHorizon;
    if (horizon < kMinHorizon) horizon = kMinHorizon;
    float t = (float)(since_change < 0.0 ? 0.0 : (since_change > (double)horizon ? (double)horizon : since_change));
    position.x += m.velocity.x * t;
    position.y += m.velocity.y * t;
    position.z += m.velocity.z * t;
    if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z)) position = m.pos;
}

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

// --- il2cpp runtime access --------------------------------------------------

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

// Il2CppClass.static_fields (il2cpp v29+ layout, libil2cpp.so 6000.3.18f1).
static constexpr uint64_t IL2CPP_CLASS_STATIC_FIELDS = 0xB8;

static uint64_t get_class_static_fields(uint64_t klass) {
    if (!klass) return 0;
    return rd_ptr(klass + IL2CPP_CLASS_STATIC_FIELDS);
}

static bool ensure_player_manager_class() {
    if (g_player_manager_class || PLAYER_MANAGER_TYPEINFO_RVA == 0) return g_player_manager_class != 0;
    uint64_t candidate = rd_ptr(g_il2cpp_base + PLAYER_MANAGER_TYPEINFO_RVA);
    if (!candidate) return false;
    std::string name = read_remote_string(rd_ptr(candidate + 0x10));
    std::string ns   = read_remote_string(rd_ptr(candidate + 0x18));
    if (name == "PlayerManager" && ns == "Oxide")
        g_player_manager_class = candidate;
    return g_player_manager_class != 0;
}

static uint64_t resolve_runtime_player_list() {
    if (!ensure_player_manager_class()) return 0;
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

static bool ensure_game_controller_class() {
    if (g_game_controller_class || GAME_CONTROLLER_TYPEINFO_RVA == 0) return g_game_controller_class != 0;
    uint64_t candidate = rd_ptr(g_il2cpp_base + GAME_CONTROLLER_TYPEINFO_RVA);
    if (!candidate) return false;
    std::string name = read_remote_string(rd_ptr(candidate + 0x10));
    std::string ns   = read_remote_string(rd_ptr(candidate + 0x18));
    if (name == "GameControllerBase" && ns == "Oxide")
        g_game_controller_class = candidate;
    return g_game_controller_class != 0;
}

static uint64_t resolve_local_player() {
    if (g_local_player && rd_ptr(g_local_player) == g_player_manager_class)
        return g_local_player;
    g_local_player = 0;

    if (!ensure_player_manager_class() || !ensure_game_controller_class()) return 0;

    uint64_t gcb_static_fields = get_class_static_fields(g_game_controller_class);
    if (!gcb_static_fields) return 0;
    uint64_t local_player = rd_ptr(gcb_static_fields + GAME_CONTROLLER_LOCAL_PLAYER_FIELD);
    if (local_player && rd_ptr(local_player) == g_player_manager_class) {
        g_local_player = local_player;
        return local_player;
    }
    return 0;
}

// --- Native transform hierarchy (fixed layout) -------------------------------
// Managed UnityEngine.Transform -> m_CachedPtr native transform. Unity 6 arm64
// native transform layout (validated against libil2cpp.so transform access):
//   +0x38 TransformData*, +0x40 transform index
//   TransformData: +0x18 matrices (Matrix34[]), +0x20 parent indices (int[])
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
    float qlen = matrix.rotation.x * matrix.rotation.x + matrix.rotation.y * matrix.rotation.y + matrix.rotation.z * matrix.rotation.z + matrix.rotation.w * matrix.rotation.w;
    return qlen >= 0.20F && qlen <= 2.0F && fabsf(matrix.scale.x) <= 10000.0F && fabsf(matrix.scale.y) <= 10000.0F && fabsf(matrix.scale.z) <= 10000.0F;
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

static bool read_transform_hierarchy_position(uint64_t native_transform, Vec3& position) {
    if (!native_transform) return false;
    uint64_t transform_data = rd_ptr(native_transform + 0x38);
    int32_t transform_index = rd<int32_t>(native_transform + 0x40);
    if (!transform_data || transform_index < 0 || transform_index > 100000) return false;
    uint64_t matrices = rd_ptr(transform_data + 0x18);
    uint64_t indices = rd_ptr(transform_data + 0x20);
    return read_transform_hierarchy_arrays(matrices, indices, transform_index, position);
}

static uint64_t resolve_player_native_transform(uint64_t player) {
    if (!player) return 0;
    return resolve_native_transform(rd_ptr(player + PLAYER_TRANSFORM));
}

// --- Real skeleton bones ------------------------------------------------------
// PlayerManager.inventory -> _playerInventoryData -> playerModelInfo holds the
// third-person model's head/body Transforms. Their live world positions follow
// the actual animation pose (crouch, lean, jump), unlike constant heights above
// the feet. The PlayerInventoryData pointer is cached per player and
// re-validated through its .player backref so a respawned model refreshes.
static std::unordered_map<uint64_t, uint64_t> g_model_info_cache;

static uint64_t resolve_player_model_info(uint64_t player) {
    if (!player) return 0;
    if (g_model_info_cache.size() > 1024) g_model_info_cache.clear();
    auto it = g_model_info_cache.find(player);
    uint64_t inv_data = 0;
    if (it != g_model_info_cache.end() && it->second) {
        // Cheap re-validation: the cached value is the PlayerInventoryData; its
        // .player backref must still point at this player.
        inv_data = it->second;
        if (rd_ptr(inv_data + INVENTORY_DATA_PLAYER_FIELD) != player) inv_data = 0;
    }
    if (!inv_data) {
        uint64_t inventory = rd_ptr(player + PLAYER_INVENTORY_FIELD);
        if (!inventory) { g_model_info_cache.erase(player); return 0; }
        inv_data = rd_ptr(inventory + PLAYER_INVENTORY_DATA_FIELD);
        if (!inv_data || rd_ptr(inv_data + INVENTORY_DATA_PLAYER_FIELD) != player) {
            g_model_info_cache.erase(player);
            return 0;
        }
        g_model_info_cache[player] = inv_data;
    }
    return rd_ptr(inv_data + INVENTORY_DATA_MODEL_FIELD);
}

// Reads the world position of the model's head bone. `anchor` is the entity
// position used for boxes; a bone that lands unreasonably far from it is
// rejected (stale model of a respawned player, mid-streaming reads).
static bool read_player_head_bone(uint64_t player, const Vec3& anchor, Vec3& head_out) {
    uint64_t model_info = resolve_player_model_info(player);
    if (!model_info) return false;
    uint64_t head_native = resolve_native_transform(rd_ptr(model_info + MODEL_INFO_HEAD_FIELD));
    if (!head_native) return false;
    Vec3 head{};
    if (!read_transform_hierarchy_position(head_native, head)) return false;
    if (!vec3_is_finite(head)) return false;
    float dx = head.x - anchor.x, dy = head.y - anchor.y, dz = head.z - anchor.z;
    if (dx * dx + dy * dy + dz * dz > 9.0F) return false; // >3m from the entity position - not this player's model
    head_out = head;
    return true;
}

// --- Camera (libunity.so offsets) ---------------------------------------------
// GameControllerBase.<zOI> -> CameraManager.m_Camera -> managed Camera ->
// m_CachedPtr native camera. Matrices are read from the native camera object
// at the offsets proven by the libunity.so icall disassembly (see
// game_offsets.h). The ORIGINAL projection (+0x130) is preferred: it never
// carries oblique/jitter modifications, so ESP geometry stays pixel-stable.
static uint64_t resolve_native_camera() {
    if (!ensure_game_controller_class()) return 0;
    uint64_t gcb_static_fields = get_class_static_fields(g_game_controller_class);
    if (!gcb_static_fields) return 0;
    uint64_t cam_mgr = rd_ptr(gcb_static_fields + GAME_CONTROLLER_CAMERA_MANAGER_FIELD);
    if (!cam_mgr) return 0;
    uint64_t managed_cam = rd_ptr(cam_mgr + CAMERA_MANAGER_CAMERA_FIELD);
    if (!managed_cam) return 0;
    return rd_ptr(managed_cam + MANAGED_CACHED_PTR);
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

// A frustum projection must scale both screen axes. The matrices are stored in
// Unity's Matrix4x4 memory order, so m[0]/m[5] are the X/Y clip scales.
// An identity matrix is rejected: m_OriginalProjectionMatrix starts out as
// identity/zero on cameras that never had SetProjectionMatrix called.
static bool matrix_is_plausible_projection(const Mat4& matrix) {
    if (!matrix_is_finite(matrix)) return false;
    if (fabsf(matrix.m[0]) < 0.0001F || fabsf(matrix.m[5]) < 0.0001F) return false;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            float expected = (i == j) ? 1.0F : 0.0F;
            if (fabsf(mat_get(matrix, i, j) - expected) > 0.0001F) return true; // some entry deviates
        }
    return false; // exact identity - not a real frustum
}

struct CameraMatrices {
    Mat4 view{};
    Mat4 projection{};
    bool from_original = false;
};

static bool read_camera_matrices(uint64_t native_cam, CameraMatrices& out) {
    if (!native_cam) return false;
    out.view = rd_m4(native_cam + NATIVE_CAMERA_VIEW_MATRIX);
    if (!matrix_is_finite(out.view)) return false;
    Mat4 original = rd_m4(native_cam + NATIVE_CAMERA_ORIGINAL_PROJECTION);
    if (matrix_is_plausible_projection(original)) {
        out.projection = original;
        out.from_original = true;
        return true;
    }
    // The original was never set (or was reset): the live projection cache is
    // the same matrix Unity's WorldToScreenPoint uses.
    Mat4 live = rd_m4(native_cam + NATIVE_CAMERA_PROJECTION_MATRIX);
    if (!matrix_is_plausible_projection(live)) return false;
    out.projection = live;
    out.from_original = false;
    return true;
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

// --- Entity positions ----------------------------------------------------------
static bool read_entity_position(uint64_t source, Vec3& position) {
    if (!source) return false;
    // Fixed dump-backed field: PlayerManager.lastTickPosition (feet level).
    position = rd_v3(source + PLAYER_POSITION);
    if (vec3_is_finite(position)) {
        apply_dead_reckoning(source, position);
        return true;
    }
    // Fallback: world render transform through the fixed hierarchy layout.
    uint64_t native = resolve_player_native_transform(source);
    if (!native) return false;
    Vec3 hierarchy{};
    if (!read_transform_hierarchy_position(native, hierarchy) || !vec3_is_finite(hierarchy)) return false;
    position = hierarchy;
    apply_dead_reckoning(source, position);
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
    g_entity_motion.clear();
    g_model_info_cache.clear();
    g_aim_targets.clear();
}

std::vector<EspBox> esp_get_boxes(int overlay_width, int overlay_height) {
    // Cleared here so early returns inside the impl never leave stale targets behind.
    g_aim_targets.clear();
    return esp_get_boxes_impl(overlay_width, overlay_height);
}

static std::vector<EspBox> esp_get_boxes_impl(int overlay_width, int overlay_height) {
    std::vector<EspBox> result;

    if (g_pid <= 0 || !g_il2cpp_base) { return result; }

    float sw = overlay_width >= 100 ? (float)overlay_width : 1080.0F;
    float sh = overlay_height >= 100 ? (float)overlay_height : 2400.0F;
    if (!std::isfinite(sw) || sw < 100.0F || sw > 10000.0F) sw = 1080.0F;
    if (!std::isfinite(sh) || sh < 100.0F || sh > 10000.0F) sh = 2400.0F;

    static std::vector<uint64_t> s_transforms;
    std::vector<uint64_t> refreshed = read_configured_player_transforms();
    if (!refreshed.empty()) s_transforms = std::move(refreshed);
    if (s_transforms.empty()) return result;

    // Camera straight from the native camera object (libunity.so offsets).
    uint64_t native_cam = resolve_native_camera();
    if (!native_cam) return result;
    CameraMatrices cam{};
    if (!read_camera_matrices(native_cam, cam)) return result;
    Mat4 vp = mat_mul(cam.projection, cam.view);

    Vec3 camera_position{};
    bool has_camera_position = camera_position_from_view(cam.view, camera_position);

    // Snapshot: the statics-provided local player (when resolved) is [0].
    bool has_local_position = false;
    Vec3 local{};
    size_t local_entity_index = s_transforms.size();

    Vec3 first_valid_position{};
    size_t first_valid_index = s_transforms.size();
    double nearest_eye_distance_squared = INFINITY;
    size_t nearest_eye_index = s_transforms.size();

    uint64_t local_ptr = resolve_local_player();
    if (local_ptr) {
        for (size_t index = 0; index < s_transforms.size(); ++index) {
            if (s_transforms[index] != local_ptr) continue;
            Vec3 candidate{};
            if (read_entity_position(s_transforms[index], candidate)) {
                local_entity_index = index;
                local = candidate;
            }
            break;
        }
    }

    for (size_t index = 0; index < s_transforms.size(); ++index) {
        Vec3 candidate{};
        if (!read_entity_position(s_transforms[index], candidate)) continue;
        if (first_valid_index == s_transforms.size()) { first_valid_index = index; first_valid_position = candidate; }
        if (index == local_entity_index) continue;
        if (!has_camera_position) continue;
        double dx = (double)candidate.x - camera_position.x, dy = (double)candidate.y - camera_position.y, dz = (double)candidate.z - camera_position.z;
        double distance_squared = dx * dx + dy * dy + dz * dz;
        if (std::isfinite(distance_squared) && distance_squared < nearest_eye_distance_squared) {
            nearest_eye_distance_squared = distance_squared;
            nearest_eye_index = index;
        }
    }

    if (local_entity_index == s_transforms.size()) {
        if (nearest_eye_index != s_transforms.size() &&
            nearest_eye_distance_squared <= 40000.0) { // within 200m of the eye
            // Statics unavailable - fall back to the player closest to the eye.
            Vec3 candidate{};
            if (read_entity_position(s_transforms[nearest_eye_index], candidate)) {
                local_entity_index = nearest_eye_index;
                local = candidate;
            }
        } else if (first_valid_index != s_transforms.size()) {
            local_entity_index = first_valid_index;
            local = first_valid_position;
        }
    }
    has_local_position = local_entity_index != s_transforms.size();
    if (!has_local_position) return result;

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

        // Real skeleton: read the model's head bone. When available, the box top
        // and the aim bones follow the live animation pose (crouch, lean, jump)
        // instead of assuming a standing model of constant height.
        Vec3 head_bone{};
        bool head_bone_ok = read_player_head_bone(s_transforms[i], feet, head_bone);
        // The head pivot must sit plausibly above the feet; otherwise treat the
        // read as garbage and fall back to the constant-height model.
        if (head_bone_ok) {
            float head_up = head_bone.y - feet.y;
            if (!(head_up > 0.2F && head_up < 2.2F)) head_bone_ok = false;
        }

        Vec3 body_bottom = {feet.x, feet.y, feet.z};
        Vec3 body_top = head_bone_ok
            ? Vec3{head_bone.x, head_bone.y + HEAD_TOP_MARGIN, head_bone.z}
            : Vec3{feet.x, feet.y + PLAYER_HEIGHT, feet.z};

        Vec2 sf{}, sh2{};
        if (!w2s(vp, body_bottom, sw, sh, sf, false)) continue;
        if (!w2s(vp, body_top, sw, sh, sh2, false)) continue;

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
            bool projected = w2s(vp, world_corners[corner], sw, sh, sc, false);
            box.corner_visible[corner] = projected && sc.x >= 0.0F && sc.x <= sw && sc.y >= 0.0F && sc.y <= sh;
            box.corners[corner][0] = projected ? sc.x : -1.0F;
            box.corners[corner][1] = projected ? sc.y : -1.0F;
        }
        result.push_back(box);

        // Exact world-space bone points projected through the live camera. Unlike
        // screen-space fractions of the bounding box, this stays anatomically correct
        // at any distance, pitch and aspect.
        EspAimTarget aim{};
        aim.box_x1 = box.x1; aim.box_y1 = box.y1;
        aim.box_x2 = box.x2; aim.box_y2 = box.y2;
        aim.distance = distance;
        Vec3 bones[3];
        if (head_bone_ok) {
            // Anchor everything to the real head bone: the head point is the
            // skull center; chest and pelvis are placed at fixed fractions of
            // the actual feet->head extent, and slide along the feet->head
            // lateral offset - so a crouched or leaning pose keeps all three
            // points inside the body.
            float span_x = head_bone.x - feet.x;
            float span_y = head_bone.y - feet.y;
            float span_z = head_bone.z - feet.z;
            bones[0] = {head_bone.x, head_bone.y + HEAD_BONE_CENTER_LIFT, head_bone.z};
            bones[1] = {feet.x + span_x * CHEST_FRACTION,  feet.y + span_y * CHEST_FRACTION,  feet.z + span_z * CHEST_FRACTION};
            bones[2] = {feet.x + span_x * PELVIS_FRACTION, feet.y + span_y * PELVIS_FRACTION, feet.z + span_z * PELVIS_FRACTION};
        } else {
            bones[0] = {feet.x, feet.y + BONE_HEAD_HEIGHT,   feet.z};
            bones[1] = {feet.x, feet.y + BONE_CHEST_HEIGHT,  feet.z};
            bones[2] = {feet.x, feet.y + BONE_PELVIS_HEIGHT, feet.z};
        }
        for (int b = 0; b < 3; ++b) {
            Vec2 sc{};
            bool ok = w2s(vp, bones[b], sw, sh, sc, false);
            ok = ok && std::isfinite(sc.x) && std::isfinite(sc.y);
            switch (b) {
                case 0: aim.head_x = sc.x;   aim.head_y = sc.y;    aim.head_ok = ok;   break;
                case 1: aim.chest_x = sc.x;  aim.chest_y = sc.y;   aim.chest_ok = ok;  break;
                default: aim.pelvis_x = sc.x; aim.pelvis_y = sc.y; aim.pelvis_ok = ok; break;
            }
        }
        g_aim_targets.push_back(aim);
    }

    return result;
}

std::vector<EspAimTarget> esp_get_aim_targets() {
    return g_aim_targets;
}

// ADS detection straight from the dump chain:
// PlayerManager.fpManager (0x90) -> FPManager._currentWeapon (0x50, FPObject)
// -> normalFOV (0x80) / aimFOV (0x84); the live FOV comes from the native
// camera (libunity.so, +0x40 - the same field GetProjectionMatrix consumes).
// Aiming zooms the camera toward aimFOV, so "fov <= midpoint" is a stable,
// deterministic ADS test with no runtime tuning.
bool esp_is_local_aiming() {
    if (g_pid <= 0 || !g_il2cpp_base) return false;
    uint64_t local = resolve_local_player();
    if (!local) return false;
    uint64_t fp_manager = rd_ptr(local + PLAYER_FP_MANAGER_FIELD);
    if (!fp_manager) return false;
    uint64_t weapon = rd_ptr(fp_manager + FP_MANAGER_CURRENT_WEAPON_FIELD);
    if (!weapon) return false;
    int32_t normal_fov = rd<int32_t>(weapon + FP_OBJECT_NORMAL_FOV_FIELD);
    int32_t aim_fov = rd<int32_t>(weapon + FP_OBJECT_AIM_FOV_FIELD);
    // Plausibility: a real weapon zooms (aim < normal) within sane FOV bounds.
    if (normal_fov <= aim_fov || normal_fov > 130 || aim_fov < 5) return false;
    uint64_t native_cam = resolve_native_camera();
    if (!native_cam) return false;
    float fov = rd<float>(native_cam + NATIVE_CAMERA_FOV);
    if (!std::isfinite(fov) || fov <= 0.0F) return false;
    float midpoint = (float)(normal_fov + aim_fov) * 0.5F;
    return fov <= midpoint;
}
