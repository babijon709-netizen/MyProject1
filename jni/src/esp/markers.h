#pragma once
// markers.h — Маркеры мира: скан реестра и выдача.
//
// Всё, что модуль отдаёт наружу: сюда смотрят те модули, которым нужны эти
// типы, данные и функции. Реализация — в markers.cpp.
#include "esp/common.h"

// ---- Типы модуля ----

// Il2CppClass -> which of the two component families this is (cached: the same
// handful of classes come back for every entity in the registry).
enum MarkerClass : uint8_t {
    MARKER_CLASS_NONE = 0, MARKER_CLASS_MINEABLE = 1,
    MARKER_CLASS_LOOT = 2, MARKER_CLASS_PICKUP = 3,
    MARKER_CLASS_BARREL = 4,
    // Имя класса прочитать не удалось (см. class_identity): семейство неизвестно,
    // и объект опознаётся дальше по имени GameObject — оно лежит в куче и читается.
    MARKER_CLASS_UNKNOWN = 5,
};

// ---- Функции, которые видят другие модули ----

uint8_t marker_class_of(uint64_t klass);

uint64_t resolve_network_client_spawned();

uint64_t resolve_network_identity_class();

bool marker_world_position(uint64_t transform, Vec3& out);

void reset_marker_caches();
