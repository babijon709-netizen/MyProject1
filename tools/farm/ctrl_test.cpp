// Стенд контроллера автофарма (запуск: sh tools/farm/run.sh).
//
// Настоящий код из jni/src/main.cpp — блок констант автофарма, namespace
// farmlog и UpdateFarm/UpdateFarmInner (его вырезает run.sh в ctrl.inc) —
// вокруг заглушек окружения: ImGui, синтетический тач, «игра», настройки.
// Цель — собрать и прогнать контроллер без NDK и поймать ошибки лога и
// regressions в логике бота до CI.
//
// Сценарий разбит на участки, чтобы за один прогон задеть каждое место события:
//   A подход и добыча узла;
//   B кривой ответ камеры (сброс выученного коэффициента) и цель далеко в
//     стороне (палец камеры уезжает в край экрана);
//   C перекрытый узел: четыре обхода и отказ от узла;
//   D мёртвый узел — удары не засчитываются 20 с (отказ без прогресса);
//   E цель пропала надолго (reason) и фарм выключили.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <vector>
#include <algorithm>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "xp.inc"

// ---- минимальный ImGui (только то, что трогает контроллер) ----
typedef unsigned int ImU32;
struct ImVec2 { float x, y; ImVec2(float a=0,float b=0):x(a),y(b){} };
#define IM_COL32(R,G,B,A) (((ImU32)(A)<<24)|((ImU32)(B)<<16)|((ImU32)(G)<<8)|(ImU32)(R))
struct ImDrawList {
    int calls = 0;
    void AddCircle(ImVec2, float, ImU32, int = 0, float = 0.f) { ++calls; }
    void AddLine(ImVec2, ImVec2, ImU32, float = 0.f) { ++calls; }
    void AddCircleFilled(ImVec2, float, ImU32, int = 0) { ++calls; }
};
namespace ImGui { inline ImDrawList* GetForegroundDrawList() { static ImDrawList dl; return &dl; } }

// ---- окружение оверлея ----
struct { int width = 2460, height = 1080, orientation = 0; } displayInfo;
static int native_window_screen_x = 2460, native_window_screen_y = 1080;
static bool g_esp_attached = true;
static int  g_farmCalib = 0;
static bool s_fingerDown = false;
static struct { bool visible = false; } g_sheet;
static struct { bool visible = false, closing = false; } g_pop;

struct AppState {
    bool  farm_on = true;
    bool  farm_wood = true, farm_stone = true, farm_metal = false, farm_sulfur = false;
    float farm_joy_x = -1.f, farm_joy_y = -1.f;
    float farm_fire_x = -1.f, farm_fire_y = -1.f;
    float farm_range = 100.f;
    bool  farm_log = true;
};
static AppState g_state;
static void SetZones() { g_state.farm_joy_x = 0.19f; g_state.farm_joy_y = 0.72f; }

// Статус-глобалы, которые контроллер публикует для меню.
int  g_farmActive = 0, g_farmPhase = 0, g_farmReason = 1, g_farmNodes = 0;
float g_farmTgtDist = 0.f, g_farmReach = 0.f;
int  g_farmTgtKind = 0, g_farmSpot = 0, g_farmStreak = 0;
bool g_farmPaused = false;
float g_farmHpPct = -1.f, g_farmSpotLife = -1.f, g_farmAttackPeriod = 0.f;
int  g_farmToolHave = 0, g_farmToolNeed = 0, g_farmXp = 0;
bool g_farmBlocked = false;

static const char* kCfgDir_() noexcept {
    // Стенд пишет лог туда, куда положил run.sh. Обфускация xp::_mk здесь не
    // нужна: это не бинарник, а тест, и путь ему задаёт окружение.
    static const char* d = getenv("FARMLOG_DIR");
    return (d && d[0]) ? d : "/tmp/farm_ctrl_test/cfg/";
}
#define kCfgDir (kCfgDir_())

// ---- игра ----
#include "game.h"

static FarmTarget g_tgt;
static bool  g_haveTgt = true;
static float g_camYaw = 20.f;
static float g_eye[3] = {1000.f, 20.f, 1000.f};
static int   g_touchDown[4] = {}, g_touchUp[4] = {};

void esp_farm_set_resources(unsigned) {}
void esp_farm_set_range(float) {}
bool esp_farm_get_target(FarmTarget& out) { if (!g_haveTgt) return false; out = g_tgt; return true; }
void esp_farm_blacklist(unsigned long long, float) {}
void esp_farm_debug(int& n, int& r) { n = 12; r = g_farmReason; }
void esp_farm_tool_info(int& h, int& n) { h = g_farmToolHave; n = g_farmToolNeed; }
float esp_camera_fov_deg() { return 60.f; }
bool esp_camera_angles(float& y, float& p) { y = g_camYaw; p = -2.f; return true; }
int  esp_camera_state() { return 5; }
bool esp_local_eye_position(float& x, float& y, float& z) { x = g_eye[0]; y = g_eye[1]; z = g_eye[2]; return true; }

