#pragma once
// ui/layout.h — Layout, AppState, ввод и анимации.
//
// Всё, что модуль отдаёт наружу: сюда смотрят те модули, которым нужны эти
// типы, данные и функции. Реализация — в ui/layout.cpp.
#include "app/common.h"

// ---- Данные модуля ----

extern int g_calibMode;

// ---- Типы модуля ----

namespace Layout {
    static constexpr float RowH      = 78.f;
    static constexpr float SliderH   = 108.f;
    static constexpr float HeaderH   = 66.f;
    static constexpr float Inset     = 16.f;
    static constexpr float PadX      = 20.f;
    static constexpr float BtnH      = 62.f;
    // Нижняя панель вкладок: строка по центру, иконка + подпись.
    // Кнопки крупные: панель выше и ячейки шире (~2x от первоначальных).
    static constexpr float BottomH   = 150.f;
    static constexpr float TabW      = 136.f;
    // Левая панель вкладок: вертикальный столбец по центру.
    static constexpr float RailW     = 128.f;
    static constexpr float TabHV     = 108.f;
}

struct AppState {
    struct SliderAnim { float pos = -1.f; float vel = 0.f; };
    struct RadioAnim  { float scale = 1.f, scaleVel = 0.f, ring = 0.f, ringVel = 0.f; };

    int   cur_tab = 1;   // при запуске открыта вкладка «Аим»
    bool  aim_touch = false, aim_pos = false, aim_special = false, aim_scope_only = false;
    int   aim_bone = 0;
    // Точка, из которой аимбот водит палец (доли экрана). Выбирается тапом по
    // экрану, как зоны автофарма; -1 = не задана (прежняя позиция 74%/50%).
    float aim_tx = -1.f, aim_ty = -1.f;
    // 0 = balanced (crosshair + range), 1 = nearest to the crosshair,
    // 2 = nearest in the world.
    int   aim_priority = 0;
    bool  esp_box = false, esp_name = false, esp_wall = false, esp_chams = false;
    bool  esp_weapon = false, esp_tracer = false, esp_skeleton = false;
    bool  esp_ore = false, esp_animal = false, esp_loot = false, esp_team = false;
    bool  esp_pickup = false;
    bool  always_day = false;     // всегда день
    float marker_dist = 150.f;
    float esp_thick = 1.5f;
    float gun_str = 5.f, gun_fov = 80.f, gun_trigger_delay = 0.0f;
    // Автофарм: главный выключатель + какие ресурсы добывать.
    bool  farm_on = false;
    bool  farm_wood = true, farm_stone = false, farm_metal = false, farm_sulfur = false;
    // Калибровка зон бота (доли экрана 0..1; -1 = не задано, берём дефолт).
    // Джойстик движения и кнопка огня/атаки — у всех раскладки разные.
    float farm_joy_x = -1.f, farm_joy_y = -1.f;
    float farm_fire_x = -1.f, farm_fire_y = -1.f;
    // Дальность поиска ресурсов, метры.
    float farm_range = 100.f;
    // Иксрей: визуально срезает мир вокруг игрока (0 = выкл), метры.
    bool  xray_on = false;
    float xray_range = 5.f;
    // ui_fps выключен навсегда (счётчик убран), рамки карточек — всегда вкл.
    bool  ui_fps = false, ui_dark_mode = true, ui_show_sep = true;
    // Положение панели вкладок: true = слева (по умолчанию), false = снизу.
    bool  ui_panel_left = true;

    float tab_alpha = 1.f, tab_slide = 0.f, tab_slide_vel = 0.f;
    float a_aim_touch = 0, a_aim_pos = 0, a_aim_spec = 0, a_aim_scope = 0;
    float a_aim_head  = 1, a_aim_chest = 0, a_aim_pelvis = 0;
    RadioAnim ra_aim_head, ra_aim_chest, ra_aim_pelvis;
    float a_aim_pr0 = 1, a_aim_pr1 = 0, a_aim_pr2 = 0;
    RadioAnim ra_aim_pr0, ra_aim_pr1, ra_aim_pr2;
    float a_esp_box = 0, a_esp_name = 0, a_esp_wall = 0, a_esp_chams = 0;
    float a_esp_weapon = 0, a_esp_tracer = 0, a_esp_skeleton = 0;
    float a_esp_ore = 0, a_esp_animal = 0, a_esp_loot = 0, a_esp_team = 0, a_esp_pickup = 0;
    float a_always_day = 0;
    float a_ui_dark = 1;
    float a_farm_on = 0, a_farm_wood = 1, a_farm_stone = 0, a_farm_metal = 0, a_farm_sulfur = 0;
    float a_xray_on = 0;

    SliderAnim sl_gun_str, sl_gun_fov, sl_esp_thick, sl_gun_trig, sl_marker_dist, sl_farm_range, sl_xray;
};

struct InputState {
    bool touchConsumed = false;
};

// ---- Константы и данные, ссылающиеся на типы модуля ----

extern InputState g_input;

extern AppState g_state;

// ---- Функции, которые видят другие модули ----

float Lerpf(float a, float b, float t);

float Clamp01(float t);

float EaseOut3(float t);

float EaseInOut(float t);

bool WasTappedHere();

bool TapInRect(ImVec2 a, ImVec2 b);

bool PtInClip(ImVec2 p);

void SpringTick(float& pos, float& vel, float target, float dt);

void Tick(float& a, bool v, float dt, float spd = 10.f);

float AimTouchFracX();

float AimTouchFracY();
