#pragma once
// skeleton_build.h — Сборка скелета из ragdoll и по именам.
//
// Всё, что модуль отдаёт наружу: сюда смотрят те модули, которым нужны эти
// типы, данные и функции. Реализация — в skeleton_build.cpp.
#include "esp/common.h"
#include "esp/skeleton_cache.h"

// ---- Функции, которые видят другие модули ----

bool build_skeleton(uint64_t player, CachedSkeleton& skeleton);

int skeleton_local_walk(const Matrix34* matrices, const int32_t* parents, int32_t count, int32_t index, Vec3& out);
