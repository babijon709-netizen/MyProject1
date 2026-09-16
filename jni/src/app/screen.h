#pragma once
// app/screen.h — Размеры экрана и центрирование окна меню.
//
// Всё, что модуль отдаёт наружу: сюда смотрят те модули, которым нужны эти
// типы, данные и функции. Реализация — в app/screen.cpp.
#include "app/common.h"

// ---- Данные модуля ----

extern float g_sw;

extern float g_sh;

extern uint32_t g_menu_orient;

extern int g_menu_dw , g_menu_dh;

// ---- Функции, которые видят другие модули ----

void VisibleScreen(float& w, float& h);

void CenterMenuOnDisplay();
