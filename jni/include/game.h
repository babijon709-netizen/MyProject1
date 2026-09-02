#pragma once
#include <sys/types.h>
#include <vector>

// Skeleton ESP: bone slots and the lines connecting them.
constexpr int ESP_BONE_COUNT      = 22;
constexpr int ESP_BONE_LINK_COUNT = 21;
extern const int ESP_BONE_LINKS[ESP_BONE_LINK_COUNT][2];

struct EspBox {
    float x1, y1, x2, y2;
    float distance;
    float corners[8][2];
    bool  corner_visible[8];

    // Skeleton (screen-space bone positions), filled when skeleton ESP is enabled.
    bool  has_skeleton;
    bool  bone_valid[ESP_BONE_COUNT];
    float bones[ESP_BONE_COUNT][2];
};

bool        esp_init(pid_t pid);
void        esp_reset();
void        esp_set_skeleton_enabled(bool enabled);
void        esp_skeleton_debug(char* out, int cap); // one-line pipeline status
std::vector<EspBox> esp_get_boxes(int screen_width, int screen_height);
