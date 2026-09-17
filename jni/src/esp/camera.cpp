// camera.cpp — Камера игры: поза, матрицы, углы, чувствительность.
//
// Модуль разрезан из прежнего монолита jni/src/game.cpp;
// что здесь лежит и кто это зовёт — в шапке camera.h и в docs/CODE_MAP.md.

#include "esp/common.h"
#include "esp/aim_points.h"
#include "esp/boxes.h"
#include "esp/farm_target.h"
#include "esp/frame.h"
#include "esp/esp_time.h"
#include "esp/il2cpp.h"
#include "esp/math.h"
#include "esp/mem.h"
#include "esp/player_pose.h"
#include "esp/transform.h"
#include "camera.h"
#include "app/diag_log.h"

// Camera state captured by the last esp_get_boxes() call (used by the aimbot
// to convert bone positions into yaw/pitch offsets from the crosshair).
float     g_cam_fov_deg = 0.0F;

// См. camera.h: 0 — поза камеры, 1 — ось прицела, 2 — удержанный/кэш камеры.
int       g_cam_view_source = 0;

// Поза камеры в этом сеансе признана негодной: её поворот не сходится ни с осью
// прицела, ни с углами прицела игры. Вид ESP тогда собирается по оси прицела,
// g_cam_forward/g_cam_pose_valid при этом НЕ переписываются.
bool      g_cam_pose_bad = false;
bool      g_cam_view_suspect = false;

// Расхождение позы камеры с углами прицела (градусы, -1 — замерить не вышло).
// Только для журнала: см. комментарий в read_native_camera_matrices.
float     g_cam_view_angle_gap_deg = -1.0F;

bool      g_cam_pose_valid = false;

// Reserved: was set when the pose had been recovered from the view matrix.
// That path is gone -- deriving the pose from the matrix made the aim throw
// itself across the screen, because the matrix the fallback reads is the
// stale cached one and every angle measured against it lags reality.
bool      g_cam_pose_derived = false;

Vec3      g_cam_pos{};

Vec3      g_cam_right{}, g_cam_up{}, g_cam_forward{};

// The game fires along PlayerEventHandler.LookDirection (= MouseLook.m_LookRoot
// forward) from the KCC eye point, NOT along the camera transform (which carries
// visual sway/kick on top). Aim angles are therefore measured against this
// reference whenever it can be read, so the aimbot steers the actual firing
// direction onto the target instead of the camera.
bool      g_aim_ref_valid = false;
bool      g_aim_ref_unverified = false;

Vec3      g_aim_ref_origin{};

Vec3      g_aim_ref_forward{}, g_aim_ref_right{}, g_aim_ref_up{};

// ---- Чувствительность взгляда (настройка игрока) ---------------------------
//
// Oxide.MouseLook.m_Sensitivity — единственный множитель, которым игра
// превращает накопленный сдвиг касания в поворот камеры: в MouseLook.ZJo
// (RVA 0x64e312c) накопленное значение читается с +0x88, умножается на +0x34 и
// уходит в применение поворота; больше ничего на путь «касание -> угол» не
// влияет. Значит град/px строго пропорционален m_Sensitivity, и аиму нужен
// именно он, а не зашитое число.
//
// Поле публичное и в обеих версиях игры лежит на одном месте (сверено по
// dump.cs релиза 205619 и беты 207986; PlayerManager.mouseLook — +0x70 в обоих).
// Поэтому это НЕ часть таблицы переключения версий (tools/offsets): там только
// то, что между сборками разъезжается.
// Константы PLAYER_MOUSE_LOOK_OFFSET / MOUSE_LOOK_SENSITIVITY_OFFSET / 
// MOUSE_LOOK_ACCUM_OFFSET живут в camera.h: ими пользуется ещё и мемори-аим
// (esp/aim_mem.cpp), а раскладка MouseLook должна быть ровно в одном месте.

uint64_t esp_resolve_mouse_look() {
    if (g_pid <= 0 || !g_il2cpp_base || !g_mem.bound()) return 0;

    uint64_t player = resolve_local_player();
    if (!player) {
        // Своей PlayerManager ещё нет (загрузка, только что респавнулись):
        // настройка клиентская, у любой PlayerManager она одна и та же.
        uint64_t list = resolve_runtime_player_list();
        if (!list) return 0;
        uint64_t items = rd_ptr(list + IL2CPP_LIST_ITEMS);
        int32_t  count = rd<int32_t>(list + IL2CPP_LIST_SIZE);
        if (!items || count <= 0 || count > 512) return 0;
        player = rd_ptr(items + IL2CPP_ARRAY_FIRST_ELEMENT);
    }
    if (!player) return 0;
    if (g_player_manager_class && rd_ptr(player) != g_player_manager_class) return 0;

    return rd_ptr(player + PLAYER_MOUSE_LOOK_OFFSET);
}

