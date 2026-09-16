#pragma once
// ui/config.h — Конфиги: файлы, XOR, слежение за каталогом.
//
// Всё, что модуль отдаёт наружу: сюда смотрят те модули, которым нужны эти
// типы, данные и функции. Реализация — в ui/config.cpp.
#include "app/common.h"

// ---- Константы модуля ----

inline constexpr int  kMaxConfigs  = 12;

// ---- Данные модуля ----

extern int g_configCount;

extern int g_configToDelete;

extern float g_cfgLoadAnim[kMaxConfigs];

extern int g_cfgLoadedIdx;

// ---- Типы модуля ----

struct ConfigEntry { char name[64] = {}; };

// ---- Константы и данные, ссылающиеся на типы модуля ----

extern ConfigEntry g_configs[kMaxConfigs];

// ---- Функции, которые видят другие модули ----

const char* kCfgDir_() noexcept;

void RememberLang();

void RestoreLang();

void RestoreBuild();

void ApplyBuildChoice(go::Build b);

void CfgScanDir();

void CfgWatchInit();

void CfgWatchFree();

void CfgWatchTick();

void ConfigSave();

void ConfigUpdate(int idx);

void ConfigLoad(int idx, bool announce = true);

void ConfigLoadLast();

void ConfigDelete(int idx);
