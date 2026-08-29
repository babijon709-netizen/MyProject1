#pragma once
#include <sys/types.h>
#include <vector>
#include "Vector.h"

// Bones used by the skeleton ESP. Order matters only for EspBox::skel_* arrays.
enum EspBone {
    BONE_HIPS = 0,
    BONE_SPINE,
    BONE_SPINE1,
    BONE_SPINE2,
    BONE_NECK,
    BONE_HEAD,
    BONE_SHOULDER_L,
    BONE_ARM_L,
    BONE_FOREARM_L,
    BONE_HAND_L,
    BONE_SHOULDER_R,
    BONE_ARM_R,
    BONE_FOREARM_R,
    BONE_HAND_R,
    BONE_UPLEG_L,
    BONE_LEG_L,
    BONE_FOOT_L,
    BONE_TOEBASE_L,
    BONE_UPLEG_R,
    BONE_LEG_R,
    BONE_FOOT_R,
    BONE_TOEBASE_R,
    ESP_BONE_COUNT
};

struct EspBox {
    float x1, y1, x2, y2;
    float distance;
    float corners[8][2];
    bool  corner_visible[8];

    // --- Skeleton ESP (bone screen points, valid while skel_valid) ---
    bool  skel_valid;
    float skel_x[ESP_BONE_COUNT];
    float skel_y[ESP_BONE_COUNT];
    bool  skel_visible[ESP_BONE_COUNT];
};

bool        esp_init(pid_t pid);
void        esp_reset();
std::vector<EspBox> esp_get_boxes(int screen_width, int screen_height, bool with_skeleton = false);
