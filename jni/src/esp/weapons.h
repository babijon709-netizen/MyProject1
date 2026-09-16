#pragma once
// weapons.h — Оружие: метка в руках игрока.
//
// Всё, что модуль отдаёт наружу: сюда смотрят те модули, которым нужны эти
// типы, данные и функции. Реализация — в weapons.cpp.
#include "esp/common.h"

// ---- Функции, которые видят другие модули ----

bool player_weapon_name(uint64_t player, char* out, size_t cap, bool& definite);

bool managed_component_gameobject_name(uint64_t managed_component, char* out, size_t cap);
