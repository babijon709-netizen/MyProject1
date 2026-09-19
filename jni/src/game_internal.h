#pragma once
// Внутренний срез слоя памяти игры: общее между game.cpp (ядро — привязка,
// камера, игроки, скелеты, кадр) и game_markers.cpp / game_farm.cpp (сканы
// маркеров и автофарма). Публичный API — в game.h; сюда — только то, чем
// эти три файла делят память и состояние.

#include "game.h"
#include "mem_io.h"
#include "Vector.h"
#include <string>
#include <unordered_map>
#include <vector>

extern memio::Reader g_mem;
extern Mat4 g_frame_vp;
extern Vec3 g_frame_local_pos;

// ---- Обёртки чтения/записи памяти игры -----------------------------------
// Подробности доступа — в mem_io.h (только /proc/<pid>/mem + кэш блоков).
inline bool rd_buf(uint64_t addr, void* out, size_t size) {
    return g_mem.read(addr, out, size);
}
template<typename T>
inline T rd(uint64_t addr) {
    T v{};
    g_mem.read(addr, &v, sizeof(T));
    return v;
}
template<typename T>
inline bool rd_exact(uint64_t addr, T& value) {
    value = {};
    if (!addr) return false;
    return g_mem.read(addr, &value, sizeof(T));
}
// То же чтение, но в обход кэша блоков — прямиком из памяти игры.
// Кэш живёт до конца кадра (frame_begin), и это правильно для ESP: за кадр
// поза игрока не меняется. Но фрикам сам пишет позицию камеры две тысячи раз
// в секунду, а поправку считает по прочитанной позиции: считав её из кэша,
// контур видел состояние ДО своей же предыдущей поправки и добавлял её ещё
// раз. Замкнутый контур с запаздыванием разгонялся — это и была жалоба 19.09
// «фрикам летит в одну сторону сам».
template<typename T>
inline bool rd_fresh(uint64_t addr, T& value) {
    value = {};
    if (!addr) return false;
    return g_mem.read_quiet(addr, &value, sizeof(T));
}
// Указатели из памяти игры приходят с меткой в старшем байте (TBI/MTE на
// Android 11+): читать по ним нельзя — ядро вернёт EIO, — и сравнивать их с
// адресами без метки тоже нельзя. Снимаем метку сразу, на входе.
inline uint64_t rd_ptr(uint64_t a) { return memio::untag(rd<uint64_t>(a)); }
inline Vec3     rd_v3 (uint64_t a) { return rd<Vec3>(a);     }
inline Mat4     rd_m4 (uint64_t a) { return rd<Mat4>(a);     }
inline bool wr_buf(uint64_t addr, const void* in, size_t size) {
    return g_mem.write(addr, in, size);
}

struct FarmEntity {
    uint64_t identity = 0;      // NetworkIdentity (stable id for the blacklist)
    uint64_t component = 0;     // the Mineable* component (fraction reads)
    uint64_t transform = 0;     // native Transform (position)
    Vec3     pos{};
    bool     pos_valid = false;
    int      kind = 0;          // 0 wood, 1 stone, 2 metal, 3 sulfur
    // Cached fractionRemaining: reading it live for EVERY node on EVERY
    // frame was a syscall storm (hundreds of memory reads per frame
    // with a full cache). The value only matters for de-prioritising
    // mined-out nodes, so a second of staleness changes nothing.
    float    fraction = -1.0F;
    int      frac_age = 0;
    // Кеш JE-экстеншена, который хранит крестик узла (OreHitstreaks для руды,
    // TreeHitstreaks для деревьев). Ищется один раз на узел и переиспользуется:
    // каждый кадр читаются только сами координаты X.
    uint64_t ext = 0;
    int      ext_kind = 0;   // FARM_EXT_NONE / _ORE / _TREE
    int      ext_age = 0;    // кадров до повторного поиска
    // Каким орудием узел добывается (MineableObject.m_RequiredToolPurpose,
    // флаги ToolPurpose). Значение из префаба и за жизнь узла не меняется,
    // поэтому читается один раз при скане. 0 = неизвестно/без требования.
    int      required_purpose = 0;
};

