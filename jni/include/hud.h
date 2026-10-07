#pragma once
// HUD поверх игры: тосты, водяной знак, геометрия видимого экрана.

void ShowToast(const char* msg);
// Скрыть текущий тост без показа замены (например, старую ошибку другого
// драйвера после успешного выбора режима чтения).
void ClearToast();
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
// Экранная кнопка фрикама (квадрат с глазом): сторона, радиус скругления и
// дефолтное положение (доли экрана = центр кнопки). Размер фиксирован,
// поэтому калибровка в меню рисует превью по тем же константам.
constexpr float kFcBtnSide = 100.f;
constexpr float kFcBtnRadius = 26.f;
constexpr float kFcBtnDefX = 0.90f;
constexpr float kFcBtnDefY = 0.50f;
// Экранная кнопка FakeLag: квадрат с надписью FL, дефолт справа чуть ниже
// фрикама чтобы не перекрывать. Калибровка 8 — та же система что у фрикама.
constexpr float kFlBtnSide = 100.f;
constexpr float kFlBtnRadius = 26.f;
constexpr float kFlBtnDefX = 0.90f;
constexpr float kFlBtnDefY = 0.66f;
