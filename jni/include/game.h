#pragma once
#include <sys/types.h>
#include <vector>
#include <cstdint>

// Forward declare PlayerSkeleton
struct PlayerSkeleton;

struct EspBox {
    float x1, y1, x2, y2;
    float distance;
    float corners[8][2];
    bool  corner_visible[8];
    uint64_t transform; // to associate skeleton
};

bool        esp_init(pid_t pid);
void        esp_reset();
std::vector<EspBox> esp_get_boxes(int screen_width, int screen_height);
void        esp_draw_skeletons(int screen_width, int screen_height, float r, float g, float b, float a);

