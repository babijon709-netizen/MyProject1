#pragma once
// Состояние приложения: AppState (всё, что видит меню), ввод, экран,
// флаги главного цикла. Определения — в main.cpp, сюда — только типы и extern.

#include <atomic>
#include "imgui.h"

struct InputState {
    bool touchConsumed = false;
};
extern InputState g_input;

struct AppState {
    struct SliderAnim { float pos = -1.f; float vel = 0.f; };
    struct RadioAnim  { float scale = 1.f, scaleVel = 0.f, ring = 0.f, ringVel = 0.f; };

    int   cur_tab = 1;   // при запуске открыта вкладка «Аим»
    bool  aim_touch = false, aim_pos = false, aim_special = false, aim_scope_only = false;
    int   aim_bone = 0;
    // Чем именно аим крутит прицел: 0 тач (синтетический палец), 1 память
    // (запись ввода взгляда в игру), 2 сайлент (запись оси выстрела).
    // Значения — enum AimMode в aim.h.
    int   aim_mode = 0;
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
    float a_aim_mode0 = 1, a_aim_mode1 = 0, a_aim_mode2 = 0;
    RadioAnim ra_aim_mode0, ra_aim_mode1, ra_aim_mode2;
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
extern AppState g_state;
extern std::atomic<bool> main_thread_flag;   // false = выход из main()
extern std::atomic<bool> g_frame_done;       // кадр дорисован (ожидание на выходе)
extern float g_sw;                           // ширина экрана (px), abs(min/max)
extern float g_sh;

constexpr int kTabCount = 6;
