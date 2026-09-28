#pragma once
// Статические оффсеты. Автоматический runtime-резолвер отключён: все места
// чтения используют значения из game_offsets.h, как в обычной сборке проекта.
#include "game_offsets.h"

namespace go_active {
using namespace game_offsets;
}

// Оставляем совместимый алиас для старых диагностических участков кода;
// значения всё равно берутся из обычного статического набора.
namespace game_offsets_beta {
using namespace game_offsets;
}

namespace go {
enum class Build : int { Release = 0, Beta = 1 };
inline Build CurrentBuild() { return Build::Release; }
inline bool BetaAvailable() { return false; }
inline void SelectBuild(Build) {}
}
