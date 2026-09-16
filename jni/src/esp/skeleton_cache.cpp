// skeleton_cache.cpp — Кеш скелетов и переключатели.
//
// Модуль разрезан из прежнего монолита jni/src/game.cpp;
// что здесь лежит и кто это зовёт — в шапке skeleton_cache.h и в docs/CODE_MAP.md.

#include "esp/common.h"
#include "esp/aim_points.h"
#include "esp/boxes.h"
#include "esp/farm_scan.h"
#include "esp/frame.h"
#include "esp/markers.h"
#include "esp/player_pose.h"
#include "esp/skeleton_build.h"
#include "esp/skeleton_names.h"
#include "esp/transform.h"
#include "esp/weapons.h"
#include "skeleton_cache.h"

std::unordered_map<uint64_t, CachedSkeleton> g_skeletons;

bool g_skeleton_enabled = false;

// The aimbot needs bone positions regardless of the skeleton ESP toggle. Bone
// resolution is therefore driven by (skeleton ESP || aim requested).
bool g_aim_bones_requested = false;

TransformHierarchyLayout g_skeleton_layout{};

bool g_skeleton_layout_valid = false;

uint64_t g_go_name_offset = 0;

bool     g_go_name_plain_pointer = false; // fallback: name stored as raw char*

bool     g_go_name_offset_valid = false;

double   g_go_name_retry_at = 0.0;   // mono_seconds: не раньше этого момента

int      g_skeleton_builds_this_frame = 0; // heavy rescans: max 1 per frame

void esp_set_skeleton_enabled(bool enabled) { g_skeleton_enabled = enabled; }

void esp_set_aim_bones_enabled(bool enabled) { g_aim_bones_requested = enabled; }

void prune_skeleton_cache(const std::vector<uint64_t>& players) {
    for (auto it = g_skeletons.begin(); it != g_skeletons.end();) {
        bool present = false;
        for (uint64_t player : players) {
            if (player == it->first) { present = true; break; }
        }
        if (!present) it = g_skeletons.erase(it);
        else ++it;
    }
}

uint64_t resolve_player_kcc(uint64_t player);
