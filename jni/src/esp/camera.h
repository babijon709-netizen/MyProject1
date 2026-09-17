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
// Углы прицела и узел, который из них поворачивается (dump.cs: Oxide.MouseLook):
//   m_LookRoot 0x28 — UnityEngine.Transform, ему игра ставит поворот;
//   LGa/KXf    0x4C — Vector2 углов (x — рыскание, y — тангаж).
// Oxide.MouseLook$$ZJo (RVA 0x64e312c) складывает сдвиг в 0x4C, нормализует
// рыскание и клэмпит тангаж, и уже из этих углов строит поворот узла:
// именно поэтому мемори-аиму правильнее писать в 0x4C, а не в узел (см.
// esp/aim_mem.cpp). Смещения одинаковы в релизе и бете (проверено по dump.7z и
// dump_beta.7z).
inline constexpr uint64_t MOUSE_LOOK_ANGLES_OFFSET      = 0x4C;
inline constexpr uint64_t MOUSE_LOOK_LOOK_ROOT_OFFSET   = 0x28;
// Пара углов в текущем оружии (Oxide.FPHitscan.<Lwj>k__BackingField, Vector2:
// x — тангаж, y — МИНУС рыскание). Игра читает её через FPManager$$ZRz
// (RVA 0x652d3b8) и пишет через FPManager$$ZRH (RVA 0x652abcc), а зовёт их
// Oxide.MouseLook: это и есть текущий прицел, который игра применяет к узлу
// m_LookRoot.
// Само смещение — в переключателе версий (FPHITSAN_LOOK_ANGLES_OFFSET в
// game_offsets.h): у релиза это 0x2D4, у беты 0x2DC (проверено дизассемблером
// beta libil2cpp: Oxide.FPManager$$swg, RVA 0x657cce4, пишет ту же пару так же,
// только на 8 байт дальше). Здесь его держать нельзя — значение зашилось бы
// релизным для обеих версий.

// ---- Функции, которые видят другие модули ----

// Объект MouseLook локального игрока; 0 — не найден (нет привязки, нет игрока).
uint64_t esp_resolve_mouse_look();

bool read_native_camera_matrices(uint64_t native_cam, float screen_aspect, Mat4& projection, Mat4& view);

bool w2s_transform_camera(const Vec3& camera_position, const Vec4& camera_rotation, const Vec3& world, float screen_width, float screen_height, Vec2& output, bool clip_to_screen = true);

bool optimize_matrix_configuration(uint64_t native_camera, const std::vector<uint64_t>& transforms);

std::vector<uint64_t> read_configured_player_transforms();

void read_local_aim_reference(uint64_t local_player, const PlayerAux* local_aux, bool local_crouched);
