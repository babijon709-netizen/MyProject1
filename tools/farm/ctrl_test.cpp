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
//   E цель пропала надолго (reason) и фарм выключили;
//   F у игры нет луча прицела (GKo пуст): тапать нельзя — удар не засчитается,
//     поэтому стик поджимает ближе, а тап держится до луча (и не дольше 2.5 с);
//   G цель мигает на один кадр (рескан реестра): смены узла быть не должно —
//     иначе каждое мигание стоит 0.7 с паузы (в логе так простаивал 14 с из 150).
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
// Сырые поля сегмента крестика: стенд отдаёт то, что положит сценарий, чтобы в
// логе была видна строка диагностики точки (SPOT_A/SPOT_B/why/len).
static float g_rawA[3] = {0.f, 0.f, 0.f}, g_rawB[3] = {0.f, 0.f, 0.f};
static int   g_rawWhy = 3;
static float g_rawLen = -1.f;
void esp_farm_spot_raw(float& ax, float& ay, float& az, float& bx, float& by, float& bz,
                       int& why, float& len) {
    ax = g_rawA[0]; ay = g_rawA[1]; az = g_rawA[2];
    bx = g_rawB[0]; by = g_rawB[1]; bz = g_rawB[2];
    why = g_rawWhy; len = g_rawLen;
}
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
    // Участок F: тапы и стик отдельно за окно «нет луча» и сразу после него.
    int   tapsNoRay = 0, stickNoRay = 0, framesNoRay = 0, tapsAfterNoRay = 0;
    int   tapDownPrev = 0;
    // Участок G: мигания цели и чем они обернулись (пауза смены = плохо,
    // удержание прежней цели = хорошо).
    int   flicks = 0, holdFrames = 0, settleFrames = 0, framesFlick = 0;
    int   flickTail = 0;   // кадров после мигания, за которые пауза = провал

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
        const bool flickWin  = (f >= 900 && f < 1100);   // G: цель мигает на кадр
        const bool noRayWin  = (f >= 3850 && f < 3970);  // F: 2 с без луча игры
        const bool afterNoRay = (f >= 3970 && f < 4190);

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
        // F: крестик жив, прицел сел, но у игры нет данных рейкаста — так
        // выглядит прицел на декали, висящей в 25 см от коры сбоку ствола.
        g_tgt.ray_valid = !noRayWin;
        g_tgt.ray_point_valid = !noRayWin;
        if (noRayWin || afterNoRay) {
            // Узел держим живым и в пределах удара, иначе после окна F бот уйдёт
            // на подход к новой цели и тапов не будет вовсе — проверка «после
            // появления луча тапы вернулись» стала бы бессмысленной.
            dist = 0.62f;
            if (hp < 60.f) hp = 60.f;
            if (frac < 0.5f) frac = 0.5f;
            spot = 3; life = 12.f;
            g_rawA[0] = 1000.f; g_rawA[1] = 20.f; g_rawA[2] = 1000.f;
            g_rawB[0] = 1000.f; g_rawB[1] = 21.f; g_rawB[2] = 1000.4f;
            g_rawWhy = 2; g_rawLen = 1.05f;
        }
        if (afterNoRay) { g_rawWhy = 0; }
        g_tgt.sx = 1200.f; g_tgt.sy = 500.f;

        // G: на один кадр «лучшим» оказывается чужой узел — так выглядит
        // рабочая цель, выпавшая из реестра при рескане.
        const unsigned long long realId = g_tgt.id;
        const bool flick = (flickWin && (f % 40) == 20);
        if (flick) { g_tgt.id = realId ^ 0xABCDULL; ++flicks; flickTail = 12; }

        UpdateFarm(dt);

        if (flickWin) {
            ++framesFlick;
            if (farmlog::g_row.ex == 4) ++holdFrames;      // цель мигнула — держим
        }
        // Паузу смены узла считаем только в хвосте после мигания: в этом же
        // окне узел может быть по-настоящему добыт, и тогда пауза законна.
        if (flickTail > 0) {
            --flickTail;
            if (farmlog::g_row.ex == 6) ++settleFrames;
        }
        g_tgt.id = realId;

        // учёт участка F: фронт пальца удара и нажатый стик
        if (g_touchDown[2] > tapDownPrev) {
            if (noRayWin) ++tapsNoRay;
            else if (afterNoRay) ++tapsAfterNoRay;
        }
        tapDownPrev = g_touchDown[2];
        if (noRayWin) { ++framesNoRay; if (g_touchDown[0] > g_touchUp[0]) ++stickNoRay; }

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
    fprintf(stderr, "НЕТ_ЛУЧА: кадров %d, тапов %d, стик жал кадров %d; ПОСЛЕ_ЛУЧА: тапов %d\n",
            framesNoRay, tapsNoRay, stickNoRay, tapsAfterNoRay);
    fprintf(stderr, "МИГАНИЕ: кадров %d, миганий %d, удержано %d, пауз_смены_после_мигания %d\n",
            framesFlick, flicks, holdFrames, settleFrames);
    return 0;
}
