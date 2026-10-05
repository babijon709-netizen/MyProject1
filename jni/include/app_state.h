#pragma once
// Состояние приложения: AppState (всё, что видит меню), ввод, экран,
// флаги главного цикла. Определения — в main.cpp, сюда — только типы и extern.

#include <atomic>
#include "imgui.h"
#include "viewmodel.h"

struct InputState {
    bool touchConsumed = false;
};
extern InputState g_input;

struct AppState {
    struct SliderAnim { float pos = -1.f; float vel = 0.f; };
    struct RadioAnim  { float scale = 1.f, scaleVel = 0.f, ring = 0.f, ringVel = 0.f; };

    int   cur_tab = 1;   // при запуске открыта вкладка «Аим»
    bool  aim_touch = false, aim_pos = false, aim_special = false, aim_scope_only = false;
    int   aim_bone = 1;   // 0 голова, 1 шея, 2 тело (как слоты аима)
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
    bool  esp_bots = false, esp_vehicles = false;
    bool  esp_scrap = false;              // бочки и дорожные знаки
    // Ящики, встроенные в постройки («Большой/Обычный ящик»): отдельный
    // тумблер, независимый от «Ящиков» (мировых контейнеров). По умолчанию
    // вкл — как было до разделения.
    bool  esp_house_boxes = true;
    bool  esp_turrets = false, esp_backpacks = false, esp_sleepers = false;
    ViewmodelSettings viewmodel;
    bool fast_fire = false, fast_reload = false, fast_projectiles = false;
    float a_custom_fov = 0.f, a_custom_hands = 0.f, a_no_recoil = 0.f, a_no_spread = 0.f,
          a_fast_loot = 0.f, a_fast_fire = 0.f, a_fast_reload = 0.f, a_fast_projectiles = 0.f;
    SliderAnim sl_custom_fov, sl_hands_x, sl_hands_y, sl_hands_z;
    bool  always_day = false;     // всегда день
    float marker_dist = 150.f;
    float esp_thick = 1.5f;
    float gun_str = 5.f, gun_fov = 80.f, gun_trigger_delay = 0.0f;
    // Упреждение аима: на сколько миллисекунд забегать вперёд по скорости
    // цели (0 = вести ровно по кости, как сейчас). 50 мс ≈ прежняя
    // константа 0.05 с. Живёт в конфиге (расширение v4).
    float aim_lead_ms = 0.f;
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
    // Фрикам: камера отделяется от тела и летает сквозь стены — обзор базы
    // перед рейдом. Скорость в м/с; пока включён, аим молчит (см. AimBegin).
    bool  freecam_on = false;
    float freecam_speed = 12.f;
    float freecam_joy_x = -1.f, freecam_joy_y = -1.f;
    // Размер джойстика фрикама: процент от базовых 130 px (база — короткая
    // сторона 1080), 60..200, 100 = прежний размер. Как и позиции органов
    // управления, живёт в .layout — привязан к устройству, а не к профилю.
    float freecam_joy_size = 100.f;
    // Круглая зона управления обзором: центр в долях экрана, радиус — доля
    // короткой стороны. -1 у центра = дефолт справа по центру.
    float freecam_look_x = -1.f, freecam_look_y = -1.f;
    float freecam_look_radius = 0.28f;
    // Экранная кнопка фрикама (квадрат с глазом): доли экрана = её ЦЕНТР,
    // -1 = не задана (дефолт — справа по центру). Ставится тапом в
    // «Разное → Фрикам → Настроить кнопку», живёт до перезапуска.
    float freecam_btn_x = -1.f, freecam_btn_y = -1.f;
    // Видимость экранной кнопки фрикама (тумблер «Кнопка на экране»).
    bool  freecam_btn_show = true;
    // Счётчик игроков (пилюля «Противники»): доли экрана = её ЦЕНТР, -1 = не
    // задан (дефолт — верх по центру). Задаётся тапом в Опциях → Худ, живёт
    // только до перезапуска — как и остальные калибровки.
    float hud_cnt_x = -1.f, hud_cnt_y = -1.f;
    // ui_fps выключен навсегда (счётчик убран), рамки карточек — всегда вкл.
    bool  ui_fps = false, ui_dark_mode = true, ui_show_sep = true;
    // Акцент интерфейса (палитра в «Опциях»): 0 = фиолет по умолчанию,
    // 1..7 = пресеты, 8 = свой цвет из ui_accent_rgb. Хранится в .accent
    // рядом с конфигами, как язык: стиль GUI глобален, а не часть профиля.
    int   ui_accent_idx = 0;
    ImVec4 ui_accent_rgb = {0.545f, 0.486f, 1.000f, 1.0f};
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
    // Растяг экрана (Aspect Ratio): множитель к полю aspect камеры ИГРЫ —
    // рендер самой игры тянется по горизонтали, модели врагов становятся
    // шире (круги/прицелы — овалами, это нормальный вид фичи). 1.0 = выкл.
    // Сессионное: бинарный CfgBlob заморожен версией (config.cpp).
    float screen_stretch_x = 1.0f;
    float a_esp_box = 0, a_esp_name = 0, a_esp_wall = 0, a_esp_chams = 0;
    float a_esp_weapon = 0, a_esp_tracer = 0, a_esp_skeleton = 0;
    float a_esp_ore = 0, a_esp_animal = 0, a_esp_loot = 0, a_esp_team = 0, a_esp_pickup = 0;
    float a_esp_bots = 0, a_esp_vehicles = 0, a_esp_scrap = 0, a_esp_house_boxes = 0;
    float a_esp_turrets = 0, a_esp_backpacks = 0, a_esp_sleepers = 0;
    float a_always_day = 0;
    float a_ui_dark = 1;
    float a_farm_on = 0, a_farm_wood = 1, a_farm_stone = 0, a_farm_metal = 0, a_farm_sulfur = 0;
    float a_xray_on = 0;
    float a_freecam_on = 0;
    float a_freecam_btn_show = 0;
    float a_fps_90 = 0, a_fps_120 = 0, a_fps_144 = 0, a_fps_rst = 0;

    SliderAnim sl_gun_str, sl_gun_fov, sl_esp_thick, sl_gun_trig, sl_marker_dist, sl_farm_range, sl_xray, sl_freecam, sl_freecam_look_radius, sl_freecam_joy_size, sl_aim_lead, sl_screen_stretch;
};
extern AppState g_state;
extern std::atomic<bool> main_thread_flag;   // false = выход из main()
extern std::atomic<bool> g_frame_done;       // кадр дорисован (ожидание на выходе)
extern float g_sw;                           // ширина экрана (px), abs(min/max)
extern float g_sh;

constexpr int kTabCount = 6;
