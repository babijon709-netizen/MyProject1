// frame.cpp — Кадр ESP: состояние, публикация, сброс.
//
// Модуль разрезан из прежнего монолита jni/src/game.cpp;
// что здесь лежит и кто это зовёт — в шапке frame.h и в docs/CODE_MAP.md.

#include "esp/common.h"
#include "esp/aim_points.h"
#include "esp/boxes.h"
#include "esp/camera.h"
#include "esp/farm_target.h"
#include "esp/game_patch.h"
#include "esp/il2cpp.h"
#include "esp/markers.h"
#include "esp/math.h"
#include "esp/mem.h"
#include "esp/names.h"
#include "esp/player_pose.h"
#include "esp/skeleton_cache.h"
#include "esp/transform.h"
#include "frame.h"

// Per-frame player list, kept across frames so a transient empty read does
// not blank the overlay, but dropped on a real world change.
std::vector<uint64_t> g_frame_transforms;

// Игроки, пропавшие из реестра в последние пару кадров. Список
// PlayerManager читается по одному указателю на элемент, и одиночный
// сбой чтения (или непрочитавшаяся проверка класса) выбрасывал игрока
// из кадра — его бокс гас и загорался обратно. Держим пропавшего ещё
// кадр-два: настоящий уход/смерть задерживается на ~30 мс, а мерцание
// исчезает. Значение — сколько кадров подряд игрока нет в списке.
std::unordered_map<uint64_t, int> g_frame_transforms_lost;

// Frame projection state, published by esp_get_boxes() so that world markers
// (ore / animals) project through exactly the same camera as the player boxes.
Mat4 g_frame_vp{};

bool g_frame_vp_valid = false;

float g_frame_sw = 0.0F, g_frame_sh = 0.0F;

Vec3 g_frame_local_pos{};

bool g_frame_local_valid = false;

// Camera basis recovered from this frame's VIEW MATRIX (not the transform
// pose). On devices where the transform pose read fails, this is the only
// camera orientation available — good enough for the farm's slow turns,
// though not for the aimbot (the fallback matrix lags a frame).
bool g_frame_cam_basis_valid = false;

Vec3 g_frame_cam_pos{}, g_frame_cam_fwd{}, g_frame_cam_right{}, g_frame_cam_up{};

// Камерный источник годен, если его позиция конечна и близка к корню игрока.
// Нулевой вектор конечен, поэтому одна проверка isfinite пропускала мусор: в
// логе 14.09.2026 было 11 кадров с origin=(0,0,0) — aim3d улетал на 864 и
// 1027 м при dist 0.8 м, а yaw на 158-164°, то есть камера получала команду
// развернуться почти на пол-оборота. Глаз выше корня игрока примерно на 1.5 м,
// так что 25 м запаса отсекают только явный мусор.
// Доступ к камере (esp_camera_angles / esp_local_eye_position) объявлен ниже,
// сразу за g_frame_cam_*: базис из матрицы вида — третий источник углов и
// позиции глаза, и он обязан быть объявлен раньше этих функций.
bool farm_cam_source_ok(const Vec3& p) {
    if (!vec3_is_finite(p)) return false;
    if (!g_frame_local_valid) return true;
    const float dx = p.x - g_frame_local_pos.x;
    const float dy = p.y - g_frame_local_pos.y;
    const float dz = p.z - g_frame_local_pos.z;
    return dx * dx + dy * dy + dz * dz < 625.0F;
}

// Направление оси -> углы (yaw вокруг мировой вертикали, pitch вверх).
// Общая часть всех источников: аим и фарм считают угол одинаково.
static bool angles_from_forward(const Vec3& f, float& yaw_deg, float& pitch_deg) {
    constexpr float rad2deg = 57.29577951F;
    float yaw = atan2f(f.x, f.z) * rad2deg;
    float horiz = sqrtf(f.x * f.x + f.z * f.z);
    float pitch = atan2f(f.y, horiz) * rad2deg;
    if (!std::isfinite(yaw) || !std::isfinite(pitch)) return false;
    yaw_deg = yaw; pitch_deg = pitch;
    return true;
}

