#pragma once
// ui/popover.h — Всплывающее окно и его содержимое.
//
// Всё, что модуль отдаёт наружу: сюда смотрят те модули, которым нужны эти
// типы, данные и функции. Реализация — в ui/popover.cpp.
#include "app/common.h"

// ---- Типы модуля ----

struct Popover {
    bool  visible    = false;
    bool  closing    = false;
    float anim       = 0.f;
    float vel        = 0.f;
    int   openFrames = 0;
    int   sectionId  = 0;
    char  title[64]  = {};
};

// ---- Константы и данные, ссылающиеся на типы модуля ----

extern Popover g_pop;

// ---- Функции, которые видят другие модули ----

void PopoverOpenColor(const char* title, ImVec4* cp);

void PopoverOpen(const char* title, int sid);

void PopoverClose();

void DrawPopover(float dt, ImVec2 menuPos, float WW, float WH);
