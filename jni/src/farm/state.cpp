// farm/state.cpp — g_farm*: состояние автофарма для окна.
//
// Модуль разрезан из прежнего монолита jni/src/main.cpp;
// что здесь лежит и кто это зовёт — в шапке farm/state.h и в docs/CODE_MAP.md.


#include "app/common.h"
#include "farm/controller.h"
#include "farm/state.h"

// Автофарм: статус для строки во вкладке «Разное» (определены рядом с
// UpdateFarm ниже).
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

bool g_farmActive = false;   // статус для строки в окне автофарма

int  g_farmPhase = 0;        // 0 простой, 1 поворот, 2 подход, 3 удар

int  g_farmNodes = 0;        // узлов насчитал последний скан реестра

int  g_farmReason = 1;       // причина простоя из esp_farm_debug()

float g_farmTgtDist = 0.f;   // метры до точки прицела

float g_farmReach = 0.f;     // FPMelee.m_MaxReach + hitRadius орудия в руках, м

int  g_farmTgtKind = 0;      // ресурс текущей цели

int  g_farmSpot = 0;         // 0 — крестика нет, иначе его источник (1 руда, 2/3 дерево)

bool g_farmPaused = false;   // цель видна, но ввод остановлен (открыто меню / работает аимбот)

int  g_farmStreak = 0;       // hitstreakIndex: сколько подряд попали в крестик

float g_farmHpPct = -1.f;    // здоровье узла в процентах (-1 = не читается)

float g_farmSpotLife = -1.f; // сколько секунд осталось жить крестику (-1 = неизвестно)

float g_farmAttackPeriod = 0.f; // секунд между ударами орудия (0 = неизвестно)

int  g_farmToolHave = 0;     // флаги ToolPurpose орудия в руках (0 = неизвестно)

int  g_farmToolNeed = 0;     // какие умения запросили отброшенные узлы

int  g_farmXp = 0;           // сколько опыта даёт текущий узел (0 = неизвестно)

bool g_farmBlocked = false;  // узел перекрыт: луч игры упёрся ближе точки прицела
