// mem.cpp — Доступ к памяти игры и состояние привязки.
//
// Модуль разрезан из прежнего монолита jni/src/game.cpp;
// что здесь лежит и кто это зовёт — в шапке mem.h и в docs/CODE_MAP.md.

#include "esp/common.h"
#include "esp/aim_points.h"
#include "esp/boxes.h"
#include "esp/camera.h"
#include "esp/farm_scan.h"
#include "esp/farm_target.h"
#include "esp/frame.h"
#include "esp/game_patch.h"
#include "esp/il2cpp.h"
#include "esp/managed.h"
#include "esp/marker_labels.h"
#include "esp/markers.h"
#include "esp/melee.h"
#include "esp/names.h"
#include "esp/player_pose.h"
#include "esp/skeleton_build.h"
#include "esp/skeleton_names.h"
#include "esp/transform.h"
#include "esp/weapons.h"
#include "mem.h"

// ---- Доступ к памяти игры ---------------------------------------------------
// Подробности — в mem_io.h: только /proc/<pid>/mem (pread/pwrite), проверка
// доступа пробой по ELF-заголовку и кэш блоков на кадр. Здесь только обёртки,
// которыми пользуется весь остальной код чтения.
memio::Reader g_mem;

bool rd_buf(uint64_t addr, void* out, size_t size) {
    return g_mem.read(addr, out, size);
}

// Указатели из памяти игры приходят с меткой в старшем байте (TBI/MTE на
// Android 11+): читать по ним нельзя — ядро вернёт EIO, — и сравнивать их с
// адресами без метки тоже нельзя. Снимаем метку сразу, на входе.
uint64_t rd_ptr(uint64_t a) { return memio::untag(rd<uint64_t>(a)); }

Vec3     rd_v3 (uint64_t a) { return rd<Vec3>(a);     }

Mat4     rd_m4 (uint64_t a) { return rd<Mat4>(a);     }

bool wr_buf(uint64_t addr, const void* in, size_t size) {
    return g_mem.write(addr, in, size);
}

pid_t     g_pid         = -1;

uint64_t  g_il2cpp_base = 0;

uint64_t  g_player_manager_class = 0;

uint64_t  g_player_manager_static_fields = 0;

uint64_t  g_game_controller_class = 0;

uint64_t  g_local_player = 0;

bool      g_matrix_configuration_validated = false;

bool      g_camera_matrix_physical_match = false;

uint64_t  g_player_position_offset = PLAYER_POSITION;

// Pipeline status for the on-screen debug line:
//   R  = ragdoll build stage (0 ok; 2 no KCC, 3 no anim, 4 anim backref,
//        5 no ragdoll, 6 no array, 7 bad count, 8/11 few bones, 9 layout,
//        10 arrays, 12 pelvis, 13 chest, 14 final; -1 never ran)
//   H  = hips candidates (name path), N = best name-path bone count
//   P  = build path used (1 ragdoll, 2 names), B = cached bones
//   F  = fill failure (0 ok, 1 cooldown, 2 build, 3 root, 4 hips gone,
//        7 projected<4)
//   C  = players with a valid cached skeleton
//   D  = distinct transform hierarchies used by the bones (re-parenting),
//        then the per-bone mask torso|armL|armR|legL|legR.
// Почему привязка не удалась — main.cpp показывает это тостом, иначе на
// «неудобных» устройствах чит молча ничего не делал.
EspAttachState g_attach_state = ESP_ATTACH_OK;

EspAttachState esp_attach_state() { return g_attach_state; }

bool esp_init(pid_t pid) {
    esp_reset();
    g_pid = pid;
    g_mem.clear_last_error();

    uint64_t candidates[8] = {};
    const int candidate_count = get_base_candidates("libil2cpp.so", candidates, 8);
    if (candidate_count == 0) {
        g_attach_state = ESP_ATTACH_NO_LIB;
        g_pid = -1;
        // Без базового адреса читать нечего.
        return false;
    }

    // База выбирается не «первая по имени в карте», а та, на которой РЕАЛЬНО
    // резолвится класс игры. После перезапуска игры в карте остаётся ещё и
    // старый образ libil2cpp.so (обычно «(deleted)»): он читается, ELF-заголовок
    // на месте, поэтому прежняя проверка доступа его принимала — а метаданные
    // внутри мертвы, и весь чит молча ничего не находил до перезапуска чита.
    uint64_t chosen = 0;
    for (int i = 0; i < candidate_count; ++i) {
        if (!g_mem.bind(pid, candidates[i])) continue;
        if (base_resolves_game(candidates[i])) { chosen = candidates[i]; break; }
        g_mem.unbind();
    }
    if (!chosen) {
        // Ни одна база не подтвердилась (игра ещё грузится?): работаем на первой
        // пригодной, как раньше, — привязка не хуже прежней.
        for (int i = 0; i < candidate_count; ++i) {
            if (g_mem.bind(pid, candidates[i])) { chosen = candidates[i]; break; }
        }
    }
    if (!chosen) {
        // /proc/<pid>/mem не открылся или не читается: доступа к памяти нет.
        g_attach_state = ESP_ATTACH_NO_ACCESS;
        g_il2cpp_base = 0;
        g_pid = -1;
        return false;
    }

    g_il2cpp_base = chosen;
    g_attach_state = ESP_ATTACH_OK;
    return true;
}

// Начало кадра: кэш блоков памяти сбрасывается, чтобы кадр читал свежее
// состояние игры, но внутри кадра повторные обращения к тем же полям не стоили
// syscall'а (см. mem_io.h).
void esp_mem_frame_begin() { g_mem.frame_begin(); }

// Привязка ещё жива? Дешёвая проверка (одно чтение): процесс мог перезапуститься
// с тем же pid, а доступ — отобрали. Без неё чит оставался «привязанным» и молча
// ничего не делал до перезапуска приложения.
bool esp_alive_check() { return g_mem.verify(); }

bool esp_rebind_memory() { return g_mem.rebind_now(); }
