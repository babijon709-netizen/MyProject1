#pragma once
// transform.h — Иерархия Transform: где у объекта позиция и как её читать.
//
// Всё, что модуль отдаёт наружу: сюда смотрят те модули, которым нужны эти
// типы, данные и функции. Реализация — в transform.cpp.
#include "esp/common.h"

// ---- Типы модуля ----

struct TransformHierarchyLayout {
    uint64_t data_offset = 0x38;
    uint64_t index_offset = 0x40;
    uint64_t matrices_offset = 0x18;
    uint64_t indices_offset = 0x20;
    bool matrices_indirect = false;
    bool indices_indirect = false;
};

// ---- Данные, которые видят другие модули ----

extern TransformHierarchyLayout g_transform_hierarchy_layout;

extern bool g_transform_hierarchy_layout_valid;

extern bool g_use_direct_player_position;

extern bool g_player_position_validated;

extern int g_direct_position_fail_streak;

extern int g_direct_position_recheck;

extern bool g_body_caches_dirty;

// ---- Функции, которые видят другие модули ----

bool read_transform_hierarchy_arrays(uint64_t matrices, uint64_t indices, int32_t transform_index, Vec3& position, Vec4* world_rotation = nullptr);

bool read_transform_hierarchy_layout(uint64_t native_transform, const TransformHierarchyLayout& layout, Vec3& position, Vec4* world_rotation = nullptr);

bool read_transform_hierarchy_position(uint64_t native_transform, Vec3& position);

uint64_t resolve_player_native_transform(uint64_t player);

bool discover_layout_from_native_transforms(const std::vector<uint64_t>& native_transforms, size_t& best_position_count, size_t& candidate_count);

bool read_entity_position(uint64_t source, Vec3& position);

bool read_entity_pose(uint64_t source, Vec3& position, Vec4& rotation);

bool position_looks_like_world_space(const Vec3& position);

bool discover_player_position_offset(const std::vector<uint64_t>& players);

void recheck_direct_player_position(const std::vector<uint64_t>& players);
