#pragma once
#include <sys/types.h>
#include <vector>

struct EspBox {
    float x1, y1, x2, y2;
    float distance;
    float corners[8][2];
    bool  corner_visible[8];
};

// Screen-space aim points projected from exact world-space body positions
// (all shares of the same frame as esp_get_boxes).
struct EspAimTarget {
    float head_x,  head_y;   bool head_ok;
    float chest_x, chest_y;  bool chest_ok;
    float pelvis_x, pelvis_y; bool pelvis_ok;
    float box_x1, box_y1, box_x2, box_y2;
    float distance;
};

bool        esp_init(pid_t pid);
void        esp_reset();
std::vector<EspBox> esp_get_boxes(int screen_width, int screen_height);
std::vector<EspAimTarget> esp_get_aim_targets();
bool        esp_is_local_aiming();

// Diagnostics: human-readable state of the last esp_get_boxes call
// (lists, bound position field, camera sources, bone reads).
// Returns false when no frame has been processed yet.
bool        esp_get_debug_text(char* out, int cap);
