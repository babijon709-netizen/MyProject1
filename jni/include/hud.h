#pragma once
// HUD поверх игры: тосты, водяной знак, геометрия видимого экрана.

void ShowToast(const char* msg);
void DrawToast(float dt);
void DrawWatermark(float dt);
// Пилка-водяной знак переключает видимость меню (тап по знаку). Меню сбрасывает
// его в true при открытии — отсюда extern.
extern bool menu_open;
// Размеры видимого экрана с учётом ориентации (displayInfo может быть
// «перевёрнутым» — нормуем).
void VisibleScreen(float& w, float& h);
