#pragma once
#include <sys/types.h>
#include <vector>

enum class EspBone : int {
    Hip = 0,
    Spine,
    Chest,
    Neck,
    Head,
    LeftShoulder,
    LeftUpperArm,
    LeftLowerArm,
    LeftHand,
    RightShoulder,
    RightUpperArm,
    RightLowerArm,
    RightHand,
    LeftThigh,
    LeftShin,
    LeftFoot,
    RightThigh,
    RightShin,
    RightFoot,
    Count
};

inline constexpr int kEspBoneCount = static_cast<int>(EspBone::Count);

struct EspQuery {
    bool box = false;
    bool chams = false;
    bool skeleton = false;
};

struct EspBox {
    float x1, y1, x2, y2;
    float distance;
    float corners[8][2];
    bool  corner_visible[8];
    float bones[kEspBoneCount][2];
    bool  bone_visible[kEspBoneCount];
    bool  has_skeleton;
};

bool        esp_init(pid_t pid);
void        esp_reset();
std::vector<EspBox> esp_get_boxes(int screen_width, int screen_height, EspQuery query = {});
