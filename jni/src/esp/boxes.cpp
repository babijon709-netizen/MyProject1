// boxes.cpp — esp_get_boxes: рамки, имена, скелет.
//
// Модуль разрезан из прежнего монолита jni/src/game.cpp;
// что здесь лежит и кто это зовёт — в шапке boxes.h и в docs/CODE_MAP.md.

#include "esp/common.h"
#include "esp/aim_points.h"
#include "esp/esp_time.h"
#include "esp/camera.h"
#include "esp/frame.h"
#include "esp/game_patch.h"
#include "esp/il2cpp.h"
#include "esp/math.h"
#include "esp/mem.h"
#include "esp/names.h"
#include "esp/player_pose.h"
#include "esp/skeleton_cache.h"
#include "esp/skeleton_names.h"
#include "esp/transform.h"
#include "esp/weapons.h"
#include "app/diag_log.h"
#include "boxes.h"

std::vector<EspBox> esp_get_boxes(int overlay_width, int overlay_height) {
    std::vector<EspBox> result;


    // Watchdog: копим не кадры, а ВРЕМЯ без публикации камеры/позиции — иначе
    // на слабом устройстве (большой экран, троттлинг, 20-30 кадров вместо 60)
    // «240 кадров» превращались в 8-12 секунд слепоты, и порог срабатывал
    // совсем не тогда, когда задумано. Смысл порога тот же: несколько секунд
    // без кадра — значит какой-то кэш или класс не пережил перезагрузку мира,
    // и всё это надо собрать заново.
    {
        static double s_publish_fail_since = 0.0;
        if (g_frame_publish_fail_streak == 0) s_publish_fail_since = mono_seconds();
        ++g_frame_publish_fail_streak;
        const double stalled = mono_seconds() - s_publish_fail_since;
        if (stalled > 3.0) {
            diag_log("esp", "сторож: %.1f с без кадра (за это время кадров %d) — сброс кэшей мира (сбросов %d)",
                     stalled, g_frame_publish_fail_streak, g_frame_watchdog_resets + 1);
            g_frame_publish_fail_streak = 0;
            s_publish_fail_since = mono_seconds();
            g_frame_transforms.clear();
            reset_world_caches();
            ++g_frame_watchdog_resets;
            // Три сброса подряд — сброс кэшей уже не помогает, значит неверна
            // сама привязка (база библиотеки, права, перезапуск игры). Просим
            // поток привязки подключиться заново — он выберет базу заново и
            // заново проверит доступ.
            if (g_frame_watchdog_resets >= 3) {
                g_frame_watchdog_resets = 0;
                g_want_reattach.store(true);
            }
        }
    }

    // Маркеры и фарм живут на опубликованном кадре камеры. Раньше флаги гасились
    // здесь же, на входе, а кадр собирался заново — и любой отказ чтения (мигнул
    // указатель камеры, не прочитались матрицы) оставлял экран пустым ровно в
    // этот кадр. По логам это и есть «ESP мерцает». Теперь кадр считается
    // несобранным, а прошлый держится ещё треть секунды: камера за это время не
    // уезжает, зато мигания нет. Совсем старый кадр не держим — см.
    // frame_drop_unpublished().
    if (g_pid <= 0 || !g_il2cpp_base) { frame_drop_unpublished(); return result; }

    uint64_t native_cam = 0;
    Mat4 projection{}, view{}, vp{};
    float sw = overlay_width >= 100 ? (float)overlay_width : 1080.0F;
    float sh = overlay_height >= 100 ? (float)overlay_height : 2400.0F;
    if (!std::isfinite(sw) || sw < 100.0F || sw > 10000.0F) sw = 1080.0F;
    if (!std::isfinite(sh) || sh < 100.0F || sh > 10000.0F) sh = 2400.0F;
    g_last_overlay_sw = sw;
    g_last_overlay_sh = sh;

    std::vector<uint64_t>& s_transforms = g_frame_transforms;
    std::vector<uint64_t> refreshed = read_configured_player_transforms();
    if (!refreshed.empty()) {
        // World reload: every PlayerManager object is new (no overlap with
        // the previous population). This also has to fire when we are alone on
        // the server -- respawning solo replaces our single PlayerManager and
        // used to leave every cache pointing at the dead one.
        // Пересечение считаем не с прошлым кадром, а со снапшотом — составом,
        // под который собраны кэши: список подменяется уже на первом кадре
        // смены, и на следующем кадре сравнение с прошлым кадром пересекается
        // само с собой, то есть полную смену состава оно бы не заметило.
        bool overlap = g_population_snapshot.empty();
        if (!overlap) {
            for (uint64_t previous : g_population_snapshot) {
                for (uint64_t current : refreshed) if (previous == current) { overlap = true; break; }
                if (overlap) break;
            }
        }
        if (!overlap) {
            // Не с первого кадра: адреса списка иногда мигают (rd_ptr вернул
            // чужую копию объекта или мусор из недописанного массива), и такой
            // одиночный кадр раньше обнулял ВСЕ кэши — боксы, метки и раскладку
            // скелета — то есть выглядел как «всё пропало и вернулось не туда».
            // Настоящая перезагрузка мира отличается тем, что новый состав
            // держится кадров подряд. Снапшот ставим ПОСЛЕ сброса: он чистит его.
            if (++g_world_change_streak >= 3) {
                reset_world_caches();
                g_population_snapshot = refreshed;
            }
        } else {
            g_world_change_streak = 0;
            g_population_snapshot = refreshed;
        }
        // Население то же, но кого-то не досчитались: почти всегда это сбой
        // чтения одного элемента, а не уход игрока. Возвращаем пропавших в
        // список ещё на десяток кадров (иначе их боксы мигают), после чего
        // отпускаем — иначе ушедший игрок остался бы в списке навсегда.
        // Было 2 кадра: список читается вразнобой, и на слабом устройстве, где
        // кадры идут по 30-40 мс, игрок успевал пропасть и вернуться СРАЗУ
        // несколько раз — это и виделось как «продают ESP и мерцают».
        if (overlap && refreshed.size() < s_transforms.size()) {
            for (uint64_t previous : s_transforms) {
                bool present = false;
                for (uint64_t current : refreshed) if (current == previous) { present = true; break; }
                if (present) { g_frame_transforms_lost.erase(previous); continue; }
                if (++g_frame_transforms_lost[previous] <= 12) refreshed.push_back(previous);
                else g_frame_transforms_lost.erase(previous);
            }
            if (g_frame_transforms_lost.size() > 256) g_frame_transforms_lost.clear();
        }
        s_transforms = std::move(refreshed);
        g_frame_transforms_empty_streak = 0;
        g_frame_transforms_empty_since = 0.0;
    } else {
        // Список не читается. Раньше решение принималось по кадрам (10 штук), и
        // на перезагрузке мира после смерти чит успевал выбросить всех игроков:
        // боксы гасли синхронно со «смертью» и возвращались позже — то самое
        // мерцание. Теперь порог — по ВРЕМЕНИ, и пока идёт перезагрузка мира,
        // прошлый состав вообще не трогаем: игра сама вернёт список, а кэши
        // сбросятся штатно, когда придёт новый состав (см. g_world_change_streak).
        ++g_frame_transforms_empty_streak;
        const double now = mono_seconds();
        if (g_frame_transforms_empty_since <= 0.0) g_frame_transforms_empty_since = now;
        if (g_frame_transforms_empty_streak >= 10 && !world_reloading() &&
            now - g_frame_transforms_empty_since > 5.0) {
            if (!s_transforms.empty()) {
                diag_log("esp", "список игроков пуст %.1f с — чищу кэши мира",
                         now - g_frame_transforms_empty_since);
                s_transforms.clear(); reset_world_caches();
            }
            g_frame_transforms_empty_since = 0.0;
        }
    }
    if (s_transforms.empty()) {
        // Alone on the server: no player boxes, but markers and the farm
        // still need this frame's camera + local position.
        g_frame_player_count = 0;   // никого нет — и «пилюля» это показывает
        g_player_data_stale = false;   // держать нечего: игроков нет
        publish_camera_only_frame(sw, sh);
        return result;
    }

    if (g_body_caches_dirty) { g_body_caches_dirty = false; g_player_aux.clear(); g_skeletons.clear(); }
    const bool want_bones = g_skeleton_enabled || g_aim_bones_requested;
    prune_player_aux(s_transforms);
    prune_player_text(s_transforms);
    prune_player_track(s_transforms);
    prune_mount_latch(s_transforms);
    if (want_bones) prune_skeleton_cache(s_transforms);
    else if (!g_skeletons.empty()) g_skeletons.clear();
    g_skeleton_builds_this_frame = 0;

    if (!g_player_position_validated) {
        if (!discover_player_position_offset(s_transforms)) {
            // Мир ещё не отдал позиции (перезагрузка после смерти). Боксов в
            // этом кадре нет, но метки и фарм живут на камере — без публикации
            // они гаснут вместе с боксами на все секунды ожидания.
            publish_camera_only_frame(sw, sh);
            return result;
        }
    } else {
        recheck_direct_player_position(s_transforms);
    }

    bool transform_camera_mode = false; // light fix: always use native cam matrices, avoid dead-body-as-camera on death
    if (!transform_camera_mode) {
        uint64_t managed_cam = 0;
        if (g_game_controller_class) {
            uint64_t gcb_sf = get_class_static_fields(g_game_controller_class);
            if (gcb_sf) {
                            uint64_t cam_mgr = rd_ptr(gcb_sf + GAME_CONTROLLER_CAMERA_MANAGER_FIELD);
                if (cam_mgr) managed_cam = rd_ptr(cam_mgr + CAMERA_MANAGER_CAMERA_FIELD);
            }
        }
        if (!managed_cam) {
            frame_drop_unpublished(); return result;
        }
        native_cam = rd_ptr(managed_cam + MANAGED_CACHED_PTR);
        if (!native_cam) {
            frame_drop_unpublished(); return result;
        }
        xray_apply(native_cam);
        always_day_tick();
        if (!read_native_camera_matrices(native_cam, sw / sh, projection, view)) {
            frame_drop_unpublished(); return result;
        }
        if (!g_matrix_configuration_validated) {
            if (!optimize_matrix_configuration(native_cam, s_transforms)) {
                frame_drop_unpublished(); return result;
            }
            if (!read_native_camera_matrices(native_cam, sw / sh, projection, view)) {
                frame_drop_unpublished(); return result;
            }
        }
        // Unity worldToClip = projection * worldToCamera (same order as native 0xe2b90c).
        vp = mat_mul(projection, view);
    }

    bool has_local_position = false;
    Vec3 local{};
    size_t local_entity_index = s_transforms.size();
    // Positions are read once per frame here and reused below: the box loop
    // must see exactly the same values the local player was picked with.
    std::vector<Vec3> positions(s_transforms.size());
    std::vector<char> position_ok(s_transforms.size(), 0);

    {
        Vec3 camera_position{};
        bool has_camera_position = g_camera_matrix_physical_match && camera_position_from_view(view, camera_position);
        double nearest_distance_squared = INFINITY;
        size_t first_valid_index = s_transforms.size();
        Vec3 first_valid_position{};
        for (size_t index = 0; index < s_transforms.size(); ++index) {
            Vec3 candidate{};
            // Мусорный отсчёт (нули после респауна, денормали, координаты чужого
            // объекта из пула) отбрасываем сразу: это finite-вектор, но не
            // мировая позиция, и именно он утаскивал бокс «в другую сторону».
            const bool read_ok = read_entity_position(s_transforms[index], candidate) &&
                                 position_looks_like_world_space(candidate);
            // Один сбой чтения или один мусорный кадр больше не гасят и не
            // дёргают бокс — см. filter_player_position.
            PlayerTrack& filter = g_player_track[s_transforms[index]];
            if (!filter_player_position(filter, read_ok, candidate)) continue;
            apply_mounted_position(s_transforms[index], candidate);
            positions[index] = candidate;
            position_ok[index] = 1;
            track_player(s_transforms[index], candidate);
            if (first_valid_index == s_transforms.size()) { first_valid_index = index; first_valid_position = candidate; }
            if (!has_camera_position) continue;
            double dx = (double)candidate.x - camera_position.x, dy = (double)candidate.y - camera_position.y, dz = (double)candidate.z - camera_position.z;
            double distance_squared = dx * dx + dy * dy + dz * dz;
            if (std::isfinite(distance_squared) && distance_squared < nearest_distance_squared) {
                nearest_distance_squared = distance_squared; local_entity_index = index; local = candidate;
            }
        }
        if (local_entity_index == s_transforms.size() && first_valid_index != s_transforms.size()) {
            local_entity_index = first_valid_index; local = first_valid_position;
        }
        has_local_position = local_entity_index != s_transforms.size();
        if (!has_local_position) {
            // Позиции не прочитались ни у одного игрока. Это бывает и на
            // одном кадре (мигнуло чтение по /proc/<pid>/mem после смерти или
            // перезагрузки мира), поэтому смещение позиции объявляем неверным
            // только по серии кадров: за этим следует поиск смещения заново с
            // ожиданием 0.6 с, и всё это время боксов нет вовсе. Метки и фарм
            // живут на кадре одной камеры — публикуем его, чтобы пропажа
            // боксов не гасила и их.
            // Пока мир перезагружается, «позиции не прочитались» ничего не
            // значит: поля игры в этот момент ещё пишутся. Иначе каждый респавн
            // заканчивался перепоиском смещения с ожиданием 0.6 с — и всё это
            // время боксов не было вовсе.
            if (!world_reloading() && ++g_local_position_fail_streak >= 10) {
                diag_log("esp", "позиции игроков не читаются %d кадров — поиск смещения заново",
                         g_local_position_fail_streak);
                g_local_position_fail_streak = 0;
                g_player_position_validated = false;
            }
            // Камера этого кадра уже прочитана — публикуем её как есть (без
            // повторного чтения и повторного xray/day-тика), иначе вместе с
            // боксами гаснут и метки, и фарм.
            // Игроки в списке есть, но их позиции в этом кадре не прочитались:
            // это тот самый момент после смерти/респавна. Рисующий слой держит
            // прошлый снимок боксов, пока признак стоит (см. g_player_data_stale).
            g_player_data_stale = true;
            if (matrix_is_finite(vp)) {
                g_frame_vp = vp; g_frame_vp_valid = true;
                g_frame_sw = sw; g_frame_sh = sh;
                Vec3 camera_only_position{};
                if (camera_position_from_view(view, camera_only_position)) {
                    g_frame_local_pos = camera_only_position;
                    g_frame_local_valid = true;
                }
                g_frame_publish_fail_streak = 0;
                g_frame_watchdog_resets = 0;
                frame_note_published();
            }
            return result;
        }
        g_local_position_fail_streak = 0;
    }

    g_frame_vp = vp;
    g_frame_vp_valid = !transform_camera_mode;
    g_frame_sw = sw; g_frame_sh = sh;
    g_frame_local_pos = local;
    g_frame_local_valid = has_local_position;
    g_frame_publish_fail_streak = 0; // this frame is healthy
    g_frame_watchdog_resets = 0;
    g_player_data_stale = false;     // кадр собран полностью
    frame_note_published();

    // Fallback camera basis straight from the view matrix (rows: right, up,
    // -forward). Kept separate from g_cam_* — the pose path stays authoritative
    // for the aimbot; this one only feeds the farm when the pose read fails.
    g_frame_cam_basis_valid = false;
    if (!transform_camera_mode) {
        Vec3 vr = {mat_get(view, 0, 0), mat_get(view, 0, 1), mat_get(view, 0, 2)};
        Vec3 vu = {mat_get(view, 1, 0), mat_get(view, 1, 1), mat_get(view, 1, 2)};
        Vec3 vf = {-mat_get(view, 2, 0), -mat_get(view, 2, 1), -mat_get(view, 2, 2)};
        Vec3 vpos{};
        if (vec3_is_finite(vr) && vec3_is_finite(vu) && vec3_is_finite(vf) &&
            camera_position_from_view(view, vpos)) {
            float fl = sqrtf(vf.x * vf.x + vf.y * vf.y + vf.z * vf.z);
            if (fl > 0.5F && fl < 2.0F) {
                g_frame_cam_pos = vpos;
                g_frame_cam_fwd = {vf.x / fl, vf.y / fl, vf.z / fl};
                g_frame_cam_right = vr;
                g_frame_cam_up = vu;
                g_frame_cam_basis_valid = true;
            }
        }
    }


    Vec3 transform_camera_position{};
    Vec4 transform_camera_rotation{};
    if (transform_camera_mode) {
        if (local_entity_index >= s_transforms.size() || !read_entity_pose(s_transforms[local_entity_index], transform_camera_position, transform_camera_rotation)) {
            g_player_position_validated = false;
            return result;
        }
        local = transform_camera_position; has_local_position = true;
    }

    // Our own team / clan, read once per frame and compared against every
    // player below. Without it nobody can be an ally.
    PlayerGroup local_group;
    bool local_group_valid = false;
    if (local_entity_index < s_transforms.size()) {
        read_player_group(s_transforms[local_entity_index], local_group);
        local_group_valid = local_group.any();
    }

    // Firing reference for the aimbot (local look direction + eye point).
    {
        uint64_t local_player = (local_entity_index < s_transforms.size()) ? s_transforms[local_entity_index] : 0;
        const PlayerAux* local_aux = nullptr;
        bool local_crouched = false;
        if (local_player && g_aim_bones_requested) {
            PlayerAux& la = player_aux(local_player);
            local_aux = &la;
            local_crouched = player_is_crouched(la);
        }
        read_local_aim_reference(local_player, local_aux, local_crouched);
    }

    // One box per player: of several objects carrying the same userID (the
    // copy left behind when he mounted a vehicle, or a respawn leftover) only
    // the one that moved most recently is drawn. The previous winner keeps the
    // spot on a tie, otherwise a parked car would make the box flip about.
    std::vector<char> suppressed(s_transforms.size(), 0);
    {
        std::unordered_map<std::string, size_t> chosen;
        for (size_t index = 0; index < s_transforms.size(); ++index) {
            if (!position_ok[index]) continue;
            auto tracked = g_player_track.find(s_transforms[index]);
            if (tracked == g_player_track.end()) continue;
            std::string uid(tracked->second.uid);
            if (uid.empty()) {
                // No userID on this object: the display name identifies the
                // account just as well, and it is already cached.
                auto text = g_player_text.find(s_transforms[index]);
                if (text != g_player_text.end() && text->second.has_name && text->second.name[0])
                    uid.assign(text->second.name, strnlen(text->second.name, sizeof(text->second.name)));
            }
            if (uid.empty()) continue;
            auto found = chosen.find(uid);
            if (found == chosen.end()) { chosen.emplace(uid, index); continue; }
            size_t rival = found->second;
            int mine = tracked->second.still_frames;
            int theirs = g_player_track[s_transforms[rival]].still_frames;
            bool mine_ride = player_mount_engaged(s_transforms[index]);
            bool their_ride = player_mount_engaged(s_transforms[rival]);
            bool take_mine;
            if (index == local_entity_index)      take_mine = true;
            else if (rival == local_entity_index) take_mine = false;
            else if (mine_ride != their_ride)     take_mine = mine_ride;
            else if (mine != theirs)              take_mine = mine < theirs;
            else {
                auto sticky = g_player_track_pick.find(uid);
                take_mine = sticky != g_player_track_pick.end() && sticky->second == s_transforms[index];
            }
            suppressed[take_mine ? rival : index] = 1;
            if (take_mine) found->second = index;
        }
        for (const auto& entry : chosen) g_player_track_pick[entry.first] = s_transforms[entry.second];
        // Ghost PlayerManager left at the boarding point: same frozen lastSaved
        // as a rider, even when userID on the leftover is empty.
        for (size_t index = 0; index < s_transforms.size(); ++index) {
            if (!position_ok[index] || suppressed[index]) continue;
            auto lit = g_mount_latch.find(s_transforms[index]);
            if (lit == g_mount_latch.end() || !lit->second.engaged || !lit->second.have_saved) continue;
            const Vec3& mount = lit->second.prev_saved;
            for (size_t other = 0; other < s_transforms.size(); ++other) {
                if (other == index || !position_ok[other] || suppressed[other]) continue;
                if (other == local_entity_index) continue;
                if (player_mount_engaged(s_transforms[other])) continue;
                if (vec3_horiz2(positions[other], mount) < 1.8F * 1.8F)
                    suppressed[other] = 1;
            }
        }
    }

    // Счётчик игроков для «пилюли» обнуляется здесь, а не в начале функции: если
    // кадр не собрался, счёт остаётся с прошлого кадра — ровно как и боксы,
    // которые на нём держатся. Иначе пилюля показывала бы ноль рядом с
    // нарисованными рамками.
    g_frame_player_count = 0;
    for (size_t i = 0; i < s_transforms.size(); ++i) {
        if (i == local_entity_index) continue;
        if (!s_transforms[i]) continue;
        if (suppressed[i]) continue;
        if (!position_ok[i]) continue;
        Vec3 feet = positions[i];

        float distance = -1.0F;
        if (has_local_position) {
            float dx = feet.x - local.x, dy = feet.y - local.y, dz = feet.z - local.z;
            distance = sqrtf(dx * dx + dy * dy + dz * dz);
            if (!std::isfinite(distance) || distance < MIN_PLAYER_DISTANCE || distance > MAX_PLAYER_DISTANCE) continue;
        }

        // Counted before any screen-space checks: the pill counter must see
        // players behind us too (360 degrees), not only the ones on screen.
        ++g_frame_player_count;

        // Crouch-aware body height from the character controller.
        PlayerAux& aux = player_aux(s_transforms[i]);
        const bool crouched = player_is_crouched(aux);
        float body_height = crouched ? aux.crouch_height : aux.normal_height;
        if (!(body_height > 0.6F && body_height < 2.6F)) body_height = PLAYER_HEIGHT;

        Vec3 body_bottom = {feet.x, feet.y, feet.z};
        Vec3 body_top = {feet.x, feet.y + body_height, feet.z};
        if (transform_camera_mode || !g_use_direct_player_position) { body_bottom.y = feet.y - 1.60F; body_top.y = feet.y + 0.20F; }

        Vec2 sf{}, sh2{};
        bool bottom_visible = transform_camera_mode
            ? w2s_transform_camera(transform_camera_position, transform_camera_rotation, body_bottom, sw, sh, sf, false)
            : w2s(vp, body_bottom, sw, sh, sf, false);
        if (!bottom_visible) continue;
        bool top_visible = transform_camera_mode
            ? w2s_transform_camera(transform_camera_position, transform_camera_rotation, body_top, sw, sh, sh2, false)
            : w2s(vp, body_top, sw, sh, sh2, false);
        if (!top_visible) continue;

        float height = fabsf(sh2.y - sf.y);
        if (!std::isfinite(height) || height < 2.0F) continue;
        float cx = (sf.x + sh2.x) * 0.5F;
        float cy = (sf.y + sh2.y) * 0.5F;
        float half_w = height * PLAYER_BOX_WIDTH_RATIO * 0.5F;
        float half_h = height * 0.5F;

        constexpr float box_half_width = 0.35F, box_half_depth = 0.35F;
        const Vec3 world_corners[8] = {
            {feet.x - box_half_width, body_bottom.y, feet.z - box_half_depth},
            {feet.x + box_half_width, body_bottom.y, feet.z - box_half_depth},
            {feet.x + box_half_width, body_bottom.y, feet.z + box_half_depth},
            {feet.x - box_half_width, body_bottom.y, feet.z + box_half_depth},
            {feet.x - box_half_width, body_top.y, feet.z - box_half_depth},
            {feet.x + box_half_width, body_top.y, feet.z - box_half_depth},
            {feet.x + box_half_width, body_top.y, feet.z + box_half_depth},
            {feet.x - box_half_width, body_top.y, feet.z + box_half_depth}
        };
        EspBox box{};
        // Lead for this player, used by every set_aim_point call below.
        {
            auto vt = g_player_track.find(s_transforms[i]);
            if (vt != g_player_track.end()) {
                g_aim_lead.x = vt->second.vel.x * kAimLeadSeconds;
                g_aim_lead.y = vt->second.vel.y * kAimLeadSeconds;
                g_aim_lead.z = vt->second.vel.z * kAimLeadSeconds;
            } else {
                g_aim_lead = {};
            }
        }
        box.id = s_transforms[i];
        box.crouched = crouched;
        box.aim_source = 0;
        box.x1 = cx - half_w; box.y1 = cy - half_h;
        box.x2 = cx + half_w; box.y2 = cy + half_h;
        box.distance = distance;
        // Display strings (cached, update-on-success so labels never flicker).
        {
            PlayerTextCache& tc = g_player_text[s_transforms[i]];
            // Пока значение не прочиталось, пробуем часто (3 кадра): сразу после
            // смерти/респавна первое чтение обычно не проходит, а с прежними 30
            // кадрами ник и оружие появлялись только через полсекунды-секунду —
            // это и выглядело как «ники мигают». Прочитанное обновляем редко.
            if (++tc.revalidate >= (tc.has_name ? 30 : 3)) {
                tc.revalidate = 0;
                char tmp[32] = {};
                if (player_display_name(s_transforms[i], tmp, sizeof(tmp))) {
                    memcpy(tc.name, tmp, sizeof(tc.name));
                    tc.has_name = true;
                }
                char weapon_tmp[48] = {};
                bool weapon_definite = false;
                if (player_weapon_name(s_transforms[i], weapon_tmp, sizeof(weapon_tmp), weapon_definite)) {
                    memcpy(tc.weapon, weapon_tmp, sizeof(tc.weapon));
                    tc.has_weapon = true;
                } else if (weapon_definite) {
                    // The synced weapon slot says the hands are empty — drop the
                    // stale label instead of showing the previous weapon forever.
                    tc.weapon[0] = '\0';
                    tc.has_weapon = false;
                }
                PlayerGroup group;
                read_player_group(s_transforms[i], group);
                tc.ally = local_group_valid && groups_are_allied(local_group, group);
                snprintf(tc.tag, sizeof(tc.tag), "%s", group.tag);
                tc.has_tag = tc.tag[0] != '\0';
            }
            box.has_name = tc.has_name;
            if (tc.has_name) memcpy(box.name, tc.name, sizeof(box.name));
            box.has_weapon = tc.has_weapon;
            if (tc.has_weapon) memcpy(box.weapon, tc.weapon, sizeof(box.weapon));
            box.ally = tc.ally;
            box.has_tag = tc.has_tag;
            if (tc.has_tag) memcpy(box.tag, tc.tag, sizeof(box.tag));
        }
        for (size_t corner = 0; corner < 8; ++corner) {
            Vec2 sc{};
            bool projected = transform_camera_mode
                ? w2s_transform_camera(transform_camera_position, transform_camera_rotation, world_corners[corner], sw, sh, sc, false)
                : w2s(vp, world_corners[corner], sw, sh, sc, false);
            box.corner_visible[corner] = projected && sc.x >= 0.0F && sc.x <= sw && sc.y >= 0.0F && sc.y <= sh;
            box.corners[corner][0] = projected ? sc.x : -1.0F;
            box.corners[corner][1] = projected ? sc.y : -1.0F;
        }
        if (want_bones && !transform_camera_mode) {
            fill_skeleton_box(s_transforms[i], vp, sw, sh, box);
            // Dummy rig stays at the boarding point while the box rides the
            // vehicle — drawing both is the two-position flicker.
            auto sk = g_skeletons.find(s_transforms[i]);
            if (sk != g_skeletons.end() && box.has_skeleton &&
                sk->second.bone_world_age[BONE_HIPS] >= 1) {
                float hx = sk->second.bone_world[BONE_HIPS].x - feet.x;
                float hz = sk->second.bone_world[BONE_HIPS].z - feet.z;
                if (hx * hx + hz * hz > 3.0F * 3.0F) {
                    box.has_skeleton = false;
                    for (int b = 0; b < ESP_BONE_COUNT; ++b) box.bone_valid[b] = false;
                }
            }
        }

        // Head slot: prefer the centre of the server-side Head hit volume over
        // the rig-derived estimate. This is the exact volume the shot is tested
        // against, so it removes the constant model/hitbox offset that makes
        // long-range head shots land low. Only accepted when it lies within
        // the body (plausibility vs feet) so a stale transform cannot hijack it.
        if (g_aim_bones_requested && !transform_camera_mode) {
            Vec3 hb{};
            if (player_head_hitbox_world(aux, hb)) {
                float dy = hb.y - feet.y;
                float dx = hb.x - feet.x, dz = hb.z - feet.z;
                if (dy > 0.5F && dy < 2.4F && (dx * dx + dz * dz) < 1.0F) {
                    bool agree = true;
                    if (box.aim_valid[0]) {
                        // Compare against the rig head point in screen space:
                        // reject if wildly different (different body).
                        Vec2 hs{};
                        if (w2s(vp, hb, sw, sh, hs, false)) {
                            float ex = hs.x - box.aim_pts[0][0], ey = hs.y - box.aim_pts[0][1];
                            float bh = fabsf(box.y2 - box.y1);
                            agree = (ex * ex + ey * ey) < (bh * 0.25F) * (bh * 0.25F) + 4.0F;
                        }
                    }
                    if (agree && set_aim_point(box, 0, hb, vp, sw, sh) && box.aim_source == 0) box.aim_source = 1;
                }
            }
        }

        // Aim fallbacks when rig bones are not (yet) available, so the aimbot
        // always has a crouch-aware target instead of a fixed-height guess.
        if (g_aim_bones_requested && !transform_camera_mode &&
            !(box.aim_valid[0] && box.aim_valid[1] && box.aim_valid[2])) {
            Vec3 head{};
            bool have_head = false;
            // (2) game-maintained head transform (moves with crouch/animation)
            if (player_head_world(aux, head)) {
                float dy = head.y - feet.y;
                float hx = head.x - feet.x, hz = head.z - feet.z;
                have_head = dy > 0.4F && dy < 2.4F && (hx * hx + hz * hz) < 1.5F * 1.5F;
            }
            const float n_down = 0.12F;                    // head -> neck
            const float c_down = crouched ? 0.26F : 0.34F; // head -> chest
            if (have_head) {
                if (!box.aim_valid[0]) { Vec3 t = head; t.y += 0.03F; if (set_aim_point(box, 0, t, vp, sw, sh) && box.aim_source == 0) box.aim_source = 2; }
                if (!box.aim_valid[1]) { Vec3 t = head; t.y -= n_down; if (set_aim_point(box, 1, t, vp, sw, sh) && box.aim_source == 0) box.aim_source = 2; }
                if (!box.aim_valid[2]) { Vec3 t = head; t.y -= c_down; if (set_aim_point(box, 2, t, vp, sw, sh) && box.aim_source == 0) box.aim_source = 2; }
            }
            // (3) feet + pose height estimate
            if (!box.aim_valid[0]) { Vec3 t = feet; t.y += body_height - 0.12F; if (set_aim_point(box, 0, t, vp, sw, sh) && box.aim_source == 0) box.aim_source = 3; }
            if (!box.aim_valid[1]) { Vec3 t = feet; t.y += body_height - 0.26F; if (set_aim_point(box, 1, t, vp, sw, sh) && box.aim_source == 0) box.aim_source = 3; }
            if (!box.aim_valid[2]) { Vec3 t = feet; t.y += body_height * 0.72F; if (set_aim_point(box, 2, t, vp, sw, sh) && box.aim_source == 0) box.aim_source = 3; }
        }
        result.push_back(box);
    }

    return result;
}

int esp_nearby_player_count() { return g_frame_player_count; }

bool esp_wants_reattach() { return g_want_reattach.exchange(false); }
