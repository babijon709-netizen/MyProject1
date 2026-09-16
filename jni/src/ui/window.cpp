// ui/window.cpp — RenderMenu: окно меню целиком.
//
// Модуль разрезан из прежнего монолита jni/src/main.cpp;
// что здесь лежит и кто это зовёт — в шапке ui/window.h и в docs/CODE_MAP.md.


#include "app/common.h"
#include "app/attach.h"
#include "app/media.h"
#include "app/screen.h"
#include "ui/config.h"
#include "ui/layout.h"
#include "ui/popover.h"
#include "ui/scroll.h"
#include "ui/sheet.h"
#include "ui/tabs.h"
#include "ui/theme.h"
#include "ui/toast.h"
#include "ui/watermark.h"
#include "ui/window.h"

// Названия вкладок — одни и те же в шапке контента и в обеих панелях.
// Русские оригиналы лежат здесь (они же ключи таблицы переводов), а язык
// подставляется на каждом обращении: подпись вкладки нужна и для отрисовки, и
// для расчёта ширины панели, а язык может смениться на ходу — массив со
// строками, собранный один раз при запуске, остался бы на языке запуска.
static const char* const kTabTitlesRu[kTabCount] = {
    XS_RU("Меню"), XS_RU("Аим"), XS_RU("ESP"), XS_RU("Разное"), XS_RU("Конфиги"), XS_RU("Опции")
};

static const char* TabTitle(int k) {
    if (k < 0 || k >= kTabCount) return "";
    return lang::text(kTabTitlesRu[k]);
}

// Ячейка вкладки в левой панели: иконка слева и крупная подпись справа.
// Иконка 60 px — в 1.5 раза больше прежних 40. Подпись — обычным начертанием
// (шрифт в сборке один, полужирного нет и подделывать его мы не будем) в два
// раза крупнее базового кегля панели. Ширина панели считается так, чтобы
// влезли и крупная иконка, и такая подпись (см. RailTabsW).
static const float kRailIcon = 60.f;    // сторона иконки вкладки

static const float kRailFS   = 1.55f;   // кегль подписи от базового (0.88 от кегля меню)

static const float kRailPadL = 14.f;    // отступ иконки от края панели

static const float kRailPadR = 10.f;    // отступ текста от края панели

static const float kRailGap  = 12.f;    // зазор между иконкой и подписью

// Сколько места нужно ячейкам левой панели: отступ + иконка + зазор + самая
// длинная подпись (вкладка 0 «Меню» не показывается, она не в счёт) + отступ.
static float RailTabsW() {
    const float fs = ImGui::GetFontSize() * 0.88f * kRailFS;
    float worst = 0.f;
    for (int k = 1; k < kTabCount; ++k)
        worst = ImMax(worst, ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0, TabTitle(k)).x);
    return kRailPadL + kRailIcon + kRailGap + worst + kRailPadR;
}

// Кружок-аватарка и «светофор» в левом верхнем углу меню.
//
// Кружок (радиус 87.75 — диаметр 175.5) живёт в панели вкладок, и панель
// считается от него: отступ + диаметр + отступ. Над кружком, в самом углу
// окна, стоят три кнопки как в macOS (красная — закрыть меню, жёлтая —
// свернуть в минимальный размер, зелёная — развернуть на весь экран и обратно).
static const float kAvatarR    = 87.75f; // радиус кружка (диаметр 175.5)

static const float kAvatarPad  = 20.f;   // отступ по бокам внутри панели

static const float kAvatarPadT = 12.f;   // от верхнего края окна

// «Светофор»: три кружка по 20 px с зазором 6.7 — та же пропорция, что в
// macOS (12 px при системном шрифте 13), в 1.5 раза меньше прежних 30/10.
static const float kTLD   = 20.f;        // диаметр кружка светофора

static const float kTLGap = 6.7f;        // зазор между кружками

static const float kTLPad = 12.f;        // отступ светофора от краёв окна

static const int   kTLN   = 3;           // красный, жёлтый, зелёный

// Ширина светофора целиком.
static float TrafficLightW(float d, float gap) { return d * kTLN + gap * (kTLN - 1); }

// Размер кружков светофора под ширину панели: в узком окне он ужимается вместе
// с панелью, чтобы не вылезти на содержимое вкладок.
static void TrafficLightMetrics(float railW, float& d, float& gap) {
    d = kTLD; gap = kTLGap;
    const float need = TrafficLightW(d, gap);
    const float have = railW - kAvatarPad * 2.f;
    if (need > 0.f && have > 0.f && need > have) {
        const float k = have / need;
        d *= k; gap *= k;
    }
}

// Ширина панели вкладок: по кружку (при радиусе 58.5 — 157 px), но не меньше,
// чем нужно светофору. На узком экране (меню сжимается под ширину дисплея)
// панель не отдаём больше 45% окна: тогда кружок и светофор уменьшаются.
static float AvatarRailW(float winW) {
    const float forAvatar = (kAvatarR + kAvatarPad) * 2.f;   // отступ + диаметр + отступ
    const float forLights = TrafficLightW(kTLD, kTLGap) + kAvatarPad * 2.f;
    const float forTabs   = RailTabsW();                     // иконка + крупная подпись
    return ImMin(ImMax(ImMax(forAvatar, forLights), forTabs), ImMax(120.f, winW * 0.45f));
}

// Радиус кружка под фактическую ширину панели.
static float AvatarR(float railW) {
    return ImMin(kAvatarR, ImMax(24.f, railW * 0.5f - kAvatarPad));
}

// Центр кружка в координатах окна меню: по центру панели вкладок, то есть ровно
// над иконками вкладок.
static float AvatarCx(float winX, float R) { return winX + kAvatarPad + R; }

// Кружок стоит под светофором.
static float AvatarCy(float winY, float R) {
    return winY + kTLPad + kTLD + kAvatarPadT + R;
}

// Низ кружка в координатах окна меню — от него начинается столбец вкладок.
static float AvatarBottom(float winY, float R) {
    return winY + kTLPad + kTLD + kAvatarPadT + R * 2.f;
}

// --- Светофор ===============================================================
// Кружки: 0 — красная, 1 — жёлтая, 2 — зелёная. Это ДЕКОР (просьба игрока:
// «он как декоративный дизайн»): тапов и действий у кружков нет, поэтому нет и
// InvisibleButton'ов — окно за них таскается как за обычную шапку.
enum TrafficLightId { TL_CLOSE = 0, TL_MIN, TL_ZOOM };

