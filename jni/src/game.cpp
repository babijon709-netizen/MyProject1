#include "game.h"
#include "game_offsets.h"
#include "Vector.h"

#include <string.h>
#include <sys/uio.h>
#include <stdio.h>
#include <stdarg.h>
#include <algorithm>
#include <cmath>
#include <chrono>
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

// Aim targets computed by esp_get_boxes for the current frame.
static std::vector<EspAimTarget> g_aim_targets;
static std::vector<EspBox> esp_get_boxes_impl(int overlay_width, int overlay_height);

// --- frame diagnostics ---------------------------------------------------------
// Filled during every esp_get_boxes call; rendered by the overlay so the live
// resolution chain (lists -> snapshot -> position field -> camera -> bones)
// can be seen on screen without any external tools.
static char g_debug_text[1024];
static size_t g_debug_len = 0;

static void dbg_clear() { g_debug_len = 0; g_debug_text[0] = '\0'; }

static void dbg_line(const char* fmt, ...) {
    if (g_debug_len + 160 >= sizeof(g_debug_text)) return;
    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(g_debug_text + g_debug_len, sizeof(g_debug_text) - g_debug_len, fmt, args);
    va_end(args);
    if (written < 0) return;
    size_t used = (size_t)written;
    if (g_debug_len + used + 1 >= sizeof(g_debug_text)) used = sizeof(g_debug_text) - g_debug_len - 1;
    g_debug_text[g_debug_len + used] = '\n';
    g_debug_len += used + 1;
    g_debug_text[g_debug_len] = '\0';
}

bool esp_get_debug_text(char* out, int cap) {
    if (!out || cap <= 0 || g_debug_len == 0) return false;
    int n = (int)g_debug_len < cap - 1 ? (int)g_debug_len : cap - 1;
    memcpy(out, g_debug_text, (size_t)n);
    out[n] = '\0';
    return true;
}

// Dead reckoning: the managed position field is a tick snapshot and can lag
// running players by meters. Per entity we estimate velocity from consecutive
// value changes and extrapolate by the time elapsed since the last change.
// Everything is bounded: the horizon is a fraction of the observed update
// interval (<=0.4s), speed is capped, and teleports zero the velocity - so the
// worst case is the raw snapshot, never garbage.
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

// --- Player lists -------------------------------------------------------------
// PlayerManager holds THREE player collections (dump.cs):
//   sleepingPlayerList (static 0x0) : FZ<PlayerManager>
//   activePlayerList   (static 0x8) : FZ<PlayerManager>
//   clientPlayerList   (static 0x10): List<PlayerManager>
// FZ<T> is a custom collection (wrapper over JD<T>: count 0x10, items 0x18),
// NOT System.List - the kind is detected by the runtime class name.
enum class ListKind { None, StdList, Fz };

static ListKind detect_list_kind(uint64_t list) {
    if (!list) return ListKind::None;
    uint64_t klass = rd_ptr(list);
    if (!klass) return ListKind::None;
    if (remote_string_equals(rd_ptr(klass + 0x10), "List`1")) return ListKind::StdList;
    if (remote_string_equals(rd_ptr(klass + 0x10), "FZ`1")) return ListKind::Fz;
    return ListKind::None;
}

static const char* list_kind_name(uint64_t list) {
    switch (detect_list_kind(list)) {
        case ListKind::StdList: return "L";
        case ListKind::Fz: return "F";
        default: return "?";
    }
}

// Resolves (data pointer = first element address, count) for any supported kind.
static bool list_header(uint64_t list, uint64_t& data, int32_t& count) {
    data = 0; count = 0;
    switch (detect_list_kind(list)) {
        case ListKind::StdList: {
            uint64_t items = rd_ptr(list + IL2CPP_LIST_ITEMS);
            int32_t n = rd<int32_t>(list + IL2CPP_LIST_SIZE);
            if (!items || n < 0 || n > 512) return false;
            data = items + IL2CPP_ARRAY_FIRST_ELEMENT;
            count = n;
            return true;
        }
        case ListKind::Fz: {
            uint64_t jd = rd_ptr(list + FZ_JD_FIELD);
            if (!jd) return false;
            int32_t n = rd<int32_t>(jd + JD_COUNT_FIELD);
            uint64_t items = rd_ptr(jd + JD_ITEMS_FIELD);
            if (!items || n < 0 || n > 512) return false;
            data = items + IL2CPP_ARRAY_FIRST_ELEMENT;
            count = n;
            return true;
        }
        default:
            return false;
    }
}

