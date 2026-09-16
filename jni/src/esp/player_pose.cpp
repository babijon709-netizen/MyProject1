// player_pose.cpp — Позиция игрока: трек, скачки, «сидит/на маунте».
//
// Модуль разрезан из прежнего монолита jni/src/game.cpp;
// что здесь лежит и кто это зовёт — в шапке player_pose.h и в docs/CODE_MAP.md.

#include "esp/common.h"
#include "esp/boxes.h"
#include "esp/camera.h"
#include "esp/frame.h"
#include "esp/managed.h"
#include "esp/math.h"
#include "esp/mem.h"
#include "esp/skeleton_cache.h"
#include "esp/skeleton_names.h"
#include "esp/transform.h"
#include "esp/weapons.h"
#include "player_pose.h"

std::unordered_map<uint64_t, PlayerAux> g_player_aux;

PlayerAux& player_aux(uint64_t player) {
    PlayerAux& aux = g_player_aux[player];
    if (aux.kcc) {
        // Cheap liveness check every frame; full re-resolve occasionally.
        if (rd_ptr(aux.kcc + KCC_PLAYER_BACKREF) != player || ++aux.revalidate >= 300) {
            aux = PlayerAux{};
        }
    }
    if (!aux.kcc) {
        if (aux.retry_cooldown > 0) { --aux.retry_cooldown; return aux; }
        uint64_t kcc = resolve_player_kcc(player);
        if (!kcc) { aux.retry_cooldown = 30; return aux; }
        aux.kcc = kcc;
        aux.revalidate = 0;
        float nh = rd<float>(kcc + KCC_NORMAL_HEIGHT);
        float ch = rd<float>(kcc + KCC_CROUCH_HEIGHT);
        if (std::isfinite(nh) && nh > 1.2F && nh < 2.6F) aux.normal_height = nh;
        if (std::isfinite(ch) && ch > 0.6F && ch < aux.normal_height) aux.crouch_height = ch;
        uint64_t head = rd_ptr(kcc + KCC_HEAD_TRANSFORM);
        aux.head_native = head ? rd_ptr(head + MANAGED_CACHED_PTR) : 0;
        if (aux.head_native && (aux.head_native < 0x10000 || aux.head_native >= 0x0001000000000000ULL))
            aux.head_native = 0;
        // Head hit volume (what the server actually tests shots against).
        aux.head_hitbox_valid = false;
        uint64_t hb_root = rd_ptr(kcc + KCC_HITBOX_ROOT);
        uint64_t hb_array = hb_root ? rd_ptr(hb_root + HITBOX_ROOT_ARRAY) : 0;
        int32_t hb_count = hb_array ? rd<int32_t>(hb_array + IL2CPP_ARRAY_LENGTH) : 0;
        if (hb_count > 0 && hb_count <= 64) {
            for (int32_t i = 0; i < hb_count; ++i) {
                uint64_t hb = rd_ptr(hb_array + IL2CPP_ARRAY_FIRST_ELEMENT + (uint64_t)i * 8);
                if (!hb || rd<int32_t>(hb + HITBOX_AREA) != 0) continue;
                Vec3 center = rd_v3(hb + HITBOX_CENTER);
                Vec3 size = rd_v3(hb + HITBOX_SIZE);
                if (!vec3_is_finite(center) || !vec3_is_finite(size)) continue;
                if (fabsf(center.x) > 1.0F || fabsf(center.y) > 1.0F || fabsf(center.z) > 1.0F) continue;
                if (!(size.x > 0.02F && size.x < 1.0F && size.y > 0.02F && size.y < 1.0F)) continue;
                uint64_t transform = native_component_transform(managed_object_native(hb));
                if (!skeleton_transform_ptr_valid(transform)) continue;
                aux.head_hitbox_transform = transform;
                aux.head_hitbox_center = center;
                aux.head_hitbox_valid = true;
                break;
            }
        }
    }
    return aux;
}

// Pose from KCC.Move: true when crouched (Pose == Crouch or State == CROUCHING).
bool player_is_crouched(const PlayerAux& aux) {
    if (!aux.kcc) return false;
    int32_t state = rd<int32_t>(aux.kcc + KCC_MOVE + 0x00);
    int32_t pose  = rd<int32_t>(aux.kcc + KCC_MOVE + 0x04);
    return pose == 1 || state == 3;
}

