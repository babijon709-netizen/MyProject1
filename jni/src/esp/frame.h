#pragma once
// frame.h — Кадр ESP: состояние, публикация, сброс.
//
// Всё, что модуль отдаёт наружу: сюда смотрят те модули, которым нужны эти
// типы, данные и функции. Реализация — в frame.cpp.
#include "esp/common.h"

// ---- Данные, которые видят другие модули ----

extern std::vector<uint64_t> g_frame_transforms;

extern std::unordered_map<uint64_t, int> g_frame_transforms_lost;

extern Mat4 g_frame_vp;

extern bool g_frame_vp_valid;

extern float g_frame_sw , g_frame_sh;

extern Vec3 g_frame_local_pos;

extern bool g_frame_local_valid;

extern bool g_frame_cam_basis_valid;

extern Vec3 g_frame_cam_pos, g_frame_cam_fwd, g_frame_cam_right, g_frame_cam_up;

extern int g_frame_player_count;

extern int g_frame_transforms_empty_streak;

extern double g_frame_transforms_empty_since;

extern int g_frame_publish_fail_streak;

extern int g_frame_watchdog_resets;

extern int g_local_position_fail_streak;

extern int g_world_change_streak;

extern std::vector<uint64_t> g_population_snapshot;

extern std::atomic<bool> g_want_reattach;

extern float g_last_overlay_sw;

extern float g_last_overlay_sh;

extern double g_frame_publish_time;

extern double g_world_reload_time;

// ---- Функции, которые видят другие модули ----

// Кадр собран: камера и позиция игрока опубликованы. Отмечается в тех местах,
// где публикация действительно удалась.
void frame_note_published();

// Кадр НЕ собрался (указатель камеры мигнул, матрицы не прочитались, позиции
// игроков не нашлись). Прошлую публикацию держим ещё kFrameHoldSeconds — за это
// время камера не уезжает, а мигание маркеров и фарма пропадает; если прошлый
// кадр старше, гасим всё честно, чтобы ничего не проецировалось через мёртвую
// матрицу.
void frame_drop_unpublished();

// Идёт перезагрузка мира: кэши сбрасывались только что. Пока так — данные игры
// ещё дописываются, и по ним нельзя ни объявлять смещение позиции неверным, ни
// считать состав игроков настоящим.
bool world_reloading();

bool farm_cam_source_ok(const Vec3& p);

void reset_world_caches();

void esp_reset();

bool publish_camera_only_frame(float sw, float sh);
