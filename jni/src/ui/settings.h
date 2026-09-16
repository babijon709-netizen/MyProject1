#pragma once
// ui/settings.h — Настройки ESP и аима (cfg::esp, cfg::aim, ui::bar).
//
// Всё, что модуль отдаёт наружу: сюда смотрят те модули, которым нужны эти
// типы, данные и функции. Реализация — в ui/settings.cpp.
#include "app/common.h"

// ---- Типы модуля ----

namespace ui { namespace bar {
    inline float g_game_alpha = 1.f;
    inline void  set_game_alpha(float a){ g_game_alpha=a; }
    inline float game_alpha(){ return g_game_alpha; }
}}

namespace cfg { namespace esp {
    inline ImVec4 box_col         = {0.20f, 0.85f, 0.35f, 1.f};
    inline ImVec4 box_col_invis   = {1.00f, 0.20f, 0.20f, 1.f};
    inline ImVec4 name_col        = {1.00f, 1.00f, 1.00f, 1.f};
    inline ImVec4 distance_col    = {0.70f, 0.70f, 0.70f, 1.f};
    inline ImVec4 weapon_col      = {1.00f, 0.95f, 0.10f, 1.f};
    inline ImVec4 tracer_col      = {1.00f, 0.20f, 0.20f, 1.f};
    inline ImVec4 skeleton_col    = {0.20f, 0.85f, 0.35f, 1.f};
    inline ImVec4 animal_col      = {1.00f, 0.60f, 0.25f, 1.f};
    inline ImVec4 loot_col        = {0.55f, 0.80f, 1.00f, 1.f};
    inline ImVec4 ally_col        = {0.25f, 0.55f, 1.00f, 1.f};
    inline ImVec4 pickup_col      = {0.60f, 1.00f, 0.60f, 1.f};
    // Tracers drawn to a team mate are always green, no matter what colour the
    // enemy tracers use — that is the whole point of telling them apart.
    inline ImVec4 ally_tracer_col = {0.20f, 0.90f, 0.35f, 1.f};

    inline bool box          = false;
    inline bool name_esp     = false;
    inline bool distance     = false;
    inline bool weapon       = false;
    inline bool tracer       = false;
    inline bool skeleton     = false;
    inline bool ore          = false;
    inline bool animal       = false;
    inline bool loot         = false;
    inline bool team         = false;
    inline bool pickup       = false;
    inline bool  vis_check        = false;
    inline bool  fill             = false;
    inline float stroke           = 2.f;
    inline float rounding         = 0.f;
    inline float fill_pct         = 50.f;
    inline int   box_type         = 0;
    inline float box_rounding     = 0.f;
    inline bool  hp_outline       = true;
    inline bool  hp_gradient      = false;
    inline float tracer_thickness = 1.5f;
    inline ImVec4 hp_min_col      = {1.00f, 0.20f, 0.10f, 1.f};
    inline ImVec4 hp_max_col      = {0.20f, 0.85f, 0.35f, 1.f};
}}

namespace cfg { namespace aim {
    inline bool  enabled           = false;
    inline bool  vis_check         = false;
    inline bool  draw_fov          = false;
    inline bool  scope_only        = false;   // aim only while ADS (прицел)
    inline float fov               = 80.f;
    inline float smoothness        = 5.f;
    inline int   bone              = 0;
    inline bool  trigger_bot       = false;
    inline bool  knife_bot         = false;
    inline float trigger_delay     = 0.0f;
    inline float fov_color[4]      = {1.0f, 1.0f, 1.0f, 1.0f};
}}
