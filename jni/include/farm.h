#pragma once
// Magic-Farm: контроллер «тихой» добычи.
//
// Бот больше не водит игрока за руку: ходит пользователь, а чит делает три
// вещи, каждая — записью в те же поля, по которым решение принимает сама игра
// (см. «Magic-Farm: ядро» в game.cpp):
//   1. Silent Melee — результат рейкаста игры (GKo) подменяется выбранным
//      узлом, поэтому штатный удар уходит в ресурс, даже когда камера смотрит
//      в другую сторону;
//   2. удары «по крестику» — точкой попадания всегда ставится живой маркер
//      серии (OreHitstreaks/TreeHitstreaks), так что каждый удар растит серию
//      и бонусную выдачу;
//   3. Melee TP — дальность удара и сфера луча взаимодействия раздуваются до
//      заданного радиуса, и узлы ломаются просто вокруг игрока.
// Единственный синтетический ввод, который остался, — тап по кнопке атаки
// (переключатель «Авто-удары»); без него чит обслуживает удары самого игрока.

void UpdateFarm(float dt);

extern bool  g_farmActive;       // в радиусе есть цель, по которой можно бить
extern int   g_farmPhase;        // 0 простой, 2 ждём такт орудия, 3 удар
extern bool  g_farmPaused;       // запись остановлена (меню / калибровка зон)
extern int   g_farmNodes;        // узлов в радиусе Magic-Farm прямо сейчас
extern int   g_farmScanned;      // узлов нашёл последний скан реестра
extern int   g_farmReason;       // причина простоя (см. esp_farm_debug)
extern float g_farmTgtDist;      // 3D-метры до точки удара текущей цели
extern float g_farmReach;        // живая дальность удара орудия, м (0 = не читается)
extern float g_farmTpReach;      // дальность, которую прописал Melee TP (0 = выкл)
extern bool  g_farmTpActive;     // Melee TP реально пишет в орудие
extern int   g_farmTgtKind;      // 0 дерево, 1 камень, 2 металл, 3 сера, 4 бочка
extern int   g_farmSpot;         // 0 крестика нет, иначе его источник (1 руда, 2/3 дерево)
extern int   g_farmStreak;       // попаданий подряд в крестик
extern float g_farmHpPct;        // здоровье узла в процентах
extern float g_farmSpotLife;     // остаток жизни крестика, с
extern float g_farmAttackPeriod; // ритм ударов орудия, с
extern int   g_farmToolHave;     // что умеет орудие в руках (флаги ToolPurpose)
extern int   g_farmToolNeed;     // что нужно отброшенным узлам
extern int   g_farmXp;           // опыт за текущий узел
extern int   g_farmTemplates;    // выучено шаблонов попадания (узлов)
extern int   g_farmHits;         // перенаправлено ударов за сессию
extern int   g_farmIndexed;      // узлов разобрано в индекс попаданий
extern int   g_farmNodesWorld;   // узлов в реестре (весь прогруженный мир)
extern int   g_farmLearn;        // луч: 0 нет, 1 есть, 2 опознан как узел
extern int   g_farmSwings;       // авто-ударов сделано за сессию
extern int   g_farmGkoHit;       // оффсет GameObject попадания внутри GKo
extern int   g_farmGkoRh;        // оффсет RaycastHit внутри GKo
extern bool  g_farmGkoKnown;     // разметка опознана по живому попаданию
extern int   g_farmRayOffA;      // найденные оффсеты обёрток рейкаста
extern int   g_farmRayOffB;
extern int   g_farmRayStage;     // стадия цепочки рейкаста игры (0..6)
extern int   g_farmRayBest;      // лучшая стадия за сессию
extern int   g_farmPinHold;      // % проверок, где подмена рейкаста уцелела

// Калибровка зон вводом с экрана (меню прячется, тап записывает точку):
// 0 — нет, 2 — кнопка удара Magic-Farm, 3 — точка аима, 4 — джойстик фрикама,
// 5 — HUD, 6 — кнопка фрикама, 7 — круглая зона обзора фрикама. См. RenderMenu.
extern int   g_calibMode;
