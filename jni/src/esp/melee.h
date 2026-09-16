#pragma once
// melee.h — Ближний бой: дальность удара и hitRadius.
//
// Всё, что модуль отдаёт наружу: сюда смотрят те модули, которым нужны эти
// типы, данные и функции. Реализация — в melee.cpp.
#include "esp/common.h"

// ---- Типы модуля ----

// ---- Дальность удара ближним орудием: FPMelee.m_MaxReach + hitRadius --------
// Сами числа сериализованы в префабе каждого инструмента, в дампе их нет: в
// конструкторе FPMelee стоят заглушки (m_MaxReach 0.5, hitRadius 0.1,
// m_TimeBetweenAttacks 0.85, m_DamagePerHit 15, m_ImpactForce 15). Поэтому
// читаем живой объект в руках.
//
// Как игру это использует (FPMelee.ZkX, дизасм билда 62a8534):
//   data = handler.RaycastData(0x160); если невалиден — handler.AimRaycast(0x168)
//   if (data.RaycastHit.distance < m_MaxReach + hitRadius) On_Hit(data)
//   else On_Woosh()
// distance — UnityEngine.RaycastHit.get_distance(), то есть 3D-метры от
// камеры/оси выстрела, а НЕ горизонтальное расстояние до узла.
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

// ---- Функции, которые видят другие модули ----

bool read_local_melee_reach(MeleeReach& out);
