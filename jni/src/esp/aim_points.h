#pragma once
// aim_points.h — Точки прицела: голова/шея/грудь и локальный ADS.
//
// Всё, что модуль отдаёт наружу: сюда смотрят те модули, которым нужны эти
// типы, данные и функции. Реализация — в aim_points.cpp.
#include "esp/common.h"

// ---- Типы модуля ----

struct LocalAimState {
    bool     aiming = false;
    int      source = 0;          // 1 = event handler Aim activity, 2 = weapon isAiming
    uint64_t event_handler = 0;
    uint64_t aim_activity = 0;
    uint64_t fp_manager = 0;
    uint64_t weapon = 0;
    int      revalidate = 0;
};

// ---- Константы модуля ----

// Where the target will be by the time the finger move we are about to send
// has travelled through the phone and come back out as camera rotation. A man
// running past keeps running during that hundredth-of-a-second or two, and
// without this the crosshair sits permanently behind him, by a distance
// proportional to how fast he runs.
//
// It is his own speed through the world, so nothing here depends on knowing
// the look sensitivity, on reading the camera angles, or on separating our
// turning from his -- the three things that have no reliable answer on this
// build. Set per player just below, applied here, and it only shifts the
// point the aim steers to: the ESP box still draws where the man actually is.
static inline constexpr float kAimLeadSeconds = 0.050F;

// ---- Данные, которые видят другие модули ----

extern LocalAimState g_aim_state;

extern Vec3 g_aim_lead;

// ---- Функции, которые видят другие модули ----

bool set_aim_point(EspBox& box, int slot, const Vec3& world_in, const Mat4& vp, float sw, float sh);

bool fill_skeleton_box(uint64_t player, const Mat4& view_projection, float sw, float sh, EspBox& box);