// Прямоугольник i-й кнопки (квадрат со стороной d + 8 — палец толще кружка).
static void TrafficLightRect(ImVec2 winPos, float d, float gap, int i, ImVec2& mn, ImVec2& mx) {
    const float pitch = d + gap;
    const float cx = winPos.x + kTLPad + i * pitch + d * 0.5f;
    const float cy = winPos.y + kTLPad + d * 0.5f;
    const float h  = d * 0.5f + 4.f;
    mn = {cx - h, cy - h};
    mx = {cx + h, cy + h};
}

// Цвета macOS: закрыть, свернуть, развернуть.
static ImU32 TrafficLightColor(int i) {
    switch (i) {
        case TL_CLOSE: return IM_COL32(255,  95,  87, 255);
        case TL_MIN:   return IM_COL32(254, 188,  46, 255);
        default:       return IM_COL32( 40, 200,  64, 255);
    }
}

// Полоса шапки контента. В раскладке с нижней панелью она раньше была втрое
// выше — под кружок аватарки и светофор; кружка там больше нет, и полоса снова
// обычной высоты (только под заголовок вкладки).
static float MenuHeaderH(bool) { return Layout::HeaderH; }

// Где стоит светофор: в САМОМ левом верхнем углу окна, в обеих раскладках.
// (В раскладке с нижней панелью он стоял по центру полосы шапки — рядом с
// кружком аватарки. Кружка там больше нет, и светофор переехал в угол.)
static ImVec2 TrafficLightPos(ImVec2 winPos, bool) { return winPos; }

