#pragma once
// farm_scan.h — Автофарм: скан узлов реестра.
//
// Всё, что модуль отдаёт наружу: сюда смотрят те модули, которым нужны эти
// типы, данные и функции. Реализация — в farm_scan.cpp.
#include "esp/common.h"

// ---- Типы модуля ----

// ---- Auto-farm state ---------------------------------------------------------
// The farm has its own entity cache (it wants trees, which the ore markers
// deliberately skip) and its own rescan cadence. Positions and loot are read
// the same way the markers read them; the walking/hitting itself is done with
// synthetic touches in main.cpp.
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

enum { FARM_EXT_NONE = 0, FARM_EXT_ORE = 1, FARM_EXT_TREE = 2 };

// ---- Данные, которые видят другие модули ----

extern std::vector<FarmEntity> g_farm_entities;

extern std::unordered_map<uint64_t, int> g_farm_blacklist;

extern int g_farm_tool_have;

extern int g_farm_tool_need;

extern int g_farm_idle_reason;

extern float g_farm_max_distance;

// ---- Функции, которые видят другие модули ----

void farm_scan_reset();

void farm_scan_tick();

bool farm_spot_on_node(const Vec3& spot, const Vec3& node, int kind);

void farm_resolve_extension(FarmEntity& entity);
