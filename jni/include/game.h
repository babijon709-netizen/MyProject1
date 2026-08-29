#pragma once
#include <sys/types.h>
#include <vector>

struct EspBox {
    float x1, y1, x2, y2;
    float distance;
    float corners[8][2];
    bool  corner_visible[8];
};

struct EspBoneLine {
    float x1, y1;
    float x2, y2;
};

struct EspSkeleton {
    std::vector<EspBoneLine> bones;
    float head_x = -1.0f;
    float head_y = -1.0f;
    float head_radius = 0.0f;
    bool  has_head = false;
    float distance = -1.0f;
};

bool        esp_init(pid_t pid);
void        esp_reset();
std::vector<EspBox>      esp_get_boxes(int screen_width, int screen_height);
std::vector<EspSkeleton> esp_get_skeletons(int screen_width, int screen_height);
