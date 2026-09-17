#include "menu.h"
#include "main.h"
#include "game.h"
#include "game_offsets_active.h"
#include "app_state.h"
#include "cfg.h"
#include "theme.h"
#include "ui_util.h"
#include "widgets.h"
#include "config.h"
#include "hud.h"
#include "audio.h"
#include "str.h"
#include "esp_draw.h"
#include "aim.h"
#include "logfile.h"                 // AimTouchFracX/Y (строки калибровки)
#include "process.h"             // g_buildPrompt (стартовый выбор версии)
#include "VidAvatar.h"
#if __has_include("media/icons.h")
#  include "media/icons.h"
#  define ICONS_AVAILABLE
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include "stb_image/stb_image.h"
#include <cfloat>
#include <cmath>

static GLuint g_tabIcons[kTabCount] = {};

static GLuint LoadTexFromMemory(const unsigned char* data, int len) {
    int w, h, ch;
    unsigned char* px = stbi_load_from_memory(data, len, &w, &h, &ch, 4);
    if (!px) return 0;
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    stbi_image_free(px);
    return tex;
}

void LoadTabIcons() {
#ifdef ICONS_AVAILABLE
    g_tabIcons[0] = LoadTexFromMemory(main_tab_png, (int)main_tab_png_len);
    g_tabIcons[1] = LoadTexFromMemory(aimbot_png,   (int)aimbot_png_len);
    g_tabIcons[2] = LoadTexFromMemory(visuals_png,  (int)visuals_png_len);
    // Tab order: 3=Разное, 4=Конфиги, 5=Опции. У «Разное» своя векторная
    // иконка (рисуется кодом), поэтому текстура ему не нужна — иначе она
    // дублировала бы иконку «Конфиги» (misc_png).
    g_tabIcons[3] = 0;
    g_tabIcons[4] = LoadTexFromMemory(misc_png,     (int)misc_png_len);
    g_tabIcons[5] = LoadTexFromMemory(settings_png, (int)settings_png_len);
#endif
}

static float pill_y   = -1.f;
static float pill_vel =  0.f;

struct TabRect { float sx; };
static TabRect tab_rects[kTabCount];


