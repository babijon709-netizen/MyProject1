#pragma once
#include <sys/types.h>
#include <vector>
#include "Vector.h"
#include "bones.h"

struct EspBox {
    float x1, y1, x2, y2;     // screen rect
    float distance;           // world distance to local player (m), -1 if unknown
    float corners[8][2];      // projected 3D-box corners (screen)
    bool  corner_visible[8];
    uint64_t source;          // PlayerManager* this box belongs to
    float speed;              // smoothed horizontal world speed (m/s)
    Vec3  feet;               // world feet position
    Vec3  head;               // world head position (crouch-aware)
    Vec3  vel;                // smoothed horizontal world velocity (x,0,z)
    float aim_vx;             // screen velocity of head (px/s), for aim feed-forward
    float aim_vy;
    bool  crouched;           // player is crouching
};

bool        esp_init(pid_t pid);
void        esp_reset();
std::vector<EspBox> esp_get_boxes(int screen_width, int screen_height);
float       esp_get_camera_fov(); // vertical FOV in degrees, -1 if unknown
bool        esp_is_aiming();      // local player is aiming down sights
uint64_t    esp_aim_hit_player(); // PlayerManager* currently hit by the aim ray, 0 if wall/none

// Project a world-space point to screen using the latest view-projection matrix.
bool        esp_world_to_screen(Vec3 world, int screen_w, int screen_h, float& sx, float& sy);

// Read the real animated bone world positions for a player (Unity 6 Animator
// skin matrices). Returns false / valid==0 when unavailable; the caller falls
// back to the procedural skeleton.
bool        esp_read_bones(uint64_t player, BoneSet& out);