// Углы камеры для ФАРМА: поза камеры (Transform), ось выстрела (LookDirection),
// а если ни то, ни другое не читается — базис из матрицы вида этого кадра.
// Фарм поворачивает камеру медленно и по экранной метке, поэтому базис (отстаёт
// на кадр) ему подходит; аимботу — нет, у него своя функция ниже.
//
// Почему базис вообще нужен. На устройстве из логов 14.09 и 15.09.2026 поза
// камеры и ось выстрела НЕ ЧИТАЮТСЯ вовсе (cam_st = 0 во всех строках аима), и
// базис из матрицы вида — единственный источник углов, который там есть: без
// него фарм висел в «нет позиции камеры».
//
// Тот же фильтр мусора, что и у точки прицела: источник с нулевой/улетевшей
// позицией не годится и для углов, иначе обучение коэффициента камеры хлебнёт
// поворот на 158° из ниоткуда (лог 14.09.2026: 11 кадров с origin=(0,0,0)).
bool esp_camera_angles(float& yaw_deg, float& pitch_deg) {
    const bool ok_ref   = g_aim_ref_valid  && farm_cam_source_ok(g_aim_ref_origin);
    const bool ok_pose  = g_cam_pose_valid && farm_cam_source_ok(g_cam_pos);
    const bool ok_frame = g_frame_cam_basis_valid && farm_cam_source_ok(g_frame_cam_pos);
    if (!ok_ref && !ok_pose && !ok_frame) return false;
    // Same reference the aim angles are measured against (firing direction
    // when available), so finger-gain learning and target lead stay consistent.
    const Vec3& f = ok_ref ? g_aim_ref_forward : ok_pose ? g_cam_forward : g_frame_cam_fwd;
    return angles_from_forward(f, yaw_deg, pitch_deg);
}

// Углы камеры ДЛЯ АИМБОТА: только НАСТОЯЩАЯ ось — поза камеры (Transform) или
// ось выстрела (LookDirection). Базис из матрицы вида здесь не участвует, хотя
// фарму он и годится.
//
// Почему. Аимбот меряет по этим углам две вещи: чувствительность (град/px,
// деление поворота камеры на свой сдвиг пальца) и «отработала ли игра прошлый
// шаг». Базис отстаёт на кадр, и оба измерения по нему врут. В логе 15.09.2026
// это видно прямо: exp/sent (он же выученный gain) скакал 0.072 -> 0.347 ->
// -0.072 — со сменой ЗНАКА, — а ход пальца доходил до 162 px за кадр, палец
// улетал в край экрана (EV «палец на краю ... перенос в центр») и камеру
// швыряло туда-обратно. Это и есть «аим дёргается».
//
// Без настоящей оси аим ведёт цель вслепую запасным коэффициентом и НЕ ждёт
// ответа камеры (s_camMovedPrev в UpdateAim) — ровно так работала сборка, где
// он вёл идеально: в том логе cam_st = 0, то есть обучать коэффициент было не
// на чем, и аим просто шёл к цели фиксированным шагом.
bool esp_aim_camera_angles(float& yaw_deg, float& pitch_deg) {
    const bool ok_ref  = g_aim_ref_valid  && farm_cam_source_ok(g_aim_ref_origin);
    const bool ok_pose = g_cam_pose_valid && farm_cam_source_ok(g_cam_pos);
    if (!ok_ref && !ok_pose) return false;
    return angles_from_forward(ok_ref ? g_aim_ref_forward : g_cam_forward, yaw_deg, pitch_deg);
}

bool esp_local_eye_position(float& x, float& y, float& z) {
    // Приоритет — точка выстрела (KCC LookDirection): она не качается от sway/
    // отдачи, поэтому производная по ней и есть реальное движение персонажа.
    if (g_aim_ref_valid && farm_cam_source_ok(g_aim_ref_origin)) {
        x = g_aim_ref_origin.x; y = g_aim_ref_origin.y; z = g_aim_ref_origin.z;
        return true;
    }
    if (g_cam_pose_valid && farm_cam_source_ok(g_cam_pos)) {
        x = g_cam_pos.x; y = g_cam_pos.y; z = g_cam_pos.z;
        return true;
    }
    // Третьим — позиция из матрицы вида. Без неё на устройстве из лога
    // 14.09.2026 mv_dps/mv_dir были -99 во всех 14499 строках: контроллер
    // движения шёл вслепую — не знал, что персонаж уже разогнался или упёрся
    // в ствол, и не мог отличить «идём» от «стоим».
    if (g_frame_cam_basis_valid && farm_cam_source_ok(g_frame_cam_pos)) {
        x = g_frame_cam_pos.x; y = g_frame_cam_pos.y; z = g_frame_cam_pos.z;
        return true;
    }
    return false;
}

