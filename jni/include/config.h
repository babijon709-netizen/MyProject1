#pragma once
// Конфиги (файлы .cfg в каталоге benzhack), язык, выбор версии (релиз/бета),
// inotify-наблюдение каталога. Каталог — CfgDir().

#include <string>
#include "game_offsets_active.h"   // go::Build

constexpr int kMaxConfigs = 12;

struct ConfigEntry { char name[64] = {}; };
extern ConfigEntry g_configs[kMaxConfigs];
extern int  g_configCount;
extern int  g_configToDelete;
extern int  g_cfgLoadedIdx;
extern char g_loadedConfigName[64];
extern float g_cfgLoadAnim[kMaxConfigs];

const char* CfgDir();
void CfgScanDir();
void CfgWatchInit();
void CfgWatchFree();
void CfgWatchTick();
void ConfigSave();
void ConfigUpdate(int idx);
void ConfigLoad(int idx, bool announce = true);
void ConfigLoadLast();
void ConfigDelete(int idx);
void ApplyBuildChoice(go::Build b);
void RestoreLang();
void RestoreBuild();
void RememberLang();
void RememberBuild();