// Нарисовать светофор. Тапы не обрабатываются: это декор, действия закрыть /
// свернуть / развернуть убраны вместе с ним (меню открывается и закрывается
// тапом по пилюле «Противники» сверху, окно таскается за шапку и растягивается
// за правый нижний угол).
static void TrafficLight(ImVec2 winPos, float railW) {
    float d = 0.f, gap = 0.f;
    TrafficLightMetrics(railW, d, gap);
    ImDrawList* fg = ImGui::GetForegroundDrawList();

    for (int i = 0; i < kTLN; ++i) {
        ImVec2 mn, mx;
        TrafficLightRect(winPos, d, gap, i, mn, mx);
        const ImVec2 c{(mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f};
        // Как в macOS: цветной кружок с чуть заметной тёмной кромкой.
        fg->AddCircleFilled(c, d * 0.5f, TrafficLightColor(i), 32);
        fg->AddCircle(c, d * 0.5f, IM_COL32(0, 0, 0, 45), 32, 1.5f);
    }
}

void RenderMenu() {
    auto& io = ImGui::GetIO();
    float dt = io.DeltaTime;
    if (dt > 0.1f) dt = 0.1f;

    if (g_menu_orient != displayInfo.orientation || g_menu_dw != (int)displayInfo.width || g_menu_dh != (int)displayInfo.height) {
        g_menu_orient = displayInfo.orientation;
        g_menu_dw = (int)displayInfo.width;
        g_menu_dh = (int)displayInfo.height;
        CenterMenuOnDisplay();
    }

    g_menuFadeIn += dt * 1.2f;
    if (g_menuFadeIn > 1.f) g_menuFadeIn = 1.f;

    CfgWatchTick();
    Tick(g_themeT, g_darkTheme, dt, 6.f);
    Tick(g_state.a_aim_touch,  g_state.aim_touch,          dt);
    Tick(g_state.a_aim_pos,    g_state.aim_pos,            dt);
    Tick(g_state.a_aim_head,   g_state.aim_bone == 0,      dt);
    Tick(g_state.a_aim_chest,  g_state.aim_bone == 1,      dt);
    Tick(g_state.a_aim_pelvis, g_state.aim_bone == 2,      dt);
    Tick(g_state.a_aim_spec,   g_state.aim_special,        dt);
    Tick(g_state.a_aim_scope,  g_state.aim_scope_only,     dt);
    Tick(g_state.a_esp_box,    g_state.esp_box,            dt);
    Tick(g_state.a_esp_name,   g_state.esp_name,           dt);
    Tick(g_state.a_esp_wall,   g_state.esp_wall,           dt);
    Tick(g_state.a_esp_chams,  g_state.esp_chams,          dt);
    Tick(g_state.a_esp_weapon, g_state.esp_weapon,         dt);
    Tick(g_state.a_esp_ore,    g_state.esp_ore,            dt);
    Tick(g_state.a_esp_animal, g_state.esp_animal,         dt);
    Tick(g_state.a_esp_loot,   g_state.esp_loot,           dt);
    Tick(g_state.a_esp_pickup, g_state.esp_pickup,         dt);
    Tick(g_state.a_always_day,  g_state.always_day,         dt);
    Tick(g_state.a_esp_team,   g_state.esp_team,           dt);
    Tick(g_state.a_aim_pr0,    g_state.aim_priority == 0,  dt);
    Tick(g_state.a_aim_pr1,    g_state.aim_priority == 1,  dt);
    Tick(g_state.a_aim_pr2,    g_state.aim_priority == 2,  dt);
    Tick(g_state.a_esp_tracer, g_state.esp_tracer,         dt);
    Tick(g_state.a_esp_skeleton, g_state.esp_skeleton,     dt);
    Tick(g_state.a_ui_dark,    g_state.ui_dark_mode,       dt);
    Tick(g_state.a_farm_on,     g_state.farm_on,     dt);
    Tick(g_state.a_farm_wood,   g_state.farm_wood,   dt);
    Tick(g_state.a_farm_stone,  g_state.farm_stone,  dt);
    Tick(g_state.a_farm_metal,  g_state.farm_metal,  dt);
    Tick(g_state.a_farm_sulfur, g_state.farm_sulfur, dt);
    Tick(g_state.a_xray_on,     g_state.xray_on,     dt);
    ApplyTheme();

    if (g_cfgLoadedIdx >= 0 && g_cfgLoadedIdx < kMaxConfigs)
        g_cfgLoadAnim[g_cfgLoadedIdx] += (1.f - g_cfgLoadAnim[g_cfgLoadedIdx]) * 10.f * dt;

    bool anyOverlayOpen = g_sheet.visible || (g_pop.visible && !g_pop.closing);

    if (io.MouseClicked[0] && anyOverlayOpen) g_input.touchConsumed = true;

    bool anyOverlayVisible = g_sheet.visible || g_pop.visible;
    if (!io.MouseDown[0] && !anyOverlayVisible) g_input.touchConsumed = false;

    DrawWatermark(dt);
    DrawToast(dt);

    // ---- Выбор версии игры (окно при запуске) -------------------------------
    // Окно в том же виде, что и остальное гуи: та же карточка, тот же радиус и
    // те же кнопки, что в меню, — только с выбором версии. От версии зависят и
    // оффсеты, и пакет, к которому подключаться, поэтому пока выбор не сделан,
    // меню не рисуется, а аим и автофарм стоят (g_buildPrompt).
    if (g_buildPrompt) {
        float dw = 0.f, dh = 0.f;
        VisibleScreen(dw, dh);
        if (dw < 100.f || dh < 100.f) return;   // размер экрана ещё неизвестен

        auto* fg = ImGui::GetForegroundDrawList();
        auto* fn = ImGui::GetFont();
        const float fs = ImGui::GetFontSize();
        const bool betaOk = go::BetaAvailable();

        const float padX = Layout::PadX;
        const float padY = Layout::Inset;
        const float tfs  = fs * 1.35f;                 // заголовок окна
        const float btnH = Layout::BtnH;
        const float gap  = 12.f;
        const float wW   = ImMax(360.f, ImMin(dw * 0.82f, WW_MIN));
        const float bw   = (wW - padX * 2.f - gap) * 0.5f;
        const float wH   = padY + tfs + 20.f + btnH + padY;
        const float x0   = (dw - wW) * 0.5f;
        const float y0   = (dh - wH) * 0.5f;

        // Окно поверх игры — как окно меню. Экран не затемняем: выбор версии —
        // единственное, что здесь показано, и гасить игру он не должен.
        fg->AddRectFilled({x0, y0}, {x0 + wW, y0 + wH}, C::U(C::Bg()), R::Card);
        if (g_state.ui_show_sep)
            fg->AddRect({x0, y0}, {x0 + wW, y0 + wH}, C::U(C::Sep()), R::Card, 0, 1.2f);

        const char* title = XS("Версия игры");
        auto tsz = fn->CalcTextSizeA(tfs, FLT_MAX, 0, title);
        fg->AddText(fn, tfs, {x0 + (wW - tsz.x) * 0.5f, y0 + padY}, C::U(C::Txt()), title);

        // Две кнопки — по половине окна каждая, как строки меню.
        const float by = y0 + padY + tfs + 20.f;
        for (int i = 0; i < 2; ++i) {
            const bool beta = (i == 1);
            const bool enabled = (!beta || betaOk);
            const bool sel = (go::CurrentBuild() == (beta ? go::Build::Beta : go::Build::Release));
            const float bx = x0 + padX + i * (bw + gap);

            fg->AddRectFilled({bx, by}, {bx + bw, by + btnH}, C::U(C::Card()), R::Btn);
            if (sel)
                fg->AddRectFilled({bx, by}, {bx + bw, by + btnH},
                    C::UA(C::Acc(), g_darkTheme ? 0.18f : 0.10f), R::Btn);
            fg->AddRect({bx, by}, {bx + bw, by + btnH},
                        sel ? C::UA(C::Acc(), 0.9f) : C::U(C::Sep()), R::Btn, 0, sel ? 2.2f : 1.2f);

            const char* name = beta ? "Beta" : "Release";
            auto nsz = fn->CalcTextSizeA(fs * 1.15f, FLT_MAX, 0, name);
            fg->AddText(fn, fs * 1.15f,
                        {bx + (bw - nsz.x) * 0.5f, by + (btnH - fs * 1.15f) * 0.5f},
                        C::UA(enabled ? (sel ? C::Acc() : C::Txt()) : C::Dim(), enabled ? 1.f : 0.6f),
                        name);

            // Тап считаем вручную: окна меню в этот момент ещё нет, а ImGui-
            // виджет ушёл бы в служебное окно «Debug##Default».
            if (enabled && !g_input.touchConsumed &&
                TapInRect({bx, by}, {bx + bw, by + btnH})) {
                ApplyBuildChoice(beta ? go::Build::Beta : go::Build::Release);
                g_input.touchConsumed = true;
                g_buildPrompt = false;
            }
        }

        return;   // пока выбираем версию, меню не рисуем
    }

    // ---- Режим калибровки зон вводом с экрана ------------------------------
    // Меню спрятано; первый тап по экрану записывает позицию (в долях экрана):
    // зону джойстика/огня автофарма или точку, из которой аимбот водит палец.
    if (g_calibMode != 0) {
        float dw = 0.f, dh = 0.f;
        VisibleScreen(dw, dh);
        if (dw < 100.f || dh < 100.f) { g_calibMode = 0; return; }
        auto* fg = ImGui::GetForegroundDrawList();

        // Затемнение + рамка-акцент.
        fg->AddRectFilled({0, 0}, {dw, dh}, IM_COL32(0, 0, 0, 90));
        fg->AddRect({4.f, 4.f}, {dw - 4.f, dh - 4.f}, C::UA(C::Acc(), 0.9f), 10.f, 0, 3.f);

        // Подпись, что тапать.
        auto* fn = ImGui::GetFont();
        const char* title = (g_calibMode == 1)
            ? XS("Тапни по центру джойстика движения")
            : (g_calibMode == 2) ? XS("Тапни по кнопке огня / атаки")
                                 : XS("Тапни по точке, где аим водит палец");
        const char* sub = XS("Тап записывает зону. Меню откроется само.");
        float tfs = ImGui::GetFontSize() * 1.5f;
        auto tsz = fn->CalcTextSizeA(tfs, FLT_MAX, 0, title);
        auto ssz = fn->CalcTextSizeA(tfs * 0.62f, FLT_MAX, 0, sub);
        float ty = dh * 0.16f;
        // Плашка под текстом, чтобы читалось на любом фоне.
        float px0 = (dw - ImMax(tsz.x, ssz.x)) * 0.5f - 26.f;
        float px1 = (dw + ImMax(tsz.x, ssz.x)) * 0.5f + 26.f;
        fg->AddRectFilled({px0, ty - 18.f}, {px1, ty + tsz.y + 10.f + ssz.y + 18.f},
                          C::UA(C::Card(), 0.92f), 18.f);
        fg->AddText(fn, tfs, {(dw - tsz.x) * 0.5f, ty}, C::U(C::Txt()), title);
        fg->AddText(fn, tfs * 0.62f, {(dw - ssz.x) * 0.5f, ty + tsz.y + 10.f}, C::U(C::Dim()), sub);

        // Пульсирующий маркер текущей сохранённой точки (если есть).
        {
            float zx = -1.f, zy = -1.f;
            if (g_calibMode == 1 && g_state.farm_joy_x >= 0.f) { zx = g_state.farm_joy_x * dw; zy = g_state.farm_joy_y * dh; }
            if (g_calibMode == 2 && g_state.farm_fire_x >= 0.f) { zx = g_state.farm_fire_x * dw; zy = g_state.farm_fire_y * dh; }
            // У точки аима маркер виден всегда: пока она не задана, показываем
            // ту позицию, из которой аим водит палец по умолчанию.
            if (g_calibMode == 3) { zx = AimTouchFracX() * dw; zy = AimTouchFracY() * dh; }
            if (zx >= 0.f) {
                float pr = 34.f + 6.f * sinf((float)ImGui::GetTime() * 4.f);
                fg->AddCircle({zx, zy}, pr, C::UA(C::Acc(), 0.85f), 40, 3.f);
                fg->AddCircleFilled({zx, zy}, 7.f, C::U(C::Acc()), 20);
            }
        }

        // Тап (отпускание пальца) — записываем зону.
        if (io.MouseReleased[0]) {
            float rx = io.MousePos.x / dw, ry = io.MousePos.y / dh;
            if (rx > 0.f && rx < 1.f && ry > 0.f && ry < 1.f) {
                const bool farm = (g_calibMode == 1 || g_calibMode == 2);
                if (g_calibMode == 1) {
                    g_state.farm_joy_x = rx; g_state.farm_joy_y = ry;
                    ShowToast(XS("Зона джойстика сохранена"));
                } else if (g_calibMode == 2) {
                    g_state.farm_fire_x = rx; g_state.farm_fire_y = ry;
                    ShowToast(XS("Зона огня сохранена"));
                } else {
                    g_state.aim_tx = rx; g_state.aim_ty = ry;
                    ShowToast(XS("Точка пальца сохранена"));
                }
                PlaySound(SND_CLICK);
                g_calibMode = 0;
                menu_open = true;
                // Вернуться туда, откуда калибровку запускали: зоны автофарма —
                // в окно автофарма, точка аима — на вкладку «Аим».
                if (farm) PopoverOpen(XS("Автофарм"), 5);
                else      g_state.cur_tab = 1;
                g_input.touchConsumed = true; // этот тап уже отработал
            }
        }
        return; // пока калибруемся, меню не рисуем
    }

    if (!menu_open) return;

    const float WW = g_win.w, WH = g_win.h;
    const float hH_ = Layout::HeaderH;

    {
        bool inResize = io.MousePos.x >= g_win.pos.x + g_win.w - R::Card - 14.f
                     && io.MousePos.x <= g_win.pos.x + g_win.w + 14.f
                     && io.MousePos.y >= g_win.pos.y + g_win.h - R::Card - 14.f
                     && io.MousePos.y <= g_win.pos.y + g_win.h + 14.f;

        if (!io.MouseDown[0]) g_win.resizing = false;

        bool anyOverlay = g_sheet.visible || (g_pop.visible && !g_pop.closing);
        if (!anyOverlay && io.MouseClicked[0] && inResize) {
            g_win.resizing         = true;
            g_win.resizeTouchStart = io.MousePos;
            g_win.sizeStart        = {g_win.w, g_win.h};
        }

        if (g_win.resizing && io.MouseDown[0]) {
            float dx = io.MousePos.x - g_win.resizeTouchStart.x;
            float dy = io.MousePos.y - g_win.resizeTouchStart.y;
            g_win.w = ImMax(WW_MIN, g_win.sizeStart.x + dx);
            g_win.h = ImMax(WH_MIN, g_win.sizeStart.y + dy);
        }
    }

    {
        if (!io.MouseDown[0]) g_win.dragging = false;

        if (!g_win.dragging && io.MouseDown[0] && !(g_sheet.visible || (g_pop.visible && !g_pop.closing)) && !g_win.resizing) {
            float tx = io.MouseClickedPos[0].x, ty = io.MouseClickedPos[0].y;
            // Тащим окно за верхнюю шапку (по всей ширине). Кружки светофора
            // больше не исключение: они декор и тапов не обрабатывают.
            bool inHdr = tx >= g_win.pos.x && tx < g_win.pos.x + g_win.w
                      && ty >= g_win.pos.y && ty < g_win.pos.y + hH_;
            if (inHdr) {
                float dx = io.MousePos.x - io.MouseClickedPos[0].x;
                float dy = io.MousePos.y - io.MouseClickedPos[0].y;
                if (fabsf(dx) + fabsf(dy) > 8.f) {
                    g_win.dragging   = true;
                    g_win.touchStart = io.MouseClickedPos[0];
                    g_win.posStart   = g_win.pos;
                }
            }
        }

        if (g_win.dragging && io.MouseDown[0]) {
            float dx   = io.MousePos.x - g_win.touchStart.x;
            float dy   = io.MousePos.y - g_win.touchStart.y;
            g_win.pos.x = g_win.posStart.x + dx;
            g_win.pos.y = g_win.posStart.y + dy;
        }
        float dw = 0.f, dh = 0.f;
        VisibleScreen(dw, dh);
        if (dw > 100.f && dh > 100.f) {
            g_win.pos.x = ImClamp(g_win.pos.x, 0.f, ImMax(0.f, dw - g_win.w));
            g_win.pos.y = ImClamp(g_win.pos.y, 0.f, ImMax(0.f, dh - g_win.h));
        }
    }

    ImGui::SetNextWindowSize({g_win.w, g_win.h}, ImGuiCond_Always);
    ImGui::SetNextWindowPos(g_win.pos, ImGuiCond_Always);

    ImGuiWindowFlags wf = ImGuiWindowFlags_NoResize    | ImGuiWindowFlags_NoCollapse
                        | ImGuiWindowFlags_NoTitleBar  | ImGuiWindowFlags_NoScrollbar
                        | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground
                        | ImGuiWindowFlags_NoMove      | ImGuiWindowFlags_NoNav;

    ImGui::Begin("##menu", nullptr, wf);
    {
        auto  bgP  = g_win.pos;
        auto* bgDL = ImGui::GetWindowDrawList();
        bgDL->AddRectFilled(bgP, {bgP.x + WW, bgP.y + WH}, C::U(C::Bg()), R::Card);
    }

    ImVec2 wp = g_win.pos;
    // Панель вкладок: слева (вертикальный столбец) или снизу (строка по
    // центру) — выбирается в «Опциях». Контент занимает остальную площадь.
    const bool  panelLeft = g_state.ui_panel_left;
    const float botH = Layout::BottomH;
    // Панель вкладок шире стандартной (128 px), если в ней стоит кружок: он
    // должен целиком помещаться в панели, иначе накрывает содержимое вкладок.
    const float railW = panelLeft ? AvatarRailW(g_win.w) : Layout::RailW;
    const float cH = panelLeft ? g_win.h : g_win.h - botH;
    const float cW = panelLeft ? g_win.w - railW : g_win.w;
    const float cX0 = panelLeft ? railW : 0.f;

    g_state.tab_alpha += (1.f - g_state.tab_alpha) * 5.f * dt;
    if (g_state.tab_alpha > .999f) g_state.tab_alpha = 1.f;
    SpringTick(g_state.tab_slide, g_state.tab_slide_vel, 0.f, dt);

    // Короткие и понятные названия вкладок (индекс = id вкладки) — общий список.
    // Подпись вкладки берётся на текущем языке: и в шапке, и в панели, и в
    // расчёте ширины ячейки.
    auto tabNames = [](int k) { return TabTitle(k); };
    // Вкладка «Меню» (id 0) удалена: её функционал переехал в «Опции».
    // id контента вкладок не меняются — конфиги и логика остаются как были.
    static constexpr int kTabShown = 5;
    static constexpr int kTabOrder[kTabShown] = {1, 2, 3, 4, 5};

    // Пропорции ячейки левой панели: подпись как можно крупнее (цель — вдвое
    // крупнее базовых 0.88 от кегля меню) и жирная, иконка — слева.
    //
    // Место считается по самой длинной подписи: у «Конфиги» при двойном кегле
    // ширина ~179 px, а в панели после иконки остаётся ~120 px, поэтому берём
    // максимальный кегль, который влезает, поджав иконку не ниже 40 px. Жирность
    // набирается проходами текста со сдвигом (см. DrawTabText) — у Roboto в
    // сборке одно начертание, полужирного файла нет.
    struct RailCell { float icon = kRailIcon, fs = 20.f, padL = kRailPadL, gap = kRailGap,
                      padR = kRailPadR; };
    auto RailMetrics = [&](float railW) -> RailCell {
        RailCell m;
        const float baseFS = ImGui::GetFontSize() * 0.88f;   // базовый кегль панели
        const float want   = baseFS * kRailFS;               // цель — крупнее базового
        // Если панель уже, чем посчитано в RailTabsW (узкий экран), кегль и
        // иконка ужимаются: сначала меньше становится иконка, потом подпись.
        auto widest = [&](float fs) {
            float worst = 0.f;
            for (int k = 0; k < kTabShown; ++k)
                worst = ImMax(worst, ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0,
                                                                    tabNames(kTabOrder[k])).x);
            return worst;
        };
        for (float fs = want; fs > baseFS; fs -= 1.f) {
            for (float icon = kRailIcon; icon >= 45.f; icon -= 5.f) {
                const float avail = railW - m.padL - m.padR - icon - m.gap;
                if (avail > 0.f && widest(fs) <= avail) { m.icon = icon; m.fs = fs; return m; }
            }
        }
        m.icon = 45.f; m.fs = baseFS;      // узкая панель — базовый кегль
        return m;
    };

    // Ячейка вкладки: иконка + подпись. В нижней панели — иконка сверху,
    // подпись под ней по центру; в левой панели — подпись СПРАВА от иконки
    // (иконки стоят на одной вертикали, подписи — в одну колонку, как в
    // боковых панелях macOS). Размеры подгоняются под ширину ячейки, поэтому
    // длинные подписи («Конфиги») не вылезают за панель.
    auto DrawTabCell = [&](ImDrawList* fdl, int i, float cellX, float cellY,
                           float cellW, float cellH, float iconSize, float lblFS,
                           bool horiz = false,
                           float padL0 = 0.f, float padR0 = 0.f, float gap0 = 0.f) {
        const bool   active = (i == g_state.cur_tab);
        const ImVec4 col    = active ? C::Acc() : C::Dim();
        auto*  font = ImGui::GetFont();

        float  icoW = iconSize, icoH = iconSize, icoX = 0.f, icoY = 0.f;
        float  fs   = lblFS;
        ImVec2 lpos{};

        if (horiz) {
            const float padL = padL0 > 0.f ? padL0 : 16.f;
            const float padR = padR0 > 0.f ? padR0 : 12.f;
            const float gap  = gap0  > 0.f ? gap0  : 12.f;
            auto labelW = [&](float f) { return font->CalcTextSizeA(f, FLT_MAX, 0, tabNames(i)).x; };
            // Подгонка — только на совсем узкой панели: обычно размеры уже
            // посчитаны в RailMetrics под самую длинную подпись.
            const float avail = cellW - padL - padR - gap;
            for (int it = 0; it < 4 && icoW + labelW(fs) > avail; ++it) {
                const float k = ImMax(0.6f, avail / ImMax(1.f, icoW + labelW(fs)));
                icoW = ImMax(24.f, icoW * k);
                fs   = ImMax(12.f, fs * ImMax(0.75f, k));
            }
            icoH = icoW;
            icoX = cellX + padL;
            icoY = cellY + (cellH - icoH) * 0.5f;
            const auto lsz = font->CalcTextSizeA(fs, FLT_MAX, 0, tabNames(i));
            lpos = {icoX + icoW + gap, cellY + (cellH - lsz.y) * 0.5f};
        } else {
            const auto  tsz    = font->CalcTextSizeA(fs, FLT_MAX, 0, tabNames(i));
            const float blockH = icoH + 6.f + tsz.y;
            icoX = cellX + cellW * 0.5f - icoW * 0.5f;
            icoY = cellY + (cellH - blockH) * 0.5f;
            lpos = {cellX + cellW * 0.5f - tsz.x * 0.5f, icoY + icoH + 6.f};
        }

        if (i == 3) {
            // У «Разное» своя векторная иконка (сетка 2x2), чтобы не
            // совпадала с иконкой «Конфиги» из общего атласа.
            float gx0 = icoX, gy0 = icoY;
            float cell = icoW * 0.44f, gap2 = icoW - cell * 2.f;
            ImU32 gcol = C::UA(C::Acc(), active ? 1.f : 0.55f);
            float rr = cell * 0.3f;
            fdl->AddRectFilled({gx0, gy0}, {gx0 + cell, gy0 + cell}, gcol, rr);
            fdl->AddRectFilled({gx0 + cell + gap2, gy0}, {gx0 + icoW, gy0 + cell}, gcol, rr);
            fdl->AddRectFilled({gx0, gy0 + cell + gap2}, {gx0 + cell, gy0 + icoW}, gcol, rr);
            // Четвёртый квадрат — контурный, чтобы иконка читалась как «прочее».
            fdl->AddRect({gx0 + cell + gap2, gy0 + cell + gap2}, {gx0 + icoW, gy0 + icoW}, gcol, rr, 0, 2.f);
        } else if (g_tabIcons[i]) {
            ImVec2 iMin = {icoX, icoY};
            ImVec2 iMax = {icoX + icoW, icoY + icoH};
            fdl->AddImageRounded((ImTextureID)(intptr_t)g_tabIcons[i], iMin, iMax,
                {0,0}, {1,1}, IM_COL32(255, 255, 255, active ? 255 : 165), 9.f);
        } else {
            // Заглушка: скруглённый квадрат с первой буквой вкладки.
            ImVec2 iMin = {icoX, icoY};
            ImVec2 iMax = {icoX + icoW, icoY + icoH};
            fdl->AddRectFilled(iMin, iMax, C::UA(C::Acc(), active ? 0.9f : 0.35f), 9.f);
            char letter[8] = {};
            int gl = 0;
            letter[gl++] = tabNames(i)[0];
            if ((unsigned char)tabNames(i)[0] >= 0xC0) letter[gl++] = tabNames(i)[1];
            float gfs = icoW * 0.55f;
            auto gsz = font->CalcTextSizeA(gfs, FLT_MAX, 0, letter);
            fdl->AddText(font, gfs,
                {icoX + icoW * 0.5f - gsz.x * 0.5f, icoY + (icoH - gsz.y) * 0.5f},
                IM_COL32(255, 255, 255, active ? 255 : 200), letter);
        }
        fdl->AddText(font, fs, lpos, C::U(col), tabNames(i));
    };

    auto TabTap = [&](int i) {
        if (WasTappedHere() && !IsScrollDragging() && !g_input.touchConsumed && g_state.cur_tab != i) {
            g_state.cur_tab       = i;
            g_state.tab_alpha     = 0.f;
            g_state.tab_slide     = 50.f;
            g_state.tab_slide_vel = 0.f;
            g_scrollMain          = {};
            PlaySound(SND_CLICK);
        }
    };

    if (panelLeft) {
        // ---- Левая панель вкладок (вертикальный столбец по центру) ------
        ImGui::SetCursorPos({0, 0});
        ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
        ImGui::BeginChild("##lp", {railW, WH}, false, ImGuiWindowFlags_NoScrollbar);
        auto  lpPos = ImGui::GetWindowPos();
        auto* ldl   = ImGui::GetWindowDrawList();
        ldl->AddRectFilled(lpPos, {lpPos.x + railW, lpPos.y + WH}, C::U(C::LeftBg()), R::Card,
                           ImDrawFlags_RoundCornersLeft);
        ldl->AddLine({lpPos.x + railW, lpPos.y + 12.f}, {lpPos.x + railW, lpPos.y + WH - 12.f},
                     C::UA(C::Sep(), 0.8f), 1.f);

        // Светофор как в macOS — в самом углу окна, кружок с видео под ним.
        TrafficLight(TrafficLightPos(g_win.pos, true), railW);
        const float avR = AvatarR(railW);
        VidAvatar::Draw(ldl, AvatarCx(lpPos.x, avR), AvatarCy(lpPos.y, avR), avR, dt,
                        C::UA(C::Acc(), 0.9f), C::U(C::LeftBg()));

        // Размеры подписи и иконки в этой панели — из ширины панели.
        const RailCell railM = RailMetrics(railW);

        // Столбец вкладок идёт ПОД кружком и не пересекается с ним: сверху
        // оставляем 12 px от кружка, снизу 6 px до края окна. Если в минимальном
        // окне (720 px) пяти ячейкам по 108 px места не хватает, высота ячейки
        // поджимается (кружок 176 px + столбец 514 px = 720) — так столбец
        // целиком остаётся внутри окна, а последняя вкладка не уезжает за край.
        const float colTop = AvatarBottom(0.f, avR) + 12.f;
        const float avail  = ImMax(64.f * kTabShown, WH - colTop - 6.f);
        const float tabH   = ImMin(Layout::TabHV, avail / (float)kTabShown);
        const float colH   = tabH * kTabShown;
        const float startY = ImMax(colTop, (WH - colH) * 0.5f);

        for (int s = 0; s < kTabShown; s++) {
            const int i = kTabOrder[s];
            ImGui::SetCursorPos({0, startY + s * tabH});
            auto pos2 = ImGui::GetCursorScreenPos();
            tab_rects[i] = {pos2.y};
            char tabId[16];
            snprintf(tabId, sizeof(tabId), "##tab%d", i);
            ImGui::InvisibleButton(tabId, {railW, tabH});
            TabTap(i);
        }

        // Пружинная «пилюля» активной вкладки скользит по вертикали.
        {
            float targetRel = tab_rects[g_state.cur_tab].sx - wp.y;
            if (pill_y < 0.f) { pill_y = targetRel; pill_vel = 0.f; }
            SpringTick(pill_y, pill_vel, targetRel, dt);
        }
        float pillY = wp.y + pill_y;

        {
            auto*       fdl = ImGui::GetForegroundDrawList();
            const float pR  = R::Pill;
            float px0 = lpPos.x + 9.f,  px1 = lpPos.x + railW - 9.f;
            float py0 = pillY + 7.f,    py1 = pillY + tabH - 7.f;
            fdl->AddRectFilled({px0, py0}, {px1, py1}, C::U(C::Card()), pR);
            fdl->AddRectFilled({px0, py0}, {px1, py1}, C::UA(C::Acc(), g_darkTheme ? 0.16f : 0.10f), pR);
            fdl->AddRect({px0, py0}, {px1, py1}, C::UA(C::Acc(), 0.5f), pR, 0, 1.5f);
            // Короткая акцентная полоска на левом краю окна.
            float icy = (py0 + py1) * 0.5f;
            fdl->AddRectFilled({lpPos.x - 1.f, icy - 16.f}, {lpPos.x + 3.f, icy + 16.f}, C::U(C::Acc()), 2.f);
        }

        {
            auto* fdl = ImGui::GetForegroundDrawList();
            for (int s = 0; s < kTabShown; s++) {
                const int i = kTabOrder[s];
                // Иконка слева, подпись справа — как в боковых панелях macOS.
                // Кегль и иконка посчитаны под ширину панели (RailMetrics).
                DrawTabCell(fdl, i, lpPos.x, tab_rects[i].sx, railW, tabH,
                            railM.icon, railM.fs, true,
                            railM.padL, railM.padR, railM.gap);
            }
        }

        ImGui::EndChild();
        ImGui::PopStyleColor();
    } else {
        // ---- Нижняя панель вкладок (строка по центру) --------------------
        ImGui::SetCursorPos({0, cH});
        ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
        ImGui::BeginChild("##lp", {WW, botH}, false, ImGuiWindowFlags_NoScrollbar);
        auto  lpPos = ImGui::GetWindowPos();
        auto* ldl   = ImGui::GetWindowDrawList();
        ldl->AddRectFilled(lpPos, {lpPos.x + WW, lpPos.y + botH}, C::U(C::LeftBg()), R::Card,
                           ImDrawFlags_RoundCornersBottom);
        ldl->AddLine({lpPos.x + 14.f, lpPos.y}, {lpPos.x + WW - 14.f, lpPos.y},
                     C::UA(C::Sep(), 0.8f), 1.f);

        // Ряд вкладок строго по центру панели.
        const float tabW   = Layout::TabW;
        const float rowW   = tabW * kTabShown;
        const float startX = (WW - rowW) * 0.5f;

        for (int s = 0; s < kTabShown; s++) {
            const int i = kTabOrder[s];   // id вкладки в этой позиции панели
            ImGui::SetCursorPos({startX + s * tabW, 0});
            auto pos2 = ImGui::GetCursorScreenPos();
            tab_rects[i] = {pos2.x};
            char tabId[16];
            snprintf(tabId, sizeof(tabId), "##tab%d", i);
            ImGui::InvisibleButton(tabId, {tabW, botH});
            TabTap(i);
        }

        // Пружинная «пилюля» активной вкладки скользит по горизонтали.
        {
            float targetRelX = tab_rects[g_state.cur_tab].sx - wp.x;
            if (pill_y < 0.f) { pill_y = targetRelX; pill_vel = 0.f; }
            SpringTick(pill_y, pill_vel, targetRelX, dt);
        }
        float pillX = wp.x + pill_y;
        float barY  = lpPos.y;

        {
            auto*       fdl = ImGui::GetForegroundDrawList();
            const float pR  = R::Pill;
            float px0 = pillX + 8.f,        px1 = pillX + tabW - 8.f;
            float py0 = barY + 12.f,        py1 = barY + botH - 14.f;
            fdl->AddRectFilled({px0, py0}, {px1, py1}, C::U(C::Card()), pR);
            fdl->AddRectFilled({px0, py0}, {px1, py1}, C::UA(C::Acc(), g_darkTheme ? 0.16f : 0.10f), pR);
            fdl->AddRect({px0, py0}, {px1, py1}, C::UA(C::Acc(), 0.5f), pR, 0, 1.5f);
            // Короткая акцентная полоска сверху — указывает на активную вкладку.
            float icx = (px0 + px1) * 0.5f;
            fdl->AddRectFilled({icx - 16.f, barY - 1.f}, {icx + 16.f, barY + 3.f}, C::U(C::Acc()), 2.f);
        }

        {
            auto* fdl = ImGui::GetForegroundDrawList();
            for (int s = 0; s < kTabShown; s++) {
                const int i = kTabOrder[s];
                DrawTabCell(fdl, i, tab_rects[i].sx, barY, tabW, botH, 62.f,
                            ImGui::GetFontSize() * 0.95f);
            }
        }

        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    ImGui::SetCursorPos({cX0, 0});
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
    ImGui::BeginChild("##cp", {cW, cH}, false,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    // Когда панель вкладок не слева, кружок аватарки рисуется в шапке контента
    // (там же, где заголовок): полосу шапки под него расширяем, иначе он налез
    // бы на первый ряд вкладки.
    // В раскладке с нижней панелью вкладок кружок и светофор стоят в шапке
    // контента: светофор у самого угла окна, кружок сразу справа от него, оба
    // по центру полосы. Полосу под них расширяем, чтобы не налезли на первый ряд.
    const float hdrH = MenuHeaderH(panelLeft);

    {
        // Шапка: заголовок вкладки по центру.
        auto titles = [](int k) { return TabTitle(k); };
        auto*  cdl = ImGui::GetWindowDrawList();
        auto   hp  = ImGui::GetWindowPos();
        const float hH = hdrH;
        const float titleFS = ImGui::GetFontSize() * 1.45f;
        auto tsz = ImGui::GetFont()->CalcTextSizeA(titleFS, FLT_MAX, 0, titles(g_state.cur_tab));
        cdl->AddText(ImGui::GetFont(), titleFS,
            {hp.x + (cW - tsz.x) * 0.5f, hp.y + (hH - tsz.y) * 0.5f}, C::U(C::Txt()), titles(g_state.cur_tab));
        if (!panelLeft) {
            // Светофор — в самом левом верхнем углу окна. Кружка аватарки в
            // этой раскладке нет (просьба игрока: с нижней панелью он мешает).
            TrafficLight(TrafficLightPos(g_win.pos, false), cW);
        }
        ImGui::Dummy({cW, hH});
    }

    {
        const float areaH = cH - hdrH;
        auto cpPos  = ImGui::GetWindowPos(); cpPos.y += hdrH;
        const float cpRight = cpPos.x + cW;

        bool sheetBlocking = g_sheet.visible || (g_pop.visible && !g_pop.closing);
        static float s_maxScroll = 0.f;

        bool clickInRight = (io.MouseClickedPos[0].x >= cpPos.x)
                         && (io.MouseClickedPos[0].x <= cpRight)
                         && (io.MouseClickedPos[0].y >= cpPos.y)
                         && (io.MouseClickedPos[0].y <= cpPos.y + areaH);
        bool mIn = clickInRight
            && io.MousePos.x >= cpPos.x  && io.MousePos.x <= cpRight
            && io.MousePos.y >= cpPos.y  && io.MousePos.y <= cpPos.y + areaH;

        auto* cdl = ImGui::GetWindowDrawList();
        cdl->PushClipRect(cpPos, {cpRight, cpPos.y + areaH - 4.f}, true);

        float slideOffset = g_state.tab_slide;
        ImGui::SetCursorPosY(hdrH - g_scrollMain.off + slideOffset);
        ImGui::SetCursorPosX(12.f);

        if (sheetBlocking) ImGui::BeginDisabled(true);
        float contentH = TabContent(g_state.cur_tab, dt, cW);
        if (sheetBlocking) ImGui::EndDisabled();

        if (g_state.tab_alpha < 0.999f) {
            float fadeA = 1.f - g_state.tab_alpha;
            cdl->AddRectFilled(cpPos, {cpRight, cpPos.y + areaH},
                C::UA(C::Bg(), fadeA), R::Card, ImDrawFlags_RoundCornersTop);
        }

        s_maxScroll = ImMax(0.f, contentH - (areaH - 4.f));
        ScrollTick(g_scrollMain, mIn, sheetBlocking, s_maxScroll, dt);

        if (s_maxScroll > 0.f) {
            const float sbPad  = 5.f;
            const float sbVPad = 8.f;
            const float sbMinH = 36.f;
            const float sbW    = 8.f;

            float ratio     = (areaH * 0.28f) / (areaH + s_maxScroll);
            float sbHTarget = ImMax(sbMinH, areaH * ratio);
            float sbTrack   = areaH - sbHTarget - sbVPad * 2.f;
            float sbTTarget = (s_maxScroll > 0.f) ? (g_scrollMain.off / s_maxScroll) * sbTrack : 0.f;

            g_scrollMain.sb_w  = sbW;
            g_scrollMain.sb_y += (sbTTarget - g_scrollMain.sb_y) * 10.f * dt;
            g_scrollMain.sb_h += (sbHTarget - g_scrollMain.sb_h) * 10.f * dt;

            float sbX0 = cpRight - sbPad - sbW;
            float sbX1 = sbX0 + sbW;
            float sbY0 = cpPos.y + sbVPad + g_scrollMain.sb_y;
            float sbY1 = sbY0 + g_scrollMain.sb_h;

            ImU32 thumbCol = C::UA(C::Dim(), 0.55f);

            auto* fdl = ImGui::GetForegroundDrawList();
            fdl->PushClipRect(cpPos, {cpRight, cpPos.y + areaH}, true);
            fdl->AddRectFilled({sbX0, sbY0}, {sbX1, sbY1}, thumbCol, sbW * 0.5f);
            fdl->PopClipRect();
        }

        cdl->PopClipRect();
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();

    {
        static constexpr float kResizePad       = 22.f;
        static constexpr float kResizeStroke     = 10.f;
        static constexpr float kResizeGlowStroke = 6.f;
        static constexpr float kResizeGlowAlpha  = 0.13f;
        static constexpr float kArcStroke        = 14.f;
        static constexpr float kArcA0            = -0.25f;
        static constexpr float kArcA1            = IM_PI * 0.5f + 0.25f;
        static constexpr float kCornerOffset     = 10.f;

        auto* rdl = ImGui::GetForegroundDrawList();

        if (g_win.resizing) {
            const float pad = kResizePad, rr = R::Card + pad;
            float t = EaseInOut(g_themeT);
            ImVec4 bc = {Lerpf(1.f, C::Dark::Acc.x, t), Lerpf(1.f, C::Dark::Acc.y, t), Lerpf(1.f, C::Dark::Acc.z, t), Lerpf(0.9f, 0.8f, t)};
            rdl->AddRect({wp.x - pad,       wp.y - pad},
                         {wp.x + g_win.w + pad, wp.y + g_win.h + pad},
                         C::U(bc), rr, 0, kResizeStroke);
            rdl->AddRect({wp.x - pad - 8.f,       wp.y - pad - 8.f},
                         {wp.x + g_win.w + pad + 8.f, wp.y + g_win.h + pad + 8.f},
                         C::UA(bc, kResizeGlowAlpha), rr + 8.f, 0, kResizeGlowStroke);
        }

        float cx = wp.x + g_win.w - R::Card - kCornerOffset;
        float cy = wp.y + g_win.h - R::Card - kCornerOffset;

        ImU32       col   = g_darkTheme ? IM_COL32(200, 200, 210, 255) : IM_COL32(22, 22, 24, 255);
        const float r     = R::Card;
        const float thick = kArcStroke;
        const float a0    = kArcA0;
        const float a1    = kArcA1;

        rdl->PathArcTo({cx, cy}, r, a0, a1, 48);
        rdl->PathStroke(col, 0, thick);

        float capR = thick * 0.5f;
        rdl->AddCircleFilled({cx + r * cosf(a0), cy + r * sinf(a0)}, capR, col, 24);
        rdl->AddCircleFilled({cx + r * cosf(a1), cy + r * sinf(a1)}, capR, col, 24);
    }

    DrawSheet(dt, wp, WW, WH);
    DrawPopover(dt, wp, WW, WH);
    ImGui::End();
    if (g_menuFadeIn < 1.f) {
        float mA = 1.f - EaseInOut(g_menuFadeIn);
        auto* ofg = ImGui::GetForegroundDrawList();
        ImVec4 bgC = C::Bg();
        ofg->AddRectFilled(g_win.pos, {g_win.pos.x + WW, g_win.pos.y + WH},
            IM_COL32(int(bgC.x*255), int(bgC.y*255), int(bgC.z*255), int(mA * 255)), R::Card);
    }
}
