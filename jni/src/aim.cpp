// Touch aimbot. Replaces the previous 1588-line controller.
// Target selection and smoothing follow the ManesWare aim core
// (manes_aim/touch_aim.cpp): nearest-in-FOV to the crosshair, pull a fraction
// of the remaining error per frame, drive the camera with a synthetic finger.
// Memory (MouseLook write) and silent (shot-axis) modes are not carried over:
// they need build-specific offsets that this tree does not provide.

#include "aim.h"
#include "silent_aim.h"
#include "game.h"
#include "esp_draw.h"
#include "app_state.h"
#include "cfg.h"
#include "menu.h"
#include "farm.h"
#include "process.h"
#include "hud.h"
#include "Android_touch/TouchHelperA.h"
#include "manes_aim_source.h"
#include <algorithm>
#include <cmath>
#include <time.h>

static constexpr float kAimTouchDefX = 0.74f;
static constexpr float kAimTouchDefY = 0.50f;
// +1: finger moving right turns the camera right. Flip to -1 if the view moves away from the target.
static constexpr float kAimDirSign   = 1.f;
// Speed slider (g_state.gun_str, 1..10, higher = faster): 1 = 25% of the error per frame at 60 fps, 10 = 100%.
static constexpr float kAimFracMin   = 0.25f;
static constexpr float kAimFracMax   = 1.00f;
// Max finger travel from its origin before the finger is lifted and re-placed.
static constexpr float kAimFingerRadiusPx = 220.f;
// Error below this is ignored (px).
static constexpr float kAimDeadbandPx     = 1.5f;

// Inactive reasons, decoded in AimMemoryDiag().inactive.
enum : int {
    kInactiveOff      = 1,
    kInactiveDetached = 2,
    kInactiveCalib    = 4,
    kInactiveFreecam  = 6,
    kInactiveNotAds   = 7,
    kInactiveScreen   = 8,
    kInactiveNoInject = 9,
};

bool s_fingerDown = false;
bool g_aimActive  = false;
std::atomic<unsigned long long> g_aim_pulse_target{0};
std::atomic<double>             g_aim_pulse_at{0.0};
std::atomic<double>             g_aim_hold_at{0.0};

static AimMemDiag s_diag;

static double MonoNow() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

float AimTouchFracX() {
    return (g_state.aim_tx > 0.f && g_state.aim_tx < 1.f) ? g_state.aim_tx : kAimTouchDefX;
}

float AimTouchFracY() {
    return (g_state.aim_ty > 0.f && g_state.aim_ty < 1.f) ? g_state.aim_ty : kAimTouchDefY;
}

// Gain is measured on device at sensitivity 2.0 (0.10 deg/px). This tree does not
// estimate it online anymore, so the reference value is returned as-is.
float AimSensitivityScale(bool& from_game) {
    from_game = false;
    return 1.f;
}

float AimSensitivityGain(bool& from_game) {
    from_game = false;
    return kAimGainAtRef;
}

static void ReleaseFinger() {
    if (s_fingerDown) {
        Touch_Up();
        s_fingerDown = false;
    }
    g_aimActive = false;
}

// Picks the aim point of the best box: nearest to the crosshair (or nearest in world
// with aim_priority == 2) inside the FOV circle. Returns false if nothing qualifies.
static bool PickTarget(const std::vector<EspBox>& boxes, float sw, float sh,
                       float& aimX, float& aimY, unsigned long long& id) {
    const float cx = sw * 0.5f, cy = sh * 0.5f;
    const float fovR = AimFovRadiusPx(sw, sh);
    const int bone = std::clamp(cfg::aim::bone, 0, 2);
    const bool worldPrio = g_state.aim_priority == 2;

    bool found = false;
    float bestScore = 0.f;
    for (const EspBox& box : boxes) {
        if (box.dead || box.respawning) continue;
        if (cfg::aim::vis_check) {
            bool any = false;
            for (int i = 0; i < 8; ++i) any = any || box.corner_visible[i];
            if (!any) continue;
        }

        float px, py;
        if (box.aim_valid[bone]) {
            px = box.aim_pts[bone][0];
            py = box.aim_pts[bone][1];
        } else {
            px = (box.x1 + box.x2) * 0.5f;
            py = box.y1 + (box.y2 - box.y1) * 0.15f;   // head estimate from box top
        }

        const float dist = std::hypot(px - cx, py - cy);
        if (dist > fovR) continue;

        const float score = worldPrio ? box.distance : dist;
        if (!found || score < bestScore) {
            found = true;
            bestScore = score;
            aimX = px;
            aimY = py;
            id = box.id;
        }
    }
    return found;
}