float TabContent(int tab, float dt, float cW) {
    float sY = ImGui::GetCursorPosY();

    if (tab == 1) {

        cfg::aim::enabled    = g_state.aim_touch;
        cfg::aim::vis_check  = g_state.aim_pos;
        cfg::aim::draw_fov   = g_state.aim_special;
        cfg::aim::scope_only = g_state.aim_scope_only;
        cfg::aim::fov        = g_state.gun_fov;
        cfg::aim::smoothness = g_state.gun_str;
        cfg::aim::bone       = g_state.aim_bone;
        cfg::aim::trigger_delay  = g_state.gun_trigger_delay;

        SHdr(XS("Аим"));
        CardBg(Layout::RowH * 3);
        const bool aimWasOn = g_state.aim_touch;
        ToggleRow("##ta1", XS("Аим"),          &g_state.aim_touch,   g_state.a_aim_touch, false, true);
        if (aimWasOn != g_state.aim_touch)
            LogLine("аим: %s (режим %d)", g_state.aim_touch ? "включён" : "выключен", g_state.aim_mode);
        ToggleRow("##ta6", XS("Только в прицеле"), &g_state.aim_scope_only, g_state.a_aim_scope, false);
        ToggleRow("##ta3", XS("Круг FOV"),         &g_state.aim_special, g_state.a_aim_spec,  true);

        SHdr(XS("Наводка"));
        CardBg(Layout::SliderH * 2);
        SliderRow("##afov", XS("Радиус"), &g_state.gun_fov, 5.f, 180.f, XS("%.0f°"), false, true, g_state.sl_gun_fov, dt);
        SliderRow("##asmt", XS("Скорость"), &g_state.gun_str, 1.f, 10.f, "%.0f", true, false, g_state.sl_gun_str, dt);

        // ---- Режим: чем именно аим крутит прицел ---------------------------
        // Тач — синтетический палец (нужна инъекция касаний); память — запись
        // ввода взгляда прямо в игру (камера едет как от свайпа, но без
        // пальца); сайлент — запись оси выстрела (камера стоит на месте, в
        // цель уходит только выстрел). Последние два работают и там, где
        // /dev/uinput занят или перехвачен.
        SHdr(XS("Режим"));
        {
            const float rowH = Layout::RowH;
            const float inset = Layout::Inset, padX = Layout::PadX;
            const float avW = ImGui::GetContentRegionAvail().x;
            auto* dl = ImGui::GetWindowDrawList();
            auto* fn = ImGui::GetFont();
            const float fs = ImGui::GetFontSize();
            const bool popBlk = (g_pop.visible && !g_pop.closing) || g_sheet.visible;
            const bool touchMode = (g_state.aim_mode == AIM_MODE_TOUCH);
            // Тачу состояние показывать нечего: у него нет записи в память.
            CardBg(rowH * (touchMode ? 3.f : 4.f));

            auto modeRow = [&](const char* id, const char* lbl, int mode, bool last, float& anim) {
                auto pos = ImGui::GetCursorScreenPos();
                const float cX = pos.x + inset, cW = avW - inset * 2.f;
                const float cy = pos.y + rowH * 0.5f;
                ImGui::InvisibleButton(id, {avW, rowH});
                const bool tapped = WasTappedHere() && !popBlk && !IsScrollDragging() && !g_input.touchConsumed;
                if (tapped && g_state.aim_mode != mode) {
                    g_state.aim_mode = mode;
                    LogLine("аим: выбран режим %d (%s), аим %s", mode, lbl,
                            g_state.aim_touch ? "включён" : "выключен");
                    char b[160];
                    snprintf(b, sizeof(b), XS("%s|%s"), XS("Режим"), lbl);
                    ShowToast(b);
                    PlaySound(SND_CLICK);
                    g_input.touchConsumed = true;
                }
                const bool on = (g_state.aim_mode == mode);
                const float r = 9.f, dotX = cX + cW - padX - r;
                dl->AddCircle({dotX, cy}, r, C::UA(on ? C::Acc() : C::Dim(), 1.f), 12, on ? 2.f : 1.6f);
                if (anim > 0.01f) dl->AddCircleFilled({dotX, cy}, r * 0.5f * anim, C::UA(C::Acc(), 1.f), 12);
                dl->AddText(fn, fs * 1.15f, {cX + padX, cy - fs * 1.15f * 0.5f},
                            C::UA(on ? C::Txt() : C::Dim(), 1.f), lbl);
                if (!last && g_state.ui_show_sep)
                    dl->AddLine({cX + padX, pos.y + rowH - 0.5f},
                                {cX + cW - padX, pos.y + rowH - 0.5f}, C::UA(C::Sep(), 1.f), 0.8f);
            };
            modeRow("##am0", XS("Тач"),     AIM_MODE_TOUCH,  false,     g_state.a_aim_mode0);
            modeRow("##am1", XS("Память"),  AIM_MODE_MEMORY, false,     g_state.a_aim_mode1);
            modeRow("##am2", XS("Сайлент"), AIM_MODE_SILENT, touchMode, g_state.a_aim_mode2);

            // Состояние записи в память. Без него на устройстве не отличить
            // «объект не нашёлся» от «пишем, но игра перезаписывает поле
            // раньше, чем читает» — снаружи оба выглядят как «аим не ведёт».
            if (!touchMode) {
                auto pos = ImGui::GetCursorScreenPos();
                const float cX = pos.x + inset, cW = avW - inset * 2.f;
                const float cy = pos.y + rowH * 0.5f;
                ImGui::InvisibleButton("##amst", {avW, rowH});
                const AimMemDiag& d = AimMemoryDiag();
                char val[32];
                if (g_state.aim_mode == AIM_MODE_SILENT) {
                    if (!d.axis) snprintf(val, sizeof(val), "%s", XS("нет оси"));
                    else         snprintf(val, sizeof(val), XS("%.0f°"), d.dev);
                } else {
                    if (!d.params)          snprintf(val, sizeof(val), "%s", XS("нет объекта"));
                    else if (!d.responded)  snprintf(val, sizeof(val), "%s", XS("нет отклика"));
                    else                    snprintf(val, sizeof(val), "%s", XS("ведёт"));
                }
                auto vsz = fn->CalcTextSizeA(fs, FLT_MAX, 0, val);
                dl->AddText(fn, fs * 1.15f, {cX + padX, cy - fs * 1.15f * 0.5f},
                            C::UA(C::Txt(), 1.f), XS("Состояние"));
                dl->AddText(fn, fs, {cX + cW - padX - vsz.x, cy - vsz.y * 0.5f},
                            C::UA(C::Acc(), 1.f), val);
            }
        }

        // ---- Тач-зона аима -------------------------------------------------
        // Где именно лежит палец, которым аимбот водит камеру. Выбирается
        // тапом по экрану — как зоны автофарма, только точка одна: меню
        // прячется, первый тап записывает её в долях экрана. В режимах записи
        // в память пальца нет вовсе, поэтому секции здесь нет.
        if (g_state.aim_mode == AIM_MODE_TOUCH) {
            SHdr(XS("Зоны бота"));
            {
                const float rowH = Layout::RowH;
                const float inset = Layout::Inset, padX = Layout::PadX;
                const float avW   = ImGui::GetContentRegionAvail().x;
                auto* dl = ImGui::GetWindowDrawList();
                auto* fn = ImGui::GetFont();
                const float fs = ImGui::GetFontSize();
                const bool popBlk = (g_pop.visible && !g_pop.closing) || g_sheet.visible;
                const bool pointSet = (g_state.aim_tx >= 0.f);

                CardBg(rowH * (pointSet ? 2.f : 1.f));

                // Строка «точка пальца»: подпись слева, доля экрана справа.
                auto pos = ImGui::GetCursorScreenPos();
                {
                    const float cX = pos.x + inset, cW = avW - inset * 2.f;
                    const float cy = pos.y + rowH * 0.5f;
                    ImGui::InvisibleButton("##aim_pt", {avW, rowH});
                    if (WasTappedHere() && !popBlk && !IsScrollDragging() && !g_input.touchConsumed) {
                        g_calibMode = 3;   // калибровка: тап по экрану задаёт точку
                        PlaySound(SND_CLICK);
                        ShowToast(XS("Задай точку тапом по экрану"));
                        g_input.touchConsumed = true;
                    }

                    char val[32];
                    snprintf(val, sizeof(val), "%d%% %d%%",
                             (int)(AimTouchFracX() * 100.f + 0.5f),
                             (int)(AimTouchFracY() * 100.f + 0.5f));
                    auto vsz = fn->CalcTextSizeA(fs, FLT_MAX, 0, val);

                    dl->AddText(fn, fs * 1.15f, {cX + padX, cy - fs * 1.15f * 0.5f},
                                C::UA(C::Txt(), 1.f), XS("Точка пальца"));
                    dl->AddText(fn, fs, {cX + cW - padX - vsz.x, cy - vsz.y * 0.5f},
                                C::UA(C::Acc(), 1.f), val);

                    if (pointSet && g_state.ui_show_sep)
                        dl->AddLine({cX + padX, pos.y + rowH - 0.5f},
                                    {cX + cW - padX, pos.y + rowH - 0.5f}, C::UA(C::Sep(), 1.f), 0.8f);
                }

                // Сброс к прежней позиции (74%/50%) — только если точка задана.
                if (pointSet) {
                    auto rpos = ImGui::GetCursorScreenPos();
                    const float cX = rpos.x + inset, cW = avW - inset * 2.f;
                    ImGui::InvisibleButton("##aim_pt_rst", {avW, rowH});
                    const bool tapped = WasTappedHere() && !popBlk && !IsScrollDragging() && !g_input.touchConsumed;
                    if (tapped) {
                        g_state.aim_tx = g_state.aim_ty = -1.f;
                        ShowToast(XS("Точка сброшена"));
                        PlaySound(SND_CLICK);
                        g_input.touchConsumed = true;
                    }
                    const char* rt = XS("Сбросить точку");
                    auto rsz = fn->CalcTextSizeA(fs * 1.05f, FLT_MAX, 0, rt);
                    dl->AddText(fn, fs * 1.05f,
                                {cX + (cW - rsz.x) * 0.5f, rpos.y + (rowH - rsz.y) * 0.5f},
                                C::UA(C::Dim(), 1.f), rt);
                }
            }
        }

        ImGui::Dummy({1.f, 12.f});

} else if (tab == 2) {
        cfg::esp::box          = g_state.esp_box;
        cfg::esp::name_esp     = g_state.esp_name;
        cfg::esp::distance     = g_state.esp_wall;
        cfg::esp::weapon       = g_state.esp_weapon;
        cfg::esp::tracer       = g_state.esp_tracer;
        cfg::esp::skeleton     = g_state.esp_skeleton;
        cfg::esp::ore          = g_state.esp_ore;
        cfg::esp::animal       = g_state.esp_animal;
        cfg::esp::loot         = g_state.esp_loot;
        cfg::esp::team         = g_state.esp_team;
        cfg::esp::pickup       = g_state.esp_pickup;



        auto* dl  = ImGui::GetWindowDrawList();
        auto* fn  = ImGui::GetFont();
        float avW = ImGui::GetContentRegionAvail().x;
        float fs  = ImGui::GetFontSize();
        const float inset = Layout::Inset, padX = Layout::PadX, rowH = Layout::RowH;
        bool popBlk = (g_pop.visible && !g_pop.closing) || g_sheet.visible;

        static const char* s_colorHoldId = nullptr;

        auto EspToggleColorRow = [&](const char* id, const char* lbl, bool* v, float* a, ImVec4* col, bool last) {
            auto pos = ImGui::GetCursorScreenPos();
            float cX = pos.x + inset, cW2 = avW - inset * 2.f;
            float cy = pos.y + rowH * 0.5f;
            auto& io2 = ImGui::GetIO();

            ImGui::InvisibleButton(id, {avW, rowH});

            float dotR = 13.f;
            float dotX = 0.f;
            if (col) {
                auto lblSz = fn->CalcTextSizeA(fs*1.15f, FLT_MAX, 0, lbl);
                dotX = cX + padX + lblSz.x + 14.f + dotR;
            }
            bool openedColor = false;
            if (col && !popBlk && !g_pop.visible) {
                float cx0 = io2.MouseClickedPos[0].x, cy0 = io2.MouseClickedPos[0].y;
                float dx = io2.MousePos.x - cx0, dy = io2.MousePos.y - cy0;
                float moved = dx*dx + dy*dy;
                bool onDot = (cx0 - dotX)*(cx0 - dotX) + (cy0 - cy)*(cy0 - cy) <= (dotR + 18.f)*(dotR + 18.f);
                if (io2.MouseReleased[0] && onDot && PtInClip({dotX, cy}) && moved < 28.f*28.f && s_colorHoldId != id) {
                    s_colorHoldId = id;
                    PopoverOpenColor(lbl, col);
                    PlaySound(SND_CLICK);
                    g_input.touchConsumed = true;
                    openedColor = true;
                }
            }
            if (!io2.MouseDown[0] && s_colorHoldId == id) s_colorHoldId = nullptr;

            bool tapped = !openedColor && WasTappedHere() && !popBlk && !IsScrollDragging() && !g_input.touchConsumed;
            if (tapped) { *v = !*v; char b[160]; snprintf(b,sizeof(b),XS("%s|%s"),lbl,*v?XS("Вкл"):XS("Выкл")); ShowToast(b); PlaySound(SND_CLICK); }
            if (col) {
                ImVec4& c = *col;
                dl->AddCircleFilled({dotX, cy}, dotR, IM_COL32((int)(c.x*255),(int)(c.y*255),(int)(c.z*255),255), 32);
                dl->AddCircle({dotX, cy}, dotR + 2.f, IM_COL32(0,0,0,80), 32, 2.f);
                dl->AddCircle({dotX, cy}, dotR + 1.f, IM_COL32(255,255,255,180), 32, 1.2f);
            }
            float textY = cy - fs * 1.15f * 0.5f;
            dl->AddText(fn, fs * 1.15f, {cX + padX, textY}, C::U(C::Txt()), lbl);
            DrawToggle(dl, cX + cW2 - 72.f - padX, cy, EaseInOut(*a));
            if (!last && g_state.ui_show_sep)
                dl->AddLine({cX+padX, pos.y+rowH-0.5f},{cX+cW2-padX, pos.y+rowH-0.5f}, C::UA(C::Sep(),0.35f), 0.8f);
        };

        SHdr(XS("Игроки"));
        {
            struct ERow { const char* id; const char* lbl; bool* v; float* a; ImVec4* col; };
            ERow rows[] = {
                {"##vb",  XS("Боксы"),      &g_state.esp_box,          &g_state.a_esp_box,          &cfg::esp::box_col},
                {"##v3",  XS("3D боксы"),   &g_state.esp_chams,        &g_state.a_esp_chams,        &cfg::esp::box_col_invis},
                {"##vn",  XS("Ники"),       &g_state.esp_name,         &g_state.a_esp_name,         &cfg::esp::name_col},
                {"##vd",  XS("Дистанция"),  &g_state.esp_wall,         &g_state.a_esp_wall,         &cfg::esp::distance_col},
                {"##vw",  XS("Оружие"),     &g_state.esp_weapon,       &g_state.a_esp_weapon,       &cfg::esp::weapon_col},
                {"##vtr", XS("Линии"),      &g_state.esp_tracer,       &g_state.a_esp_tracer,       &cfg::esp::tracer_col},
                {"##vsk", XS("Скелеты"),    &g_state.esp_skeleton,     &g_state.a_esp_skeleton,     &cfg::esp::skeleton_col},
                {"##vtm", XS("Свои"),       &g_state.esp_team,         &g_state.a_esp_team,         &cfg::esp::ally_col},
            };
            constexpr int NP = 8;
            CardBg(rowH * NP);
            for (int i = 0; i < NP; i++)
                EspToggleColorRow(rows[i].id, rows[i].lbl, rows[i].v, rows[i].a, rows[i].col, i == NP-1);
        }

        SHdr(XS("Мир"));
        {
            struct ERow { const char* id; const char* lbl; bool* v; float* a; ImVec4* col; };
            ERow rows[] = {
                // No colour dot: every resource paints itself (stone grey,
                // metal orange, sulfur yellow).
                {"##vor", XS("Руда"),      &g_state.esp_ore,          &g_state.a_esp_ore,          nullptr},
                {"##van", XS("Животные"), &g_state.esp_animal,       &g_state.a_esp_animal,       &cfg::esp::animal_col},
                {"##vlt", XS("Ящики"),    &g_state.esp_loot,         &g_state.a_esp_loot,         &cfg::esp::loot_col},
                {"##vpk", XS("Предметы"), &g_state.esp_pickup,       &g_state.a_esp_pickup,       &cfg::esp::pickup_col},
            };
            constexpr int NW = 4;
            CardBg(rowH * NW);
            for (int i = 0; i < NW; i++)
                EspToggleColorRow(rows[i].id, rows[i].lbl, rows[i].v, rows[i].a, rows[i].col, i == NW-1);
        }

        SHdr(XS("Дальность"));
        CardBg(Layout::SliderH);
        SliderRow("##vmd", XS("Показывать до"), &g_state.marker_dist,
                  25.f, 300.f, XS("%.0f м"), true, true, g_state.sl_marker_dist, dt);

        ImGui::Dummy({1.f, 8.f});
        CollapsibleHeader("##veh1", XS("Ещё настройки"), 1);

        ImGui::Dummy({1.f, 12.f});

    } else if (tab == 4) {
        // Конфиги (saved profiles). Kept at index 4 so the Мемори tab sits
        // directly above it in the tab bar.

        auto* dl  = ImGui::GetWindowDrawList();
        auto* fn  = ImGui::GetFont();
        float avW = ImGui::GetContentRegionAvail().x;
        const float inset = Layout::Inset;
        const float fs = ImGui::GetFontSize();

        SHdr(XS("Новый конфиг"));
        {
            const float btnH = 72.f;
            auto pos = ImGui::GetCursorScreenPos();
            float cardX0 = pos.x + inset, cardX1 = pos.x + avW - inset;

            dl->AddRectFilled({cardX0, pos.y}, {cardX1, pos.y + btnH},
                C::U(C::Card()), R::Btn);
            if (g_state.ui_show_sep)
                dl->AddRect({cardX0, pos.y}, {cardX1, pos.y + btnH},
                    C::UA(C::Sep(), 0.9f), R::Btn, 0, 1.2f);

            ImGui::InvisibleButton("##cfgcreate", {avW, btnH});
            bool popBlocking2 = (g_pop.visible && !g_pop.closing) || g_sheet.visible;
            bool tapped = WasTappedHere() && !popBlocking2 && !IsScrollDragging() && !g_input.touchConsumed;
            if (tapped) ConfigSave();

            float cy2 = pos.y + btnH * 0.5f;
            float icX = cardX0 + 28.f + 20.f;
            dl->AddCircleFilled({icX, cy2}, 20.f, C::UA(C::Acc(), 0.15f), 32);
            dl->AddCircle({icX, cy2}, 20.f, C::UA(C::Acc(), 0.7f), 32, 1.5f);
            dl->AddLine({icX - 9.f, cy2}, {icX + 9.f, cy2}, C::U(C::Acc()), 3.f);
            dl->AddLine({icX, cy2 - 9.f}, {icX, cy2 + 9.f}, C::U(C::Acc()), 3.f);

            const char* createTxt = XS("Создать конфиг");
            float textY = pos.y + (btnH - fs * 1.25f) * 0.5f;
            dl->AddText(fn, fs * 1.25f, {icX + 32.f, textY}, C::U(C::Acc()), createTxt);
        }

        SHdr(XS("Сохранённые"));

        if (g_configCount == 0) {
            const float emptyH = 120.f;
            auto ep = ImGui::GetCursorScreenPos();
            dl->AddRectFilled({ep.x + inset, ep.y}, {ep.x + avW - inset, ep.y + emptyH},
                C::U(C::Card()), R::Card);
            if (g_state.ui_show_sep)
                dl->AddRect({ep.x + inset, ep.y}, {ep.x + avW - inset, ep.y + emptyH},
                    C::UA(C::Sep(), 0.9f), R::Card, 0, 1.2f);
            ImGui::Dummy({avW, emptyH});
            const char* emptyTxt = XS("Нет конфигов");
            auto eSz = fn->CalcTextSizeA(fs * 1.05f, FLT_MAX, 0, emptyTxt);
            dl->AddText(fn, fs * 1.05f,
                {ep.x + (avW - eSz.x) * 0.5f, ep.y + (emptyH - fs * 1.05f) * 0.5f},
                C::U(C::Dim()), emptyTxt);
        } else {
            auto& io2 = ImGui::GetIO();
            bool popBlocking2 = (g_pop.visible && !g_pop.closing) || g_sheet.visible;

            const float padH    = 12.f;
            const float padTop  = 14.f;
            const float padBot  = 14.f;
            const float icSz    = 62.f;
            const float nameGap = 12.f;
            const float midGap  = 10.f;
            const float btnH2   = 46.f;
            const float btnGapX =  6.f;
            const float cardGap = 10.f;

            float nameFS = fs * 1.15f;
            float subFS  = fs * 0.78f;
            float textH  = fn->CalcTextSizeA(nameFS, FLT_MAX, 0, XS("A")).y;
            float subH   = fn->CalcTextSizeA(subFS,  FLT_MAX, 0, XS("A")).y;
            float topH   = ImMax(icSz, textH + 6.f + subH);
            float cardH  = padTop + topH + midGap + btnH2 + padBot;

            for (int ci = 0; ci < g_configCount; ci++) {
                float loadA = (ci < kMaxConfigs) ? g_cfgLoadAnim[ci] : 0.f;
                float easedA = loadA * loadA * (3.f - 2.f * loadA);

                auto pos = ImGui::GetCursorScreenPos();
                float cx0 = pos.x + inset, cx1 = pos.x + avW - inset;
                float cw  = cx1 - cx0;

                {
                    ImVec4 bg = C::Card(), ac = C::Acc();
                    float mix = (g_darkTheme ? 0.14f : 0.06f) * easedA;
                    ImU32 cardFill = IM_COL32(
                        int(bg.x*255*(1.f-mix) + ac.x*255*mix),
                        int(bg.y*255*(1.f-mix) + ac.y*255*mix),
                        int(bg.z*255*(1.f-mix) + ac.z*255*mix), 255);
                    dl->AddRectFilled({cx0, pos.y}, {cx1, pos.y + cardH}, cardFill, R::Card);

                    float borderA = Lerpf(g_state.ui_show_sep ? 0.9f : 0.f, 0.65f, easedA);
                    ImVec4 borderCol = g_state.ui_show_sep
                        ? ImVec4{Lerpf(C::Sep().x, ac.x, easedA), Lerpf(C::Sep().y, ac.y, easedA), Lerpf(C::Sep().z, ac.z, easedA), borderA}
                        : ImVec4{ac.x, ac.y, ac.z, borderA};
                    if (borderA > 0.01f)
                        dl->AddRect({cx0, pos.y}, {cx1, pos.y + cardH}, C::U(borderCol), R::Card, 0, Lerpf(1.2f, 2.2f, easedA));
                }

                float rowCY = pos.y + padTop + topH * 0.5f;
                float icX  = cx0 + padH;
                float icY  = rowCY - icSz * 0.5f;
                float icCX = icX + icSz * 0.5f;

                if (g_tabIcons[4]) {
                    dl->AddImageRounded((ImTextureID)(intptr_t)g_tabIcons[4],
                        {icX, icY}, {icX + icSz, icY + icSz},
                        {0,0}, {1,1}, IM_COL32(255,255,255,255), 12.f);
                } else {
                    float icR = 14.f;
                    ImU32 icBg = g_darkTheme
                        ? IM_COL32(120, 105, 240, 255)
                        : IM_COL32(108, 92, 231, 255);
                    dl->AddRectFilled({icX, icY}, {icX + icSz, icY + icSz}, icBg, icR);
                    float lh = icSz * 0.20f;
                    float lx = icCX - lh * 0.45f, ly = rowCY + lh * 0.42f;
                    dl->AddLine({icCX - lh, rowCY}, {lx, ly},      IM_COL32(255,255,255,245), 3.2f);
                    dl->AddLine({lx, ly}, {icCX + lh, rowCY - lh}, IM_COL32(255,255,255,245), 3.2f);
                }

                const char* badgeTxt = XS("Загружен");
                auto btsz    = fn->CalcTextSizeA(subFS, FLT_MAX, 0, badgeTxt);
                float dotR   = 3.5f;
                float bPadX  = 8.f, bPadY = 5.f;
                float bW     = dotR*2.f + 5.f + btsz.x + bPadX * 2.f;
                float bH     = btsz.y + bPadY * 2.f;

                float nameAreaR = easedA > 0.01f ? (cx1 - bW * easedA - 10.f) : (cx1 - padH);
                float nameX = icX + icSz + nameGap;
                float nameY = rowCY - (textH + 6.f + subH) * 0.5f;

                dl->PushClipRect({nameX, pos.y}, {nameAreaR, pos.y + cardH}, true);
                dl->AddText(fn, nameFS, {nameX, nameY}, C::U(C::Txt()), g_configs[ci].name);
                dl->AddText(fn, subFS, {nameX, nameY + textH + 6.f}, C::UA(C::Dim(), 0.7f), XS("config"));
                dl->PopClipRect();

                if (easedA > 0.01f) {
                    ImVec4 ac = C::Acc();
                    float bX  = cx1 - bW - 10.f;
                    float bY2 = rowCY - bH * 0.5f;
                    float a   = easedA;

                    dl->AddRectFilled({bX, bY2}, {bX + bW, bY2 + bH},
                        C::UA(ac, (g_darkTheme ? 0.20f : 0.10f) * a), bH * 0.5f);
                    dl->AddRect({bX, bY2}, {bX + bW, bY2 + bH},
                        C::UA(ac, 0.55f * a), bH * 0.5f, 0, 1.3f);
                    dl->AddCircleFilled({bX + bPadX + dotR, bY2 + bH*0.5f}, dotR, C::UA(ac, 0.95f * a), 16);
                    dl->AddText(fn, subFS,
                        {bX + bPadX + dotR*2.f + 5.f, bY2 + bPadY},
                        C::UA(ac, 0.95f * a), badgeTxt);
                }

                float bY  = pos.y + padTop + topH + midGap;
                float totalBtnW = cw - padH * 2.f;
                float bw3 = (totalBtnW - btnGapX * 2.f) / 3.f;
                float b1X = cx0 + padH;
                float b2X = b1X + bw3 + btnGapX;
                float b3X = b2X + bw3 + btnGapX;

                auto DrawBtn = [&](float bx, float bw, ImU32 fillL, ImU32 fillD, ImU32 txtCol, const char* label) {
                    ImU32 fill = g_darkTheme ? fillD : fillL;
                    dl->AddRectFilled({bx, bY}, {bx + bw, bY + btnH2}, fill, R::Btn);
                    auto lsz = fn->CalcTextSizeA(fs * 0.93f, FLT_MAX, 0, label);
                    dl->PushClipRect({bx+3.f, bY}, {bx+bw-3.f, bY+btnH2}, true);
                    dl->AddText(fn, fs * 0.93f,
                        {bx + bw*0.5f - lsz.x*0.5f, bY + (btnH2 - lsz.y)*0.5f},
                        txtCol, label);
                    dl->PopClipRect();
                };

                DrawBtn(b1X, bw3,
                    IM_COL32(238, 235, 255, 255),
                    IM_COL32(58,  48,  140, 255),
                    g_darkTheme ? IM_COL32(170, 158, 255, 255) : IM_COL32(96, 80, 220, 255),
                    XS("Загрузить"));

                DrawBtn(b2X, bw3,
                    IM_COL32(232, 248, 237, 255),
                    IM_COL32(20,  80,  40,  255),
                    g_darkTheme ? IM_COL32(80, 210, 120, 255) : IM_COL32(25, 140, 60, 255),
                    XS("Сохранить"));

                DrawBtn(b3X, bw3,
                    IM_COL32(255, 237, 236, 255),
                    IM_COL32(100, 20,  20,  255),
                    g_darkTheme ? IM_COL32(255, 100, 90, 255) : IM_COL32(210, 30, 20, 255),
                    XS("Удалить"));

                ImGui::InvisibleButton(("##cfg_" + std::to_string(ci)).c_str(), {avW, cardH});

                if (!popBlocking2 && !IsScrollDragging() && !g_input.touchConsumed && io2.MouseReleased[0]
                    && PtInClip(io2.MouseClickedPos[0])) {
                    auto mp  = io2.MousePos;
                    auto cp2 = io2.MouseClickedPos[0];
                    if (mp.x  >= b1X && mp.x  <= b1X+bw3 && mp.y  >= bY && mp.y  <= bY+btnH2
                     && cp2.x >= b1X && cp2.x <= b1X+bw3 && cp2.y >= bY && cp2.y <= bY+btnH2)
                        ConfigLoad(ci);
                    else if (mp.x  >= b2X && mp.x  <= b2X+bw3 && mp.y  >= bY && mp.y  <= bY+btnH2
                     && cp2.x >= b2X && cp2.x <= b2X+bw3 && cp2.y >= bY && cp2.y <= bY+btnH2)
                        ConfigUpdate(ci);
                    else if (mp.x  >= b3X && mp.x  <= b3X+bw3 && mp.y  >= bY && mp.y  <= bY+btnH2
                     && cp2.x >= b3X && cp2.x <= b3X+bw3 && cp2.y >= bY && cp2.y <= bY+btnH2) {
                        g_configToDelete = ci;
                        PopoverOpen(XS("Удалить конфиг?"), 3);
                    }
                }

                if (ci < g_configCount - 1)
                    ImGui::Dummy({1.f, cardGap});
            }
        }

    } else if (tab == 5) {
        // Опции (язык + интерфейс + система) — нижняя вкладка.
        // Счётчик FPS и «Рамки карточек» убраны: рамки включены принудительно.
        // ---- Язык: русский или английский -------------------------------
        // Переводится всё, что рисует оверлей: подписи меню и вкладок, тосты и
        // подписи визуалов (оружие, предметы, животные, ящики). Переключение
        // применяется сразу, на текущем кадре. Названия самих языков пишем на
        // них самих — так строка понятна при любом выбранном языке.
        SHdr(XS("Язык"));
        {
            auto* dl  = ImGui::GetWindowDrawList();
            auto* fn  = ImGui::GetFont();
            float avW = ImGui::GetContentRegionAvail().x;
            const float inset = Layout::Inset;
            const float rowH  = Layout::RowH;
            auto  pos = ImGui::GetCursorScreenPos();
            bool  popBlk = (g_pop.visible && !g_pop.closing) || g_sheet.visible;

            const char* const langNames[2] = { "Русский", "English" };
            const float gap = 10.f;
            const float halfW = (avW - inset * 2.f - gap) * 0.5f;

            for (int li = 0; li < 2; li++) {
                const bool sel = (lang::current() == (li == 0 ? lang::LANG_RU : lang::LANG_EN));
                const float x0 = pos.x + inset + li * (halfW + gap);
                const float x1 = x0 + halfW;

                dl->AddRectFilled({x0, pos.y}, {x1, pos.y + rowH}, C::U(C::Card()), R::Card);
                if (sel) {
                    dl->AddRectFilled({x0, pos.y}, {x1, pos.y + rowH},
                        C::UA(C::Acc(), g_darkTheme ? 0.16f : 0.10f), R::Card);
                    dl->AddRect({x0, pos.y}, {x1, pos.y + rowH}, C::UA(C::Acc(), 0.8f), R::Card, 0, 2.f);
                } else if (g_state.ui_show_sep) {
                    dl->AddRect({x0, pos.y}, {x1, pos.y + rowH}, C::U(C::Sep()), R::Card, 0, 1.2f);
                }

                auto lsz = fn->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, 0, langNames[li]);
                dl->AddText(fn, ImGui::GetFontSize(),
                    {x0 + (halfW - lsz.x) * 0.5f, pos.y + (rowH - lsz.y) * 0.5f},
                    C::U(sel ? C::Acc() : C::Txt()), langNames[li]);

                char lid[16]; snprintf(lid, sizeof(lid), "##lang%d", li);
                ImGui::SetCursorScreenPos({x0, pos.y});
                ImGui::InvisibleButton(lid, {halfW, rowH});
                if (WasTappedHere() && !popBlk && !IsScrollDragging() && !g_input.touchConsumed && !sel) {
                    lang::set(li == 0 ? lang::LANG_RU : lang::LANG_EN);
                    RememberLang();
                    // Тост — уже на выбранном языке: он и показывает, что
                    // переключение сработало в этом же кадре.
                    char _b[160];
                    snprintf(_b, sizeof(_b), "%s|%s", lang::text(XS_RU("Язык")), langNames[li]);
                    ShowToast(_b);
                    PlaySound(SND_CLICK);
                }
            }
            ImGui::SetCursorScreenPos({pos.x, pos.y + rowH});
            ImGui::Dummy({avW, 0.f});
        }

        SHdr(XS("Интерфейс"));
        CardBg(Layout::RowH * 1);
        if (ToggleRow("##ud2", XS("Тёмная тема"), &g_state.ui_dark_mode, g_state.a_ui_dark, true, true))
            g_darkTheme = g_state.ui_dark_mode;

        // ---- Положение панели вкладок: слева или снизу ------------------
        // Перенесено сюда из удалённой вкладки «Меню».
        SHdr(XS("Панель вкладок"));
        {
            auto* dl  = ImGui::GetWindowDrawList();
            auto* fn  = ImGui::GetFont();
            float avW = ImGui::GetContentRegionAvail().x;
            const float inset = Layout::Inset;
            const float fs = ImGui::GetFontSize();

            const float cardH = 120.f;
            const float gap   = 10.f;
            auto  pos   = ImGui::GetCursorScreenPos();
            float x0    = pos.x + inset;
            float cardW = (avW - inset * 2.f - gap) * 0.5f;
            bool  popBlk = (g_pop.visible && !g_pop.closing) || g_sheet.visible;

            struct POpt { const char* lbl; bool left; };
            const POpt opts[2] = { { XS("Слева"), true }, { XS("Снизу"), false } };

            for (int oi = 0; oi < 2; oi++) {
                float ox0 = x0 + oi * (cardW + gap);
                float ox1 = ox0 + cardW;
                bool  sel = (g_state.ui_panel_left == opts[oi].left);

                dl->AddRectFilled({ox0, pos.y}, {ox1, pos.y + cardH}, C::U(C::Card()), R::Card);
                if (sel) {
                    dl->AddRectFilled({ox0, pos.y}, {ox1, pos.y + cardH},
                        C::UA(C::Acc(), g_darkTheme ? 0.16f : 0.10f), R::Card);
                    dl->AddRect({ox0, pos.y}, {ox1, pos.y + cardH}, C::UA(C::Acc(), 0.8f), R::Card, 0, 2.f);
                } else if (g_state.ui_show_sep) {
                    dl->AddRect({ox0, pos.y}, {ox1, pos.y + cardH}, C::U(C::Sep()), R::Card, 0, 1.2f);
                }

                // Мини-схема окна: прямоугольник с панелью слева или снизу.
                float mw = 74.f, mh = 52.f;
                float mx = ox0 + (cardW - mw) * 0.5f;
                float my = pos.y + 14.f;
                ImU32 frameCol = C::UA(sel ? C::Acc() : C::Dim(), sel ? 0.95f : 0.6f);
                dl->AddRect({mx, my}, {mx + mw, my + mh}, frameCol, 6.f, 0, 2.f);
                if (opts[oi].left)
                    dl->AddRectFilled({mx + 3.f, my + 3.f}, {mx + 3.f + 16.f, my + mh - 3.f},
                                      C::UA(sel ? C::Acc() : C::Dim(), sel ? 0.85f : 0.4f), 4.f);
                else
                    dl->AddRectFilled({mx + 3.f, my + mh - 3.f - 12.f}, {mx + mw - 3.f, my + mh - 3.f},
                                      C::UA(sel ? C::Acc() : C::Dim(), sel ? 0.85f : 0.4f), 4.f);

                auto lsz = fn->CalcTextSizeA(fs * 1.0f, FLT_MAX, 0, opts[oi].lbl);
                dl->AddText(fn, fs * 1.0f,
                    {ox0 + (cardW - lsz.x) * 0.5f, pos.y + cardH - 16.f - lsz.y},
                    C::U(sel ? C::Acc() : C::Txt()), opts[oi].lbl);

                char oid[16]; snprintf(oid, sizeof(oid), "##pstyle%d", oi);
                ImGui::SetCursorScreenPos({ox0, pos.y});
                ImGui::InvisibleButton(oid, {cardW, cardH});
                if (WasTappedHere() && !popBlk && !IsScrollDragging() && !g_input.touchConsumed
                    && g_state.ui_panel_left != opts[oi].left) {
                    g_state.ui_panel_left = opts[oi].left;
                    pill_y = -1.f; pill_vel = 0.f;   // «пилюля» меняет ось — сброс пружины
                    ShowToast(opts[oi].left ? XS("Панель слева") : XS("Панель снизу"));
                    PlaySound(SND_CLICK);
                }
            }
            ImGui::SetCursorScreenPos({pos.x, pos.y + cardH});
            ImGui::Dummy({avW, 0.f});
        }

        SHdr(XS("Система"));
        {
            auto* dl  = ImGui::GetWindowDrawList();
            float avW = ImGui::GetContentRegionAvail().x;
            const float rowH = Layout::RowH, inset = Layout::Inset;
            auto pos = ImGui::GetCursorScreenPos();
            dl->AddRectFilled({pos.x + inset, pos.y}, {pos.x + avW - inset, pos.y + rowH}, C::U(C::Card()), R::Card);
            if (g_state.ui_show_sep)
                dl->AddRect({pos.x + inset, pos.y}, {pos.x + avW - inset, pos.y + rowH}, C::U(C::Sep()), R::Card, 0, 1.2f);
            ImGui::InvisibleButton("##exit", {avW, rowH});
            if (WasTappedHere() && !IsScrollDragging())
                PopoverOpen(XS("Выйти?"), 2);
            const char* exitTxt = XS("Выйти");
            float exitFS = ImGui::GetFontSize() * 1.15f;
            auto  tsz = ImGui::GetFont()->CalcTextSizeA(exitFS, FLT_MAX, 0, exitTxt);
            float tx  = pos.x + (avW - tsz.x) * 0.5f;
            float ty  = pos.y + (rowH - exitFS) * 0.5f;
            dl->AddText(ImGui::GetFont(), exitFS, {tx, ty}, C::U(C::Red()), exitTxt);
        }
    } else if (tab == 3) {
        // Разное: каждая крупная функция — своя карточка-«вкладка»,
        // открывающая отдельное окно (как «Ещё настройки» в ESP).
        SHdr(XS("Функции"));
        CollapsibleHeader("##fnfarm", XS("Автофарм"), 5);

        // Иксрей: рендер отсекает всё ближе выбранной дистанции — стены и
        // текстуры вокруг игрока пропадают, видно что за ними.
        SHdr(XS("Иксрей"));
        CardBg(Layout::RowH + Layout::SliderH);
        ToggleRow("##xr0", XS("Иксрей"), &g_state.xray_on, g_state.a_xray_on, false, true);
        SliderRow("##xr1", XS("Дальность"), &g_state.xray_range,
                  1.f, 50.f, XS("%.0f м"), true, false, g_state.sl_xray, dt);

        // Всегда день: время суток каждую секунду возвращается в полдень.
        SHdr(XS("Мир"));
        CardBg(Layout::RowH);
        ToggleRow("##wd0", XS("Всегда день"), &g_state.always_day, g_state.a_always_day, true, true);

        ImGui::Dummy({1.f, 12.f});
    }

    ImGui::Dummy({1.f, 12.f});
    return ImGui::GetCursorPosY() - sY;
}

