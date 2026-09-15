#pragma once

bool Touch_Init(int w, int h, uint32_t orientation_, bool readOnly);
void UpdateScreenData(int w, int h, uint32_t orientation_);

void Touch_Close();
void Touch_Down(float x, float y);
void Touch_Move(float x, float y);
void Touch_Up();
// Touch-controller units per screen pixel (>= 1 when the digitizer is finer
// than the display). The synthetic finger can only stop on that grid.
float Touch_DeviceUnitsPerPixel();

// Extra synthetic fingers for the auto-farm (0 = move joystick, 1 = look,
// 2 = tap). Separate uinput slots, so they can act at the same time as the
// aimbot finger driven by Touch_Down/Touch_Move/Touch_Up.
void Touch_Down_N(int finger, float x, float y);
void Touch_Up_N(int finger);
// True when synthetic touches actually reach the game (uinput device was
// created). False in read-only fallback mode — everything that injects
// touches (aimbot, auto-farm) silently does nothing then.
bool Touch_CanInject();

// Диагностика: сколько раз Upload() собрал пакет синтетического тача и сколько
// миллисекунд на это ушло суммарно (счётчики накопительные). Хозяин цикла
// оверлея снимает их раз в кадр, считая разницу, — по этой паре видно, сколько
// лага приносит инъекция касаний, а не чтение памяти игры.
void Touch_UploadStats(unsigned long long& calls, double& ms);