enum MarkerClass : uint8_t {
    MARKER_CLASS_NONE = 0, MARKER_CLASS_MINEABLE = 1,
    MARKER_CLASS_LOOT = 2, MARKER_CLASS_PICKUP = 3,
    MARKER_CLASS_BARREL = 4,
    // Имя класса прочитать не удалось (см. class_identity): семейство неизвестно,
    // и объект опознаётся дальше по имени GameObject — оно лежит в куче и читается.
    MARKER_CLASS_UNKNOWN = 5,
};

// Инструмент/статус, общий с автофармой (определено в game_markers.cpp).
extern int g_farm_tool_have;
extern int g_farm_tool_need;
extern int g_farm_idle_reason;

// Состояние реестра/скана, общее с автофармой (определено в game_markers.cpp).
extern std::vector<FarmEntity> g_farm_entities;
extern std::unordered_map<uint64_t, int> g_farm_blacklist;
extern double g_farm_next_scan;

// Резолверы реестра маркеров (определены в game_markers.cpp).
int read_managed_collection(uint64_t object, uint64_t* out, int max_items);
uint8_t marker_class_of(uint64_t klass);
uint64_t resolve_network_client_spawned();
uint64_t resolve_network_identity_class();
bool marker_world_position(uint64_t transform, Vec3& out);
// Камера/прицел (определены в game.cpp).
extern Vec3 g_cam_pos;
extern Vec3 g_cam_right, g_cam_up, g_cam_forward;
extern Vec3 g_aim_ref_forward, g_aim_ref_right, g_aim_ref_up;
extern Vec3 g_frame_cam_fwd, g_frame_cam_right, g_frame_cam_up;
extern Vec3 g_frame_cam_pos;
extern Vec3 g_aim_ref_origin;
void collect_transform_subtree(uint64_t root, std::vector<uint64_t>& nodes, size_t max_nodes);


// ---- Общие структуры -------------------------------------------------------
struct TransformHierarchyLayout {
    uint64_t data_offset = 0x38;
    uint64_t index_offset = 0x40;
    uint64_t matrices_offset = 0x18;
    uint64_t indices_offset = 0x20;
    bool matrices_indirect = false;
    bool indices_indirect = false;
};

struct MeleeReach {
    bool  valid = false;
    float max_reach = 0.0F;   // FPMelee.m_MaxReach  (0x128)
    float hit_radius = 0.0F;  // FPMelee.hitRadius   (0x12C)
    float total = 0.0F;       // порог засчёта удара, 3D-метры от глаза
    float ray_length = 0.0F;  // RaycastManager.m_RayLength (0x38)
    char  tool[24] = {};      // имя класса орудия
    // Ритм ударов этого орудия: FPMelee.m_TimeBetweenAttacks (0x130) и
    // pauseAfterAttack (0x134). Всё, что чаще первого, игра ставит в очередь
    // и съедает, так что такт бота берётся отсюда, а не из миллисекунд «на глаз».
    float time_between_attacks = 0.0F;
    float pause_after_attack = 0.0F;
    // Что орудие умеет (FPTool.m_ToolPurposes, флаги ToolPurpose). Читается
    // только у FPTool/FPChainsaw — у прочих FPMelee на 0x160 свои поля.
    int   tool_purposes = 0;
    bool  purposes_valid = false;
    // Что прямо сейчас видит прицел. Игра сама кастует лучи (RaycastManager) и
    // кладёт результат в активности PlayerEventHandler (Gum); FPMelee.ZkX
    // берёт distance именно оттуда. По нему видно, не перекрыт ли узел: луч
    // упёрся ближе, чем наша точка прицела, — значит удар уйдёт в перекрытие.
    bool  ray_valid = false;      // в активностях есть GKo
    bool  ray_hit_object = false; // у попадания есть GameObject
    float ray_distance = 0.0F;    // м от камеры вдоль прицела (0 = неизвестно)
    // Куда именно упёрся луч игры (m_Point) и нормаль поверхности там
    // (m_Normal). Нужны, чтобы в логе автофарма видеть разницу между нашей
    // точкой прицела и реальным попаданием луча: по ней эмпирически меряется
    // сдвиг декали крестика от коры (0.25 м по дампу) и проверяется, что
    // прицел стоит на мешевом коллайдере, а не в воздухе рядом с ним.
    bool  ray_point_valid = false;
    Vec3  ray_point{}, ray_normal{};
    // В ЧЁМ именно остановился луч: RaycastHit.m_Collider (managed Collider) и
    // GameObject попадания (GKo.m_HitObject, тот же, из которого выше
    // ray_hit_object). По ним отличаем «луч упёрся в сам узел добычи» от «узел
    // перекрыт чужой геометрией» — см. ray_hit_is_self_node.
    uint64_t ray_collider = 0;
    uint64_t ray_hit_go = 0;
};