bool player_head_world(const PlayerAux& aux, Vec3& out) {
    if (!aux.head_native) return false;
    if (g_skeleton_layout_valid &&
        read_transform_hierarchy_layout(aux.head_native, g_skeleton_layout, out)) return vec3_is_finite(out);
    return read_transform_hierarchy_position(aux.head_native, out) && vec3_is_finite(out);
}

// World-space centre of the Head hit volume: transform TRS applied to the
// local centre (HitBox.transform.TransformPoint(center)).
bool player_head_hitbox_world(const PlayerAux& aux, Vec3& out) {
    if (!aux.head_hitbox_valid || !aux.head_hitbox_transform) return false;
    Vec3 pos{}; Vec4 rot{};
    const TransformHierarchyLayout& layout = g_skeleton_layout_valid ? g_skeleton_layout : g_transform_hierarchy_layout;
    if (!(g_skeleton_layout_valid || g_transform_hierarchy_layout_valid)) return false;
    if (!read_transform_hierarchy_layout(aux.head_hitbox_transform, layout, pos, &rot)) return false;
    // Lossy scale is ~1 on character rigs; rotate the local centre and offset.
    Vec3 offset = rotate_vector(rot, aux.head_hitbox_center);
    out = {pos.x + offset.x, pos.y + offset.y, pos.z + offset.z};
    return vec3_is_finite(out);
}

void prune_player_aux(const std::vector<uint64_t>& players) {
    for (auto it = g_player_aux.begin(); it != g_player_aux.end();) {
        bool present = false;
        for (uint64_t player : players) if (player == it->first) { present = true; break; }
        if (!present) it = g_player_aux.erase(it); else ++it;
    }
}

std::unordered_map<uint64_t, PlayerTrack> g_player_track;

// uid -> the object we drew last time, so a tie does not flip between copies.
std::unordered_map<std::string, uint64_t> g_player_track_pick;

void prune_player_track(const std::vector<uint64_t>& players) {
    for (auto it = g_player_track.begin(); it != g_player_track.end();) {
        bool present = false;
        for (uint64_t player : players) if (player == it->first) { present = true; break; }
        if (!present) it = g_player_track.erase(it); else ++it;
    }
    if (g_player_track_pick.size() > 256) g_player_track_pick.clear();
}

float vec3_horiz2(const Vec3& a, const Vec3& b) {
    float dx = a.x - b.x, dz = a.z - b.z;
    return dx * dx + dz * dz;
}

static bool player_is_mounted(uint64_t player) {
    if (!player) return false;
    uint32_t vehicle = rd<uint32_t>(player + PLAYER_VEHICLE_ID);
    if (vehicle != 0 && vehicle != 0xFFFFFFFFu) return true;
    // Copters / some seats leave vehicleID at 0; seatID is still non-zero.
    uint32_t seat = rd<uint32_t>(player + PLAYER_SEAT_ID);
    return seat != 0 && seat != 0xFFFFFFFFu && seat < 32u;
}

static bool player_saved_position(uint64_t player, Vec3& out) {
    uint64_t off = g_player_position_offset ? g_player_position_offset : (uint64_t)PLAYER_POSITION;
    out = rd_v3(player + off);
    return vec3_is_finite(out) && position_looks_like_world_space(out);
}

// World position of the player's own transform (worldCameraRoot). While he is
// mounted this is the only source that moves with the vehicle.
static bool player_rendered_position(uint64_t player, Vec3& out) {
    uint64_t native = resolve_player_native_transform(player);
    if (!native) return false;
    if (g_skeleton_layout_valid && read_transform_hierarchy_layout(native, g_skeleton_layout, out)
        && vec3_is_finite(out) && position_looks_like_world_space(out))
        return true;
    return read_transform_hierarchy_position(native, out) && vec3_is_finite(out)
        && position_looks_like_world_space(out);
}