// Players near us this frame (all 360 degrees, not only the ones projected
// on screen). Feeds the enemy-counter pill in the overlay.
int  g_frame_player_count = 0;

int      g_frame_transforms_empty_streak = 0;

// Frames in a row esp_get_boxes() gave up before publishing this frame's
// camera / local position (see the watchdog at the top of it).
int      g_frame_publish_fail_streak = 0;

// Сколько раз подряд сторож сбрасывал кэши, не получив кадра. Три подряд —
// повод перепривязаться целиком (см. esp_get_boxes).
int      g_frame_watchdog_resets = 0;

// Кадры подряд, в которых позиции игроков не прочитались НИ У КОГО. Одного
// такого кадра мало, чтобы объявить смещение позиции неверным: за этим идёт
// поиск смещения заново с ожиданием «поля допишутся» (0.6 с), и всё это время
// боксов нет вовсе — со стороны это «мерцание на полсекунды». Серия кадров
// отличает мигнувшее чтение от настоящей перезагрузки мира.
int      g_local_position_fail_streak = 0;

// Кадры подряд, в которых состав игроков не пересекается с тем, по которому
// построены кэши (см. esp_get_boxes). Настоящая перезагрузка мира держится
// кадров подряд, а одиночный кадр с чужими адресами — это сбой чтения списка, и
// обнулять по нему все кэши (боксы, метки, раскладку скелета) нельзя.
// Снапшот хранит именно тот состав, под который собраны кэши: сравнивать с
// прошлым кадром нельзя — список подменяется уже на первом кадре смены, и
// следующий кадр пересекается сам с собой (смену состава это бы не заметило).
int      g_world_change_streak = 0;

std::vector<uint64_t> g_population_snapshot;

// Поток привязки в main.cpp читает этот флаг и переподключается к игре заново.
std::atomic<bool> g_want_reattach{false};

// Everything derived from a particular world/session. Called when the whole
// player population is replaced (scene reload / new session) or the player
// list disappears for a while, so no stale pointers survive into the next
// world. Deliberately NOT tied to the camera object: the game swaps cameras
// while aiming, which must not disturb boxes or skeletons.
// Defined with the marker code further down (needs its caches).
void reset_marker_caches();

void reset_world_caches() {
    g_matrix_configuration_validated = false; g_camera_matrix_physical_match = false;
    g_player_position_validated = false;
    g_population_snapshot.clear(); g_world_change_streak = 0;
    // The bone-learned transform layout dies with the old world: after a
    // reload it reads garbage from recycled memory (finite numbers, wrong
    // places). It is relearned from the first nearby skeleton; markers use
    // the self-probing path meanwhile.
    g_skeleton_layout = {}; g_skeleton_layout_valid = false;
    g_use_direct_player_position = true; g_player_position_offset = PLAYER_POSITION;
    g_direct_position_fail_streak = 0; g_direct_position_recheck = 0;
    g_local_player = 0;
    g_aim_state = {};
    g_aim_ref_valid = false;
    g_player_aux.clear();
    g_player_text.clear();
    g_player_track.clear();
    g_player_track_pick.clear();
    g_frame_transforms_lost.clear();
    g_mount_latch.clear();
    g_skeletons.clear();
    reset_marker_caches();
}

