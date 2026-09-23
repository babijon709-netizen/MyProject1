#pragma once
// ASCII-примитивы «терминала Linux»: рамки псевдографики, линейки,
// чекбоксы [x], радио (●), полосы из блоков, сканлайны ЭЛТ, мигающий
// курсор. Весь стиль GUI строится на этих примитивах поверх моноширинного
// шрифта (DejaVu Sans Mono, см. Font/TermFont.h).
//
// Все функции рисуют СИМВОЛАМИ через ImDrawList::AddText — ни одного
// векторного скругления: рамка из ┌─┐│└┘ выглядит в терминале ровно так же.
// Кегль по умолчанию — кегль меню (ImGui::GetFontSize()); ячейка знакоместа
// берётся из ширины моносимвола, поэтому сетка рамок всегда совпадает с
// текстом строкой ниже/выше.

#include "imgui.h"

namespace ascii {

// Малый кегль моноширинного шрифта (для ASCII-видео в кружке и мелких
// надписей). Регистрируется при старте из draw.cpp; если не зарегистрирован —
// возвращается основной шрифт меню.
void    SetSmallFont(ImFont* f);
ImFont* SmallFont();

// Ячейка знакоместа (ширина/высота) моноширинного шрифта при кегле fs.
// fs <= 0 — текущий кегль меню, fn == nullptr — текущий шрифт.
ImVec2 Cell(ImFont* fn, float fs);
ImVec2 Cell(float fs = 0.f);

// Рамка псевдографики по прямоугольнику mn..mx (изнутри, символами).
// dbl = true — двойная линия ╔═╗ (окно терминала), false — одинарная ┌─┐
// (карточки, поповеры). Если в прямоугольник влезает меньше двух знакомест
// по любой оси — не рисуется.
void Frame(ImDrawList* dl, const ImVec2& mn, const ImVec2& mx, ImU32 col,
           bool dbl = false, float fs = 0.f);

// Горизонтальная линейка «────[ Текст ]────» (label может быть nullptr —
// тогда просто линия). cy — вертикаль ЦЕНТРА строки символов.
// leftAlign = true — «Текст ────» от левого края.
void HRule(ImDrawList* dl, const char* label, float x0, float x1, float cy,
           ImU32 col, ImU32 labelCol = 0, float fs = 0.f, bool leftAlign = false);

// Вертикальная линейка «│» сверху вниз (разделитель панелей).
void VRule(ImDrawList* dl, float x, float y0, float y1, ImU32 col, float fs = 0.f);

// Чекбокс «[x]»/«[ ]»: pos — левый верхний угол, t — анимация 0..1
// (пока 0 < t < 1, «x» проявляется прозрачностью). Ширина — 3 знакоместа.
void Check(ImDrawList* dl, const ImVec2& pos, float t,
           ImU32 onCol, ImU32 offCol, float fs = 0.f);

// Радиокнопка «(●)»/«( )» с центром в точке c. Ширина — 3 знакоместа.
void Radio(ImDrawList* dl, const ImVec2& c, bool on,
           ImU32 onCol, ImU32 offCol, float fs = 0.f);

// Полоса-слайдер «[████▌░░░░]» длиной cells знакомест. pos — левый верхний
// угол. Заполненная часть — onCol, хвост — offCol; активный хвост ярче сам
// по себе не становится — яркоту задают цвета вызывающего.
void Bar(ImDrawList* dl, const ImVec2& pos, float frac, int cells,
         ImU32 onCol, ImU32 offCol, float fs = 0.f);

// Сканлайны ЭЛТ: тёмные строки через каждые 4 px поверх прямоугольника.
void Scanlines(ImDrawList* dl, const ImVec2& mn, const ImVec2& mx,
               float alpha = 0.05f);

// Терминальный курсор «▌»: общая фаза мигания (период 1.06 с — как в
// linux-консоли), поэтому все курсоры интерфейса мигают синхронно.
bool BlinkOn();
void Cursor(ImDrawList* dl, const ImVec2& pos, ImU32 col, float fs = 0.f);

// Инверсный текст (выделение как в mc/tmux): плашка bg + текст fg.
void InverseText(ImDrawList* dl, const ImVec2& pos, const char* text, float fs,
                 ImU32 bg, ImU32 fg, float alpha = 1.f);

}  // namespace ascii