// Seated body: characterModel is parented to the seat even when worldCameraRoot
// is left behind at the boarding point.
static bool player_model_position(uint64_t player, Vec3& out) {
    uint64_t root = skeleton_model_root(player);
    if (!root) return false;
    if (g_skeleton_layout_valid && read_transform_hierarchy_layout(root, g_skeleton_layout, out)
        && vec3_is_finite(out) && position_looks_like_world_space(out))
        return true;
    return read_transform_hierarchy_position(root, out) && vec3_is_finite(out)
        && position_looks_like_world_space(out);
}

// worldCameraRoot sits at eye level, the box is built from the feet.
static constexpr float kCameraRootHeight = 1.60F;

std::unordered_map<uint64_t, MountLatch> g_mount_latch;

bool player_mount_engaged(uint64_t player) {
    auto found = g_mount_latch.find(player);
    return found != g_mount_latch.end() && found->second.engaged;
}

void apply_mounted_position(uint64_t player, Vec3& feet) {
    Vec3 saved{};
    if (!player_saved_position(player, saved)) saved = feet;

    bool id_mounted = player_is_mounted(player);
    if (g_mount_latch.size() > 256 && g_mount_latch.find(player) == g_mount_latch.end()) return;
    MountLatch& latch = g_mount_latch[player];

    if (latch.have_saved) {
        if (vec3_horiz2(saved, latch.prev_saved) < 0.15F * 0.15F) {
            if (latch.saved_still < 100000) ++latch.saved_still;
        } else {
            latch.saved_still = 0;
        }
    }
    latch.prev_saved = saved;
    latch.have_saved = true;

    // On foot the box already sits on lastSaved. Skip the transform walks
    // unless the seat says mounted, we are already riding, or lastSaved has
    // been frozen long enough that this may be a boarding (copter vehicleID
    // is often 0).
    if (!id_mounted && !latch.engaged && (latch.saved_still < 8 || (latch.saved_still & 7) != 0))
        return;

    Vec3 cam{};
    bool have_cam = player_rendered_position(player, cam);
    Vec3 model{};
    bool have_model = player_model_position(player, model);

    // Direct path builds the box from feet; hierarchy path treats the value as
    // eye height and subtracts 1.60 later. Keep whichever space `feet` is in.
    const bool ground = g_use_direct_player_position;
    Vec3 live{};
    bool have_live = false;
    float live_h2 = -1.0F;
    if (have_cam) {
        Vec3 p = cam;
        if (ground) p.y -= kCameraRootHeight;
        if (position_looks_like_world_space(p) || position_looks_like_world_space(cam)) {
            live = p;
            have_live = true;
            live_h2 = vec3_horiz2(p, saved);
        }
    }
    if (have_model) {
        Vec3 p = model;
        if (!ground) p.y += 0.90F; // hips -> roughly eye, matches hierarchy boxes
        float h2 = vec3_horiz2(p, saved);
        if (!have_live || h2 > live_h2 + 0.25F) {
            live = p;
            have_live = true;
            live_h2 = h2;
        }
    }
    if (!ground && !have_live && position_looks_like_world_space(feet)) {
        live = feet;
        have_live = true;
        live_h2 = vec3_horiz2(feet, saved);
    }

    bool pos_mounted = latch.saved_still >= 8 && have_live && live_h2 > 1.6F * 1.6F;
    bool keep_mounted = latch.engaged && have_live && live_h2 > 1.6F * 1.6F;
    if (id_mounted || pos_mounted || keep_mounted) {
        latch.engaged = true;
        latch.unmounted_streak = 0;
    } else if (latch.engaged) {
        if (++latch.unmounted_streak >= 45) {
            latch.engaged = false;
            latch.last_ok = false;
            latch.unmounted_streak = 0;
        }
    }
    if (!latch.engaged) return;

    if (have_live) {
        float last_h2 = latch.last_ok ? vec3_horiz2(latch.last, saved) : 0.0F;
        // Rendered transform snapped back to the frozen boarding point.
        if (latch.last_ok && live_h2 < 1.25F * 1.25F && last_h2 > 2.5F * 2.5F) {
            feet = latch.last;
            return;
        }
        // Same flicker, but the bad sample did not land exactly on lastSaved.
        if (latch.last_ok && last_h2 > 4.0F &&
            vec3_horiz2(live, latch.last) > 4.0F * 4.0F && live_h2 < last_h2 * 0.25F) {
            feet = latch.last;
            return;
        }
        latch.last = live;
        latch.last_ok = true;
        feet = live;
        return;
    }
    if (latch.last_ok) feet = latch.last;
}

