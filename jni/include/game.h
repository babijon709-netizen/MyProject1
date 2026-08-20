#pragma once
#include <sys/types.h>
#include <vector>

struct EspBox {
    float x1, y1, x2, y2;
    float distance;
    float corners[8][2];
    bool  corner_visible[8];
    uint64_t source; // PlayerManager* this box belongs to
    float speed;     // horizontal speed m/s (from position delta), -1 if unknown
};

bool        esp_init(pid_t pid);
void        esp_reset();
std::vector<EspBox> esp_get_boxes(int screen_width, int screen_height);
float       esp_get_camera_fov(); // vertical FOV in degrees, -1 if unknown
bool        esp_is_aiming();      // local player is aiming down sights
uint64_t    esp_aim_hit_player(); // PlayerManager* currently hit by the aim ray, 0 if wall/none