// A collection whose checked elements are all live PlayerManager instances.
static bool list_valid_with_players(uint64_t list, int32_t max_check) {
    uint64_t data = 0; int32_t count = 0;
    if (!list_header(list, data, count)) return false;
    if (count == 0) return false;
    int32_t checked = 0;
    for (int32_t index = 0; index < count && checked < max_check; ++index) {
        uint64_t player = rd_ptr(data + (uint64_t)index * sizeof(uint64_t));
        if (!player) continue;
        if (rd_ptr(player) != g_player_manager_class) return false;
        ++checked;
    }
    return checked > 0;
}

static void collect_list_players(uint64_t list, std::unordered_set<uint64_t>& unique, std::vector<uint64_t>& out) {
    uint64_t data = 0; int32_t count = 0;
    if (!list_header(list, data, count)) return;
    for (int32_t index = 0; index < count; ++index) {
        uint64_t player = rd_ptr(data + (uint64_t)index * sizeof(uint64_t));
        if (!player) continue;
        if (rd_ptr(player) != g_player_manager_class) continue;
        if (unique.insert(player).second) out.push_back(player);
    }
}

static bool resolve_player_snapshot(std::vector<uint64_t>& snapshot) {
    if (!ensure_player_manager_class()) return false;
    if (!g_player_manager_static_fields)
        g_player_manager_static_fields = get_class_static_fields(g_player_manager_class);
    if (!g_player_manager_static_fields) return false;

    const uint64_t list_offsets[3] = {
        PLAYER_MANAGER_STATIC_FIELDS_ACTIVE,   // activePlayerList   (0x8, FZ)
        PLAYER_MANAGER_STATIC_FIELDS_LIST,     // clientPlayerList   (0x10, List)
        PLAYER_MANAGER_STATIC_FIELDS_SLEEPING, // sleepingPlayerList (0x0, FZ)
    };
    bool any_resolved = false;
    for (uint64_t off : list_offsets) {
        uint64_t list = rd_ptr(g_player_manager_static_fields + off);
        if (!list) continue;
        uint64_t data = 0; int32_t count = 0;
        if (!list_header(list, data, count)) continue; // unknown kind or garbage
        any_resolved = true;
        if (count > 0 && list_valid_with_players(list, 4)) break;
    }
    if (!any_resolved) return false;

    std::unordered_set<uint64_t> unique;
    snapshot.clear();
    uint64_t local = resolve_local_player();
    if (local && rd_ptr(local) == g_player_manager_class) {
        snapshot.push_back(local); // local player is always snapshot[0]
        unique.insert(local);
    }
    for (uint64_t off : list_offsets)
        collect_list_players(rd_ptr(g_player_manager_static_fields + off), unique, snapshot);
    return !snapshot.empty();
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

static bool try_read_transform_world(uint64_t native_transform, Vec3& pos, Vec4& rot) {
    if (!native_transform) return false;
    uint64_t transform_data = rd_ptr(native_transform + 0x38);
    int32_t transform_index = rd<int32_t>(native_transform + 0x40);
    if (!transform_data || transform_index < 0 || transform_index > 100000) return false;
    uint64_t matrices = rd_ptr(transform_data + 0x18);
    uint64_t indices = rd_ptr(transform_data + 0x20);
    return read_transform_hierarchy_arrays(matrices, indices, transform_index, pos, &rot);
}

static uint64_t resolve_player_native_transform(uint64_t player) {
    if (!player) return 0;
    return resolve_native_transform(rd_ptr(player + PLAYER_TRANSFORM));
}

// --- Real skeleton bones ------------------------------------------------------
// PlayerManager.inventory -> _playerInventoryData -> playerModelInfo holds the
// third-person model's head/body Transforms. Their live world positions follow
// the actual animation pose (crouch, lean, jump) and are INDEPENDENT of the
// position field - which makes them the cross-validator for binding that field.
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
    Vec3 head{}; Vec4 head_rot{};
    if (!try_read_transform_world(head_native, head, head_rot)) return false;
    if (!vec3_is_finite(head)) return false;
    float dx = head.x - anchor.x, dy = head.y - anchor.y, dz = head.z - anchor.z;
    if (dx * dx + dy * dy + dz * dz > 9.0F) return false; // >3m from the anchor - not this player's model
    head_out = head;
    return true;
}

