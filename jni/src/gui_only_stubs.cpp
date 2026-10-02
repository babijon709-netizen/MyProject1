// GUI-only host adapters.
// The standalone GUI executable deliberately has no game, process, memory,
// kernel, or synthetic-touch implementation. These no-op services keep the
// extracted menu renderable while making every non-GUI feature inert.

#include "app_state.h"
#include "aim.h"
#include "Android_touch/TouchHelperA.h"
#include "esp_draw.h"
#include "farm.h"
#include "game.h"
#include "process.h"
#include "viewmodel.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <sys/types.h>
#include <vector>

pid_t g_target_pid = -1;
bool g_esp_attached = false;
std::atomic<bool> g_attach_scanned{true};

void start_attach_thread() {}
void stop_attach_thread() {}
void process_detach() {}
pid_t process_find_game_pid() { return -1; }
int process_find_unity_candidates(pid_t*, int) { return 0; }

bool s_fingerDown = false;
bool g_aimActive = false;
static AimMemDiag g_aim_diag{};

void UpdateAim(float) {}
void AimEndFrame() {}
float AimSensitivityScale(bool& from_game) { from_game = false; return kAimGainAtRef; }
float AimSensitivityGain(bool& from_game) { from_game = false; return kAimGainAtRef; }
float AimTouchFracX() { return g_state.aim_tx >= 0.f ? g_state.aim_tx : 0.74f; }
float AimTouchFracY() { return g_state.aim_ty >= 0.f ? g_state.aim_ty : 0.50f; }
const AimMemDiag& AimMemoryDiag() { return g_aim_diag; }

bool g_farmActive = false;
int g_farmPhase = 0;
int g_farmNodes = 0;
int g_farmReason = 1;
float g_farmTgtDist = 0.f;
float g_farmReach = 0.f;
int g_farmTgtKind = 0;
int g_farmSpot = 0;
bool g_farmPaused = true;
int g_farmStreak = 0;
float g_farmHpPct = 0.f;
float g_farmSpotLife = 0.f;
float g_farmAttackPeriod = 0.f;
int g_farmToolHave = 0;
int g_farmToolNeed = 0;
int g_farmXp = 0;
bool g_farmBlocked = false;

void UpdateFarm(float) {}

bool esp_init(pid_t, bool) { return false; }
void esp_reset() {}
EspAttachState esp_attach_state() { return ESP_ATTACH_NO_PID; }
void esp_mem_frame_begin() {}
void esp_set_mem_backend(int) {}
bool esp_mem_backend_kernel() { return false; }
int esp_mem_last_errno() { return 0; }
bool esp_silent_target(int) { return false; }
bool esp_silent_read(uint64_t, void*, size_t) { return false; }
bool esp_silent_write(uint64_t, const void*, size_t) { return false; }
bool esp_alive_check() { return false; }
unsigned long long esp_mem_reads_total() { return 0; }
unsigned long long esp_mem_read_fails_total() { return 0; }
void esp_set_skeleton_enabled(bool) {}
void esp_set_markers_enabled(bool, bool, bool, bool, bool, bool, bool, bool, bool, bool) {}
void esp_set_always_day(bool) {}
void esp_set_marker_max_distance(float) {}
std::vector<EspMarker> esp_get_markers() { return {}; }
void esp_set_aim_bones_enabled(bool) {}
void esp_set_bot_aim_enabled(bool) {}
void esp_bot_aim_maintain() {}
std::vector<BotAimEnt> esp_bot_aim_list() { return {}; }
std::vector<EspBox> esp_get_boxes(int, int) { return {}; }
int esp_nearby_player_count() { return 0; }
bool esp_local_player_is_aiming() { return false; }
int esp_local_aim_source() { return 0; }

void esp_farm_set_resources(unsigned) {}
void esp_farm_set_range(float) {}
bool esp_farm_get_target(FarmTarget&) { return false; }
bool esp_farm_cross_pin(const FarmTarget&, bool) { return false; }
bool esp_farm_cross_healthy() { return false; }
void esp_farm_blacklist(unsigned long long, float) {}
void esp_farm_debug(int& nodes_cached, int& idle_reason) {
    nodes_cached = 0;
    idle_reason = 1;
}
void esp_farm_tool_info(int& purposes_have, int& purposes_need) {
    purposes_have = 0;
    purposes_need = 0;
}
void esp_farm_spot_raw(float& ax, float& ay, float& az,
                       float& bx, float& by, float& bz,
                       int& why, float& len) {
    ax = ay = az = bx = by = bz = 0.f;
    why = 3;
    len = -1.f;
}
void esp_set_xray(float) {}
float esp_camera_fov_deg() { return 0.f; }
bool esp_camera_angles(float&, float&) { return false; }
bool esp_aim_camera_angles(float&, float&) { return false; }
int esp_camera_state() { return 0; }
bool esp_read_look_sensitivity(float&) { return false; }
bool esp_local_eye_position(float&, float&, float&) { return false; }
bool esp_mem_aim_look_params(float&, bool&, bool&) { return false; }
bool esp_mem_aim_read_angles(float&, float&) { return false; }
bool esp_mem_aim_write_angles(float, float) { return false; }
bool esp_mem_aim_last_shot(float&) { return false; }
bool esp_mem_aim_read_fire_dir(float&, float&, float&) { return false; }
bool esp_mem_aim_write_fire_dir(float, float, float) { return false; }
bool esp_silent_publish(uint64_t, const float[3]) { return false; }
bool esp_silent_wants_no_spread() { return false; }
bool esp_freecam_set(bool) { return false; }
bool esp_freecam_active() { return false; }
void esp_freecam_diag(int& code, float& dist) { code = 1; dist = 0.f; }
bool esp_freecam_move(float, float, float) { return false; }
bool esp_freecam_look(float, float) { return false; }

void DrawEspOverlay() {}
const std::vector<EspBox>& FrameBoxes(float, float) {
    static const std::vector<EspBox> empty;
    return empty;
}
float AimFovRadiusPx(float, float) { return 0.f; }

void esp_viewmodel_tick(const ViewmodelSettings&) {}
viewmodel::Diagnostics esp_viewmodel_diagnostics() { return {}; }
viewmodel::FpsUiInfo esp_viewmodel_fps_ui() { return {}; }
void esp_viewmodel_reset(bool) {}

bool Touch_Init(int, int, uint32_t, bool) { return false; }
void UpdateScreenData(int, int, uint32_t) {}
void Touch_Close() {}
void Touch_Down(float, float) {}
void Touch_Move(float, float) {}
void Touch_Up() {}
float Touch_DeviceUnitsPerPixel() { return 1.f; }
void Touch_Down_N(int, float, float) {}
void Touch_Up_N(int) {}
bool Touch_CanInject() { return false; }
void Touch_UploadStats(unsigned long long& calls, double& ms,
                       unsigned long long& skipped) {
    calls = 0;
    ms = 0.0;
    skipped = 0;
}
void Touch_BlockRect(bool, float, float, float, float) {}
void Touch_BlockCircles(bool, const float*, const float*, const float*, int) {}
int Touch_GetActive(float*, float*, int*, int) { return 0; }
