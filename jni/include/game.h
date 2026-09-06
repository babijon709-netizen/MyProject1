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

    // Player name / held weapon (UTF-8, may be empty). Filled from the
    // nicklabel (UI.Text) and FPManager current-weapon chains.
    bool  has_name = false;
    char  name[32] = {};
    bool  has_weapon = false;
    char  weapon[48] = {}; // localized, UTF-8 (Russian names are wider)

    // Team / clan. `ally` is true when this player shares the local player's
    // team name or clan id; `tag` is the clan tag, shown next to the name.
    bool  ally = false;
    bool  has_tag = false;
    char  tag[16] = {};

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

// World markers: ore nodes and animals, drawn as a small labelled pill at the
// object's screen position. Both come from the same networked source
// (Oxide.MineableObject in Mirror's client registry), so one struct covers them.
enum EspMarkerKind { ESP_MARKER_ORE = 0, ESP_MARKER_ANIMAL = 1, ESP_MARKER_LOOT = 2, ESP_MARKER_PICKUP = 3 };
struct EspMarker {
    float x = 0.0F, y = 0.0F;   // screen position (top-centre of the pill)
    float distance = 0.0F;      // metres from the local player
    int   kind = ESP_MARKER_ORE;
    // Ore markers carry their own colour (one per resource); when has_color is
    // false the caller picks the colour for that kind.
    bool  has_color = false;
    // Elite crates: the overlay cycles the label colour through the spectrum.
    bool  rainbow = false;
    unsigned char color_rgb[3] = {255, 255, 255};
    char  name[40] = {};        // localized label (UTF-8), may carry a stack size
};

bool        esp_init(pid_t pid);
void        esp_reset();
void        esp_set_skeleton_enabled(bool enabled);
// Enable the ore / animal / loot / pickup marker scan (all off = no work).
void        esp_set_markers_enabled(bool ore, bool animals, bool loot, bool pickups);
// Markers further away than this (metres) are dropped. Keeps the screen clean
// on open terrain, where the registry easily holds hundreds of nodes.
void        esp_set_marker_max_distance(float metres);
// Markers for the current frame, nearest first. Call after esp_get_boxes():
// it reuses the camera/projection state that call established.
std::vector<EspMarker> esp_get_markers();
// Resolve bones (for aim points) even when skeleton ESP drawing is off.
void        esp_set_aim_bones_enabled(bool enabled);
std::vector<EspBox> esp_get_boxes(int screen_width, int screen_height);

// Players near the local player as of the last esp_get_boxes() call, in all
// directions (360 degrees) — NOT limited to the ones visible on screen.
// Feeds the enemy-counter pill in the overlay.
int         esp_nearby_player_count();

// True while the local player is aiming down sights (ADS) with the current
// weapon. Returns false when the state cannot be read (not attached, no
// weapon, menus, etc.), so "aim only while scoped" fails closed.
bool        esp_local_player_is_aiming();

// ---- Auto-farm ---------------------------------------------------------------
// The touch controller in main.cpp walks to the nearest selected resource node
// and swings at it; this side only finds the node and tells where to look.
struct FarmTarget {
    bool  valid = false;
    unsigned long long id = 0;
    int   kind = 0;                 // 0 wood, 1 stone, 2 metal, 3 sulfur
    float yaw = 0.f, pitch = 0.f;   // degrees from camera forward (+right, +up)
    float dist = 0.f;               // metres from the local player
    float fraction = -1.f;          // resource remaining 0..1, -1 unknown
    bool  has_spot = false;         // true when aiming at the glowing weak spot
    // Screen-space position of the aim point (for the on-screen target mark).
    bool  on_screen = false;
    float sx = 0.f, sy = 0.f;
};
// Which resources to farm: bit0 wood, bit1 stone, bit2 metal, bit3 sulfur.
// 0 disables the scan entirely (no extra work per frame).
void        esp_farm_set_resources(unsigned mask);
// Search radius for farm nodes in metres (clamped to 10..300).
void        esp_farm_set_range(float meters);
// Nearest matching node as of the last esp_get_boxes() (needs its camera).
bool        esp_farm_get_target(FarmTarget& out);
// Give up on a node (unreachable / stuck) for `seconds`.
void        esp_farm_blacklist(unsigned long long id, float seconds);
// Why the last esp_farm_get_target() returned nothing + how many nodes the
// last registry scan cached. Reasons: 0 ok, 1 farm off, 2 frame not published
// (no camera/local position this frame), 3 registry scan found no matching
// nodes, 4 nodes exist but none in range / all blacklisted, 5 camera pose
// unreadable (cannot compute angles).
void        esp_farm_debug(int& nodes_cached, int& idle_reason);

// Vertical field of view (degrees) of the game camera as last read by
// esp_get_boxes(). 0 if unknown.
float       esp_camera_fov_deg();

// Absolute camera orientation (degrees; yaw around world up, pitch +up) as of
// the last esp_get_boxes(). Returns false if the camera pose is unknown.
bool        esp_camera_angles(float& yaw_deg, float& pitch_deg);
// Diagnostic: bit 0 camera pose known, bit 1 pose derived from the view
// matrix, bit 2 firing reference in use. See esp_camera_state() in game.cpp.
int         esp_camera_state();