// --- Position field binding ----------------------------------------------------
// The installed game build can be newer than the dump, shifting PlayerManager's
// Vector3 field offsets. All SIX documented position-like fields (lastTick/
// lastSaved/lastDeath positions, two private positions, originalPosition) are
// tried and the live one is bound ONCE by cross-validating against the
// skeleton: a real feet position must sit under the player's independently-read
// head bone, and real players must be near the camera eye. Re-bound only when
// the bound field loses all evidence (respawn of the layout, game update).
static uint64_t g_position_offset = 0;          // 0 => hierarchy-transform mode
static int      g_position_rebind_cooldown = 0;

static bool read_head_bone_at(uint64_t native_transform, Vec3& position) {
    uint64_t transform_data = rd_ptr(native_transform + 0x38);
    int32_t transform_index = rd<int32_t>(native_transform + 0x40);
    if (!transform_data || transform_index < 0 || transform_index > 100000) return false;
    uint64_t matrices = rd_ptr(transform_data + 0x18);
    uint64_t indices = rd_ptr(transform_data + 0x20);
    return read_transform_hierarchy_arrays(matrices, indices, transform_index, position);
}

static const uint64_t kPositionFieldCandidates[6] = {
    PLAYER_POSITION,        // 0x1D0 lastTickPosition
    PLAYER_POSITION + 0xC,  // 0x1DC lastSavedPosition
    PLAYER_POSITION + 0x18, // 0x1E8 lastDeathPosition
    0x2D8,                  // qEY
    0x2E4,                  // qEX
    0x338,                  // originalPosition
};

static float vec3_distance(const Vec3& a, const Vec3& b) {
    float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

static uint64_t select_position_field(const std::vector<uint64_t>& players, const Vec3& eye, bool has_eye,
                                      int* out_agree, int* out_near) {
    int best_index = -1;
    double best_score = 0.0;
    int best_agreements = 0;
    int best_near = 0;
    for (int i = 0; i < 6; ++i) {
        int agreements = 0, near_eye = 0;
        for (uint64_t player : players) {
            Vec3 p = rd_v3(player + kPositionFieldCandidates[i]);
            if (!vec3_is_finite(p)) continue;
            Vec3 head{};
            if (read_player_head_bone(player, p, head)) ++agreements; // head exists within 3m of this field's feet
            if (has_eye && vec3_distance(p, eye) <= MAX_PLAYER_DISTANCE) ++near_eye;
        }
        double score = (double)agreements * 10000.0 + (double)near_eye * 10.0 + (i == 0 ? 1.0 : 0.0);
        if (score > best_score || best_index < 0) {
            best_score = score;
            best_index = i;
            best_agreements = agreements;
            best_near = near_eye;
        }
    }
    if (out_agree) *out_agree = best_index >= 0 ? best_agreements : -1;
    if (out_near) *out_near = best_index >= 0 ? best_near : -1;
    if (best_index < 0) return 0;
    if (best_agreements > 0) return kPositionFieldCandidates[best_index];
    // No skeleton evidence at all (models not loaded): accept the field only on
    // strong eye-proximity evidence; otherwise fall back to the dump default.
    bool near_evidence = has_eye && best_score > 10.0;
    return near_evidence ? kPositionFieldCandidates[best_index] : PLAYER_POSITION;
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

static bool matrix_is_finite(const Mat4& matrix) {
    bool has_non_zero = false;
    for (float value : matrix.m) {
        if (!std::isfinite(value) || fabsf(value) > 1000000.0F) return false;
        if (fabsf(value) > 0.000001F) has_non_zero = true;
    }
    return has_non_zero;
}

// A frustum projection must scale both screen axes and must not be the
// identity placeholder that m_OriginalProjectionMatrix starts out as.
static bool projection_structurally_valid(const Mat4& matrix) {
    if (!matrix_is_finite(matrix)) return false;
    if (fabsf(matrix.m[0]) < 0.0001F || fabsf(matrix.m[5]) < 0.0001F) return false;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            float expected = (i == j) ? 1.0F : 0.0F;
            if (fabsf(matrix.m[(size_t)i * 4 + j] - expected) > 0.0001F) return true; // deviates from identity
        }
    return false; // exact identity/zero - not a frustum
}

static bool matrix_is_identity(const Mat4& matrix) {
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            float expected = (i == j) ? 1.0F : 0.0F;
            if (fabsf(matrix.m[(size_t)i * 4 + j] - expected) > 0.0001F) return false;
        }
    return true;
}

