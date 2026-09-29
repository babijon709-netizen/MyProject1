#pragma once
// Автофарм: контроллер синтетических тачей (джойстик, камера, удары).
// Статусные g_farm* — для строки во вкладке «Разное» (см. RenderMenu).

void UpdateFarm(float dt);


extern bool g_farmActive;
extern int  g_farmPhase;
extern int  g_farmNodes;      // сколько узлов нашёл последний скан реестра
extern int  g_farmReason;     // причина простоя (см. esp_farm_debug)
extern float g_farmTgtDist;   // дистанция до точки прицела, м
extern float g_farmReach;     // живая дальность удара орудия, м (0 = не прочиталась)
extern int  g_farmTgtKind;    // 0 дерево, 1 камень, 2 металл, 3 сера
extern int  g_farmSpot;       // 0 крестика нет, иначе его источник (1 руда, 2/3 дерево)
extern bool g_farmPaused;     // бот на паузе (открыто меню / камеру ведёт аимбот)
extern int  g_farmStreak;     // сколько попаданий подряд игра засчитала в крестик
extern float g_farmHpPct;     // здоровье узла в процентах
extern float g_farmSpotLife;  // остаток жизни крестика, с
extern float g_farmAttackPeriod; // ритм ударов орудия, с
extern int  g_farmToolHave;   // что умеет орудие в руках (флаги ToolPurpose)
extern int  g_farmToolNeed;   // что нужно ближайшим узлам (флаги ToolPurpose)
extern int  g_farmXp;         // опыт за текущий узел
extern bool g_farmBlocked;    // узел перекрыт

// Калибровка зон вводом с экрана (меню прячется, тап записывает точку):
// 0 — нет, 1 — зона джойстика автофарма, 2 — зона огня автофарма,
// 3 — точка, из которой аимбот водит палец. См. RenderMenu.
extern int  g_calibMode;