#pragma once
// app/media.h — Иконки вкладок: загрузка GL-текстур.
//
// Всё, что модуль отдаёт наружу: сюда смотрят те модули, которым нужны эти
// типы, данные и функции. Реализация — в app/media.cpp.
#include "app/common.h"

// ---- Константы модуля ----

inline constexpr int kTabCount = 6;

// ---- Данные модуля ----

extern GLuint g_tabIcons[kTabCount];

// ---- Функции, которые видят другие модули ----

void LoadTabIcons();

void LoadAnimeImage();