static void mat_transpose_inplace(Mat4& matrix) {
    for (int i = 0; i < 4; ++i)
        for (int j = i + 1; j < 4; ++j)
            std::swap(matrix.m[(size_t)i * 4 + j], matrix.m[(size_t)j * 4 + i]);
}

// Y clip scale of a perspective frustum is cot(fovY / 2); it sits at the (1,1)
// diagonal entry, which occupies mem[5] in EITHER storage order, so this
// scores a projection against the live camera FOV without assuming a layout.
static float projection_fov_mismatch(const Mat4& projection, float fov_deg) {
    if (!std::isfinite(fov_deg) || fov_deg <= 1.0F || fov_deg >= 179.0F) return 0.0F;
    float cot = 1.0F / tanf(fov_deg * (3.14159265F / 180.0F) * 0.5F);
    float y = projection.m[5];
    if (!std::isfinite(y) || fabsf(y) < 0.001F) return INFINITY;
    return fabsf(y - cot) / cot;
}

// GL-style perspective built from the camera FOV, stored ROW-major (native
// unity::math::Matrix4x4f convention). Rows 0/1/3 are exact; row 2 (near/far
// depth mapping) is unused by the world-to-screen math.
static bool synthesize_projection(float fov_deg, float aspect, Mat4& out) {
    if (!std::isfinite(fov_deg) || fov_deg <= 1.0F || fov_deg >= 179.0F) return false;
    if (!std::isfinite(aspect) || aspect < 0.2F || aspect > 8.0F) return false;
    float cot = 1.0F / tanf(fov_deg * (3.14159265F / 180.0F) * 0.5F);
    float near_z = 0.3F, far_z = 1000.0F;
    float c22 = -(far_z + near_z) / (far_z - near_z);  // math (2,2)
    float c23 = -2.0F * far_z * near_z / (far_z - near_z); // math (2,3)
    out = Mat4{};
    out.m[0] = cot / aspect; // math (0,0)
    out.m[5] = cot;          // math (1,1)
    out.m[10] = c22;         // math (2,2)
    out.m[11] = c23;         // math (2,3)
    out.m[14] = -1.0F;       // math (3,2): w-row = (0,0,-1,0) -> clip_w = -z_view
    return true;
}

// Storage-order votes over the 64-byte native camera buffers. The GL-style
// perspective has its w-row z coefficient (-1) at MATH position (row3,col2):
// row-major memory puts it at mem[14], column-major at mem[11]. Native
// unity::math::Matrix4x4f is row-major, which is the expected answer.
// (+1 variants cover reversed-Z depth matrices.)
static int storage_vote_projection(const Mat4& p) {
    auto near_value = [](float v, float target) { return fabsf(v - target) <= 0.02F; };
    bool row_major = near_value(p.m[14], -1.0F) || near_value(p.m[14], 1.0F);
    bool col_major = near_value(p.m[11], -1.0F) || near_value(p.m[11], 1.0F);
    if (row_major == col_major) return 0;
    return col_major ? 1 : -1;
}

struct CameraMatrices {
    Mat4 view{};
    Mat4 projection{};
    Vec3 eye{};
    bool has_eye = false;
    char view_src[12] = "-";   // "pose", "cache", "root"
    char proj_src[12] = "-";   // "live", "nonjit", "orig", "synth"
};

