#pragma once
// player_pose.h — Позиция игрока: трек, скачки, «сидит/на маунте».
//
// Всё, что модуль отдаёт наружу: сюда смотрят те модули, которым нужны эти
// типы, данные и функции. Реализация — в player_pose.cpp.
#include "esp/common.h"

// ---- Типы модуля ----

// Per-player auxiliary data resolved through the KCC (character controller):
// the game-maintained head transform and the current pose. Used for
// crouch-aware boxes and as the aim target when rig bones are unavailable.
struct PlayerAux {
    uint64_t kcc = 0;
    uint64_t head_native = 0;   // native Transform of KCC.head
    uint64_t head_hitbox_transform = 0; // native Transform of the Head HitBox
    Vec3     head_hitbox_center{};      // HitBox.center (local to that transform)
    bool     head_hitbox_valid = false;
    float    normal_height = 1.8F;
    float    crouch_height = 1.1F;
    int      retry_cooldown = 0;
    int      revalidate = 0;
    // Held-weapon chain (resolved lazily, revalidated by their back-references).
    uint64_t weapon_component = 0; // PlayerWeapon (NetworkBehaviour)
    uint64_t model_info = 0;       // PlayerModelInfo (weapon holders)
    int      weapon_retry = 0;
};

struct PlayerTrack {
    char uid[40] = {};      // userID, empty when it could not be read
    int  uid_recheck = 0;   // objects are pooled and reused for other players
    Vec3 last{};
    bool has_last = false;
    int  still_frames = 0;  // consecutive frames without movement
    // How fast this player is actually moving through the world, in metres
    // per second. World space on purpose: it does not care where the camera
    // is pointing or how fast it is turning, so it stays correct even when
    // the camera angles cannot be read at all.
    Vec3   vel{};
    Vec3   vel_ref{};       // position the current estimate was measured from
    double vel_ref_t = 0.0; // and when
    bool   have_vel_ref = false;
    // Последняя ПРИНЯТАЯ позиция и счётчики временного фильтра (см.
    // filter_player_position): бокс рисуется по ним, а не по сырому чтению.
    Vec3   drawn{};
    bool   has_drawn = false;
    double drawn_t = 0.0;
    int    hold_frames = 0;  // сколько кадров живём без успешного чтения
    Vec3   jump{};           // подозрительный отсчёт, ждущий подтверждения
    bool   has_jump = false;
    int    jump_frames = 0;
};

// Mounted-state latch. lastSavedPosition freezes at the boarding point while
// the rendered transform (and/or the character model) rides with the vehicle.
// vehicleID / the transform also flicker mid-tick. Engage on seat/vehicle OR
// on lastSaved freeze + a live visual that has left the mount; never snap the
// box back to that mount mid-ride.
struct MountLatch {
    int  unmounted_streak = 0;
    int  saved_still = 0;
    bool engaged = false;
    Vec3 last{};
    bool last_ok = false;
    Vec3 prev_saved{};
    bool have_saved = false;
};

// ---- Данные, которые видят другие модули ----

extern std::unordered_map<uint64_t, PlayerAux> g_player_aux;

extern std::unordered_map<uint64_t, PlayerTrack> g_player_track;

extern std::unordered_map<std::string, uint64_t> g_player_track_pick;

extern std::unordered_map<uint64_t, MountLatch> g_mount_latch;

// ---- Функции, которые видят другие модули ----

PlayerAux& player_aux(uint64_t player);

bool player_is_crouched(const PlayerAux& aux);

bool player_head_world(const PlayerAux& aux, Vec3& out);

bool player_head_hitbox_world(const PlayerAux& aux, Vec3& out);

void prune_player_aux(const std::vector<uint64_t>& players);

void prune_player_track(const std::vector<uint64_t>& players);

float vec3_horiz2(const Vec3& a, const Vec3& b);

bool player_mount_engaged(uint64_t player);

void apply_mounted_position(uint64_t player, Vec3& feet);

void prune_mount_latch(const std::vector<uint64_t>& players);

PlayerTrack& track_player(uint64_t player, const Vec3& position);

bool filter_player_position(PlayerTrack& track, bool read_ok, Vec3& pos);
