#pragma once
// game_patch.h — Запись в память игры: X-ray и «всегда день».
//
// Всё, что модуль отдаёт наружу: сюда смотрят те модули, которым нужны эти
// типы, данные и функции. Реализация — в game_patch.cpp.
#include "esp/common.h"

// ---- Данные, которые видят другие модули ----

extern unsigned g_farm_mask;

extern uint64_t g_xray_cam;

extern bool g_xray_saved_valid;

extern uint64_t g_day_tod;

extern std::atomic<uint64_t> g_day_cycle_addr;

extern int g_day_retry;

// ---- Функции, которые видят другие модули ----

void xray_apply(uint64_t native_cam);

void always_day_tick();
