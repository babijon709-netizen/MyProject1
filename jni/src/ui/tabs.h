#pragma once
// ui/tabs.h — TabContent: содержимое вкладок.
//
// Всё, что модуль отдаёт наружу: сюда смотрят те модули, которым нужны эти
// типы, данные и функции. Реализация — в ui/tabs.cpp.
#include "app/common.h"

// ---- Функции, которые видят другие модули ----

float TabContent(int tab, float dt, float cW);
