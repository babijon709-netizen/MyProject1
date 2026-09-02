#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <sys/types.h>
#include "../include/Vector.h" // Includes Vec3, Vec2 etc.
#include "ImGui/imgui.h"

// -------------------------------------------------------------------------
// PlayerSkeleton struct adapted for external pointers (uint64_t)
// -------------------------------------------------------------------------
struct PlayerSkeleton
{
    uint64_t Player = 0;
    std::string PlayerName = "";

    uint64_t Root = 0;
    uint64_t Armature = 0;

    uint64_t Hips = 0;
    uint64_t Spine = 0;
    uint64_t Spine1 = 0;
    uint64_t Spine2 = 0;
    uint64_t Neck = 0;
    uint64_t Head = 0;

    uint64_t Head_HitBox = 0;
    uint64_t Hips_HitBox = 0;
    uint64_t Spine_HitBox = 0;
    uint64_t Spine1_HitBox = 0;
    uint64_t Spine2_HitBox = 0;

    uint64_t ShoulderL = 0;
    uint64_t ArmL = 0;
    uint64_t ForeArmL = 0;
    uint64_t HandL = 0;

    uint64_t HandIndex1L = 0;
    uint64_t HandIndex2L = 0;
    uint64_t HandIndex3L = 0;

    uint64_t HandMiddle1L = 0;
    uint64_t HandMiddle2L = 0;
    uint64_t HandMiddle3L = 0;

    uint64_t HandPinky1L = 0;
    uint64_t HandPinky2L = 0;
    uint64_t HandPinky3L = 0;

    uint64_t HandRing1L = 0;
    uint64_t HandRing2L = 0;
    uint64_t HandRing3L = 0;

    uint64_t HandThumb1L = 0;
    uint64_t HandThumb2L = 0;
    uint64_t HandThumb3L = 0;

    uint64_t ShoulderR = 0;
    uint64_t ArmR = 0;
    uint64_t ForeArmR = 0;
    uint64_t HandR = 0;

    uint64_t HandIndex1R = 0;
    uint64_t HandIndex2R = 0;
    uint64_t HandIndex3R = 0;

    uint64_t HandMiddle1R = 0;
    uint64_t HandMiddle2R = 0;
    uint64_t HandMiddle3R = 0;

    uint64_t HandPinky1R = 0;
    uint64_t HandPinky2R = 0;
    uint64_t HandPinky3R = 0;

    uint64_t HandRing1R = 0;
    uint64_t HandRing2R = 0;
    uint64_t HandRing3R = 0;

    uint64_t HandThumb1R = 0;
    uint64_t HandThumb2R = 0;
    uint64_t HandThumb3R = 0;

    uint64_t UpLegL = 0;
    uint64_t LegL = 0;
    uint64_t FootL = 0;
    uint64_t ToeBaseL = 0;

    uint64_t UpLegR = 0;
    uint64_t LegR = 0;
    uint64_t FootR = 0;
    uint64_t ToeBaseR = 0;

    bool Cached = false;
};

// -------------------------------------------------------------------------
// Callbacks to decouple from game.cpp's static functions
// -------------------------------------------------------------------------
typedef bool (*W2S_Callback)(const Vec3& world, Vec2& screen);
typedef bool (*GetTransformPosition_Callback)(uint64_t transform, Vec3& outPos);

// Initialize PID for remote reading
void Skeleton_SetPID(pid_t pid);

// Set Unity offsets if needed (default Unity 2019-2021)
void Skeleton_SetOffsets(uint64_t goOffset = 0x30, uint64_t nameOffset = 0x60, uint64_t childrenOffset = 0x70);

// Recursively caches bones matching exact names externally
void CacheSkeleton(uint64_t transform, PlayerSkeleton& skel, int depth = 0);

// Draws all bones connected in a skeleton structure
void DrawSkeletonESP(std::unordered_map<uint64_t, PlayerSkeleton>& skeletonCache, std::vector<uint64_t>& activePlayers, W2S_Callback w2s_cb, GetTransformPosition_Callback pos_cb, float screenHeight, ImU32 color);

extern bool ESP_Skeleton;