// View matrix (GL-style, camera looks down -Z view space, row-major) built
// from the camera transform's world position + rotation.
static Mat4 view_from_camera_pose(const Vec3& position, const Vec4& rotation) {
    Vec3 right = rotate_vector(rotation, {1.0F, 0.0F, 0.0F});
    Vec3 up    = rotate_vector(rotation, {0.0F, 1.0F, 0.0F});
    Vec3 fwd   = rotate_vector(rotation, {0.0F, 0.0F, 1.0F});
    Mat4 view{};
    view.m[0] =  right.x; view.m[1] =  right.y; view.m[2] =  right.z; view.m[3] = -(right.x * position.x + right.y * position.y + right.z * position.z);
    view.m[4] =  up.x;    view.m[5] =  up.y;    view.m[6] =  up.z;    view.m[7] = -(up.x * position.x + up.y * position.y + up.z * position.z);
    view.m[8] = -fwd.x;   view.m[9] = -fwd.y;   view.m[10] = -fwd.z;  view.m[11] = (fwd.x * position.x + fwd.y * position.y + fwd.z * position.z);
    view.m[12] = 0.0F;    view.m[13] = 0.0F;    view.m[14] = 0.0F;    view.m[15] = 1.0F;
    return view;
}

// --- Camera transform discovery ------------------------------------------------
// The engine computes the view fresh from the camera transform for its own
// WorldToScreenPoint; the +0x70 managed-facing cache can stay identity when no
// C# code touches worldToCameraMatrix. So we locate the camera's native
// Transform* inside the native camera object (validated by resolving its world
// position through the transform hierarchy and, when the local player is
// known, by requiring it to sit near the player's eye) and cache the offset.
static bool likely_native_ptr(uint64_t value) {
    return value >= 0x10000 && value < 0x0001000000000000ULL && (value & 0x7) == 0;
}

static uint64_t g_camera_transform = 0;
static uint64_t g_camera_transform_offset = 0;
static int      g_camera_transform_backoff = 0;

static uint64_t find_camera_transform(uint64_t native_cam, const Vec3& eye_hint, bool has_hint) {
    uint64_t fallback = 0;
    for (uint64_t off = 0x8; off <= 0x600; off += 8) {
        uint64_t candidate = rd_ptr(native_cam + off);
        if (!likely_native_ptr(candidate) || candidate == native_cam) continue;
        Vec3 pos{}; Vec4 rot{};
        if (!try_read_transform_world(candidate, pos, rot)) continue;
        if (has_hint) {
            float dx = pos.x - eye_hint.x, dy = pos.y - eye_hint.y, dz = pos.z - eye_hint.z;
            if (dx * dx + dy * dy + dz * dz > 64.0F) continue; // >8m from the player's eye - not the view camera
            g_camera_transform = candidate;
            g_camera_transform_offset = off;
            return candidate;
        }
        if (!fallback) fallback = candidate; // keep first hierarchy-valid candidate as a weaker option
    }
    if (fallback) {
        // Remember only the offset-resolved pair is safe to reuse; an
        // unhinted fallback must be re-validated every frame by the caller.
        g_camera_transform = fallback;
        g_camera_transform_offset = 0; // mark as unanchored: re-resolve via scan
    }
    return fallback;
}

// Resolves the camera transform (cached) and its current world pose.
static bool resolve_camera_pose(uint64_t native_cam, const Vec3& eye_hint, bool has_hint, Vec3& pos, Vec4& rot) {
    if (g_camera_transform_backoff > 0) --g_camera_transform_backoff;

    if (g_camera_transform) {
        if (g_camera_transform_offset) {
            uint64_t current = rd_ptr(native_cam + g_camera_transform_offset);
            if (current != g_camera_transform) g_camera_transform = 0;
        }
        if (g_camera_transform && !try_read_transform_world(g_camera_transform, pos, rot)) {
            g_camera_transform = 0;
        }
    }
    if (!g_camera_transform && g_camera_transform_backoff <= 0) {
        g_camera_transform_backoff = 300; // ~5s at 60fps between scans
        find_camera_transform(native_cam, eye_hint, has_hint);
        if (g_camera_transform) {
            if (!try_read_transform_world(g_camera_transform, pos, rot)) g_camera_transform = 0;
        }
    }
    return g_camera_transform != 0;
}