static float AimSpeedFraction() {
    float sm = g_state.gun_str;
    if (!(sm >= 1.f)) sm = 1.f;
    if (sm > 10.f) sm = 10.f;
    return kAimFracMin + (sm - 1.f) / 9.f * (kAimFracMax - kAimFracMin);
}

static void UpdateAimTouch(float dt) {
    static float s_fx = 0.f, s_fy = 0.f;
    static float s_ox = 0.f, s_oy = 0.f;

    s_diag = AimMemDiag{};
    g_aimActive = false;

    if (!g_state.aim_touch)                   { s_diag.inactive = kInactiveOff;      ReleaseFinger(); return; }
    if (!g_esp_attached)                      { s_diag.inactive = kInactiveDetached; ReleaseFinger(); return; }
    if (g_calibMode != 0)                     { s_diag.inactive = kInactiveCalib;    ReleaseFinger(); return; }
    if (g_state.freecam_on)                   { s_diag.inactive = kInactiveFreecam;  ReleaseFinger(); return; }
    if (!Touch_CanInject())                   { s_diag.inactive = kInactiveNoInject; ReleaseFinger(); return; }

    if (g_state.aim_scope_only) {
        const bool ads = esp_local_player_is_aiming();
        s_diag.ads = ads ? 1 : 0;
        s_diag.ads_source = esp_local_aim_source();
        if (!ads) { s_diag.inactive = kInactiveNotAds; ReleaseFinger(); return; }
    }

    float sw = 0.f, sh = 0.f;
    VisibleScreen(sw, sh);
    if (sw < 100.f || sh < 100.f) { s_diag.inactive = kInactiveScreen; ReleaseFinger(); return; }

    const std::vector<EspBox>& boxes = FrameBoxes(sw, sh);
    float aimX = 0.f, aimY = 0.f;
    unsigned long long id = 0;
    if (!PickTarget(boxes, sw, sh, aimX, aimY, id)) {
        s_diag.no_target = 1;
        ReleaseFinger();
        return;
    }

    g_aim_pulse_target.store(id, std::memory_order_relaxed);
    g_aim_hold_at.store(MonoNow(), std::memory_order_relaxed);
    g_aimActive = true;

    const float cx = sw * 0.5f, cy = sh * 0.5f;
    const float errX = aimX - cx, errY = aimY - cy;

    if (!s_fingerDown) {
        s_ox = AimTouchFracX() * sw;
        s_oy = AimTouchFracY() * sh;
        s_fx = s_ox;
        s_fy = s_oy;
        Touch_Down(s_fx, s_fy);
        s_fingerDown = true;
        g_aim_pulse_at.store(MonoNow(), std::memory_order_relaxed);
    }

    if (std::hypot(errX, errY) <= kAimDeadbandPx) {
        s_diag.on_target = 1;
        Touch_Move(s_fx, s_fy);   // keep the touch alive, no travel
        return;
    }

    const float pull = manes_aim_source::frame_gain(AimSpeedFraction(), dt);
    s_fx += errX * pull * kAimDirSign;
    s_fy += errY * pull * kAimDirSign;

    if (std::hypot(s_fx - s_ox, s_fy - s_oy) > kAimFingerRadiusPx) {
        Touch_Up();
        s_fx = s_ox;
        s_fy = s_oy;
        Touch_Down(s_fx, s_fy);
    }
    s_fx = std::clamp(s_fx, 0.f, sw - 1.f);
    s_fy = std::clamp(s_fy, 0.f, sh - 1.f);
    Touch_Move(s_fx, s_fy);
    s_diag.writes++;
}

void AimEndFrame() {
    silent_aim::clear();
}

void UpdateAim(float dt) {
    UpdateAimTouch(dt);
}

const AimMemDiag& AimMemoryDiag() {
    return s_diag;
}
