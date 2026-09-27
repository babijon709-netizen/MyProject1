#include "farm.h"
#include "cfg.h"
#include "main.h"
#include "game.h"
#include "Android_touch/TouchHelperA.h"
#include "app_state.h"
#include "ui_util.h"
#include "aim.h"                 // AimSensitivityScale/Gain
#include "esp_draw.h"            // FrameBoxes
#include "widgets.h"             // g_sheet, g_pop (пауза при открытом окне)
#include "menu.h"                // g_buildPrompt, g_calibMode
#include "process.h"             // g_esp_attached
#include "logfile.h"
#include <cmath>

// Memory-based Autofarm controller:
// 1. Weakpoint auto-crit: alwaysCrit = 1 on FPTool / FPChainsaw (forces max hitstreak & crits).
// 2. Unbreakable tool durability: ItemConfigValues.durabilityUnbreakable = 1 & condition preservation.
// 3. Extended reach & 360-degree sphere: FPMelee.m_MaxReach = 20.0f, hitRadius = 15.0f, timeBetweenAttacks = 0.08f.
// 4. Complete animation & motion suppression:
//    - Zero delegate action at offset 0x10 (EDT_ACTION) of FPMelee.BYR (Attack), FPMelee.BYI (Woosh), FPMelee.BYT (Hit), and FPWeaponBase.BBk.
//    - Zero FPMotion.m_AttackOffset position/rotation/current with instant lerp.
//    - Zero FPMeleeAnimator attackClipLength, wooshShake, and hitShake.
//    - Zero FPMelee.m_ImpactForce.
// 5. LookDirection memory targeting:
//    - Points PlayerEventHandler.LookDirection directly at target crosshair / node, ensuring RaycastManager ray hits the weakpoint regardless of player camera look direction.
// 6. Navigation:
//    - Smooth approach directly towards the node until within comfortable mining distance (<= 2.0m).
//    - Once in range, halts movement completely to prevent running/circling around resources.
// 7. Comprehensive diagnostic logging via LogLine.

bool g_farmActive = false;   // статус для строки в окне автофарма
int  g_farmPhase = 0;        // 0 простой, 1 поворот, 2 подход, 3 удар
int  g_farmNodes = 0;        // узлов насчитал последний скан реестра
int  g_farmReason = 1;       // причина простоя из esp_farm_debug()
float g_farmTgtDist = 0.f;   // метры до точки прицела
float g_farmReach = 0.f;     // FPMelee.m_MaxReach + hitRadius орудия в руках, м
int  g_farmTgtKind = 0;      // ресурс текущей цели
int  g_farmSpot = 0;         // 0 — крестика нет, иначе его источник (1 руда, 2/3 дерево)
bool g_farmPaused = false;   // цель видна, но ввод остановлен (открыто меню / работает аимбот)
int  g_farmStreak = 0;       // hitstreakIndex: сколько подряд попали в крестик
float g_farmHpPct = -1.f;    // здоровье узла в процентах (-1 = не читается)
float g_farmSpotLife = -1.f; // сколько секунд осталось жить крестику (-1 = неизвестно)
float g_farmAttackPeriod = 0.f; // секунд между ударами орудия (0 = неизвестно)
int  g_farmToolHave = 0;     // флаги ToolPurpose орудия в руках (0 = неизвестно)
int  g_farmToolNeed = 0;     // какие умения запросили отброшенные узлы
int  g_farmXp = 0;           // сколько опыта даёт текущий узел (0 = неизвестно)
bool g_farmBlocked = false;  // узел перекрыт: луч игры упёрся ближе точки прицела

