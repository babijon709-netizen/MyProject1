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
