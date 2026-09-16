#pragma once
// ui/scroll.h — Прокрутка панелей и состояние окна.
//
// Всё, что модуль отдаёт наружу: сюда смотрят те модули, которым нужны эти
// типы, данные и функции. Реализация — в ui/scroll.cpp.
#include "app/common.h"
#include "app/media.h"

// ---- Константы модуля ----

extern const float WW_MIN;

extern const float WH_MIN;

// ---- Данные модуля ----

extern float pill_y;

extern float pill_vel;

extern ImVec2 g_popOpenClickPos;

// ---- Типы модуля ----

struct ScrollState {
    float off      = 0.f;
    float vel      = 0.f;
    float lastTy   = 0.f;
    bool  drag     = false;
    bool  dragging = false;
    int   axis     = 0;
    float sb_alpha = 0.f;
    float sb_idle  = 0.f;
    float sb_w     = 3.f;
    float sb_y     = 0.f;
    float sb_h     = 48.f;
    bool  sb_hot   = false;
};

struct TabRect { float sx; };

struct WindowState {
    ImVec2 pos              = {-1.f, -1.f};
    float  w                = WW_MIN;
    float  h                = WH_MIN;
    bool   dragging         = false;
    ImVec2 touchStart       = {0, 0};
    ImVec2 posStart         = {0, 0};
    bool   resizing         = false;
    ImVec2 resizeTouchStart = {0, 0};
    ImVec2 sizeStart        = {WW_MIN, WH_MIN};
};

// ---- Константы и данные, ссылающиеся на типы модуля ----

extern TabRect tab_rects[kTabCount];

extern ScrollState g_scrollMain;

extern ScrollState g_scrollPop;

extern WindowState g_win;

// ---- Функции, которые видят другие модули ----

void ScrollTick(ScrollState& s, bool mIn, bool blocked, float& maxScroll, float dt);

bool IsScrollDragging();
