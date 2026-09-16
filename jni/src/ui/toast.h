#pragma once
// ui/toast.h — Всплывающие подсказки.
//
// Всё, что модуль отдаёт наружу: сюда смотрят те модули, которым нужны эти
// типы, данные и функции. Реализация — в ui/toast.cpp.
#include "app/common.h"

// ---- Функции, которые видят другие модули ----

void ShowToast(const char* msg);

void DrawToast(float dt);