bool esp_read_look_sensitivity(float& out) {
    uint64_t mouse_look = esp_resolve_mouse_look();
    if (!mouse_look) return false;
    float value = rd<float>(mouse_look + MOUSE_LOOK_SENSITIVITY_OFFSET);
    // Мусор в поле (объект переиспользован, память переехала) отдаём как отказ:
    // заведомо чужое число здесь хуже, чем запасное.
    if (!std::isfinite(value) || value < 0.05F || value > 100.0F) return false;
    out = value;
    return true;
}

// ---- Углы прицела игры как самостоятельный источник поворота ----------------
//
// MouseLook (0x4C): x — рыскание, y — тангаж, градусы. На устройстве это
// единственный источник поворота, который читается всегда: он не идёт ни через
// Transform (иерархия на этой сборке Unity читается не всегда), ни через матрицы
// камеры. Мемори-аим пишет именно сюда, поэтому вид, собранный из этих углов,
// согласован с тем, что аим делает с прицелом.
bool esp_read_look_angles(float& yaw_deg, float& pitch_deg) {
    const uint64_t mouse_look = esp_resolve_mouse_look();
    if (!mouse_look) return false;
    const uint64_t field = mouse_look + MOUSE_LOOK_ANGLES_OFFSET;
    float yaw = rd<float>(field);
    float pitch = rd<float>(field + sizeof(float));
    if (!std::isfinite(yaw) || !std::isfinite(pitch)) return false;
    // Границы нарочно широкие: игре рыскание приходит завёрнутым в 0..360, а
    // тангаж ограничен её же пределами. Отсекаем только заведомый мусор.
    if (fabsf(yaw) > 10000.0F || fabsf(pitch) > 90.5F) return false;
    yaw_deg = yaw;
    pitch_deg = pitch;
    return true;
}

Vec3 forward_from_look_angles(float yaw_deg, float pitch_deg) {
    constexpr float kDegToRad = 0.01745329252F;
    const float yaw = yaw_deg * kDegToRad;
    const float pitch = pitch_deg * kDegToRad;
    const float cos_pitch = cosf(pitch);
    return {sinf(yaw) * cos_pitch, sinf(pitch), cosf(yaw) * cos_pitch};
}

// Насколько поза камеры расходится с углами прицела, градусы; <0 — сравнить
// не с чем (углы не читаются).
static float camera_pose_vs_look_angle_gap() {
    float yaw = 0.0F, pitch = 0.0F;
    if (!g_cam_pose_valid || !esp_read_look_angles(yaw, pitch)) return -1.0F;
    const Vec3 by_angles = forward_from_look_angles(yaw, pitch);
    const Vec3& by_pose = g_cam_forward;
    const float dot = by_pose.x * by_angles.x + by_pose.y * by_angles.y + by_pose.z * by_angles.z;
    if (!std::isfinite(dot)) return -1.0F;
    const float clamped = dot > 1.0F ? 1.0F : (dot < -1.0F ? -1.0F : dot);
    return acosf(clamped) * 57.29577951F;
}

static bool read_camera_transform_pose(uint64_t native_transform, Vec3& position, Vec4& rotation) {
    if (!native_transform) return false;
    if (g_transform_hierarchy_layout_valid) {
        if (read_transform_hierarchy_layout(native_transform, g_transform_hierarchy_layout, position, &rotation))
            return true;
    }
    // Probe the common TransformAccess layouts used elsewhere in this file.
    uint64_t transform_data = rd_ptr(native_transform + 0x38);
    int32_t transform_index = rd<int32_t>(native_transform + 0x40);
    if (!transform_data || transform_index < 0 || transform_index > 100000) {
        transform_data = rd_ptr(native_transform + 0x18);
        transform_index = rd<int32_t>(native_transform + 0x20);
    }
    if (!transform_data || transform_index < 0 || transform_index > 100000) return false;
    const uint64_t data_offsets[][2] = {{0x18, 0x20}, {0x08, 0x10}};
    for (const auto& offsets : data_offsets) {
        uint64_t matrix_pointer = rd_ptr(transform_data + offsets[0]);
        uint64_t index_pointer = rd_ptr(transform_data + offsets[1]);
        if (!matrix_pointer || !index_pointer) continue;
        const uint64_t matrix_candidates[] = {matrix_pointer, rd_ptr(matrix_pointer)};
        const uint64_t index_candidates[] = {index_pointer, rd_ptr(index_pointer)};
        for (uint64_t matrices : matrix_candidates) {
            for (uint64_t indices_ptr : index_candidates) {
                if (read_transform_hierarchy_arrays(matrices, indices_ptr, transform_index, position, &rotation))
                    return true;
            }
        }
    }
    return false;
}