// Состояние ядра, общее со сканами (определено в game.cpp).
extern TransformHierarchyLayout g_skeleton_layout;

// ---- Состояние ядра (определено в game.cpp), которым пользуются сканы ----
extern pid_t     g_pid;
extern uint64_t  g_il2cpp_base;
extern uint64_t  g_game_controller_class;
extern uint64_t  g_local_player;
extern bool      g_cam_pose_valid;
extern bool      g_aim_ref_valid;
extern unsigned  g_farm_mask;
extern bool      g_frame_vp_valid;
extern float     g_frame_sw, g_frame_sh;
extern bool      g_frame_local_valid;
extern bool      g_frame_cam_basis_valid;
extern bool      g_skeleton_layout_valid;
extern bool      g_go_name_offset_valid;
extern float     g_last_overlay_sw, g_last_overlay_sh;

// ---- Функции ядра, которыми пользуются сканы ------------------------------
bool valid_obj(uint64_t p);
bool vec3_is_finite(const Vec3& value);
bool read_managed_string(uint64_t str_obj, char* out, size_t cap);
bool position_looks_like_world_space(const Vec3& position);
bool read_transform_hierarchy_position(uint64_t native_transform, Vec3& position);
bool ensure_gameobject_name_offset(uint64_t player);
bool managed_component_gameobject_name(uint64_t managed_component, char* out, size_t cap);
uint64_t managed_object_native(uint64_t managed);
double mono_seconds();
bool w2s(const Mat4& vp, const Vec3& world, float sw, float sh, Vec2& out, bool clip_to_screen = true);
std::string read_remote_string(uint64_t address, bool* readable = nullptr);
bool read_managed_string_ex(uint64_t str_obj, char* out, size_t cap, int32_t max_chars);
bool read_transform_hierarchy_layout(uint64_t native_transform, const TransformHierarchyLayout& layout,
                                     Vec3& position, Vec4* world_rotation = nullptr);
uint64_t native_component_transform(uint64_t native_component);
int class_identity(uint64_t klass, const char* expected_name, const char* expected_ns);
bool class_looks_alive(uint64_t klass);
uint64_t get_class_static_fields(uint64_t klass);
bool discover_layout_from_native_transforms(const std::vector<uint64_t>& native_transforms,
                                            size_t& best_position_count, size_t& candidate_count);
bool publish_camera_only_frame(float sw, float sh);
bool object_class_name_is(uint64_t obj, const char* expected, bool accept_when_unreadable = false);
bool read_transform_name(uint64_t transform, char* out, size_t cap);
bool read_local_melee_reach(MeleeReach& out);
bool aim_angles_for(const Vec3& world, const Vec2& screen, float sw, float sh, float& yaw_deg, float& pitch_deg);
bool farm_cam_source_ok(const Vec3& p);

// ---- Подсистема маркеров (game_markers.cpp) --------------------------------
// Сброс кэшей маркеров при esp_reset()/сбросе мира.
void reset_marker_caches();
// Сброс скана автофарма (определены в game_farm.cpp) — зовётся при сбросе мира.
void farm_scan_abort();
void farm_scan_reset();
