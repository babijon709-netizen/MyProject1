#pragma once
// names.h — Имена игроков: что считать настоящим ником.
//
// Всё, что модуль отдаёт наружу: сюда смотрят те модули, которым нужны эти
// типы, данные и функции. Реализация — в names.cpp.
#include "esp/common.h"

// ---- Типы модуля ----

// Visible nickname, best source first. Sources are ranked so a real,
// human-readable name always wins over a machine-generated id ("F7GDJ6472D");
// such an id is only used as a last resort when no real name is available.
// ---- Team / clan membership -------------------------------------------------
// PlayerManager syncs teamName / clanId / clanTag to every client, so two
// players can be compared directly. Team names are per-session and clan ids are
// stable, and either one matching makes the two players allies.
struct PlayerGroup {
    char team[40] = {};
    char clan[48] = {};
    char tag[16]  = {};
    bool any() const { return team[0] || clan[0] || tag[0]; }
};

// Cached display strings per player (updated on success only, so a transient
// failed read never makes labels flicker). Refreshed periodically to pick up
// nickname/weapon changes.
struct PlayerTextCache {
    char name[32] = {};
    bool has_name = false;
    char weapon[48] = {}; // localized UTF-8, matches EspBox::weapon
    bool has_weapon = false;
    bool ally = false;    // shares the local player's team or clan
    char tag[16] = {};    // clan tag
    bool has_tag = false;
    int  revalidate = 30; // first sighting resolves immediately
};

// ---- Данные, которые видят другие модули ----

extern std::unordered_map<uint64_t, PlayerTextCache> g_player_text;

// ---- Функции, которые видят другие модули ----

bool valid_obj(uint64_t p);

void read_player_group(uint64_t player, PlayerGroup& out);

bool groups_are_allied(const PlayerGroup& local, const PlayerGroup& other);

bool player_display_name(uint64_t player, char* out, size_t cap);

bool read_item_data_display_name(uint64_t item_data, char* out, size_t cap);

bool fp_object_display_name(uint64_t weapon, uint64_t player, bool strict, char* out, size_t cap);

void prune_player_text(const std::vector<uint64_t>& players);
