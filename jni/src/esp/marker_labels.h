#pragma once
// marker_labels.h — Маркеры: подписи и цвета сущностей.
//
// Всё, что модуль отдаёт наружу: сюда смотрят те модули, которым нужны эти
// типы, данные и функции. Реализация — в marker_labels.cpp.
#include "esp/common.h"

// ---- Типы модуля ----

// What a single marker looks like: kind picks the toggle it belongs to, and ore
// markers carry a fixed colour per resource (stone grey, metal orange, sulfur
// yellow) instead of one configurable colour for all of them.
struct MarkerLook {
    int kind = ESP_MARKER_ORE;
    const char* label = nullptr;
    bool has_color = false;
    bool rainbow = false; // drawn in a cycling rainbow colour (elite crates)
    unsigned char rgb[3] = {255, 255, 255};
};

// ---- Данные, которые видят другие модули ----

extern const MarkerLook kSmashBox;

// ---- Функции, которые видят другие модули ----

bool marker_for_entity_type(int32_t type, MarkerLook& look);

bool animal_look_from_object_name(const char* raw, MarkerLook& look);

bool barrel_look_from_object_name(const char* raw, MarkerLook& look);

bool loot_marker(uint64_t component, const char* object_name, const char* root_name, MarkerLook& look);

int read_managed_collection(uint64_t object, uint64_t* out, int max_items);

bool marker_from_loot(uint64_t mineable, MarkerLook& look);

bool gather_marker_from_loot(uint64_t mineable, MarkerLook& look);

bool pickup_marker(uint64_t component, char* label, size_t label_cap);
