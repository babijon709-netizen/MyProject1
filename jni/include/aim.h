#pragma once
// Аимбот: ведёт камеру синтетическим «пальцем взгляда».
// Коэффициент град/px выводится из настройки чувствительности клиента
// (AimSensitivityScale/Gain) — автофарм доводит камеру теми же функциями.

void UpdateAim(float dt);
float AimSensitivityScale(bool& from_game);
float AimSensitivityGain(bool& from_game);

// Палец взгляда аимбота сейчас на экране (автофарм уступает камеру, пока он ведёт).
extern bool s_fingerDown;
// Запасной коэффициент град/px (измерено на устройстве при чувствительности 2.0).
constexpr float kAimGainAtRef = 0.10f;

// Точка, из которой аимбот водит палец (доли экрана; дефолт 74%/50%).
float AimTouchFracX();
float AimTouchFracY();
