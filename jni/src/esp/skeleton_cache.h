#pragma once
// skeleton_cache.h — Кеш скелетов и переключатели.
//
// Всё, что модуль отдаёт наружу: сюда смотрят те модули, которым нужны эти
// типы, данные и функции. Реализация — в skeleton_cache.cpp.
#include "esp/common.h"
#include "esp/transform.h"

// ---- Типы модуля ----

struct CachedSkeleton {
    uint64_t bone_transform[ESP_BONE_COUNT] = {};
    Vec3     bone_world[ESP_BONE_COUNT] = {};
    uint8_t  bone_world_age[ESP_BONE_COUNT] = {}; // 0 = none, 1 = fresh, grows on reuse
    uint64_t model_root = 0;
    int      bone_count = 0;
    bool     valid = false;
    int      retry_cooldown = 0;
    int      fail_streak = 0;
    int      revalidate_timer = 0;
};

// ---- Данные, которые видят другие модули ----

extern std::unordered_map<uint64_t, CachedSkeleton> g_skeletons;

extern bool g_skeleton_enabled;

extern bool g_aim_bones_requested;

extern TransformHierarchyLayout g_skeleton_layout;

extern bool g_skeleton_layout_valid;

extern uint64_t g_go_name_offset;

extern bool g_go_name_plain_pointer;

extern bool g_go_name_offset_valid;

extern double g_go_name_retry_at;

extern int g_skeleton_builds_this_frame;

// ---- Функции, которые видят другие модули ----

void prune_skeleton_cache(const std::vector<uint64_t>& players);
