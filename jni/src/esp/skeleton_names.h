#pragma once
// skeleton_names.h — Скелет: имена костей, поиск скелета игрока, KCC/ragdoll.
//
// Всё, что модуль отдаёт наружу: сюда смотрят те модули, которым нужны эти
// типы, данные и функции. Реализация — в skeleton_names.cpp.
#include "esp/common.h"

// ---- Типы модуля ----

enum SkeletonBone {
    BONE_HIPS = 0, BONE_SPINE, BONE_SPINE1, BONE_SPINE2, BONE_NECK, BONE_HEAD,
    BONE_SHOULDER_L, BONE_ARM_L, BONE_FOREARM_L, BONE_HAND_L,
    BONE_SHOULDER_R, BONE_ARM_R, BONE_FOREARM_R, BONE_HAND_R,
    BONE_UPLEG_L, BONE_LEG_L, BONE_FOOT_L, BONE_TOE_L,
    BONE_UPLEG_R, BONE_LEG_R, BONE_FOOT_R, BONE_TOE_R
};

// ---- Функции, которые видят другие модули ----

uint64_t skeleton_model_root(uint64_t player);

int read_transform_children(uint64_t transform, uint64_t* out, int max_children);

void collect_transform_subtree(uint64_t root, std::vector<uint64_t>& nodes, size_t max_nodes);

int match_bone_name(const char* raw_name);

bool read_gameobject_name_at(uint64_t native_go, uint64_t name_offset, bool plain_pointer, char* out, size_t cap);

bool read_transform_name(uint64_t transform, char* out, size_t cap);

bool discover_gameobject_name_offset(const std::vector<uint64_t>& nodes);

bool resolve_skeleton_layout(uint64_t sample_transform);

uint64_t managed_object_native(uint64_t managed);

bool skeleton_transform_ptr_valid(uint64_t transform);

uint64_t native_component_transform(uint64_t native_component);

uint64_t resolve_player_kcc(uint64_t player);

uint64_t resolve_player_ragdoll(uint64_t player, uint64_t& kcc_out);

int read_parent_chain_remote(uint64_t indices, int32_t index, int32_t* chain, int cap);

uint64_t skeleton_child_on_chain(uint64_t parent_transform, uint64_t data, const int32_t* chain, int chain_length);

bool ensure_gameobject_name_offset(uint64_t player);
