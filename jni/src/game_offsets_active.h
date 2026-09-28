#pragma once

// Единственный поддерживаемый клиент — текущий релиз. Все адреса являются
// compile-time константами из проверенного release-дампа: выбора набора,
// автопоиска и вычисления смещений в запущенной игре нет.
#include "game_offsets.h"

// Сохраняем прежнее имя пространства для потребителей оффсетов. using-declaration
// экспортирует constexpr-значения game_offsets без изменяемой таблицы в памяти.
namespace go_active {
using namespace game_offsets;
} // namespace go_active
