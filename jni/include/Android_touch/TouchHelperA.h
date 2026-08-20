#pragma once

bool Touch_Init(int w, int h, uint32_t orientation_, bool readOnly);
void UpdateScreenData(int w, int h, uint32_t orientation_);

void Touch_Close();
void Touch_Down(float x, float y);
void Touch_Move(float x, float y);
void Touch_Up();

// Aim finger: runs in parallel with the user's own touches.
void Touch_AimDown(float x, float y);
void Touch_AimMove(float x, float y);
void Touch_AimUp();