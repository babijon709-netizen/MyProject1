// transform.cpp — Иерархия Transform: где у объекта позиция и как её читать.
//
// Модуль разрезан из прежнего монолита jni/src/game.cpp;
// что здесь лежит и кто это зовёт — в шапке transform.h и в docs/CODE_MAP.md.

#include "esp/common.h"
#include "esp/aim_points.h"
#include "esp/boxes.h"
#include "esp/camera.h"
#include "esp/farm_scan.h"
#include "esp/frame.h"
#include "esp/il2cpp.h"
#include "esp/markers.h"
#include "esp/math.h"
#include "esp/melee.h"
#include "esp/mem.h"
#include "esp/player_pose.h"
#include "esp/skeleton_build.h"
#include "esp/skeleton_cache.h"
#include "esp/skeleton_names.h"
#include "transform.h"
#include "app/diag_log.h"

TransformHierarchyLayout g_transform_hierarchy_layout{};

bool g_transform_hierarchy_layout_valid = false;

bool      g_use_direct_player_position = true;

bool      g_player_position_validated = false;

bool read_transform_hierarchy_arrays(uint64_t matrices, uint64_t indices, int32_t transform_index, Vec3& position, Vec4* world_rotation) {
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

bool read_transform_hierarchy_layout(uint64_t native_transform, const TransformHierarchyLayout& layout, Vec3& position, Vec4* world_rotation) {
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

bool position_looks_like_world_space(const Vec3& position); // defined below

// Разбор адресов иерархии по известной раскладке. Проверка — полным чтением
// позы: подходит только та пара массивов, по которой узел действительно
// читается (иначе «нашли» бы адрес случайного поля).
static bool resolve_arrays_by_layout(uint64_t native_transform, const TransformHierarchyLayout& layout,
                                     int32_t& transform_index, uint64_t& matrices, uint64_t& indices,
                                     Vec3* pose_out) {
    uint64_t transform_data = rd_ptr(native_transform + layout.data_offset);
    transform_index = rd<int32_t>(native_transform + layout.index_offset);
    if (!transform_data || transform_index < 0 || transform_index > 100000) return false;
    matrices = rd_ptr(transform_data + layout.matrices_offset);
    indices  = rd_ptr(transform_data + layout.indices_offset);
    if (layout.matrices_indirect) matrices = rd_ptr(matrices);
    if (layout.indices_indirect)  indices  = rd_ptr(indices);
    if (!matrices || !indices) return false;
    Vec3 probe{};
    if (!read_transform_hierarchy_arrays(matrices, indices, transform_index, probe)) return false;
    if (pose_out) *pose_out = probe;
    return true;
}

// Тот же перебор, что у read_camera_transform_pose: TransformAccess живёт на
// +0x38/+0x40 у старых сборок и на +0x18/+0x20 у этой. Нужен, когда выученная
// раскладка ещё не известна или умерла со сменой мира.
static bool resolve_arrays_by_probe(uint64_t native_transform, int32_t& transform_index,
                                    uint64_t& matrices, uint64_t& indices, Vec3* pose_out) {
    uint64_t transform_data = rd_ptr(native_transform + 0x38);
    transform_index = rd<int32_t>(native_transform + 0x40);
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
        for (uint64_t candidate_matrices : matrix_candidates) {
            for (uint64_t candidate_indices : index_candidates) {
                Vec3 probe{};
                if (!read_transform_hierarchy_arrays(candidate_matrices, candidate_indices, transform_index, probe)) continue;
                matrices = candidate_matrices; indices = candidate_indices;
                if (pose_out) *pose_out = probe;
                return true;
            }
        }
    }
    return false;
}

bool resolve_transform_arrays(uint64_t native_transform, int32_t& transform_index,
                              uint64_t& matrices, uint64_t& indices) {
    if (!native_transform) return false;
    if (g_transform_hierarchy_layout_valid &&
        resolve_arrays_by_layout(native_transform, g_transform_hierarchy_layout, transform_index, matrices, indices, nullptr))
        return true;
    return resolve_arrays_by_probe(native_transform, transform_index, matrices, indices, nullptr);
}

static bool read_local_matrix34(uint64_t native_transform, Matrix34& out) {
    int32_t transform_index = -1;
    uint64_t matrices = 0, indices = 0;
    if (!resolve_transform_arrays(native_transform, transform_index, matrices, indices)) return false;
    if (!rd_exact(matrices + (uint64_t)transform_index * sizeof(Matrix34), out)) return false;
    return matrix34_is_valid(out);
}

bool read_transform_local_rotation(uint64_t native_transform, Vec4& local_rotation) {
    Matrix34 matrix{};
    if (!read_local_matrix34(native_transform, matrix)) return false;
    Vec4 rotation = matrix.rotation;
    if (!normalize_quaternion(rotation)) return false;
    local_rotation = rotation;
    return true;
}

bool read_transform_parent_world_rotation(uint64_t native_transform, Vec4& parent_world_rotation) {
    int32_t transform_index = -1;
    uint64_t matrices = 0, indices = 0;
    if (!resolve_transform_arrays(native_transform, transform_index, matrices, indices)) return false;
    Vec4 accumulated = {0.f, 0.f, 0.f, 1.f};
    int32_t parent = -2;
    if (!rd_exact(indices + (uint64_t)transform_index * sizeof(int32_t), parent)) return false;
    int32_t previous_parent = transform_index;
    int depth = 0;
    while (parent >= 0 && depth++ < 128) {
        if (parent > 100000 || parent == previous_parent) return false;
        Matrix34 matrix{};
        if (!rd_exact(matrices + (uint64_t)parent * sizeof(Matrix34), matrix) || !matrix34_is_valid(matrix)) return false;
        accumulated = multiply_quaternion(matrix.rotation, accumulated);
        previous_parent = parent;
        if (!rd_exact(indices + (uint64_t)parent * sizeof(int32_t), parent)) return false;
    }
    if (parent != -1 || depth >= 128) return false;
    if (!normalize_quaternion(accumulated)) return false;
    parent_world_rotation = accumulated;
    return true;
}

bool write_transform_local_rotation(uint64_t native_transform, const Vec4& local_rotation) {
    int32_t transform_index = -1;
    uint64_t matrices = 0, indices = 0;
    if (!resolve_transform_arrays(native_transform, transform_index, matrices, indices)) return false;
    Vec4 rotation = local_rotation;
    if (!normalize_quaternion(rotation)) return false;
    // Пишем ровно ту запись, которую читает поза: Matrix34 — это
    // {translation, rotation, scale} (см. Vector.h), локальный поворот лежит
    // вторым полем.
    const uint64_t address = matrices + (uint64_t)transform_index * sizeof(Matrix34)
                           + offsetof(Matrix34, rotation);
    return wr_buf(address, &rotation, sizeof(Vec4));
}

bool read_transform_hierarchy_position(uint64_t native_transform, Vec3& position) {
    if (!native_transform) return false;
    // The learned layout is only a fast path. It is learned from PLAYER
    // transforms and dies with a world reload — after a respawn it fails (or
    // reads garbage) for every entity. Returning its result directly here
    // was the solo-markers-after-respawn bug: the self-probing fallback
    // below (which needs no players and no learning) was never reached.
    if (g_transform_hierarchy_layout_valid) {
        int32_t transform_index = -1;
        uint64_t matrices = 0, indices = 0;
        Vec3 pose{};
        if (resolve_arrays_by_layout(native_transform, g_transform_hierarchy_layout,
                                     transform_index, matrices, indices, &pose) &&
            vec3_is_finite(pose) && position_looks_like_world_space(pose)) {
            position = pose;
            return true;
        }
    }
    // Same probing as read_camera_transform_pose: TransformAccess lives at
    // +0x38/+0x40 on older builds and at +0x18/+0x20 on this one. Markers ran
    // only the first probe, which is why they worked ONLY once a nearby
    // player's skeleton had taught us the layout — the camera (with both
    // probes) worked solo all along.
    int32_t transform_index = -1;
    uint64_t matrices = 0, indices = 0;
    Vec3 pose{};
    if (!resolve_arrays_by_probe(native_transform, transform_index, matrices, indices, &pose)) return false;
    position = pose;
    return true;
}

uint64_t resolve_player_native_transform(uint64_t player) {
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

bool discover_layout_from_native_transforms(const std::vector<uint64_t>& native_transforms,
                                                   size_t& best_position_count, size_t& candidate_count) {
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

static bool discover_transform_hierarchy_layout(const std::vector<uint64_t>& players, size_t& best_position_count, size_t& candidate_count) {
    std::vector<uint64_t> native_transforms;
    std::unordered_set<uint64_t> unique_transforms;
    for (uint64_t player : players) {
        uint64_t native_transform = resolve_player_native_transform(player);
        if (native_transform && unique_transforms.insert(native_transform).second)
            native_transforms.push_back(native_transform);
    }
    return discover_layout_from_native_transforms(native_transforms, best_position_count, candidate_count);
}

bool read_entity_position(uint64_t source, Vec3& position) {
    if (!source) return false;
    if (g_use_direct_player_position && g_player_position_offset != 0) { position = rd_v3(source + g_player_position_offset); return vec3_is_finite(position); }
    uint64_t native = resolve_player_native_transform(source);
    if (!native) return false;
    return read_transform_hierarchy_position(native, position);
}

bool read_entity_pose(uint64_t source, Vec3& position, Vec4& rotation) {
    if (!source || g_use_direct_player_position || !g_transform_hierarchy_layout_valid) return false;
    uint64_t native = resolve_player_native_transform(source);
    if (!native) return false;
    return read_transform_hierarchy_layout(native, g_transform_hierarchy_layout, position, &rotation);
}

// A world position we would believe from a single sample: inside the map
// bounds and not a pile of denormals. Used when we are the only player on the
// server, where the "several players spread out" test below cannot run.
bool position_looks_like_world_space(const Vec3& position) {
    if (!vec3_is_finite(position)) return false;
    if (fabsf(position.x) > 20000.0F || fabsf(position.z) > 20000.0F) return false;
    if (fabsf(position.y) > 10000.0F) return false;
    return fabsf(position.x) + fabsf(position.y) + fabsf(position.z) > 0.01F;
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
    // Alone on the server (or everybody standing on the same spot) there is no
    // spread to measure, so a single plausible world position has to do. Not
    // accepting it used to kill the whole ESP after a solo respawn: the offset
    // never re-validated and every frame bailed out early until app restart.
    if (valid < 2 || extent < 0.1) {
        if (!position_looks_like_world_space(minimum)) return false;
        score = (double)valid * 1000000.0;
        return true;
    }
    score = (double)valid * 1000000.0 + std::min(extent, 999999.0);
    return true;
}

// Direct (PlayerManager field) position offsets, most trusted first. The
// canonical field is lastSavedPosition (0x1D0); lastTickPosition (0x1C8) is
// equivalent. The rest are legacy guesses kept as a last resort only.
static const uint64_t k_known_position_offsets[] = {0x1D0, 0x1C8, 0x1E0, 0x2D0, 0x2DC, 0x1D4, 0x1DC, 0x1E8};

// Последнее ПОДТВЕРЖДЁННОЕ смещение позиции. Живёт до перепривязки: раскладка
// класса не меняется внутри одной сборки игры, поэтому после смены мира (смерть,
// респавн) правильное смещение уже известно — искать его заново с ожиданием
// «поля допишутся» не нужно, достаточно проверить. Именно этот поиск с ожиданием
// и был тем «боксы пропали на секунду и вернулись» после респавна.
static uint64_t s_last_good_offset = 0;

void reset_player_position_memory() { s_last_good_offset = 0; }

static uint64_t find_direct_player_position_offset(const std::vector<uint64_t>& players) {
    bool saved_use_direct = g_use_direct_player_position;
    g_use_direct_player_position = true;
    uint64_t best_offset = 0;
    double best_score = 0.0;
    for (uint64_t offset : k_known_position_offsets) {
        double score = 0.0;
        if (!evaluate_player_position_offset(players, offset, score)) continue;
        // Prefer the offset that validates for the most players; on a tie the
        // earlier (more trusted) entry wins regardless of spatial extent.
        double count = floor(score / 1000000.0), best_count = floor(best_score / 1000000.0);
        if (count > best_count) { best_offset = offset; best_score = score; }
    }
    g_use_direct_player_position = saved_use_direct;
    return best_offset;
}

// Frames in a row the direct offsets failed to validate. Right after a world
// reload the position fields are still zero for a few frames; falling back to
// the transform-hierarchy path on the very first failure used to lock the ESP
// into that mode (boxes hanging 1.6 m below the player) until restart.
double mono_seconds();   // определён ниже: часы ожидания

int g_direct_position_fail_streak = 0;

int g_direct_position_recheck = 0;

// Когда началась текущая серия неудач прямых полей. Ожидание «поля допишутся»
// считается и по кадрам, и по времени: 60 кадров на перезагрузке мира (там же
// просаживается FPS) растягивались в две-три секунды, и всё это время боксов не
// было вовсе — ровно то, на что жалоба «после смерти прогружаются не сразу».
static double g_direct_position_fail_since = 0.0;

static constexpr double kDirectPositionSettleSeconds = 0.6;

bool g_body_caches_dirty = false; // clear per-player caches on the next frame

bool discover_player_position_offset(const std::vector<uint64_t>& players) {
    // Сначала — прошлое подтверждённое смещение. Оно верно настолько часто, что
    // отдельный поиск по восьми кандидатам после респавна просто не нужен.
    if (s_last_good_offset) {
        double sticky_score = 0.0;
        if (evaluate_player_position_offset(players, s_last_good_offset, sticky_score)) {
            g_direct_position_fail_streak = 0;
            g_direct_position_fail_since = 0.0;
            g_use_direct_player_position = true; g_player_position_offset = s_last_good_offset;
            g_player_position_validated = true; g_matrix_configuration_validated = false;
            return true;
        }
    }
    uint64_t best_offset = find_direct_player_position_offset(players);
    if (best_offset) {
        g_direct_position_fail_streak = 0;
        g_direct_position_fail_since = 0.0;
        g_use_direct_player_position = true; g_player_position_offset = best_offset;
        g_player_position_validated = true; g_matrix_configuration_validated = false;
        if (best_offset != s_last_good_offset) {
            diag_log("esp", "смещение позиции игрока: 0x%llx → 0x%llx",
                     (unsigned long long)s_last_good_offset, (unsigned long long)best_offset);
            s_last_good_offset = best_offset;
        }
        return true;
    }
    // Даём прямым полям устояться, прежде чем уходить на обход иерархии, но не
    // дольше положенного: и по кадрам (60), и по времени (0.6 с) — что раньше.
    const double now = mono_seconds();
    if (g_direct_position_fail_since == 0.0) g_direct_position_fail_since = now;
    ++g_direct_position_fail_streak;
    if (g_direct_position_fail_streak < 60 &&
        (now - g_direct_position_fail_since) < kDirectPositionSettleSeconds) return false;
    size_t discovered_position_count = 0, hierarchy_candidate_count = 0;
    if (discover_transform_hierarchy_layout(players, discovered_position_count, hierarchy_candidate_count)) {
        g_use_direct_player_position = false;
        g_player_position_validated = true; g_matrix_configuration_validated = false;
        g_direct_position_recheck = 0;
        return true;
    }
    return false;
}

// While on the hierarchy fallback, keep probing the direct fields and switch
// back as soon as they validate again.
void recheck_direct_player_position(const std::vector<uint64_t>& players) {
    if (g_use_direct_player_position || !g_player_position_validated) return;
    if (++g_direct_position_recheck < 15) return;
    g_direct_position_recheck = 0;
    uint64_t best_offset = find_direct_player_position_offset(players);
    if (!best_offset) return;
    g_direct_position_fail_streak = 0;
    g_direct_position_fail_since = 0.0;
    g_use_direct_player_position = true; g_player_position_offset = best_offset;
    g_matrix_configuration_validated = false;
    g_body_caches_dirty = true;
}