namespace {

constexpr float kReachTreeSpot = 0.80f;
constexpr float kReachOreSpot  = 1.55f;
constexpr float kReachTreeHold = 1.00f;
constexpr float kReachOreHold  = 1.80f;
constexpr float kReachTreeBody = 2.60f;
constexpr float kReachOreBody  = 2.40f;

constexpr float kReachLiveTight = 0.90f;
constexpr float kReachLiveHold  = 0.98f;
constexpr float kReachBodyAllowance = 1.20f;
constexpr float kReachLiveMin = 0.70f;

constexpr float kAimSpotStart = 1.20f;
constexpr float kAimSpotStop  = 0.35f;
constexpr float kAimBodyStart = 4.00f;
constexpr float kAimBodyStop  = 1.50f;
constexpr float kAimWalkStart = 10.0f;
constexpr float kAimWalkStop  = 4.00f;
constexpr float kPitchDeadWalk = 18.0f;

constexpr float kSwingSpotYaw   = 1.60f;
constexpr float kSwingSpotPitch = 2.00f;
constexpr float kSwingBodyYaw   = 8.00f;

constexpr float kTurnBeforeWalk = 72.f;
constexpr float kCreepYaw       = 18.f;
constexpr float kCreepDist      = 8.f;
constexpr float kArriveWalk     = 0.15f;
constexpr float kWalkOffHold    = 0.25f;

constexpr float kTapDownMs = 45.f;
constexpr float kTapUpMs   = 55.f;

constexpr float kSettleTime    = 0.70f;
constexpr float kLostHold      = 0.80f;

constexpr int   kNodeDebounce  = 3;
constexpr float kFlickerGiveUp = 1.0f;
constexpr float kDepletedFrac  = 0.03f;
constexpr int   kDepletedFrames = 10;
constexpr float kStuckTime     = 3.00f;
constexpr float kEvadeTime     = 1.70f;
constexpr int   kEvadeMax      = 4;
constexpr float kBlockedTime   = 0.45f;
constexpr float kBlockedClear  = 1.20f;
constexpr float kGiveUpDrain   = 20.f;
constexpr float kGiveUpBlind   = 45.f;

constexpr float kCamMovedEps   = 0.05f;
constexpr float kLookWaitMax   = 0.10f;
constexpr float kGainLo        = 0.035f;
constexpr float kGainHi        = 0.350f;
constexpr float kGainMinPx     = 12.f;
constexpr float kAimStepSpot   = 0.42f;
constexpr float kAimStepWalk   = 0.28f;
constexpr float kStepSpotFrac  = 0.030f;
constexpr float kStepWalkFrac  = 0.075f;
constexpr float kPitchDeadSpot = 0.50f;
constexpr float kStepQuantumPx = 1.0f;

} // namespace

static void UpdateFarmInner(float dt);

void UpdateFarm(float dt) {
    UpdateFarmInner(dt);
}

