#pragma once
// ui/sheet.h — Нижняя шторка и кнопки выхода.
//
// Всё, что модуль отдаёт наружу: сюда смотрят те модули, которым нужны эти
// типы, данные и функции. Реализация — в ui/sheet.cpp.
#include "app/common.h"

// ---- Типы модуля ----

struct Sheet {
    bool   visible    = false;
    float  anim       = 0.f;
    float  vel        = 0.f;
    bool   closing    = false;
    int    openFrames = 0;
    char   title[64]  = {};
    int    type       = 0;
    bool*  boolP      = nullptr;
    float* animP      = nullptr;
    float* slP        = nullptr;
    float  slMin      = 0;
    float  slMax      = 1;
    const char* slFmt = "%.1f";
};

// ---- Константы и данные, ссылающиеся на типы модуля ----

extern Sheet g_sheet;

// ---- Функции, которые видят другие модули ----

void DrawExitButtons(ImDrawList* fg, ImFont* fn, float fs, float alpha, float b1X, float b2X, float bW, float bY, float btnH, bool blocked, const ImVec2& mousePos, const ImVec2& clickedPos, bool mouseReleased, bool isSheet);

void DrawSheet(float dt, ImVec2 menuPos, float WW, float WH);
