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
// Геометрия пилюли-счётчика за последний отрисованный кадр — калибровка в
// Опциях → Худ рисует по ней живое превью. DrawWatermark бежит раньше меню в
// том же кадре, так что данные всегда свежие.
struct HudCounterGeom { float bX = 0.f, bY = 0.f, bW = 0.f, bH = 0.f; };
const HudCounterGeom& HudCounterLast();
// Экранная кнопка фрикама (квадрат с глазом): сторона и радиус скругления.
// Положение по умолчанию — в блоке кнопок слева, под пилюлей-счётчиком
// (см. DrawWatermark); калибровка в меню рисует превью по тем же константам.
constexpr float kFcBtnSide = 100.f;
constexpr float kFcBtnRadius = 26.f;
