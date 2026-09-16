#pragma once
// farm/controller.h — UpdateFarm/UpdateFarmInner: контроллер автофарма.
//
// Всё, что модуль отдаёт наружу: сюда смотрят те модули, которым нужны эти
// типы, данные и функции. Реализация — в farm/controller.cpp.
#include "app/common.h"

// ---- Функции, которые видят другие модули ----

void UpdateFarm(float dt);