void prune_mount_latch(const std::vector<uint64_t>& players) {
    for (auto it = g_mount_latch.begin(); it != g_mount_latch.end();) {
        bool present = false;
        for (uint64_t player : players) if (player == it->first) { present = true; break; }
        if (!present) it = g_mount_latch.erase(it); else ++it;
    }
}

PlayerTrack& track_player(uint64_t player, const Vec3& position) {
    PlayerTrack& track = g_player_track[player];
    if (--track.uid_recheck <= 0) {
        char uid[40] = {};
        // Перезаписываем кеш ТОЛЬКО успешным чтением. Безусловный memcpy
        // стирал userID после любого сбоя чтения, и на пару секунд дубли
        // объекта (копия после посадки в транспорт, остаток респауна)
        // переставали подавляться: второй бокс вспыхивал в стороне и пропадал.
        if (read_managed_string_ex(rd_ptr(player + PLAYER_USER_ID), uid, sizeof(uid), 39)) {
            memcpy(track.uid, uid, sizeof(track.uid));
            track.uid_recheck = 120; // ~2 s: pooled objects change owner
        } else {
            // Старый кеш не трогаем. Если кеша нет вовсе, пробуем быстрее.
            track.uid_recheck = track.uid[0] ? 120 : 20;
        }
    }
    // Velocity, measured between the moments the position actually changed.
    // A remote player's position only arrives on the network tick, so most
    // frames repeat the previous one; differencing those would read zero.
    {
        const double now = mono_seconds();
        if (!track.have_vel_ref) {
            track.vel_ref = position; track.vel_ref_t = now; track.have_vel_ref = true;
        } else {
            const float mx = position.x - track.vel_ref.x;
            const float my = position.y - track.vel_ref.y;
            const float mz = position.z - track.vel_ref.z;
            const float moved = mx * mx + my * my + mz * mz;
            const double span = now - track.vel_ref_t;
            if (moved > 0.0004F) {                     // moved more than 2 cm
                if (span > 0.02 && span < 0.5) {
                    const Vec3 v = { (float)(mx / span), (float)(my / span), (float)(mz / span) };
                    const float speed = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
                    if (std::isfinite(speed) && speed < 12.0F) {   // faster than a man can run: a teleport
                        track.vel.x = track.vel.x * 0.5F + v.x * 0.5F;
                        track.vel.y = track.vel.y * 0.5F + v.y * 0.5F;
                        track.vel.z = track.vel.z * 0.5F + v.z * 0.5F;
                    }
                }
                track.vel_ref = position; track.vel_ref_t = now;
            } else if (span > 0.3) {                   // stood still: stop leading
                track.vel = {};
                track.vel_ref = position; track.vel_ref_t = now;
            }
        }
    }
    if (track.has_last) {
        float dx = position.x - track.last.x;
        float dy = position.y - track.last.y;
        float dz = position.z - track.last.z;
        if (dx * dx + dy * dy + dz * dz > 0.0025F) track.still_frames = 0;      // > 5 cm
        else if (track.still_frames < 100000) ++track.still_frames;
    }
    track.last = position;
    track.has_last = true;
    return track;
}

// ---- Временной фильтр позиции ------------------------------------------------
// Два артефакта, которые видели как «визуалы мерцают и телепаются в другую
// сторону»:
//   1. один кадр без успешного чтения позиции — бокс пропадал и возвращался;
//   2. один мусорный отсчёт (позиция не дописана игрой, объект из пула чужой,
//      чтение попало между кадрами симуляции) — бокс улетал в сторону и на
//      следующем кадре возвращался обратно.
// Ни то ни другое не похоже на настоящее движение: позиция игрока меняется
// плавно, а телепорт/респаун держится в памяти и на следующем кадре тоже.
// Поэтому одиночный сбой чтения дорисовываем по последней принятой позиции
// (продолжая её по измеренной скорости), а подозрительный скачок рисуем
// только когда он подтверждается подряд несколькими кадрами.
//
// Допуск скачка взят с запасом: бег ~8 м/с, техника до ~50 м/с, а координаты
// чужих игроков приходят пачкой раз в ~0.1 с, так что законный «прыжок» между
// кадрами может быть в несколько метров. Всё, что больше, почти всегда мусор —
// и даже если это настоящий телепорт, мы отстанем от него на пару кадров.
static constexpr int   kPosHoldFrames        = 3;     // ~50 мс без чтения — ещё не пропажа

