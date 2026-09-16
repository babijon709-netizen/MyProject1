// ui/layout.cpp — Layout, AppState, ввод и анимации.
//
// Модуль разрезан из прежнего монолита jni/src/main.cpp;
// что здесь лежит и кто это зовёт — в шапке ui/layout.h и в docs/CODE_MAP.md.


#include "app/common.h"
#include "aim/update.h"
#include "farm/controller.h"
#include "ui/config.h"
#include "ui/esp_overlay.h"
#include "ui/popover.h"
#include "ui/sheet.h"
#include "ui/tabs.h"
#include "ui/toast.h"
#include "ui/watermark.h"
#include "ui/widgets.h"
#include "ui/window.h"
#include "ui/layout.h"

// Калибровка зон вводом с экрана (меню прячется, тап записывает точку):
// 0 — нет, 1 — зона джойстика автофарма, 2 — зона огня автофарма,
// 3 — точка, из которой аимбот водит палец. См. RenderMenu.
extern int  g_calibMode;

static std::mt19937& GetRNG() {
    static std::mt19937 rng(std::random_device{}());
    return rng;
}

float Lerpf(float a, float b, float t)   { return a + (b - a) * t; }

float Clamp01(float t)                    { return t < 0.f ? 0.f : t > 1.f ? 1.f : t; }

float EaseOut3(float t)                   { float i = 1 - t; return 1 - i * i * i; }

float EaseInOut(float t)                  { return t * t * (3 - 2 * t); }

bool WasTappedHere() {
    const auto& io = ImGui::GetIO();
    if (!io.MouseReleased[0]) return false;
    auto min = ImGui::GetItemRectMin();
    auto max = ImGui::GetItemRectMax();
    // Строки контента существуют и за пределами видимой области (скролл),
    // поэтому нажатие засчитывается только по видимой (не обрезанной клипом)
    // части строки. Без этого тап по нижней панели вкладок «проваливается»
    // в невидимую строку под ней и включает её функцию.
    {
        ImVec2 clMin = ImGui::GetWindowDrawList()->GetClipRectMin();
        ImVec2 clMax = ImGui::GetWindowDrawList()->GetClipRectMax();
        min.x = ImMax(min.x, clMin.x); min.y = ImMax(min.y, clMin.y);
        max.x = ImMin(max.x, clMax.x); max.y = ImMin(max.y, clMax.y);
        if (min.x >= max.x || min.y >= max.y) return false;
    }
    auto cp  = io.MouseClickedPos[0];
    auto mp  = io.MousePos;
    bool started = cp.x >= min.x && cp.x <= max.x && cp.y >= min.y && cp.y <= max.y;
    bool ended   = mp.x >= min.x - 12.f && mp.x <= max.x + 12.f && mp.y >= min.y - 12.f && mp.y <= max.y + 12.f;
    float dx = mp.x - cp.x, dy = mp.y - cp.y;
    return started && ended && (dx * dx + dy * dy) <= 30.f * 30.f;
}

// Тап по прямоугольнику без виджета ImGui. Нужен там, где гуи рисуется до окна
// меню (стартовый выбор версии игры): ImGui-виджет в этот момент не попадает
// ни в одно окно, ImGui кладёт его в своё служебное «Debug##Default» — и тап не
// срабатывает, и на экране появляется лишнее окно «Debug». Условия те же, что у
// WasTappedHere: палец опущен внутри, отпущен рядом, сдвиг не больше 30 px.
bool TapInRect(ImVec2 a, ImVec2 b) {
    const auto& io = ImGui::GetIO();
    if (!io.MouseReleased[0]) return false;
    const ImVec2 cp = io.MouseClickedPos[0];
    const ImVec2 mp = io.MousePos;
    const bool started = cp.x >= a.x && cp.x <= b.x && cp.y >= a.y && cp.y <= b.y;
    const bool ended   = mp.x >= a.x - 12.f && mp.x <= b.x + 12.f &&
                         mp.y >= a.y - 12.f && mp.y <= b.y + 12.f;
    const float tdx = mp.x - cp.x, tdy = mp.y - cp.y;
    return started && ended && (tdx * tdx + tdy * tdy) <= 30.f * 30.f;
}

// Точка внутри текущего клип-прямоугольника окна? Ручные обработчики кликов
// (карточки конфигов, цветные точки и т.п.) обязаны проверять это, иначе тап
// по нижней панели вкладок «проваливается» в прокрученную за неё строку.
bool PtInClip(ImVec2 p) {
    auto* dl = ImGui::GetWindowDrawList();
    ImVec2 mn = dl->GetClipRectMin(), mx = dl->GetClipRectMax();
    return p.x >= mn.x && p.x <= mx.x && p.y >= mn.y && p.y <= mx.y;
}

void SpringTick(float& pos, float& vel, float target, float dt) {
    static constexpr float kStiffness = 180.f;
    static constexpr float kDamping   = 26.f;
    static constexpr float kPosTol    = 0.3f;
    static constexpr float kVelTol    = 1.f;
    float force = kStiffness * (target - pos) - kDamping * vel;
    vel += force * dt;
    pos += vel * dt;
    if (fabsf(target - pos) < kPosTol && fabsf(vel) < kVelTol) { pos = target; vel = 0.f; }
}

void Tick(float& a, bool v, float dt, float spd) {
    if (dt > 0.032f) dt = 0.032f;
    a += (float(v) - a) * spd * dt;
    if (!v && a < .002f) a = 0.f;
    if (v && a > .998f) a = 1.f;
}

InputState g_input;

AppState g_state;

// Точка, из которой аимбот водит палец (доли экрана). Пока в меню не задана —
// прежняя позиция: справа, по вертикали по центру.
static constexpr float kAimTouchDefX = 0.74f;

static constexpr float kAimTouchDefY = 0.50f;

float AimTouchFracX() {
    return (g_state.aim_tx > 0.f && g_state.aim_tx < 1.f) ? g_state.aim_tx : kAimTouchDefX;
}

float AimTouchFracY() {
    return (g_state.aim_ty > 0.f && g_state.aim_ty < 1.f) ? g_state.aim_ty : kAimTouchDefY;
}

// Калибровка зон вводом с экрана: 0 — нет, 1 — ждём тап по джойстику
// автофарма, 2 — по кнопке огня, 3 — по точке, из которой аимбот водит палец.
// Пока калибровка активна, меню скрыто и первый тап по экрану пишет позицию
// (в долях экрана) в g_state.
int  g_calibMode = 0;
