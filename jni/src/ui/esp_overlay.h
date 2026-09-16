#pragma once
// ui/esp_overlay.h — Отрисовка ESP поверх игры.
//
// Всё, что модуль отдаёт наружу: сюда смотрят те модули, которым нужны эти
// типы, данные и функции. Реализация — в ui/esp_overlay.cpp.
#include "app/common.h"

// ---- Функции, которые видят другие модули ----

const std::vector<EspBox>& FrameBoxes(float sw, float sh);

void DrawEspOverlay();
