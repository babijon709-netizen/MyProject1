#pragma once
#include <sys/types.h>
#include <vector>

// Skeleton ESP: bone slots and the lines connecting them.
constexpr int ESP_BONE_COUNT      = 22;

struct EspBox {
    unsigned long long id;   // stable per-player identity (PlayerManager*), for target stickiness
    float x1, y1, x2, y2;
    float distance;
    float corners[8][2];
    bool  corner_visible[8];

    // Skeleton (screen-space bone positions), filled when skeleton ESP is enabled.
    bool  has_skeleton;
    bool  bone_valid[ESP_BONE_COUNT];
    float bones[ESP_BONE_COUNT][2];

    // Aim targets (screen-space), always filled when the bone transforms are
    // resolvable — independent of whether skeleton ESP drawing is enabled.
    //   aim_pts[0] = head (skull centre), [1] = neck, [2] = chest (upper spine).
    // aim_valid[i] == false means the corresponding bone could not be read and
    // the caller should fall back to the box estimate.
    bool  aim_valid[3];
    // Where the aim points came from: 0 none, 1 rig bones (exact),
    // 2 KCC head transform (crouch-aware), 3 feet + pose height (estimate).
    int   aim_source;
    bool  crouched;
    float aim_pts[3][2];
    // Angular offset of each aim point from the camera forward axis, in
    // degrees (yaw = +right, pitch = +up). Only meaningful when aim_valid[i].
    float aim_yaw[3];
    float aim_pitch[3];
};

bool        esp_init(pid_t pid);
void        esp_reset();
void        esp_set_skeleton_enabled(bool enabled);
// Resolve bones (for aim points) even when skeleton ESP drawing is off.
void        esp_set_aim_bones_enabled(bool enabled);
// Vertical offset (metres) added to the head aim point; positive = higher.
void        esp_set_aim_head_lift(float metres);
std::vector<EspBox> esp_get_boxes(int screen_width, int screen_height);

// True while the local player is aiming down sights (ADS) with the current
// weapon. Returns false when the state cannot be read (not attached, no
// weapon, menus, etc.), so "aim only while scoped" fails closed.
bool        esp_local_player_is_aiming();

// Vertical field of view (degrees) of the game camera as last read by
// esp_get_boxes(). 0 if unknown.
float       esp_camera_fov_deg();

// Absolute camera orientation (degrees; yaw around world up, pitch +up) as of
// the last esp_get_boxes(). Returns false if the camera pose is unknown.
bool        esp_camera_angles(float& yaw_deg, float& pitch_deg);