void esp_reset() {
    g_mem.unbind();
    g_attach_state = ESP_ATTACH_OK;
    g_pid = -1; g_il2cpp_base = 0;
    g_xray_cam = 0; g_xray_saved_valid = false; // процесс ушёл — восстанавливать нечего
    g_day_tod = 0; g_day_retry = 0; g_day_cycle_addr.store(0);
    g_frame_transforms.clear(); g_frame_transforms_empty_streak = 0;
    g_frame_publish_fail_streak = 0; g_frame_watchdog_resets = 0;
    g_local_position_fail_streak = 0; g_world_change_streak = 0;
    g_population_snapshot.clear();
    g_offset_extent_fail_streak = 0;
    g_want_reattach.store(false);
    g_aim_ref_valid = false;
    g_player_manager_class = 0; g_player_manager_static_fields = 0;
    g_game_controller_class = 0; g_local_player = 0;
    g_matrix_configuration_validated = false; g_camera_matrix_physical_match = false;
    g_player_position_offset = PLAYER_POSITION;
    g_transform_hierarchy_layout = {}; g_transform_hierarchy_layout_valid = false;
    g_use_direct_player_position = true;
    g_player_position_validated = false;
    g_direct_position_fail_streak = 0; g_direct_position_recheck = 0;
    g_cam_fov_deg = 0.0F; g_cam_pose_valid = false; g_cam_pose_derived = false;
    g_aim_state = {};
    g_player_aux.clear();
    g_player_text.clear();
    g_player_track.clear();
    g_player_track_pick.clear();
    g_frame_transforms_lost.clear();
    g_mount_latch.clear();
    g_skeletons.clear();
    g_skeleton_layout = {}; g_skeleton_layout_valid = false;
    g_go_name_offset = 0; g_go_name_plain_pointer = false;
    g_go_name_offset_valid = false; g_go_name_retry_at = 0.0;
    g_frame_vp_valid = false; g_frame_local_valid = false;
    reset_marker_caches();
}

// Last overlay size esp_get_boxes() was called with — the camera-only frame
// fallback below needs plausible screen dimensions even when the box pipeline
// bailed out before publishing anything.
float g_last_overlay_sw = 1080.0F;

float g_last_overlay_sh = 2400.0F;

bool publish_camera_only_frame(float sw, float sh) {
    if (g_pid <= 0 || !g_il2cpp_base) return false;
    // Resolves g_game_controller_class as a side effect — without it the
    // camera lookup below has no class to read statics from.
    resolve_local_player();
    if (!g_game_controller_class) return false;
    uint64_t gcb_sf = get_class_static_fields(g_game_controller_class);
    if (!gcb_sf) return false;
    uint64_t cam_mgr = rd_ptr(gcb_sf + GAME_CONTROLLER_CAMERA_MANAGER_FIELD);
    if (!cam_mgr) return false;
    uint64_t managed_cam = rd_ptr(cam_mgr + CAMERA_MANAGER_CAMERA_FIELD);
    if (!managed_cam) return false;
    uint64_t cam_native = rd_ptr(managed_cam + MANAGED_CACHED_PTR);
    if (!cam_native) return false;
    if (!(sw >= 100.0F) || !(sh >= 100.0F)) { sw = 1080.0F; sh = 2400.0F; }
    Mat4 solo_proj{}, solo_view{};
    xray_apply(cam_native);
    always_day_tick();
    if (!read_native_camera_matrices(cam_native, sw / sh, solo_proj, solo_view)) return false;
    Vec3 cam_pos{};
    if (!camera_position_from_view(solo_view, cam_pos)) return false;
    g_frame_vp = mat_mul(solo_proj, solo_view);
    g_frame_vp_valid = true;
    g_frame_sw = sw; g_frame_sh = sh;
    g_frame_local_pos = cam_pos;
    g_frame_local_valid = true;
    g_frame_publish_fail_streak = 0;
    g_frame_watchdog_resets = 0;
    // Camera basis for the farm, same shape as the main path builds.
    Vec3 vr = {mat_get(solo_view, 0, 0), mat_get(solo_view, 0, 1), mat_get(solo_view, 0, 2)};
    Vec3 vu = {mat_get(solo_view, 1, 0), mat_get(solo_view, 1, 1), mat_get(solo_view, 1, 2)};
    Vec3 vf = {-mat_get(solo_view, 2, 0), -mat_get(solo_view, 2, 1), -mat_get(solo_view, 2, 2)};
    if (vec3_is_finite(vr) && vec3_is_finite(vu) && vec3_is_finite(vf)) {
        float fl = sqrtf(vf.x * vf.x + vf.y * vf.y + vf.z * vf.z);
        if (fl > 0.5F && fl < 2.0F) {
            g_frame_cam_pos = cam_pos;
            g_frame_cam_fwd = {vf.x / fl, vf.y / fl, vf.z / fl};
            g_frame_cam_right = vr;
            g_frame_cam_up = vu;
            g_frame_cam_basis_valid = true;
        }
    }
    return true;
}
