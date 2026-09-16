#pragma once
// ui/widgets.h — Строки-переключатели, слайдеры, карточки.
//
// Всё, что модуль отдаёт наружу: сюда смотрят те модули, которым нужны эти
// типы, данные и функции. Реализация — в ui/widgets.cpp.
#include "app/common.h"
#include "ui/layout.h"

// ---- Функции, которые видят другие модули ----

void DrawToggle(ImDrawList* dl, float ax, float cy, float t);

void RenderToggleRowVisuals(ImDrawList* dl, ImFont* fn, float fs, float rowX, float rowY, float rowW, const char* lbl, float animT, float alpha, bool showSep, bool last);

void RenderSliderVisuals(ImDrawList* dl, ImFont* fn, float fs, float rowX, float rowY, float rowW, float rowH, const char* lbl, const char* fmt, float v, float animPos, bool active, float alpha, bool showSep, bool last);

bool ToggleRow(const char* id, const char* lbl, bool* v, float& anim, bool last = false, bool first = false);

void TickSliderAnim(AppState::SliderAnim& anim, float target, bool act, float dt);

bool SliderRow(const char* id, const char* lbl, float* v, float mn, float mx, const char* fmt, bool last, bool first, AppState::SliderAnim& anim, float dt);

void CardBg(float h, ImDrawFlags flags = ImDrawFlags_RoundCornersAll);

void SHdr(const char* t, float top = 18.f);

bool CollapsibleHeader(const char* id, const char* lbl, int secId = 0);

bool TickSlideAnim(float& anim, float& vel, bool closing, float dt);
