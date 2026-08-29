#pragma once
#include <sys/types.h>
#include <vector>

struct EspBox {
    float x1, y1, x2, y2;
    float distance;
    float corners[8][2];
    bool  corner_visible[8];
};

struct BoneScreen {
    float x, y;
    bool  valid;
};

struct EspSkeleton {
    enum {
        B_HIPS = 0, B_SPINE, B_SPINE1, B_SPINE2, B_NECK, B_HEAD,
        B_SHOULDER_L, B_ARM_L, B_FOREARM_L, B_HAND_L,
        B_SHOULDER_R, B_ARM_R, B_FOREARM_R, B_HAND_R,
        B_UPLEG_L, B_LEG_L, B_FOOT_L, B_TOEBASE_L,
        B_UPLEG_R, B_LEG_R, B_FOOT_R, B_TOEBASE_R,
        BONE_COUNT
    };
    BoneScreen bones[BONE_COUNT];
    bool  valid = false;
    float distance = -1.0f;
};

bool        esp_init(pid_t pid);
void        esp_reset();
std::vector<EspBox> esp_get_boxes(int screen_width, int screen_height);
std::vector<EspSkeleton> esp_get_skeletons(int screen_width, int screen_height);
