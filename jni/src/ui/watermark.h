#pragma once
// ui/watermark.h — Пилюли-подписи поверх игры.
//
// Всё, что модуль отдаёт наружу: сюда смотрят те модули, которым нужны эти
// типы, данные и функции. Реализация — в ui/watermark.cpp.
#include "app/common.h"

// ---- Данные модуля ----

extern bool menu_open;

// ---- Функции, которые видят другие модули ----

void DrawWatermark(float dt);
