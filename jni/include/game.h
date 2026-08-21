#pragma once
#include <sys/types.h>
#include <cstdint>
#include <vector>
#include "Vector.h"

enum EspBone : std::uint8_t {
    ESP_BONE_HEAD = 0,
    ESP_BONE_NECK,
    ESP_BONE_CHEST,
    ESP_BONE_PELVIS,
    ESP_BONE_LEFT_SHOULDER,
    ESP_BONE_LEFT_ELBOW,
    ESP_BONE_LEFT_HAND,
    ESP_BONE_RIGHT_SHOULDER,
    ESP_BONE_RIGHT_ELBOW,
    ESP_BONE_RIGHT_HAND,
    ESP_BONE_LEFT_HIP,
    ESP_BONE_LEFT_KNEE,
    ESP_BONE_LEFT_FOOT,
    ESP_BONE_RIGHT_HIP,
    ESP_BONE_RIGHT_KNEE,
    ESP_BONE_RIGHT_FOOT,
    ESP_BONE_COUNT
};

struct EspBox {
    float x1, y1, x2, y2;     // screen rect
    float distance;           // world distance to local player (m), -1 if unknown
    float corners[8][2];      // projected 3D-box corners (screen)
    bool  corner_visible[8];
    uint64_t source;          // PlayerManager* this box belongs to
    float speed;              // smoothed horizontal world speed (m/s)
    Vec3  feet;               // world feet position
    Vec3  head;               // world head position (actual animated head when available)
    Vec3  vel;                // smoothed horizontal world velocity (x,0,z)
    float aim_vx;             // screen velocity of head (px/s), for aim feed-forward
    float aim_vy;
    bool  crouched;           // player is crouching

    // Animated humanoid joints. These are read from the enemy Animator's
    // native Transform hierarchy every frame, so crouch/aim/run animations are
    // reflected by the overlay instead of using a fixed stick figure.
    Vec3  bones[ESP_BONE_COUNT];
    float bone_screen[ESP_BONE_COUNT][2];
    bool  bone_visible[ESP_BONE_COUNT];
    bool  skeleton_valid;

    char  name[64];
    char  weapon[32];
    float health;
    float max_health;
};

bool        esp_init(pid_t pid);
void        esp_reset();
std::vector<EspBox> esp_get_boxes(int screen_width, int screen_height);
float       esp_get_camera_fov(); // vertical FOV in degrees, -1 if unknown
bool        esp_is_aiming();      // local player is aiming down sights
uint64_t    esp_aim_hit_player(); // PlayerManager* currently hit by the aim ray, 0 if wall/none

// Project a world-space point to screen using the latest view-projection matrix.
bool        esp_world_to_screen(Vec3 world, int screen_w, int screen_h, float& sx, float& sy);