static bool read_camera_matrices(uint64_t native_cam, const Vec3& eye_hint, bool has_hint,
                                 float screen_width, float screen_height, CameraMatrices& out) {
    if (!native_cam) return false;
    float fov = rd<float>(native_cam + NATIVE_CAMERA_FOV);

    // --- projection: best FOV-matching candidate, live cache first ---
    static const uint64_t candidate_offsets[3] = {
        NATIVE_CAMERA_PROJECTION_MATRIX,
        NATIVE_CAMERA_NON_JITTERED_PROJECTION,
        NATIVE_CAMERA_ORIGINAL_PROJECTION,
    };
    Mat4 projection{};
    bool have_projection = false;
    float best_mismatch = INFINITY;
    int projection_vote = 0;
    int choice_proj = -1;
    for (int i = 0; i < 3; ++i) {
        Mat4 candidate = rd_m4(native_cam + candidate_offsets[i]);
        if (!projection_structurally_valid(candidate)) continue;
        float mismatch = projection_fov_mismatch(candidate, fov);
        if (mismatch < best_mismatch) {
            best_mismatch = mismatch;
            projection = candidate;
            projection_vote = storage_vote_projection(candidate);
            have_projection = true;
            choice_proj = i;
        }
    }
    bool synthesized = false;
    if (!have_projection) {
        // Every cached projection is unusable: rebuild one from the live FOV.
        // The overlay shares the game's screen, so its w/h ratio is the aspect.
        float aspect = (std::isfinite(screen_height) && screen_height >= 100.0F) ? screen_width / screen_height : 0.0F;
        if (!synthesize_projection(fov, aspect, projection)) return false;
        synthesized = true;
        snprintf(out.proj_src, sizeof(out.proj_src), "synth");
    } else {
        snprintf(out.proj_src, sizeof(out.proj_src), "%s",
                 choice_proj == 0 ? "live" : (choice_proj == 1 ? "nonjit" : "orig"));
        if (projection_vote > 0) {
            mat_transpose_inplace(projection); // buffer was column-major
        }
        // projection_vote < 0 (or 0): already row-major math, use as-is.
    }

    // --- view ---
    Vec3 cam_pos{}; Vec4 cam_rot{};
    Mat4 view{};
    bool view_ok = false;
    if (resolve_camera_pose(native_cam, eye_hint, has_hint, cam_pos, cam_rot)) {
        view = view_from_camera_pose(cam_pos, cam_rot);
        out.eye = cam_pos;
        out.has_eye = true;
        snprintf(out.view_src, sizeof(out.view_src), "pose@%llx",
                 (unsigned long long)(g_camera_transform_offset ? g_camera_transform_offset : 0));
        view_ok = true;
    }
    if (!view_ok) {
        // Managed-facing worldToCameraMatrix cache. Only trusted when it is
        // neither identity (never refreshed) nor zero. Same storage order as
        // the projection buffers of the same native object.
        Mat4 cached = rd_m4(native_cam + NATIVE_CAMERA_VIEW_MATRIX);
        if (matrix_is_finite(cached) && !matrix_is_identity(cached)) {
            view = cached;
            view_ok = true;
            int vote = synthesized ? storage_vote_projection(view) : projection_vote;
            if (vote > 0) mat_transpose_inplace(view); // buffer was column-major
            snprintf(out.view_src, sizeof(out.view_src), "cache");
        }
    }
    if (!view_ok) {
        // Last resort: the local player's worldCameraRoot (the third-person
        // camera rig) - the pre-offsets approach that rendered correctly.
        uint64_t local = resolve_local_player();
        if (local) {
            uint64_t root = resolve_player_native_transform(local);
            Vec3 pos{}; Vec4 rot{};
            if (root && try_read_transform_world(root, pos, rot)) {
                view = view_from_camera_pose(pos, rot);
                out.eye = pos;
                out.has_eye = true;
                snprintf(out.view_src, sizeof(out.view_src), "root");
                view_ok = true;
            }
        }
    }
    if (!view_ok) return false;

    out.view = view;
    out.projection = projection;
    return true;
}

