#pragma once
// aim/controller.h — Параметры аима, чувствительность, палец.
//
// Всё, что модуль отдаёт наружу: сюда смотрят те модули, которым нужны эти
// типы, данные и функции. Реализация — в aim/controller.cpp.
#include "app/common.h"

// ---- Константы модуля ----

inline constexpr float kAimGainAtRef      = 0.10f; // измерено на устройстве при ней же

// ---- Данные модуля ----

extern bool s_fingerDown;

// ---- Типы модуля ----

struct AimTarget {
    bool  valid = false;
    unsigned long long id = 0;
    float yaw = 0.f, pitch = 0.f;   // degrees from crosshair (+right, +up)
    float sx = 0.f, sy = 0.f;       // screen position (px)
    float dist = 0.f;               // pixel distance from crosshair
    float world_dist = 0.f;
    int   bone = -1;                // слот точки прицела (0 голова, 1 шея, 2 грудь)
};

// ---- Функции, которые видят другие модули ----

ImU32 ColU32(const ImVec4& c);

float AimFovRadiusPx(float sw, float sh);

void AimReleaseFinger(bool& fingerDown);

float AimSensitivityScale(bool& from_game);

float AimSensitivityGain(bool& from_game);
