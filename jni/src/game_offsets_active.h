#pragma once
// Статические оффсеты. Автоматический runtime-резолвер отключён: все места
// чтения используют значения из game_offsets.h, как в обычной сборке проекта.
#include "game_offsets.h"

namespace go_active {
#define GO_FIELD(name, beta_value) inline std::uint64_t name = game_offsets::name;
#define GO_CONST(name, beta_value) inline float name = game_offsets::name;
#include "game_offsets_active.inc"
#undef GO_FIELD
#undef GO_CONST
using game_offsets::ToolPurpose;
using game_offsets::MineableEntityType;
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