bool read_native_camera_matrices(uint64_t native_cam, float screen_aspect, Mat4& projection, Mat4& view) {
    if (!native_cam) return false;

    static Mat4 s_last_view{};
    static Mat4 s_last_proj{};
    static bool s_last_ok = false;

    // Fresh view from the Camera's live Transform (cam+0x20). The +0x70 cache is
    // only rebuilt inside Unity getters when dirty-flag +0x502 is set — we never
    // run those getters, so raw +0x70 drifts while the camera moves.
    bool have_live_view = false;
    uint64_t native_transform = rd_ptr(native_cam + CAMERA_NATIVE_TRANSFORM);
    if (native_transform) {
        Vec3 cam_pos{};
        Vec4 cam_rot{};
        bool pose_ok = read_camera_transform_pose(native_transform, cam_pos, cam_rot);
        bool fin_ok = pose_ok && vec3_is_finite(cam_pos);
        bool quat_ok = fin_ok && normalize_quaternion(cam_rot);
        // Teleport rejection: a read that lands mid-update inside the game
        // can return a garbage-but-finite pose. One such frame throws every
        // box/marker across the screen (the flicker artifacts). A camera
        // cannot move 30 m in one frame — reject the sample and reuse the
        // last view; a REAL teleport (respawn) sticks, so after a few
        // consecutive "jumps" the new position is accepted.
        static Vec3 s_last_cam_pos{};
        static bool s_last_cam_pos_ok = false;
        static int  s_pose_jump_streak = 0;
        if (quat_ok && s_last_cam_pos_ok && s_last_ok) {
            float jx = cam_pos.x - s_last_cam_pos.x;
            float jy = cam_pos.y - s_last_cam_pos.y;
            float jz = cam_pos.z - s_last_cam_pos.z;
            float j2 = jx * jx + jy * jy + jz * jz;
            if (j2 > 30.0F * 30.0F && s_pose_jump_streak < 4) {
                ++s_pose_jump_streak;
                quat_ok = false; // fall through to the cached view below
            } else {
                s_pose_jump_streak = 0;
            }
        }
        if (quat_ok) {
            view = mat_world_to_camera(cam_pos, cam_rot);
            have_live_view = matrix_is_finite(view);
            if (have_live_view) {
                g_cam_pos = cam_pos;
                g_cam_right = rotate_vector(cam_rot, {1.0F, 0.0F, 0.0F});
                g_cam_up = rotate_vector(cam_rot, {0.0F, 1.0F, 0.0F});
                g_cam_forward = rotate_vector(cam_rot, {0.0F, 0.0F, 1.0F});
                g_cam_pose_valid = true;
                s_last_cam_pos = cam_pos;
                s_last_cam_pos_ok = true;
            }
        }
    }
    if (have_live_view && (g_cam_pose_bad || g_cam_view_suspect) && g_aim_ref_valid) {
        // Поза камеры читается, но доверять ей нельзя: либо она уже уличена в
        // вранье (g_cam_pose_bad), либо по кадрам видно, что ВСЕ игроки уходят ЗА
        // камеру (g_cam_view_suspect — так выглядит и устаревший вид после смерти,
        // когда камера уехала, а позу прочитать не удалось). Вид собираем по оси
        // прицела: она с устройства читается всегда.
        have_live_view = false;
    }
    if (!have_live_view) {
        // Поза камеры не читается (в журналах устройства это cam_st = 0 у аима).
        // До сих пор вид в этом случае брался из кэша камеры +0x70, а он ленивый:
        // его пересобирает только геттер Unity при взведённом флаге +0x502, и мы
        // таких геттеров не зовём. По журналу устройства 17.09.2026 цена этому
        // видна прямо: «рамок нет: спис 11, ниже экрана 10, выше экрана 0» —
        // все игроки оказывались ЗА камерой, то есть кэш описывал старый кадр.
        //
        // Берём ось прицела игрока (LookDirection из обработчика событий): её
        // читает и проверяет read_local_aim_reference, по ней же целится аим.
        // Это источник из дампа, а не догадка. Меняем ТОЛЬКО вид ESP: g_cam_pose_valid,
        // g_cam_forward и g_cam_pos остаются как были, иначе тач-аим поворачивает
        // не туда (это уже случилось 17 сентября 2026, см. camera.h).
        bool from_aim_axis = false;
        Vec3 axis = g_aim_ref_forward;
        const float axis_length = sqrtf(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
        if (g_aim_ref_valid && vec3_is_finite(axis) && vec3_is_finite(g_aim_ref_origin) && axis_length > 0.5F) {
            const Vec3 forward = {axis.x / axis_length, axis.y / axis_length, axis.z / axis_length};
            Vec3 right = cross_product({0.0F, 1.0F, 0.0F}, forward);
            const float right_length = sqrtf(right.x * right.x + right.y * right.y + right.z * right.z);
            if (right_length > 0.01F) {
                right = {right.x / right_length, right.y / right_length, right.z / right_length};
                const Vec3 up = cross_product(forward, right);
                Mat4 rebuilt = mat_view_from_basis(right, up, forward, g_aim_ref_origin);
                if (matrix_is_finite(rebuilt)) {
                    view = rebuilt;
                    have_live_view = true;
                    from_aim_axis = true;
                }
            }
        }
        if (from_aim_axis) {
            static double s_axis_report_next = 0.0;
            g_cam_view_source = 1;
            const double now = mono_seconds();
            if (now >= s_axis_report_next) {
                s_axis_report_next = now + 30.0;
                diag_log("esp", "вид: поза камеры не читается — строю по оси прицела, точка %.1f %.1f %.1f, ось %.2f %.2f %.2f (без сверки: %d)",
                         (double)g_aim_ref_origin.x, (double)g_aim_ref_origin.y, (double)g_aim_ref_origin.z,
                         (double)g_aim_ref_forward.x, (double)g_aim_ref_forward.y, (double)g_aim_ref_forward.z,
                         g_aim_ref_unverified ? 1 : 0);
            }
        } else if (s_last_ok && matrix_is_finite(s_last_view)) {
            view = s_last_view;
            g_cam_view_source = 2;
        } else {
            // Последний резерв — кэш камеры +0x70. Нативная Matrix4x4f
            // раскладывается по памяти построчно, а наш mat_get поколоночный
            // (см. math.h), поэтому «как есть» она читается ПЕРЕВЁРНУТОЙ: поворот
            // выворачивается, и все точки уходят за камеру («рамок нет: низ N,
            // вх 0» в журнале устройства). Перевод при этом читается одинаково,
            // поэтому позиция камеры выглядит правильной и ошибку не видно.
            // Раскладку определяет view_orientation, и по ней же решается,
            // переставлять ли матрицу.
            const Mat4 raw = rd_m4(native_cam + CAMERA_VIEW_MATRIX);
            const int orientation = view_orientation(raw);
            if (orientation == 0) {
                if (s_last_ok && matrix_is_finite(s_last_view)) {
                    view = s_last_view;
                    g_cam_view_source = 2;
                } else {
                    return false;
                }
            } else {
                view = orientation == 2 ? mat_transposed(raw) : raw;
                g_cam_view_source = 2;
                static bool s_layout_reported = false;
                if (!s_layout_reported) {
                    s_layout_reported = true;
                    diag_log("esp", "вид: кэш камеры +0x70, раскладка %d (2 — нативная построчная, переставил)", orientation);
                }
            }
        }
    } else {
        g_cam_view_source = 0;
    }

    // Углы прицела игры против позы камеры — ТОЛЬКО замер для журнала.
    //
    // 17 сентября 2026 вид пробовали брать из этих углов, когда поза расходится
    // с ними больше 60°: на устройстве это сразу уронило тач-аим («целится в
    // пустоту и отдергивает») — и это правильно, потому что готовая ось аима
    // (esp_aim_camera_angles) считается как раз по g_cam_forward. Менять вид по
    // расхождению с углами нельзя: у самих углов другая конвенция знаков, и
    // подмена оси ломает наведение. Здесь остаётся замер: если он покажет
    // расхождение, разбираться нужно по нему, а не подменой.
    {
        static int s_gap_check_countdown = 0;
        static double s_gap_report_next = 0.0;
        if (--s_gap_check_countdown <= 0) {
            s_gap_check_countdown = 20;   // ~3 раза в секунду
            const float gap = camera_pose_vs_look_angle_gap();
            if (gap >= 0.0F) g_cam_view_angle_gap_deg = gap;
            const double now = mono_seconds();
            if (gap > 20.0F && now >= s_gap_report_next) {
                s_gap_report_next = now + 30.0;
                diag_log("esp", "углы прицела и поза камеры расходятся на %.0f° (поза %.1f, %.1f, %.1f)",
                         (double)gap, (double)g_cam_forward.x, (double)g_cam_forward.y, (double)g_cam_forward.z);
            }
        }
    }
    if (have_live_view) s_last_view = view;

    // Projection params (FOV/aspect/clip) are stored as plain floats and stay hot.
    float fov = rd<float>(native_cam + CAMERA_FOV_DEGREES);
    float aspect = rd<float>(native_cam + CAMERA_ASPECT);
    float z_near = rd<float>(native_cam + CAMERA_NEAR_CLIP);
    float z_far = rd<float>(native_cam + CAMERA_FAR_CLIP);
    if (!(aspect > 0.1F && aspect < 10.0F))
        aspect = (screen_aspect > 0.1F && screen_aspect < 10.0F) ? screen_aspect : (9.0F / 16.0F);
    if (!(z_near > 0.001F && z_near < 100.0F)) z_near = 0.1F;
    if (!(z_far > z_near && z_far < 100000.0F)) z_far = 1000.0F;
    // Проекция считается из поля угла обзора — как было до 17 сентября 2026,
    // когда брать кеш камеры (0xB0) первым оказалось перебором: кеш ленивый
    // (§3.1), и подмена проекции на «может быть, свежую» меняет экранные
    // координаты всех целей, а на них стоит наведение. Кеш остаётся только
    // последним резервом (ниже) и замером: расхождение угла обзора с полем
    // пишется в журнал — оно и отвечает на вопрос про ось угла (Camera.fovAxis).
    if (std::isfinite(fov) && fov > 1.0F && fov < 179.0F) g_cam_fov_deg = fov;
    projection = mat_perspective(fov, aspect, z_near, z_far);
    {
        static double s_fov_report_next = 0.0;
        Mat4 cached = rd_m4(native_cam + CAMERA_PROJECTION_MATRIX);
        const int orientation = matrix_is_finite(cached) ? perspective_orientation(cached) : 0;
        if (orientation == 2) cached = mat_transposed(cached);
        if (orientation) {
            const float scale_y = mat_get(cached, 1, 1);
            const float matrix_fov = 2.0F * atanf(1.0F / scale_y) * 57.29577951F;
            const double now = mono_seconds();
            if (std::isfinite(matrix_fov) && matrix_fov > 1.0F && matrix_fov < 179.0F &&
                std::isfinite(fov) && fabsf(fov - matrix_fov) > 2.0F && now >= s_fov_report_next) {
                s_fov_report_next = now + 60.0;
                diag_log("esp", "угол обзора: поле %.1f°, кеш камеры %.1f° (ориентация %d) — считаю по полю",
                         (double)fov, (double)matrix_fov, orientation);
            }
        }
    }
    if (!matrix_is_finite(projection)) {
        if (s_last_ok && matrix_is_finite(s_last_proj)) {
            projection = s_last_proj;
        } else {
            // Резерв: кэш проекции камеры. Раскладку определяем так же, как для
            // вида: нативная Matrix4x4f построчная, и «как есть» перспектива
            // читается перевёрнутой (у неё строка w становится столбцом).
            Mat4 cached = rd_m4(native_cam + CAMERA_PROJECTION_MATRIX);
            const int orientation = perspective_orientation(cached);
            if (orientation == 0) return false;
            projection = orientation == 2 ? mat_transposed(cached) : cached;
        }
    }
    if (matrix_is_finite(projection)) s_last_proj = projection;
    if (have_live_view && matrix_is_finite(view)) {
        s_last_view = view;
        s_last_ok = true;
    }
    return true;
}

bool w2s_transform_camera(const Vec3& camera_position, const Vec4& camera_rotation, const Vec3& world, float screen_width, float screen_height, Vec2& output, bool clip_to_screen) {
    if (screen_width < 100.0F || screen_height < 100.0F) return false;
    Vec3 relative = {world.x - camera_position.x, world.y - camera_position.y, world.z - camera_position.z};
    Vec4 inverse_rotation = {-camera_rotation.x, -camera_rotation.y, -camera_rotation.z, camera_rotation.w};
    Vec3 camera_space = rotate_vector(inverse_rotation, relative);
    if (!vec3_is_finite(camera_space) || camera_space.z <= 0.05F) return false;
    constexpr float vertical_fov_radians = 1.0471975512F;
    float tangent = tanf(vertical_fov_radians * 0.5F);
    float aspect = screen_width / screen_height;
    float normalized_x = camera_space.x / (camera_space.z * tangent * aspect);
    float normalized_y = camera_space.y / (camera_space.z * tangent);
    if (!std::isfinite(normalized_x) || !std::isfinite(normalized_y)) return false;
    if (clip_to_screen && (fabsf(normalized_x) > 1.0F || fabsf(normalized_y) > 1.0F)) return false;
    output.x = (normalized_x + 1.0F) * 0.5F * screen_width;
    output.y = (1.0F - normalized_y) * 0.5F * screen_height;
    return std::isfinite(output.x) && std::isfinite(output.y);
}

// Кадры подряд, в которых выборка позиций игроков сошлась в одну точку (см.
// optimize_matrix_configuration): одному кадру столько же доверия, сколько
// серии — нельзя, за приговором следует перепоиск смещения с ожиданием 0.6 с.
int g_offset_extent_fail_streak = 0;

bool optimize_matrix_configuration(uint64_t native_camera, const std::vector<uint64_t>& transforms) {
    std::vector<Vec3> samples;
    for (uint64_t source : transforms) {
        Vec3 position{};
        if (!read_entity_position(source, position)) continue;
        // Нули после респауна и мусор из пула — не мировые позиции. Раньше они
        // попадали в выборку, сходились в одну точку и объявляли смещение
        // неверным: дальше шёл поиск смещения заново с ожиданием 0.6 с, и всё
        // это время боксов не было вовсе.
        if (!position_looks_like_world_space(position)) continue;
        samples.push_back(position);
        if (samples.size() >= 24) break;
    }
    if (samples.empty()) {
        // Nothing readable this frame; keep the validated offset and retry.
        return false;
    }

    Vec3 minimum = samples[0], maximum = samples[0];
    for (const Vec3& position : samples) {
        minimum.x = std::min(minimum.x, position.x); minimum.y = std::min(minimum.y, position.y); minimum.z = std::min(minimum.z, position.z);
        maximum.x = std::max(maximum.x, position.x); maximum.y = std::max(maximum.y, position.y); maximum.z = std::max(maximum.z, position.z);
    }
    float extent = fabsf(maximum.x - minimum.x) + fabsf(maximum.y - minimum.y) + fabsf(maximum.z - minimum.z);
    // Several players that never move apart mean the offset is not a position
    // at all -- but only when there are several of them. Alone on the server a
    // zero extent is normal and must not invalidate anything.
    //
    // Столпившиеся игроки и мигнувшее чтение выглядят одинаково, поэтому
    // приговор смещению выносим только по серии кадров: одиночный кадр
    // с одинаковыми отсчётами раньше запускал поиск смещения заново вместе с
    // ожиданием 0.6 с, и всё это время боксов не было («мерцание на полсекунды»).
    if (samples.size() >= 2 && extent < 0.1F) {
        // Во время перезагрузки мира «все игроки в одной точке» — норма: позиции
        // ещё не дописаны. Приговор смещению тут не выносим, иначе каждый
        // респавн начинался с перепоиска смещения и ожидания 0.6 с без боксов.
        if (!world_reloading() && ++g_offset_extent_fail_streak >= 3) {
            g_offset_extent_fail_streak = 0;
            g_player_position_validated = false;
            diag_log("esp", "позиции игроков сошлись в точку (разброс %.2f м) — поиск смещения заново",
                     (double)extent);
        }
        return false;
    }
    g_offset_extent_fail_streak = 0;
    if (samples.size() < 2 && !position_looks_like_world_space(samples[0])) return false;

    Mat4 validated_projection{}, validated_view{};
    if (!read_native_camera_matrices(native_camera, 0.0F, validated_projection, validated_view))
        return false;
    Vec3 camera_position{};
    double nearest_camera_distance_squared = INFINITY;
    if (camera_position_from_view(validated_view, camera_position)) {
        for (const Vec3& sample : samples) {
            double dx = (double)sample.x - camera_position.x, dy = (double)sample.y - camera_position.y, dz = (double)sample.z - camera_position.z;
            double distance_squared = dx * dx + dy * dy + dz * dz;
            if (std::isfinite(distance_squared)) nearest_camera_distance_squared = std::min(nearest_camera_distance_squared, distance_squared);
        }
    }
    g_camera_matrix_physical_match = std::isfinite(nearest_camera_distance_squared) && nearest_camera_distance_squared <= 100.0;
    g_matrix_configuration_validated = true;
    return true;
}

std::vector<uint64_t> read_configured_player_transforms() {
    std::vector<uint64_t> transforms;
    uint64_t list = resolve_runtime_player_list();
    if (!list) return transforms;

    uint64_t local_player = resolve_local_player();
    if (local_player && !player_list_contains(list, local_player)) {
        // Alone on the server the runtime list is empty — the local player
        // is only registered there while networked players are around. He is
        // still perfectly valid (his class just re-checked in the resolver),
        // and dropping him here was what killed markers/farm solo: the frame
        // below never got a local position. Only treat him as stale when the
        // list actually has entries that he is missing from.
        int32_t list_count = rd<int32_t>(list + IL2CPP_LIST_SIZE);
        if (list_count > 0) {
            if (local_player == g_local_player) g_local_player = 0;
            local_player = 0;
        }
    }
    uint64_t local_source = local_player;

    for (int attempt = 0; attempt < 3; ++attempt) {
        if (attempt > 0) {
            uint64_t refreshed_list = resolve_runtime_player_list();
            if (refreshed_list) list = refreshed_list;
        }
        uint64_t items = rd_ptr(list + IL2CPP_LIST_ITEMS);
        int32_t count = rd<int32_t>(list + IL2CPP_LIST_SIZE);
        if (!items || count <= 0 || count > 512) {
            // Empty list, but the local player himself is known: he alone is
            // enough for the whole pipeline (camera, matrix validation and
            // the local position all work from one sample).
            if (local_source && count == 0) return {local_source};
            continue;
        }

        std::vector<uint64_t> snapshot;
        snapshot.reserve((size_t)count + 1);
        if (local_source) snapshot.push_back(local_source);

        for (int32_t index = 0; index < count; ++index) {
            uint64_t player = rd_ptr(items + IL2CPP_ARRAY_FIRST_ELEMENT + (uint64_t)index * sizeof(uint64_t));
            if (!player) continue;
            if (g_player_manager_class && rd_ptr(player) != g_player_manager_class) continue;
            if (player == local_source) continue;
            snapshot.push_back(player);
        }

        uint64_t confirmed_items = rd_ptr(list + IL2CPP_LIST_ITEMS);
        int32_t confirmed_count = rd<int32_t>(list + IL2CPP_LIST_SIZE);
        if (items == confirmed_items && count == confirmed_count && !snapshot.empty())
            return snapshot;
    }
    return transforms;
}

float esp_camera_fov_deg() { return g_cam_fov_deg; }

// Bit 0: camera pose known. Bit 2: the firing reference (look direction from
// the eye point) is in use instead of the camera axis. Bit 1 не выставляется
// (там когда-то отмечалась поза, выведенная из матрицы вида) и оставлен, чтобы
// не разошлись номера битов в разборе строки AIM: cam_st 0 означает «настоящей
// оси нет» — ровно то, что видит аим (esp_aim_camera_angles).
int esp_camera_state() {
    return (g_cam_pose_valid ? 1 : 0) | (g_cam_pose_derived ? 2 : 0) | (g_aim_ref_valid ? 4 : 0);
}

// KCC.Move.Position: the simulated character position (capsule bottom) the
// game itself moves the character with. Independent of the discovered
// PlayerManager position field, and validated by the KCC back-reference.
static bool player_kcc_position(const PlayerAux& aux, Vec3& out) {
    if (!aux.kcc) return false;
    Vec3 p = rd_v3(aux.kcc + KCC_MOVE + 0x0C);
    if (!vec3_is_finite(p)) return false;
    float magnitude = fabsf(p.x) + fabsf(p.y) + fabsf(p.z);
    if (magnitude < 0.01F || magnitude > 100000.0F) return false;
    out = p;
    return true;
}

// Local firing reference: LookDirection from the event handler plus the eye
// point the hitscan ray starts from. Falls back to the camera when unavailable
// or implausible (must stay within ~20 deg of the camera forward).
void read_local_aim_reference(uint64_t local_player, const PlayerAux* local_aux, bool local_crouched) {
    g_aim_ref_valid = false;
    g_aim_ref_unverified = false;
    if (!local_player) return;
    uint64_t handler = rd_ptr(local_player + PLAYER_EVENT_HANDLER);
    if (!handler || rd_ptr(handler + EVENT_HANDLER_MANAGER_BACKREF) != local_player) return;
    uint64_t look = rd_ptr(handler + EVENT_HANDLER_LOOK_DIRECTION);
    if (!look) return;
    Vec3 dir = rd_v3(look + SYNC_VALUE_OFFSET);
    if (!vec3_is_finite(dir)) return;
    float len = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (!(len > 0.5F && len < 2.0F)) return;
    dir = {dir.x / len, dir.y / len, dir.z / len};
    // Ось проверяется по двум свидетелям: поза камеры и углы прицела игры
    // (MouseLook 0x4c/0x50). Одной позы мало — журнал устройства 17.09.2026
    // показывает, что поза камеры врёт: в «рамок нет» все игроки оказываются ЗА
    // камерой (w отрицательный), то есть её поворот читается неверно. Сверка
    // только по позе в этом случае отвергает НАСТОЯЩУЮ ось (она из обработчика
    // событий игрока, LookDirection), аим остаётся без оси и целится по мусору:
    // «целится в пустоту и отдергивает в сторону». Поэтому при расхождении
    // позы и оси третьим голосом идут углы прицела: если с ними сходится ось,
    // а поза нет — врёт поза, и ось принимается (а поза помечается негодной для
    // вида ESP, см. g_cam_pose_bad).
    const float dot_pose = g_cam_pose_valid
        ? dir.x * g_cam_forward.x + dir.y * g_cam_forward.y + dir.z * g_cam_forward.z
        : -2.0F;
    if (g_cam_pose_valid && dot_pose > 0.94F) {
        g_aim_ref_unverified = false;
    } else if (g_cam_pose_valid) {
        float look_yaw = 0.0F, look_pitch = 0.0F;
        bool axis_wins = false;
        float dot_look_axis = -2.0F, dot_look_pose = -2.0F;
        if (esp_read_look_angles(look_yaw, look_pitch)) {
            const Vec3 look_fwd = forward_from_look_angles(look_yaw, look_pitch);
            dot_look_axis = dir.x * look_fwd.x + dir.y * look_fwd.y + dir.z * look_fwd.z;
            dot_look_pose = g_cam_forward.x * look_fwd.x + g_cam_forward.y * look_fwd.y + g_cam_forward.z * look_fwd.z;
            axis_wins = dot_look_axis > 0.94F && dot_look_axis > dot_look_pose;
        }
        if (!axis_wins) return; // > ~20 deg away from the camera: not the look root
        g_cam_pose_bad = true;
        g_aim_ref_unverified = false;
        diag_log("aim", "поза камеры против оси прицела: с углами прицела сошлась ось (совпадение %.2f), а поза нет (%.2f) — беру ось, поза негодна для вида ESP",
                 (double)dot_look_axis, (double)dot_look_pose);
    } else {
        // Сверять не с чем. Ось всё равно берём: она из обработчика событий игрока
        // и нужна и аиму (без неё он вовсе не находит поворот камеры), и как
        // источник вида для ESP. Но помечаем её как непроверенную: по этому флагу
        // видно в журнале, что ось идёт без сверки.
        g_aim_ref_unverified = true;
    }
    Vec3 world_up = {0.0F, 1.0F, 0.0F};
    Vec3 right = cross_product(world_up, dir);
    float rl = sqrtf(right.x * right.x + right.y * right.y + right.z * right.z);
    if (!(rl > 0.01F)) return; // looking straight up/down: keep camera basis
    right = {right.x / rl, right.y / rl, right.z / rl};
    Vec3 up = cross_product(dir, right);

    // Eye point: KCC position + (capsule height + lookHeightOffset) * up. Only
    // trusted when it lands close to the camera; otherwise use the camera.
    Vec3 origin = g_cam_pos;
    if (local_aux && local_aux->kcc) {
        Vec3 kcc_pos{};
        if (player_kcc_position(*local_aux, kcc_pos)) {
            float h = local_crouched ? local_aux->crouch_height : local_aux->normal_height;
            float look_offset = rd<float>(local_aux->kcc + KCC_LOOK_HEIGHT_OFFSET);
            if (!std::isfinite(look_offset) || fabsf(look_offset) > 1.0F) look_offset = 0.0F;
            Vec3 eye = {kcc_pos.x, kcc_pos.y + h + look_offset, kcc_pos.z};
            float dx = eye.x - g_cam_pos.x, dy = eye.y - g_cam_pos.y, dz = eye.z - g_cam_pos.z;
            // Поза камеры не читается — сверять глаз не с чем; но он посчитан из
            // KCC, который читается независимо от Transform, и по нему строится
            // вид ESP, так что берём его как есть.
            if (!g_cam_pose_valid || dx * dx + dy * dy + dz * dz < 0.5F * 0.5F) origin = eye;
        }
    }
    g_aim_ref_origin = origin;
    g_aim_ref_forward = dir;
    g_aim_ref_right = right;
    g_aim_ref_up = up;
    g_aim_ref_valid = true;
}
