#pragma once

// ============================================================================
//  ESP skeleton (bone) data.
//
//  The bone list below is taken from the Oxide: Survival Island Il2Cpp dump
//  (dump 1.7z -> dump.cs -> `HumanBodyBones` enum, UnityEngine.AnimationModule,
//  TypeDefIndex 27326). The game uses a standard Unity humanoid rig, so the
//  skeleton is drawn as a proportional stick figure over the player bounding
//  box produced by esp_get_boxes().
//
//  Joint positions are normalized to the box: u in [0..1] across the width,
//  v in [0..1] from the top (head) to the bottom (feet) of the box.
// ============================================================================

namespace skeleton {

enum Bone : int {
    BONE_HIPS = 0,
    BONE_SPINE,
    BONE_CHEST,
    BONE_UPPER_CHEST,
    BONE_NECK,
    BONE_HEAD,
    BONE_LEFT_SHOULDER,
    BONE_LEFT_UPPER_ARM,
    BONE_LEFT_LOWER_ARM,
    BONE_LEFT_HAND,
    BONE_RIGHT_SHOULDER,
    BONE_RIGHT_UPPER_ARM,
    BONE_RIGHT_LOWER_ARM,
    BONE_RIGHT_HAND,
    BONE_LEFT_UPPER_LEG,
    BONE_LEFT_LOWER_LEG,
    BONE_LEFT_FOOT,
    BONE_RIGHT_UPPER_LEG,
    BONE_RIGHT_LOWER_LEG,
    BONE_RIGHT_FOOT,
    BONE_COUNT
};

struct Joint {
    int   bone; // Bone id
    float u;    // normalized horizontal position inside the box (0..1)
    float v;    // normalized vertical position inside the box (0 = top, 1 = bottom)
};

struct Edge {
    int a; // joint index
    int b; // joint index
};

// One entry per bone, laid out proportionally inside the player box.
inline constexpr Joint k_joints[BONE_COUNT] = {
    { BONE_HIPS,             0.50f, 0.50f },
    { BONE_SPINE,            0.50f, 0.38f },
    { BONE_CHEST,            0.50f, 0.30f },
    { BONE_UPPER_CHEST,      0.50f, 0.24f },
    { BONE_NECK,             0.50f, 0.18f },
    { BONE_HEAD,             0.50f, 0.08f },
    { BONE_LEFT_SHOULDER,    0.40f, 0.24f },
    { BONE_LEFT_UPPER_ARM,   0.34f, 0.34f },
    { BONE_LEFT_LOWER_ARM,   0.30f, 0.46f },
    { BONE_LEFT_HAND,        0.27f, 0.56f },
    { BONE_RIGHT_SHOULDER,   0.60f, 0.24f },
    { BONE_RIGHT_UPPER_ARM,  0.66f, 0.34f },
    { BONE_RIGHT_LOWER_ARM,  0.70f, 0.46f },
    { BONE_RIGHT_HAND,       0.73f, 0.56f },
    { BONE_LEFT_UPPER_LEG,   0.42f, 0.62f },
    { BONE_LEFT_LOWER_LEG,   0.40f, 0.80f },
    { BONE_LEFT_FOOT,        0.40f, 0.96f },
    { BONE_RIGHT_UPPER_LEG,  0.58f, 0.62f },
    { BONE_RIGHT_LOWER_LEG,  0.60f, 0.80f },
    { BONE_RIGHT_FOOT,       0.60f, 0.96f },
};

// Connections between joints ("bones").
inline constexpr Edge k_edges[] = {
    { BONE_HEAD,              BONE_NECK },
    { BONE_NECK,              BONE_UPPER_CHEST },
    { BONE_UPPER_CHEST,       BONE_CHEST },
    { BONE_CHEST,             BONE_SPINE },
    { BONE_SPINE,             BONE_HIPS },
    { BONE_UPPER_CHEST,       BONE_LEFT_SHOULDER },
    { BONE_UPPER_CHEST,       BONE_RIGHT_SHOULDER },
    { BONE_LEFT_SHOULDER,     BONE_LEFT_UPPER_ARM },
    { BONE_LEFT_UPPER_ARM,    BONE_LEFT_LOWER_ARM },
    { BONE_LEFT_LOWER_ARM,    BONE_LEFT_HAND },
    { BONE_RIGHT_SHOULDER,    BONE_RIGHT_UPPER_ARM },
    { BONE_RIGHT_UPPER_ARM,   BONE_RIGHT_LOWER_ARM },
    { BONE_RIGHT_LOWER_ARM,   BONE_RIGHT_HAND },
    { BONE_HIPS,              BONE_LEFT_UPPER_LEG },
    { BONE_HIPS,              BONE_RIGHT_UPPER_LEG },
    { BONE_LEFT_UPPER_LEG,    BONE_LEFT_LOWER_LEG },
    { BONE_LEFT_LOWER_LEG,    BONE_LEFT_FOOT },
    { BONE_RIGHT_UPPER_LEG,   BONE_RIGHT_LOWER_LEG },
    { BONE_RIGHT_LOWER_LEG,   BONE_RIGHT_FOOT },
};

constexpr int k_edge_count = (int)(sizeof(k_edges) / sizeof(k_edges[0]));

} // namespace skeleton
