#pragma once
// camera.h — Камера игры: поза, матрицы, углы, чувствительность.
//
// Всё, что модуль отдаёт наружу: сюда смотрят те модули, которым нужны эти
// типы, данные и функции. Реализация — в camera.cpp.
#include "esp/common.h"
#include "esp/player_pose.h"

// ---- Данные, которые видят другие модули ----

extern float g_cam_fov_deg;

extern bool g_cam_pose_valid;

extern bool g_cam_pose_derived;

extern Vec3 g_cam_pos;

extern Vec3 g_cam_right, g_cam_up, g_cam_forward;

extern bool g_aim_ref_valid;

extern Vec3 g_aim_ref_origin;

extern Vec3 g_aim_ref_forward, g_aim_ref_right, g_aim_ref_up;

extern int g_offset_extent_fail_streak;

// ---- Данные, которые видят другие модули ----

// MouseLook локального игрока (Oxide.MouseLook): через него игра превращает
// касание в поворот камеры. Раскладка сверена по dump.cs релиза 205619 и беты
// 207986; подробности — в комментарии к esp_read_look_sensitivity.
//   PLAYER_MOUSE_LOOK_OFFSET      — поле PlayerManager, где лежит MouseLook;
//   MOUSE_LOOK_SENSITIVITY_OFFSET — m_Sensitivity: множитель «сдвиг -> угол»;
//   MOUSE_LOOK_ACCUM_OFFSET       — накопленный за такт сдвиг взгляда: игра
//                                   читает его, умножает на m_Sensitivity и
//                                   применяет к повороту (RVA 0x64e312c).
// Последнее поле нужно мемори-аиму: запись в него — это тот же поворот, что
// даёт касание, только без пальца (см. esp/aim_mem.cpp).
inline constexpr uint64_t PLAYER_MOUSE_LOOK_OFFSET      = 0x70;
inline constexpr uint64_t MOUSE_LOOK_SENSITIVITY_OFFSET = 0x34;
inline constexpr uint64_t MOUSE_LOOK_ACCUM_OFFSET       = 0x88;

// ---- Функции, которые видят другие модули ----

// Объект MouseLook локального игрока; 0 — не найден (нет привязки, нет игрока).
uint64_t esp_resolve_mouse_look();

bool read_native_camera_matrices(uint64_t native_cam, float screen_aspect, Mat4& projection, Mat4& view);

bool w2s_transform_camera(const Vec3& camera_position, const Vec4& camera_rotation, const Vec3& world, float screen_width, float screen_height, Vec2& output, bool clip_to_screen = true);

bool optimize_matrix_configuration(uint64_t native_camera, const std::vector<uint64_t>& transforms);

std::vector<uint64_t> read_configured_player_transforms();

void read_local_aim_reference(uint64_t local_player, const PlayerAux* local_aux, bool local_crouched);
