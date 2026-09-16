#pragma once
// farm/state.h — g_farm*: состояние автофарма для окна.
//
// Всё, что модуль отдаёт наружу: сюда смотрят те модули, которым нужны эти
// типы, данные и функции. Реализация — в farm/state.cpp.
#include "app/common.h"

// ---- Данные модуля ----

extern bool g_farmActive;

extern int g_farmPhase;

extern int g_farmNodes;

extern int g_farmReason;

extern float g_farmTgtDist;

extern float g_farmReach;

extern int g_farmTgtKind;

extern int g_farmSpot;

extern bool g_farmPaused;

extern int g_farmStreak;

extern float g_farmHpPct;

extern float g_farmSpotLife;

extern float g_farmAttackPeriod;

extern int g_farmToolHave;

extern int g_farmToolNeed;

extern int g_farmXp;

extern bool g_farmBlocked;
