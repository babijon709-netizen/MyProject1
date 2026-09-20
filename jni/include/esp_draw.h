#pragma once
// ESP-оверлей: один снимок боксов/маркеров на кадр + отрисовка.
// FrameBoxes() шарят аимбот и автофарм (те же боксы, один снимок).

#include <vector>
#include "game.h"

void EspScreenSize(float& sw, float& sh);
// Снимок боксов/маркеров с памяти игры. Зовут с рабочего потока памяти,
// чтобы поток презента оверлея не ждал process_vm_readv.
void EspHarvestFrame(float sw, float sh);
void DrawEspOverlay();
const std::vector<EspBox>& FrameBoxes(float sw, float sh);
// Радиус круга FOV аима в px (см. комментарий в реализации).
float AimFovRadiusPx(float sw, float sh);