static void FrameBoxes(float, float) {}
static void Touch_Down_N(int id, float x, float y) { (void)x; (void)y; if (id>=0&&id<4) ++g_touchDown[id]; }
static void Touch_Up_N(int id) { if (id>=0&&id<4) ++g_touchUp[id]; }

#include "ctrl.inc"

int main(int argc, char** argv) {
    const int frames = (argc > 1) ? atoi(argv[1]) : 4200;
    SetZones();
    const float dt = 1.f / 60.f;
    unsigned long long id = 0x5152535455ULL;
    float hp = 100.f, frac = 1.f, dist = 14.f;
    int spot = 0, strk = 0;
    float life = -1.f;
    bool deadNode = false, camCrazy = false;
    float camScale = 1.f;

    g_tgt.valid = true; g_tgt.id = id; g_tgt.kind = 0;
    g_tgt.node_health_max = 100.f; g_tgt.node_experience = 12;
    g_tgt.melee_reach = 2.4f; g_tgt.attack_period = 0.62f;
    g_tgt.tool_purposes = 1; g_farmToolHave = 1; g_farmToolNeed = 1;
    g_tgt.ext_found = true; g_tgt.spot_front = true; g_tgt.on_screen = true;
    g_tgt.ray_valid = true;

    for (int f = 0; f < frames; ++f) {
        // --- участок E: цель пропала надолго, потом фарм выключили ---
        const bool lostLong  = (f >= 3600 && f < 3700);
        const bool farmOff   = (f >= 3750 && f < 3820);
        g_haveTgt = !lostLong;
        g_state.farm_on = !farmOff;
        if (lostLong) g_farmReason = 4;

        // --- участки ---
        deadNode = (f >= 2200 && f < 3600);         // D
        camCrazy = (f >= 700 && f < 760);           // B: камера отвечает втрое
        camScale = camCrazy ? 3.f : 1.f;
        const bool bigYaw = (f >= 620 && f < 700);  // B: цель далеко в стороне
        const bool blockedWin = (f >= 1400 && f < 2150); // C

        // цель
        if (!deadNode && f % 200 == 0 && dist > 0.62f) dist -= 0.9f;  // мир приближается
        if (deadNode) dist = 0.62f;
        if (blockedWin) dist = 0.62f;   // перекрытие проверяется только в фазе удара
        g_tgt.id = id;
        g_tgt.kind = (f >= 1400) ? 1 : 0;
        g_tgt.node_dist = dist;
        g_tgt.aim_dist = dist;
        g_tgt.aim_3d = dist + 1.5f;
        g_tgt.walk_dist = (dist > 2.4f) ? dist - 2.4f : 0.08f;
        g_tgt.walk_yaw = bigYaw ? 118.f : 2.5f;
        g_tgt.yaw = bigYaw ? 120.f : 0.4f * sinf(f * 0.31f);
        g_tgt.pitch = -1.5f;
        g_tgt.has_spot = (spot > 0);
        g_tgt.spot_source = spot;
        g_tgt.at_spot = (spot > 0);
        g_tgt.streak = strk;
        g_tgt.spot_life = life;
        g_tgt.fraction = frac;
        g_tgt.node_health = hp;
        g_tgt.node_health_max = 100.f;
        g_tgt.ray_distance = blockedWin ? 1.05f : dist + 1.6f;
        g_tgt.ray_blocked = blockedWin;
        g_tgt.sx = 1200.f; g_tgt.sy = 500.f;

        UpdateFarm(dt);

        // мир реагирует
        g_camYaw += g_tgt.yaw * 0.3f * camScale;
        if (g_touchDown[0] > g_touchUp[0] && !deadNode) { g_eye[0] += 0.05f; g_eye[2] += 0.03f; }
        if (g_farmPhase == 3 && f % 38 == 0 && !deadNode && !blockedWin) {
            hp -= 6.5f; frac -= 0.05f;
            if (spot == 0) { spot = 2; life = 12.f; } else { strk++; }
        }
        if (life > 0.f) { life -= dt; if (life <= 0.f) { life = -1.f; spot = 0; strk = 0; } }
        if (hp <= 0.f || frac <= 0.f) {   // узел добыт — контроллер сменит цель
            id += 0x111111; hp = 100.f; frac = 1.f; dist = 9.f; spot = 0; strk = 0; life = -1.f;
        }
    }
    fprintf(stderr, "касаний: down %d/%d/%d up %d/%d/%d, фаза %d, причина %d\n",
            g_touchDown[0], g_touchDown[1], g_touchDown[2],
            g_touchUp[0], g_touchUp[1], g_touchUp[2], g_farmPhase, g_farmReason);
    return 0;
}
