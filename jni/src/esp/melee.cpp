// melee.cpp — Ближний бой: дальность удара и hitRadius.
//
// Модуль разрезан из прежнего монолита jni/src/game.cpp;
// что здесь лежит и кто это зовёт — в шапке melee.h и в docs/CODE_MAP.md.

#include "esp/common.h"
#include "esp/farm_target.h"
#include "esp/il2cpp.h"
#include "esp/managed.h"
#include "esp/math.h"
#include "esp/mem.h"
#include "esp/names.h"
#include "esp/transform.h"
#include "melee.h"

// Имена классов ближнего орудия читаемые и между билдами не ротируют
// (dump.cs: Oxide.FPMelee -> Oxide.FPTool -> Oxide.FPChainsaw, плюс FPSpear,
// FPBuildingHammer, FPTorch). Проверка обязательна: у прочих FPObject на 0x128
// свои поля (у FPCrossbow/FPSnowball там m_MaxDistance — сотни метров).
static const char* const kMeleeFamily[] = {
    "FPTool", "FPChainsaw", "FPMelee", "FPSpear", "FPBuildingHammer", "FPTorch"};

static uint64_t s_melee_klass = 0;

static bool     s_melee_klass_ok = false;

static char     s_melee_klass_name[24] = {};

bool read_local_melee_reach(MeleeReach& out) {
    out = MeleeReach{};
    if (g_pid <= 0 || !g_il2cpp_base) return false;
    const uint64_t local = resolve_local_player();
    if (!local) return false;
    const uint64_t fp_manager = rd_ptr(local + PLAYER_FP_MANAGER);
    if (!valid_obj(fp_manager)) return false;
    uint64_t weapon = rd_ptr(fp_manager + FPMANAGER_CURRENT_WEAPON);
    if (!valid_obj(weapon) || rd_ptr(weapon + FPOBJECT_PLAYER_BACKREF) != local) {
        weapon = rd_ptr(fp_manager + FPMANAGER_CURRENT_OBJECT);
        if (!valid_obj(weapon) || rd_ptr(weapon + FPOBJECT_PLAYER_BACKREF) != local)
            return false;
    }
    const uint64_t klass = rd_ptr(weapon);
    if (!valid_obj(klass)) return false;
    if (klass != s_melee_klass) {  // имя класса читаем один раз на предмет в руках
        s_melee_klass = klass;
        s_melee_klass_ok = false;
        s_melee_klass_name[0] = '\0';
        const std::string name = read_remote_string(rd_ptr(klass + IL2CPP_CLASS_NAME));
        if (!name.empty() && name.size() < sizeof(s_melee_klass_name)) {
            for (const char* family : kMeleeFamily) {
                if (name == family) { s_melee_klass_ok = true; break; }
            }
            memcpy(s_melee_klass_name, name.c_str(), name.size() + 1);
        }
    }
    if (!s_melee_klass_ok) return false;

    float reach = 0.0F, radius = 0.0F;
    if (!rd_exact(weapon + FPMELEE_MAX_REACH, reach) ||
        !rd_exact(weapon + FPMELEE_HIT_RADIUS, radius)) return false;
    if (!std::isfinite(reach) || !std::isfinite(radius)) return false;
    // Правдоподобие: настоящие орудия — единицы метров. Вне диапазона значит
    // либо класс совпал случайно, либо чтение разъехалось после апдейта игры.
    if (reach < 0.2F || reach > 20.0F || radius < 0.0F || radius > 5.0F) return false;

    out.valid = true;
    out.max_reach = reach;
    out.hit_radius = radius;
    out.total = reach + radius;
    strncpy(out.tool, s_melee_klass_name, sizeof(out.tool) - 1);

    // Длина луча, которым игра ищет точку взаимодействия (префаб
    // RaycastManager, default 1.5). m_AimRayLength (0x3C) и радиус сферы (0x40)
    // при доставании орудия перезаписываются его же m_MaxReach/hitRadius
    // (FPMelee.On_Draw -> RaycastManager.ZIj), поэтому интересен именно 0x38.
    const uint64_t rm = rd_ptr(weapon + FPOBJECT_RAYCAST_MANAGER);
    if (valid_obj(rm) && rd_ptr(rm + RAYCASTMAN_PLAYER) == local) {
        float ray = 0.0F;
        if (rd_exact(rm + RAYCASTMAN_RAY_LENGTH, ray) && std::isfinite(ray) &&
            ray > 0.0F && ray < 500.0F) out.ray_length = ray;
    }

    // Ритм ударов. В конструкторе FPMelee стоят заглушки (0.85/0.15), настоящие
    // значения сериализованы в префабе каждого инструмента, поэтому читаем их
    // так же, как дальность, — из живого орудия, с проверкой правдоподобия.
    float tba = 0.0F, pause = 0.0F;
    if (rd_exact(weapon + FPMELEE_TIME_BETWEEN_ATTACKS, tba) &&
        std::isfinite(tba) && tba > 0.05F && tba < 10.0F)
        out.time_between_attacks = tba;
    if (rd_exact(weapon + FPMELEE_PAUSE_AFTER_ATTACK, pause) &&
        std::isfinite(pause) && pause >= 0.0F && pause < 10.0F)
        out.pause_after_attack = pause;

    // Умения орудия (какой ресурс оно вообще может добывать).
    if (!strcmp(s_melee_klass_name, "FPTool") || !strcmp(s_melee_klass_name, "FPChainsaw")) {
        int32_t purposes = 0;
        if (rd_exact(weapon + FPTOOL_TOOL_PURPOSES, purposes)) {
            const int32_t known = (int32_t)ToolPurpose::CutWood |
                                  (int32_t)ToolPurpose::BreakRocks |
                                  (int32_t)ToolPurpose::CutAnimals;
            if (purposes != 0 && (purposes & ~known) == 0) {
                out.tool_purposes = (int)purposes;
                out.purposes_valid = true;
            }
        }
    }

    // Луч прицела. Порядок ровно как в FPMelee.ZkX (0x6533a20): сначала
    // RaycastData (0x160), при null — AimRaycast (0x168); значение лежит в
    // обёртке GuI`1<GKo> на +0x20, а «валидность» там — просто data != null.
    {
        const uint64_t handler = rd_ptr(weapon + FPOBJECT_EVENT_HANDLER);
        if (valid_obj(handler)) {
            uint64_t data = 0;
            const uint64_t primary = rd_ptr(handler + GUM_RAYCAST_DATA);
            if (valid_obj(primary)) data = rd_ptr(primary + GUI_VALUE);
            if (!valid_obj(data)) {
                const uint64_t fallback = rd_ptr(handler + GUM_AIM_RAYCAST);
                if (valid_obj(fallback)) data = rd_ptr(fallback + GUI_VALUE);
            }
            if (valid_obj(data)) {
                out.ray_valid = true;
                // GameObject и Collider попадания держим указателями: по ним
                // проверяется, не в сам ли узел упёрся луч (у камня точка
                // прицела внутри породы, и без этого свой камень выглядит
                // стеной — бот вечно обходил его, не сделав ни удара).
                const uint64_t hit_go = rd_ptr(data + GKO_HIT_OBJECT);
                out.ray_hit_go = valid_obj(hit_go) ? hit_go : 0;
                out.ray_hit_object = (out.ray_hit_go != 0);
                const uint64_t hit_col = rd_ptr(data + GKO_RAYCAST_HIT + RAYCASTHIT_COLLIDER);
                out.ray_collider = valid_obj(hit_col) ? hit_col : 0;
                float dist = 0.0F;
                if (rd_exact(data + GKO_RAYCAST_HIT + RAYCASTHIT_DISTANCE, dist) &&
                    std::isfinite(dist) && dist > 0.0F && dist < 1000.0F)
                    out.ray_distance = dist;
                Vec3 rp{}, rn{};
                if (rd_exact(data + GKO_RAYCAST_HIT + RAYCASTHIT_POINT, rp) &&
                    vec3_is_finite(rp) && position_looks_like_world_space(rp)) {
                    out.ray_point = rp;
                    out.ray_point_valid = true;
                    if (rd_exact(data + GKO_RAYCAST_HIT + RAYCASTHIT_NORMAL, rn) &&
                        vec3_is_finite(rn))
                        out.ray_normal = rn;
                }
            }
        }
    }
    return true;
}