static float mat_get(const Mat4& matrix, int row, int column) {
    return matrix.m[(size_t)row * 4 + column];
}
static Mat4 mat_mul(const Mat4& a, const Mat4& b) {
    Mat4 result{};
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column) {
            float value = 0.0F;
            for (int k = 0; k < 4; ++k) value += mat_get(a, row, k) * mat_get(b, k, column);
            result.m[(size_t)row * 4 + column] = value;
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
static bool read_hierarchy_position(uint64_t player, Vec3& position) {
    uint64_t native = resolve_player_native_transform(player);
    if (!native) return false;
    Vec3 hierarchy{}; Vec4 hierarchy_rot{};
    if (!try_read_transform_world(native, hierarchy, hierarchy_rot) || !vec3_is_finite(hierarchy)) return false;
    position = hierarchy;
    return true;
}

static bool read_entity_position(uint64_t source, Vec3& position) {
    if (!source) return false;
    if (g_position_offset) {
        position = rd_v3(source + g_position_offset);
        if (vec3_is_finite(position)) {
            apply_dead_reckoning(source, position);
            return true;
        }
    }
    // Hierarchy-transform mode / fallback: the player's world render transform.
    if (!read_hierarchy_position(source, position)) return false;
    apply_dead_reckoning(source, position);
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
    g_camera_transform = 0; g_camera_transform_offset = 0; g_camera_transform_backoff = 0;
    g_position_offset = 0; g_position_rebind_cooldown = 0;
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
    dbg_clear();

    if (g_pid <= 0 || !g_il2cpp_base) { return result; }

    float sw = overlay_width >= 100 ? (float)overlay_width : 1080.0F;
    float sh = overlay_height >= 100 ? (float)overlay_height : 2400.0F;
    if (!std::isfinite(sw) || sw < 100.0F || sw > 10000.0F) sw = 1080.0F;
    if (!std::isfinite(sh) || sh < 100.0F || sh > 10000.0F) sh = 2400.0F;

    // Player snapshot from all three statics lists (local player first).
    static std::vector<uint64_t> s_transforms;
    {
        std::vector<uint64_t> refreshed;
        if (resolve_player_snapshot(refreshed) && !refreshed.empty())
            s_transforms = std::move(refreshed);
    }
    if (s_transforms.empty()) { dbg_line("lists: snapshot EMPTY"); return result; }
    {
        uint64_t slp = rd_ptr(g_player_manager_static_fields + PLAYER_MANAGER_STATIC_FIELDS_SLEEPING);
        uint64_t act = rd_ptr(g_player_manager_static_fields + PLAYER_MANAGER_STATIC_FIELDS_ACTIVE);
        uint64_t cli = rd_ptr(g_player_manager_static_fields + PLAYER_MANAGER_STATIC_FIELDS_LIST);
        uint64_t d; int32_t n;
        int ns = list_header(slp, d, n) ? n : -1;
        int na = list_header(act, d, n) ? n : -1;
        int nc = list_header(cli, d, n) ? n : -1;
        dbg_line("lists: slp[%s]=%d act[%s]=%d cli[%s]=%d snap=%zu",
                 list_kind_name(slp), ns, list_kind_name(act), na, list_kind_name(cli), nc,
                 s_transforms.size());
    }

    // Eye hint independent of the (possibly unbound) position field: the local
    // player's worldCameraRoot transform.
    Vec3 eye_hint{};
    bool has_eye_hint = false;
    {
        uint64_t local_hint = resolve_local_player();
        if (local_hint) {
            Vec3 pos{}; Vec4 rot{};
            uint64_t root = resolve_player_native_transform(local_hint);
            if (root && try_read_transform_world(root, pos, rot) && vec3_is_finite(pos)) {
                eye_hint = pos;
                has_eye_hint = true;
            } else {
                Vec3 feet = rd_v3(local_hint + PLAYER_POSITION);
                if (vec3_is_finite(feet)) {
                    eye_hint = {feet.x, feet.y + 1.6F, feet.z};
                    has_eye_hint = true;
                }
            }
        }
    }

    // Camera straight from the native camera object (libunity.so offsets).
    uint64_t native_cam = resolve_native_camera();
    if (!native_cam) return result;
    CameraMatrices cam{};
    if (!read_camera_matrices(native_cam, eye_hint, has_eye_hint, sw, sh, cam)) {
        dbg_line("cam: FAILED (native=%llx)", (unsigned long long)native_cam);
        return result;
    }
    Mat4 vp = mat_mul(cam.projection, cam.view);

    Vec3 camera_position{};
    bool has_camera_position = cam.has_eye || camera_position_from_view(cam.view, camera_position);
    if (cam.has_eye) camera_position = cam.eye;
    dbg_line("cam: v=%s p=%s fov=%.0f eye=%.0f,%.0f,%.0f",
             cam.view_src, cam.proj_src, (double)rd<float>(native_cam + NATIVE_CAMERA_FOV),
             (double)camera_position.x, (double)camera_position.y, (double)camera_position.z);

    // Bind the position field once (and re-bind only when evidence is lost).
    if (!g_position_offset || --g_position_rebind_cooldown <= 0) {
        int bind_agree = -1, bind_near = -1;
        uint64_t bound = select_position_field(s_transforms, camera_position, has_camera_position, &bind_agree, &bind_near);
        if (bound) {
            if (bound != g_position_offset) g_entity_motion.clear();
            g_position_offset = bound;
        }
        g_position_rebind_cooldown = 300; // re-validate the binding every ~5s at 60fps
        dbg_line("pos: off=0x%llx agree=%d near=%d (cands 1D0/1DC/1E8/2D8/2E4/338)",
                 (unsigned long long)g_position_offset, bind_agree, bind_near);
    }

    // Local entity: snapshot[0] when statics resolved it, else the entity
    // closest to the camera eye (bounded).
    bool has_local_position = false;
    Vec3 local{};
    size_t local_entity_index = s_transforms.size();

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
    if (local_entity_index == s_transforms.size() && has_camera_position) {
        double best = 40000.0; // 200m
        for (size_t index = 0; index < s_transforms.size(); ++index) {
            Vec3 candidate{};
            if (!read_entity_position(s_transforms[index], candidate)) continue;
            double dx = (double)candidate.x - camera_position.x, dy = (double)candidate.y - camera_position.y, dz = (double)candidate.z - camera_position.z;
            double distance_squared = dx * dx + dy * dy + dz * dz;
            if (std::isfinite(distance_squared) && distance_squared < best) {
                best = distance_squared;
                local_entity_index = index;
                local = candidate;
            }
        }
    }
    has_local_position = local_entity_index != s_transforms.size();
    dbg_line("local: idx=%zu of %zu (statics=%d)", local_entity_index, s_transforms.size(), local_ptr ? 1 : 0);
    if (!has_local_position) { dbg_line("local: NONE -> no boxes"); return result; }

    int dbg_pos_ok = 0, dbg_pos_fail = 0, dbg_bones_ok = 0;
    for (size_t i = 0; i < s_transforms.size(); ++i) {
        if (i == local_entity_index) continue;
        if (!s_transforms[i]) continue;
        Vec3 feet{};
        if (!read_entity_position(s_transforms[i], feet)) { ++dbg_pos_fail; continue; }
        ++dbg_pos_ok;

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
        if (head_bone_ok) ++dbg_bones_ok;
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

    dbg_line("ents: pos ok=%d fail=%d bones=%d boxes=%zu", dbg_pos_ok, dbg_pos_fail, dbg_bones_ok, result.size());
    // raw field dump for the first non-local entity (position binding evidence)
    for (size_t i = 0; i < s_transforms.size(); ++i) {
        if (i == local_entity_index || !s_transforms[i]) continue;
        Vec3 a = rd_v3(s_transforms[i] + 0x1D0);
        Vec3 b = rd_v3(s_transforms[i] + 0x338);
        dbg_line("ent[%zu] 1D0=%.0f,%.0f,%.0f 338=%.0f,%.0f,%.0f", i,
                 (double)a.x, (double)a.y, (double)a.z, (double)b.x, (double)b.y, (double)b.z);
        break;
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