static uint32_t g_menu_orient = 0xFFFFFFFFu;
static int g_menu_dw = 0, g_menu_dh = 0;


void CenterMenuOnDisplay() {
    float dw = 0.f, dh = 0.f;
    VisibleScreen(dw, dh);
    if (dw < 100.f || dh < 100.f) return;
    if (g_win.w > dw - 16.f) g_win.w = ImMax(160.f, dw - 16.f);
    if (g_win.h > dh - 16.f) g_win.h = ImMax(160.f, dh - 16.f);
    g_win.pos.x = (dw - g_win.w) * 0.5f;
    g_win.pos.y = (dh - g_win.h) * 0.5f;
    g_win.dragging = false;
    g_win.resizing = false;
}



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
    Tick(g_state.a_aim_mode0,  g_state.aim_mode == AIM_MODE_TOUCH,  dt);
    Tick(g_state.a_aim_mode1,  g_state.aim_mode == AIM_MODE_MEMORY, dt);
    Tick(g_state.a_aim_mode2,  g_state.aim_mode == AIM_MODE_SILENT, dt);
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


// Калибровка зон вводом с экрана: 0 — нет, 1 — ждём тап по джойстику
// автофарма, 2 — по кнопке огня, 3 — по точке, из которой аимбот водит палец.
// Пока калибровка активна, меню скрыто и первый тап по экрану пишет позицию
// (в долях экрана) в g_state.
int  g_calibMode = 0;