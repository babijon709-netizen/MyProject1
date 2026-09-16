// ui/tabs.cpp — TabContent: содержимое вкладок.
//
// Модуль разрезан из прежнего монолита jni/src/main.cpp;
// что здесь лежит и кто это зовёт — в шапке ui/tabs.h и в docs/CODE_MAP.md.


#include "app/common.h"
#include "app/media.h"
#include "ui/config.h"
#include "ui/layout.h"
#include "ui/popover.h"
#include "ui/scroll.h"
#include "ui/settings.h"
#include "ui/sheet.h"
#include "ui/theme.h"
#include "ui/toast.h"
#include "ui/widgets.h"
#include "ui/window.h"
#include "ui/tabs.h"
#include "esp/aim_mem.h"

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

        // ---- Режим аима: палец или память ----------------------------------
        // «Тач» — прежний аимбот: водит палец через uinput. «Мемори» — доворот
        // записью поворота в память игры (esp/aim_mem.cpp): не зависит от прав на
        // uinput и от настройки чувствительности, а ошибка прицела берётся не с
        // экрана, а из самих углов — поэтому точнее. Дорожку записи модуль не
        // угадывает, а подтверждает замером, и строка под переключателем говорит,
        // чем дело кончилось на этом устройстве. Пока идёт подбор, аим камеру не
        // трогает — поэтому строка обязана быть видна (подбор ограничен по
        // времени, а если не вышло — работает тач).
        SHdr(XS("Режим"));
        {
            const float avW   = ImGui::GetContentRegionAvail().x;
            const float inset = Layout::Inset;
            const float rowH  = Layout::RowH;
            auto  pos = ImGui::GetCursorScreenPos();
            auto* dl  = ImGui::GetWindowDrawList();
            auto* fn  = ImGui::GetFont();
            const float fs = ImGui::GetFontSize();
            const bool popBlk = (g_pop.visible && !g_pop.closing) || g_sheet.visible;
            const char* const modeNames[2] = { XS("Тач"), XS("Мемори") };
            const int modeIdx = (g_state.aim_mode == 1) ? 1 : 0;
            const float gap = 10.f;
            const float halfW = (avW - inset * 2.f - gap) * 0.5f;

            for (int mi = 0; mi < 2; mi++) {
                const bool sel = (modeIdx == mi);
                const float x0 = pos.x + inset + mi * (halfW + gap);
                const float x1 = x0 + halfW;

                dl->AddRectFilled({x0, pos.y}, {x1, pos.y + rowH}, C::U(C::Card()), R::Card);
                if (sel) {
                    dl->AddRectFilled({x0, pos.y}, {x1, pos.y + rowH},
                        C::UA(C::Acc(), g_darkTheme ? 0.16f : 0.10f), R::Card);
                    dl->AddRect({x0, pos.y}, {x1, pos.y + rowH}, C::UA(C::Acc(), 0.8f), R::Card, 0, 2.f);
                } else if (g_state.ui_show_sep) {
                    dl->AddRect({x0, pos.y}, {x1, pos.y + rowH}, C::U(C::Sep()), R::Card, 0, 1.2f);
                }

                auto lsz = fn->CalcTextSizeA(fs, FLT_MAX, 0, modeNames[mi]);
                dl->AddText(fn, fs, {x0 + (halfW - lsz.x) * 0.5f, pos.y + (rowH - lsz.y) * 0.5f},
                            C::U(sel ? C::Acc() : C::Txt()), modeNames[mi]);

                char mid[20]; snprintf(mid, sizeof(mid), "##aimmode%d", mi);
                ImGui::SetCursorScreenPos({x0, pos.y});
                ImGui::InvisibleButton(mid, {halfW, rowH});
                if (WasTappedHere() && !popBlk && !IsScrollDragging() && !g_input.touchConsumed && !sel) {
                    g_state.aim_mode = mi;
                    char _b[160]; snprintf(_b, sizeof(_b), XS("%s|%s"), XS("Режим"), modeNames[mi]);
                    ShowToast(_b);
                    PlaySound(SND_CLICK);
                    g_input.touchConsumed = true;
                }
            }
            ImGui::SetCursorScreenPos({pos.x, pos.y + rowH});
            ImGui::Dummy({avW, 0.f});
        }

        // Строка состояния мемори-режима: что именно нашлось на этом устройстве.
        if (g_state.aim_mode == 1) {
            const int st   = esp_mem_aim_state();
            const int path = esp_mem_aim_path();
            const int rsn  = esp_mem_aim_reason();
            const char* head    = XS("Проверю, когда закроешь меню");
            const char* detail  = "";
            ImVec4      headCol = C::Dim();

            auto pathName = [](int p) -> const char* {
                switch (p) {
                    case MEM_PATH_STATE_QUAT:  return XS("кватернион поворота");
                    case MEM_PATH_STATE_DEG:   return XS("углы поворота, градусы");
                    case MEM_PATH_STATE_RAD:   return XS("углы поворота, радианы");
                    case MEM_PATH_INPUT:       return XS("ввод поворота игры");
                    case MEM_PATH_TRANSFORM:   return XS("узел камеры");
                    default:                   return "";
                }
            };
            auto reasonName = [](int r) -> const char* {
                switch (r) {
                    case MEM_REASON_NO_MOUSE_LOOK: return XS("нет доступа к MouseLook");
                    case MEM_REASON_NO_ANGLES:     return XS("углы прицела не читаются");
                    case MEM_REASON_NO_FIELD:      return XS("поля поворота не найдены");
                    case MEM_REASON_WRITE_LOST:    return XS("запись не доворачивает прицел");
                    default:                       return "";
                }
            };

            if (!g_state.aim_touch) {
                // Самотест идёт только при включённом аиме: без него это были бы
                // пробные довороты прицела «просто так», при живом игроке.
                head = XS("Сначала включи «Аим»");
            } else if (st == MEM_AIM_PROBING) {
                head = XS("Подбираю способ записи…");
                headCol = C::Txt();
                detail = XS("аим пока не трогает камеру");
            } else if (st == MEM_AIM_READY) {
                head = XS("Через память — готово");
                headCol = C::Acc();
                detail = pathName(path);
            } else if (st == MEM_AIM_UNSUPPORTED) {
                head = XS("Через память нельзя — работает тач");
                headCol = C::Red();
                detail = reasonName(rsn);
            }

            const float avW = ImGui::GetContentRegionAvail().x;
            const float cardH = 96.f;
            auto p0 = ImGui::GetCursorScreenPos();
            auto* dl = ImGui::GetWindowDrawList();
            auto* fn = ImGui::GetFont();
            const float fs = ImGui::GetFontSize();
            CardBg(cardH);
            dl->AddText(fn, fs, {p0.x + Layout::Inset + Layout::PadX, p0.y + 24.f}, C::U(headCol), head);
            if (detail && detail[0])
                dl->AddText(fn, fs * 0.9f, {p0.x + Layout::Inset + Layout::PadX, p0.y + 58.f},
                            C::U(C::Dim()), detail);
            ImGui::Dummy({avW, cardH});
        }

        SHdr(XS("Аим"));
        CardBg(Layout::RowH * 3);
        ToggleRow("##ta1", XS("Аим"),          &g_state.aim_touch,   g_state.a_aim_touch, false, true);
        ToggleRow("##ta6", XS("Только в прицеле"), &g_state.aim_scope_only, g_state.a_aim_scope, false);
        ToggleRow("##ta3", XS("Круг FOV"),         &g_state.aim_special, g_state.a_aim_spec,  true);

        SHdr(XS("Наводка"));
        CardBg(Layout::SliderH * 2);
        SliderRow("##afov", XS("Радиус"), &g_state.gun_fov, 5.f, 180.f, XS("%.0f°"), false, true, g_state.sl_gun_fov, dt);
        SliderRow("##asmt", XS("Скорость"), &g_state.gun_str, 1.f, 10.f, "%.0f", true, false, g_state.sl_gun_str, dt);

        // ---- Тач-зона аима -------------------------------------------------
        // Где именно лежит палец, которым аимбот водит камеру. Выбирается
        // тапом по экрану — как зоны автофарма, только точка одна: меню
        // прячется, первый тап записывает её в долях экрана.
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
