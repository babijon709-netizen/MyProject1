#pragma once
// Конфиги (файлы .cfg в каталоге benzware): всё состояние UI и режим чтения
// живут внутри конфига, inotify-наблюдение каталога. Каталог — CfgDir().

#include <string>

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
// Восстановление из самого свежего конфига (при старте, до загрузки конфига).
void RestoreLang();
void RestoreAccent();
void RestoreLayout();
// Режим чтения, выбранный на экране выбора; -1 если ничего нет.
// Источник: /storage/emulated/0/benzware/.mode (пишется каждым выбором),
// затем (legacy) трейлер самого свежего конфига.
int  CfgSavedMemBackend();
// Записать выбор экрана режима в .mode (значения KERNEL.md §6).
// Только при реальном выборе; закрытие экрана без тапа не пишет.
void CfgWriteMemBackend(int backend);