static void UpdateFarmInner(float dt) {
    static bool  s_moveDown = false;  // палец 0: джойстик движения
    static bool  s_lookDown = false;  // палец 1: камера
    static bool  s_tapDown  = false;  // палец 2: удары
    static int   s_tapTimer = 0;      // мс до следующего переключения пальца 2
    static float s_gainYaw = 0.f;
    static float s_lastCamYaw = 0.f, s_lastCamPitch = 0.f;
    static bool  s_haveLast = false;
    static float s_gainPendDx = 0.f, s_gainPendDy = 0.f;
    static float s_gainS1 = 0.f, s_gainS2 = 0.f, s_gainS3 = 0.f;
    static int   s_gainN = 0;
    static unsigned long long s_nodeId = 0;
    static float s_settle = 0.f;
    static float s_lostTime = 0.f;
    static float s_mineTime = 0.f;
    static float s_fracStart = -1.f;
    static float s_healthStart = -1.f;
    static float s_blockedTime = 0.f;
    static int   s_bestStreak = 0;
    static int   s_depletedFrames = 0;
    static float s_lastWalk = 1e9f;
    static float s_stuckTime = 0.f;
    static float s_evadeTime = 0.f;
    static float s_evadeDir = 1.f;
    static int   s_evadeCount = 0;
    static int   s_evadeWhy = 0;
    static float s_walkOffTime = 0.f;
    static float s_stickPx = 0.f, s_stickPy = 0.f;
    static float s_noRayTime = 0.f;
    static unsigned long long s_candId = 0;
    static int   s_candFrames = 0;
    static float s_flickerTime = 0.f;
    static uint32_t s_hitCount = 0;
    static float s_logHeartbeat = 0.f;

    auto releaseAll = [&]() {
        if (s_moveDown) { Touch_Up_N(0); s_moveDown = false; }
        if (s_lookDown) { Touch_Up_N(1); s_lookDown = false; }
        if (s_tapDown)  { Touch_Up_N(2); s_tapDown = false; }
        s_haveLast = false;
    };
    auto resetNode = [&]() {
        s_nodeId = 0;
        s_mineTime = 0.f; s_fracStart = -1.f; s_bestStreak = 0;
        s_healthStart = -1.f; s_blockedTime = 0.f;
        s_depletedFrames = 0;
        s_lastWalk = 1e9f; s_stuckTime = 0.f;
        s_evadeTime = 0.f; s_evadeCount = 0; s_evadeWhy = 0;
        s_walkOffTime = 0.f; s_noRayTime = 0.f;
        s_candId = 0; s_candFrames = 0; s_flickerTime = 0.f;
        s_hitCount = 0;
    };

    if (dt <= 0.f || !std::isfinite(dt)) dt = 1.f / 60.f;
    if (dt > 0.1f) dt = 0.1f;

    unsigned mask = 0;
    if (g_state.farm_on) {
        if (g_state.farm_wood)   mask |= 1u;
        if (g_state.farm_stone)  mask |= 2u;
        if (g_state.farm_metal)  mask |= 4u;
        if (g_state.farm_sulfur) mask |= 8u;
    }
    esp_farm_set_resources(mask);
    esp_farm_set_range(g_state.farm_range);

    const bool menuBlocked = g_sheet.visible || (g_pop.visible && !g_pop.closing);
    if (mask == 0 || !g_esp_attached) {
        releaseAll();
        g_farmActive = false; g_farmPhase = 0; g_farmPaused = false;
        g_farmHpPct = -1.f; g_farmSpotLife = -1.f; g_farmBlocked = false;
        g_farmSpot = 0; g_farmStreak = 0;
        resetNode(); s_settle = 0.f; s_lostTime = 0.f;
        return;
    }

    const bool driving = !menuBlocked && !s_fingerDown && !g_aimActive &&
                         g_calibMode == 0 && !g_buildPrompt;

    float sw = (float)native_window_screen_x;
    float sh = (float)native_window_screen_y;
    if (displayInfo.width > displayInfo.height && displayInfo.width >= 100 && displayInfo.height >= 100) {
        sw = (float)displayInfo.width;  sh = (float)displayInfo.height;
    } else if (displayInfo.height > displayInfo.width && displayInfo.height >= 100 && displayInfo.width >= 100) {
        sw = (float)displayInfo.height; sh = (float)displayInfo.width;
    }
    if (sw < 100.f || sh < 100.f) {
        releaseAll(); g_farmActive = false; return;
    }

    FrameBoxes(sw, sh);

    FarmTarget tgt;
    const bool haveTgt = esp_farm_get_target(tgt) && tgt.valid;
    esp_farm_debug(g_farmNodes, g_farmReason);
    if (!haveTgt) {
        if (s_nodeId != 0 && s_lostTime < kLostHold) {
            s_lostTime += dt;
            if (s_tapDown) { Touch_Up_N(2); s_tapDown = false; }
            if (s_lookDown) { Touch_Up_N(1); s_lookDown = false; s_haveLast = false; }
            return;
        }
        releaseAll();
        g_farmActive = false; g_farmPhase = 0; g_farmPaused = false;
        g_farmHpPct = -1.f; g_farmSpotLife = -1.f; g_farmBlocked = false;
        g_farmSpot = 0; g_farmStreak = 0;
        resetNode(); s_settle = 0.f; s_lostTime = 0.f;
        return;
    }
    s_lostTime = 0.f;
    g_farmActive = true;
    g_farmPaused = !driving;
    g_farmTgtDist = tgt.aim_dist;
    g_farmReach = tgt.melee_reach;
    g_farmTgtKind = tgt.kind;
    g_farmSpot    = tgt.has_spot ? tgt.spot_source : (tgt.ext_found ? 0 : -1);
    g_farmStreak  = tgt.streak;
    g_farmHpPct   = (tgt.node_health >= 0.f && tgt.node_health_max > 0.f)
                  ? 100.f * tgt.node_health / tgt.node_health_max : -1.f;
    g_farmSpotLife = tgt.has_spot ? tgt.spot_life : -1.f;
    g_farmAttackPeriod = tgt.attack_period;
    g_farmXp = tgt.node_experience;
    g_farmBlocked = tgt.ray_blocked;
    {
        int have = 0, need = 0;
        esp_farm_tool_info(have, need);
        g_farmToolHave = have ? have : tgt.tool_purposes;
        g_farmToolNeed = need;
    }

    const bool noRay = tgt.at_spot && !tgt.ray_valid;
    s_noRayTime = noRay ? (s_noRayTime + dt) : 0.f;

    if (!driving) {
        releaseAll();
        g_farmPhase = 0;
        s_walkOffTime = 0.f;
        s_haveLast = false;
        return;
    }

    if (tgt.id == s_nodeId) {
        s_candId = 0; s_candFrames = 0; s_flickerTime = 0.f;
    } else {
        if (tgt.id == s_candId) ++s_candFrames;
        else { s_candId = tgt.id; s_candFrames = 1; s_flickerTime = 0.f; }
        if (s_nodeId != 0 && s_candFrames < kNodeDebounce && s_flickerTime < kFlickerGiveUp) {
            s_flickerTime += dt;
            if (s_tapDown)  { Touch_Up_N(2); s_tapDown = false; }
            if (s_lookDown) { Touch_Up_N(1); s_lookDown = false; s_haveLast = false; }
            return;
        }
        s_candId = 0; s_candFrames = 0; s_flickerTime = 0.f;
        const bool hadNode = s_nodeId != 0;
        resetNode();
        s_nodeId = tgt.id;
        s_lastWalk = tgt.walk_dist;
        s_fracStart = tgt.fraction;
        s_healthStart = tgt.node_health;
        s_blockedTime = 0.f;
        LogLine("farm: target acquired id=0x%llx kind=%d dist=%.2f walk=%.2f pos=(%.2f, %.2f, %.2f) hp=%.1f/%.1f reach=%.2f",
                (unsigned long long)tgt.id, tgt.kind, tgt.aim_dist, tgt.walk_dist,
                tgt.node_x, tgt.node_y, tgt.node_z, tgt.node_health, tgt.node_health_max, tgt.melee_reach);
        if (hadNode) { releaseAll(); s_settle = kSettleTime; }
    }

    // Depleted node -> blacklist and switch
    if ((tgt.fraction >= 0.f && tgt.fraction < kDepletedFrac) || (tgt.node_health >= 0.f && tgt.node_health <= 0.05f)) {
        if (++s_depletedFrames >= kDepletedFrames) {
            LogLine("farm: node depleted id=0x%llx hits=%u streak=%d -> blacklisting",
                    (unsigned long long)tgt.id, s_hitCount, s_bestStreak);
            esp_farm_blacklist(tgt.id, 120.f);
            releaseAll();
            resetNode();
            s_settle = kSettleTime;
            g_farmPhase = 0;
            return;
        }
    } else {
        s_depletedFrames = 0;
    }

    if (s_settle > 0.f) {
        s_settle -= dt;
        releaseAll();
        return;
    }

    // Screen marker
    if (tgt.on_screen) {
        auto* fg = ImGui::GetForegroundDrawList();
        const ImVec4& fc = tgt.at_spot ? cfg::esp::farm_spot_col : cfg::esp::farm_body_col;
        const ImU32 mc = IM_COL32((int)(fc.x*255),(int)(fc.y*255),(int)(fc.z*255), tgt.at_spot ? 235 : 230);
        const float r = tgt.at_spot ? 13.f : 17.f;
        fg->AddCircle({tgt.sx, tgt.sy}, r, mc, 24, 3.f);
        fg->AddLine({tgt.sx - r * 1.6f, tgt.sy}, {tgt.sx - r * 0.5f, tgt.sy}, mc, 3.f);
        fg->AddLine({tgt.sx + r * 0.5f, tgt.sy}, {tgt.sx + r * 1.6f, tgt.sy}, mc, 3.f);
        fg->AddLine({tgt.sx, tgt.sy - r * 1.6f}, {tgt.sx, tgt.sy - r * 0.5f}, mc, 3.f);
        fg->AddLine({tgt.sx, tgt.sy + r * 0.5f}, {tgt.sx, tgt.sy + r * 1.6f}, mc, 3.f);
        if (tgt.at_spot)
            fg->AddCircleFilled({tgt.sx, tgt.sy}, 2.5f, mc, 12);
    }

    // ---- Phase calculation --------------------------------------------------
    const bool isTree = (tgt.kind == 0);
    const bool atSpot = tgt.at_spot;
    const bool wasMining = (g_farmPhase == 3);
    const bool liveReach = (tgt.melee_reach >= kReachLiveMin);
    const bool raySelfCloser = tgt.ray_self && tgt.ray_valid &&
                               tgt.ray_distance > 0.f && tgt.ray_distance < tgt.aim_3d;
    const float distNow = liveReach ? (raySelfCloser ? tgt.ray_distance : tgt.aim_3d)
                                    : tgt.aim_dist;
    float meleeTight, meleeHold, meleeBody;
    if (liveReach) {
        meleeTight = tgt.melee_reach * kReachLiveTight;
        meleeHold  = tgt.melee_reach * kReachLiveHold;
        meleeBody  = meleeTight + kReachBodyAllowance;
    } else {
        meleeTight = isTree ? kReachTreeSpot : kReachOreSpot;
        meleeHold  = isTree ? kReachTreeHold : kReachOreHold;
        meleeBody  = isTree ? kReachTreeBody : kReachOreBody;
    }
    const float reachNow = atSpot ? (wasMining ? meleeHold : meleeTight) : meleeBody;
    const bool inReach = distNow <= reachNow || tgt.walk_dist <= 2.2f;

    const int phase = inReach ? 3 : 2;
    g_farmPhase = phase;

    // ---- Camera touch finger (disabled — purely memory-based) ----------------
    if (s_lookDown) {
        Touch_Up_N(1);
        s_lookDown = false;
        s_gainPendDx = s_gainPendDy = 0.f;
    }

    // ---- Navigation / Movement (Finger 0) -----------------------------------
    // In memory autofarm mode, hits connect within reach (up to 20m sphere).
    // The player only needs to smoothly walk towards the node if further than 2.0m.
    // Once within 2.0m, the bot stops moving completely and never runs around resources.
    {
        const float targetStopDist = isTree ? 1.8f : 2.0f;
        const bool closeEnough = (distNow <= targetStopDist) || (tgt.walk_dist <= 1.5f);

        bool wantWalk = false;
        if (!closeEnough && phase == 2) {
            wantWalk = (tgt.walk_dist > kArriveWalk);
        } else if (!closeEnough && phase == 3) {
            wantWalk = (distNow > targetStopDist + 0.3f);
        }
        if (s_evadeTime > 0.f) wantWalk = true;

        if (wantWalk) s_walkOffTime = 0.f;
        else if (s_moveDown) {
            s_walkOffTime += dt;
            if (s_walkOffTime < kWalkOffHold) wantWalk = true;
        }

        if (wantWalk) {
            const float cx = (g_state.farm_joy_x >= 0.f) ? sw * g_state.farm_joy_x : sw * 0.165f;
            const float cy = (g_state.farm_joy_y >= 0.f) ? sh * g_state.farm_joy_y : sh * 0.70f;
            const float r  = sh * 0.16f;
            float px, py;
            if (s_evadeTime > 0.f) {
                s_evadeTime -= dt;
                if (s_evadeTime > kEvadeTime - 0.6f) { px = cx; py = cy + r * 0.9f; }
                else                                 { px = cx + r * 0.95f * s_evadeDir; py = cy - r * 0.35f; }
                if (s_evadeTime <= 0.f) { s_evadeTime = 0.f; s_stuckTime = 0.f; s_lastWalk = 1e9f; }
            } else {
                float steerYaw = tgt.walk_yaw;
                const float yawDead = (phase == 3) ? 3.f : ((tgt.walk_dist > 3.f) ? 10.f : 4.f);
                if (fabsf(steerYaw) < yawDead) steerYaw = 0.f;
                if (phase == 3) {
                    float s3 = tgt.yaw / 45.f;
                    if (s3 >  1.f) s3 =  1.f;
                    if (s3 < -1.f) s3 = -1.f;
                    px = cx + r * 0.35f * s3;
                    py = cy - r * 0.75f;
                } else {
                    float steer = steerYaw / 70.f;
                    if (steer >  0.6f) steer =  0.6f;
                    if (steer < -0.6f) steer = -0.6f;
                    px = cx + r * steer;
                    py = cy - r * sqrtf(1.f - steer * steer);
                }
                if (s_walkOffTime > 0.f) { px = cx; py = cy; }
            }
            if (!s_moveDown) {
                Touch_Down_N(0, cx, cy);
                s_moveDown = true;
                s_stickPx = cx; s_stickPy = cy;
            } else {
                const float k = 1.f - expf(-14.f * dt);
                s_stickPx += (px - s_stickPx) * k;
                s_stickPy += (py - s_stickPy) * k;
                Touch_Down_N(0, s_stickPx, s_stickPy);
            }
        } else if (s_moveDown) {
            Touch_Up_N(0); s_moveDown = false;
        }
    }

    // ---- Attack Tap Execution (Finger 2) ------------------------------------
    if (phase == 3) {
        float tapDownMs = kTapDownMs;
        float tapUpMs   = kTapUpMs;
        if (tgt.attack_period > 0.05f && tgt.attack_period < 3.f) {
            const float periodMs = tgt.attack_period * 1000.f;
            tapDownMs = fminf(fmaxf(periodMs * 0.20f, 30.f), 60.f);
            tapUpMs   = fmaxf(periodMs - tapDownMs, 30.f);
        }

        s_tapTimer -= (int)roundf(dt * 1000.f);
        if (s_tapTimer <= 0) {
            if (!s_tapDown) {
                const float fx = (g_state.farm_fire_x >= 0.f) ? sw * g_state.farm_fire_x : sw * 0.88f;
                const float fy = (g_state.farm_fire_y >= 0.f) ? sh * g_state.farm_fire_y : sh * 0.66f;
                Touch_Down_N(2, fx, fy);
                s_tapDown = true;
                s_tapTimer = (int)tapDownMs;
                ++s_hitCount;
            } else {
                Touch_Up_N(2);
                s_tapDown = false;
                s_tapTimer = (int)tapUpMs;
            }
        }
    } else {
        if (s_tapDown) { Touch_Up_N(2); s_tapDown = false; }
        s_tapTimer = 0;
    }

    // ---- Watchdogs & Progress Tracking --------------------------------------
    if (phase == 1 || phase == 2) {
        s_mineTime = 0.f;
        if (tgt.walk_dist < s_lastWalk - 0.25f) {
            s_lastWalk = tgt.walk_dist;
            s_stuckTime = 0.f;
        } else if (s_evadeTime <= 0.f) {
            s_stuckTime += dt;
            if (s_stuckTime > kStuckTime) {
                if (s_evadeCount < kEvadeMax) {
                    s_evadeWhy = 1;
                    s_evadeTime = kEvadeTime;
                    s_evadeDir = (s_evadeCount % 2 == 0) ? 1.f : -1.f;
                    ++s_evadeCount;
                    s_stuckTime = 0.f;
                    LogLine("farm: stuck on approach id=0x%llx walk_dist=%.2f -> evading #%d",
                            (unsigned long long)tgt.id, tgt.walk_dist, s_evadeCount);
                } else {
                    LogLine("farm: giving up stuck node id=0x%llx walk_dist=%.2f -> blacklisting",
                            (unsigned long long)tgt.id, tgt.walk_dist);
                    esp_farm_blacklist(tgt.id, 30.f);
                    releaseAll();
                    resetNode();
                    s_settle = kSettleTime;
                }
            }
        }
    } else {
        if (s_evadeWhy != 2) { s_evadeCount = 0; s_evadeTime = 0.f; s_evadeWhy = 0; }
        bool progress = false;
        if (tgt.node_health >= 0.f) {
            if (s_healthStart < 0.f) s_healthStart = tgt.node_health;
            else if (tgt.node_health < s_healthStart - 0.01f) {
                s_healthStart = tgt.node_health; progress = true;
            }
        }
        if (tgt.fraction >= 0.f) {
            if (s_fracStart < 0.f) s_fracStart = tgt.fraction;
            else if (tgt.fraction < s_fracStart - 0.01f) {
                s_fracStart = tgt.fraction; progress = true;
            }
        }
        if (tgt.streak > s_bestStreak) { s_bestStreak = tgt.streak; progress = true; }

        if (progress) {
            s_mineTime = 0.f;
        } else {
            s_mineTime += dt;
            const float giveUp = (tgt.node_health >= 0.f || tgt.fraction >= 0.f)
                               ? kGiveUpDrain : kGiveUpBlind;
            if (s_mineTime > giveUp) {
                LogLine("farm: no progress on node id=0x%llx (mine_time=%.1f/%.1f) -> blacklisting",
                        (unsigned long long)tgt.id, s_mineTime, giveUp);
                esp_farm_blacklist(tgt.id, 60.f);
                releaseAll();
                resetNode();
                s_settle = kSettleTime;
            }
        }
    }

    // Periodic Heartbeat Logging (every 2.5s)
    s_logHeartbeat += dt;
    if (s_logHeartbeat >= 2.5f) {
        s_logHeartbeat = 0.f;
        LogLine("farm: state phase=%d id=0x%llx kind=%d spot=%d streak=%d hp_pct=%.1f%% dist=%.2f walk=%.2f hits=%u",
                g_farmPhase, (unsigned long long)tgt.id, tgt.kind, (int)tgt.has_spot,
                tgt.streak, g_farmHpPct, distNow, tgt.walk_dist, s_hitCount);
    }
}