static constexpr int   kPosJumpConfirmFrames = 2;     // скачок должен повториться

static constexpr float kPosJumpMeters        = 3.0F;  // базовый допуск, метры

static constexpr float kPosJumpSpeed         = 55.0F; // плюс м/с на каждый кадр

static constexpr float kPosExtrapolateLimit  = 1.5F;  // насколько дорисовываем по скорости

static constexpr float kPosJumpSameSpot      = 1.5F;  // «тот же» подозрительный отсчёт

// Возвращает false, когда бокс в этом кадре рисовать не надо. pos — вход
// (сырое чтение, при read_ok) и выход (то, что рисуем).
bool filter_player_position(PlayerTrack& track, bool read_ok, Vec3& pos) {
    const double now = mono_seconds();

    if (!track.has_drawn) {
        if (!read_ok) return false;
        track.drawn = pos; track.drawn_t = now; track.has_drawn = true;
        track.hold_frames = 0; track.has_jump = false; track.jump_frames = 0;
        return true;
    }

    double dt = now - track.drawn_t;
    if (!(dt > 0.0)) dt = 0.0;
    if (dt > 0.25) dt = 0.25;
    const float fdt = (float)dt;

    // Последняя принятая позиция, продвинутая по измеренной скорости; дальше
    // метра-полутора не продлеваем, чтобы не унести бокс самим фильтром.
    Vec3 predicted = track.drawn;
    {
        const float mx = track.vel.x * fdt, my = track.vel.y * fdt, mz = track.vel.z * fdt;
        const float step2 = mx * mx + my * my + mz * mz;
        const float limit2 = kPosExtrapolateLimit * kPosExtrapolateLimit;
        const float scale = (step2 > limit2 && step2 > 0.0F) ? sqrtf(limit2 / step2) : 1.0F;
        predicted.x += mx * scale; predicted.y += my * scale; predicted.z += mz * scale;
    }

    if (!read_ok) {
        if (++track.hold_frames > kPosHoldFrames) return false;  // объект реально пропал
        pos = predicted;
        return true;
    }

    const float dx = pos.x - predicted.x, dy = pos.y - predicted.y, dz = pos.z - predicted.z;
    const float dev = sqrtf(dx * dx + dy * dy + dz * dz);
    const float limit = kPosJumpMeters + kPosJumpSpeed * fdt;
    if (std::isfinite(dev) && dev <= limit) {
        track.drawn = pos; track.drawn_t = now;
        track.hold_frames = 0; track.has_jump = false; track.jump_frames = 0;
        return true;
    }

    // Скачок за пределы правдоподобия. Один и тот же отсчёт подряд — похоже на
    // настоящий телепорт, принимаем; каждый кадр разный — это мусор, остаёмся
    // на последней хорошей позиции.
    bool same = false;
    if (track.has_jump) {
        const float jx = pos.x - track.jump.x, jy = pos.y - track.jump.y, jz = pos.z - track.jump.z;
        same = std::isfinite(jx) && (jx * jx + jy * jy + jz * jz) < kPosJumpSameSpot * kPosJumpSameSpot;
    }
    if (same) ++track.jump_frames;
    else { track.jump = pos; track.jump_frames = 1; }
    track.has_jump = true;

    if (track.jump_frames >= kPosJumpConfirmFrames) {
        track.drawn = pos; track.drawn_t = now;
        track.has_jump = false; track.jump_frames = 0; track.hold_frames = 0;
        // Скорость через телепорт не измеряется — сбрасываем, иначе упреждение
        // будет на пару кадров смотреть в старую сторону.
        track.vel = {}; track.have_vel_ref = false;
        return true;
    }
    pos = predicted;
    return true;
}
