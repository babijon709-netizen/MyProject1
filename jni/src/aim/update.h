#pragma once
// aim/update.h — UpdateAim: один такт аимбота.
//
// Всё, что модуль отдаёт наружу: сюда смотрят те модули, которым нужны эти
// типы, данные и функции. Реализация — в aim/update.cpp.
#include "app/common.h"

// ---- Функции, которые видят другие модули ----

void UpdateAim(float dt);
