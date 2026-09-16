// ui/sheet.cpp — Нижняя шторка и кнопки выхода.
//
// Модуль разрезан из прежнего монолита jni/src/main.cpp;
// что здесь лежит и кто это зовёт — в шапке ui/sheet.h и в docs/CODE_MAP.md.


#include "app/common.h"
#include "aim/update.h"
#include "app/lifecycle.h"
#include "farm/controller.h"
#include "ui/layout.h"
#include "ui/popover.h"
#include "ui/scroll.h"
#include "ui/tabs.h"
#include "ui/theme.h"
#include "ui/toast.h"
#include "ui/widgets.h"
#include "ui/window.h"
#include "ui/sheet.h"

Sheet g_sheet;

void SheetOpen(const char* title, int type, bool* bp = nullptr, float* ap = nullptr,
               float* sp = nullptr, float mn = 0, float mx = 1, const char* fmt = "%.1f") {
    if (g_sheet.visible) return;
    snprintf(g_sheet.title, sizeof(g_sheet.title), "%s", title);
    g_sheet.type    = type; g_sheet.boolP = bp; g_sheet.animP = ap;
    g_sheet.slP     = sp;   g_sheet.slMin = mn; g_sheet.slMax = mx; g_sheet.slFmt = fmt;
    g_sheet.visible = true;  g_sheet.anim  = 0.f; g_sheet.vel = 0.f;
    g_sheet.closing = false; g_sheet.openFrames = 0;
}

void SheetClose() {
    if (!g_sheet.visible || g_sheet.closing) return;
    if (g_sheet.openFrames < 4) return;
    g_sheet.closing = true;
}

void DrawExitButtons(ImDrawList* fg, ImFont* fn, float fs, float alpha,
                             float b1X, float b2X, float bW, float bY, float btnH,
                             bool blocked,
                             const ImVec2& mousePos, const ImVec2& clickedPos,
                             bool mouseReleased,
                             bool isSheet) {
    fg->AddRectFilled({b1X, bY}, {b1X + bW, bY + btnH}, C::UA(C::Txt(), 0.08f * alpha), R::Btn);
    fg->AddRectFilled({b2X, bY}, {b2X + bW, bY + btnH}, C::UA(C::Red(), alpha), R::Btn);
    if (g_state.ui_show_sep) {
        fg->AddRect({b1X, bY}, {b1X + bW, bY + btnH}, C::UA(C::Txt(), 0.22f * alpha), R::Btn, 0, 1.2f);
        fg->AddRect({b2X, bY}, {b2X + bW, bY + btnH}, C::UA(C::Txt(), 0.22f * alpha), R::Btn, 0, 1.2f);
    }
    auto c1Sz = fn->CalcTextSizeA(fs * 1.15f, FLT_MAX, 0, XS("Отмена"));
    auto c2Sz = fn->CalcTextSizeA(fs * 1.15f, FLT_MAX, 0, XS("Выйти"));
    fg->AddText(fn, fs * 1.15f, {b1X + bW * 0.5f - c1Sz.x * 0.5f, bY + (btnH - fs * 1.15f) * 0.5f},
        C::UA(C::Txt(), alpha), XS("Отмена"));
    fg->AddText(fn, fs * 1.15f, {b2X + bW * 0.5f - c2Sz.x * 0.5f, bY + (btnH - fs * 1.15f) * 0.5f},
        IM_COL32(255, 255, 255, int(255 * alpha)), XS("Выйти"));
    if (!blocked && mouseReleased) {
        bool dH1 = clickedPos.x >= b1X && clickedPos.x <= b1X+bW && clickedPos.y >= bY && clickedPos.y <= bY+btnH;
        bool dH2 = clickedPos.x >= b2X && clickedPos.x <= b2X+bW && clickedPos.y >= bY && clickedPos.y <= bY+btnH;
        bool uH1 = mousePos.x >= b1X && mousePos.x <= b1X+bW && mousePos.y >= bY && mousePos.y <= bY+btnH;
        bool uH2 = mousePos.x >= b2X && mousePos.x <= b2X+bW && mousePos.y >= bY && mousePos.y <= bY+btnH;
        if (uH1 && dH1) { if (isSheet) SheetClose(); else PopoverClose(); }
        if (uH2 && dH2) main_thread_flag = false;
    }
}

void DrawSheet(float dt, ImVec2 menuPos, float WW, float WH) {
    if (!g_sheet.visible) return;

    auto* fg = ImGui::GetForegroundDrawList();
    auto& io = ImGui::GetIO();
    auto* fn = ImGui::GetFont();
    float fs = ImGui::GetFontSize();

    g_sheet.openFrames++;

    if (!TickSlideAnim(g_sheet.anim, g_sheet.vel, g_sheet.closing, dt)) {
        g_sheet.anim = 0.f; g_sheet.vel = 0.f;
        g_sheet.visible = false; g_sheet.closing = false;
        return;
    }

    float easeT  = EaseInOut(g_sheet.anim);
    float sheetH = (g_sheet.type == 2) ? 280.f : 380.f;
    float sheetW = WW - 24.f;
    float sheetX = menuPos.x + (WW - sheetW) * 0.5f;
    float sheetYFull   = (g_sheet.type == 2)
        ? menuPos.y + (WH - sheetH) * 0.5f
        : menuPos.y + WH - sheetH - 16.f;
    float sheetYHidden = menuPos.y + WH;
    float sheetY = Lerpf(sheetYHidden, sheetYFull, easeT);

    {
        const float fgR = R::Sheet;
        int ba = int(Lerpf(0.f, 180.f, easeT));
        fg->AddRectFilled(menuPos, {menuPos.x + WW, menuPos.y + WH}, C::UA(C::Bg(), ba / 255.f), fgR);
        fg->AddRectFilled({menuPos.x + 2, menuPos.y + 2}, {menuPos.x + WW - 2, menuPos.y + WH - 2}, C::UA(C::Bg(), ba * 0.6f / 255.f), fgR - 1);
        fg->AddRectFilled({menuPos.x + 4, menuPos.y + 4}, {menuPos.x + WW - 4, menuPos.y + WH - 4}, C::UA(C::Bg(), ba * 0.4f / 255.f), fgR - 2);
        fg->AddRectFilled(menuPos, {menuPos.x + WW, menuPos.y + WH}, IM_COL32(20, 20, 40, int(Lerpf(0.f, 60.f, easeT))), fgR);
        fg->AddRect(menuPos, {menuPos.x + WW, menuPos.y + WH}, IM_COL32(255, 255, 255, int(Lerpf(0.f, 40.f, easeT))), fgR, 0, 1.5f);
    }

    fg->PushClipRect({menuPos.x - 40.f, menuPos.y + 1.f}, {menuPos.x + WW + 40.f, menuPos.y + WH - 1.f}, true);

    fg->AddRectFilled({sheetX, sheetY}, {sheetX + sheetW, sheetY + sheetH}, C::U(C::Card()), R::Sheet);

    {
        float dhW = 60.f, dhH = 6.f;
        float dhX = sheetX + sheetW * 0.5f - dhW * 0.5f, dhY = sheetY + 12.f;
        fg->AddRectFilled({dhX, dhY}, {dhX + dhW, dhY + dhH}, C::UA(C::Dim(), 0.4f * easeT), dhH * 0.5f);
        bool inHandle = io.MousePos.x >= sheetX && io.MousePos.x <= sheetX + sheetW
                     && io.MousePos.y >= sheetY  && io.MousePos.y <= sheetY + 44.f;
        if (g_sheet.type != 2) {
            if (!g_sheet.closing && inHandle && io.MouseDown[0] && io.MouseDelta.y > 3.f)
                SheetClose();
        } else {
            if (io.MouseClicked[0] && inHandle) {
                g_win.dragging   = true;
                g_win.touchStart = io.MousePos;
                g_win.posStart   = g_win.pos;
            }
        }
    }

    float titleFS = fs * 1.45f;
    {
        auto tsz   = fn->CalcTextSizeA(titleFS, FLT_MAX, 0, g_sheet.title);
        fg->AddText(fn, titleFS, {sheetX + sheetW * 0.5f - tsz.x * 0.5f, sheetY + 32.f}, C::U(C::Txt()), g_sheet.title);
    }

    if (g_sheet.type != 2) {
        float cR  = 17.f, cx2 = sheetX + sheetW - cR - 16.f, cy2 = sheetY + 32.f + titleFS * 0.5f;
        fg->AddCircleFilled({cx2, cy2}, cR, C::U(C::Sep()), 32);
        float cs = 6.f;
        fg->AddLine({cx2 - cs, cy2 - cs}, {cx2 + cs, cy2 + cs}, C::U(C::Dim()), 2.4f);
        fg->AddLine({cx2 + cs, cy2 - cs}, {cx2 - cs, cy2 + cs}, C::U(C::Dim()), 2.4f);
        if (!g_sheet.closing && io.MouseClicked[0] &&
            fabsf(io.MousePos.x - cx2) < cR + 8.f && fabsf(io.MousePos.y - cy2) < cR + 8.f)
            SheetClose();
    }

    {
        float lineY = sheetY + 32.f + titleFS + 14.f;
        fg->AddLine({sheetX + 18.f, lineY}, {sheetX + sheetW - 18.f, lineY}, C::U(C::Sep()), 0.8f);
    }

    float cY = sheetY + 32.f + titleFS + 26.f;
    float padX = 28.f;

    if (g_sheet.type == 0 && g_sheet.boolP) {
        bool*  v  = g_sheet.boolP;
        float* a  = g_sheet.animP;
        float  t  = EaseInOut(*a);
        float  tW = 72.f, tH = 44.f;
        float  tx = sheetX + sheetW * 0.5f - tW * 0.5f, tcy = cY + 20.f + tH * 0.5f;
        DrawToggle(fg, tx, tcy, t);
        if (!g_sheet.closing && io.MouseClicked[0]) {
            if (io.MousePos.x >= tx - 12 && io.MousePos.x <= tx + tW + 12
             && io.MousePos.y >= cY + 8.f && io.MousePos.y <= cY + tH + 32.f)
                { *v=!*v; char _b[160]; snprintf(_b,sizeof(_b),XS("%s|%s"),g_sheet.title,*v?XS("Вкл"):XS("Выкл")); ShowToast(_b); }
        }
        const char* st = *v ? XS("Включено") : XS("Выключено");
        auto stSz = fn->CalcTextSizeA(fs * 1.1f, FLT_MAX, 0, st);
        fg->AddText(fn, fs * 1.1f, {sheetX + sheetW * 0.5f - stSz.x * 0.5f, cY + tH + 26.f},
            *v ? C::U(C::Acc()) : C::U(C::Dim()), st);
    } else if (g_sheet.type == 1 && g_sheet.slP) {
        float* v = g_sheet.slP; float mn = g_sheet.slMin, mx = g_sheet.slMax;
        char vb[24]; snprintf(vb, 24, g_sheet.slFmt, *v);
        auto vSz = fn->CalcTextSizeA(fs * 2.0f, FLT_MAX, 0, vb);
        fg->AddText(fn, fs * 2.0f, {sheetX + sheetW * 0.5f - vSz.x * 0.5f, cY + 10.f}, C::U(C::Acc()), vb);
        float trkY = cY + 78.f, trkX = sheetX + padX + 16.f;
        float trkW = sheetW - padX * 2.f - 32.f, kR2 = 20.f, trkH = 8.f;
        float frac = Clamp01((*v - mn) / (mx - mn)), kx = trkX + frac * trkW;
        fg->AddRectFilled({trkX, trkY - trkH * 0.5f}, {trkX + trkW, trkY + trkH * 0.5f}, C::U(C::TrkOff()), trkH);
        fg->AddRectFilled({trkX, trkY - trkH * 0.5f}, {kx,           trkY + trkH * 0.5f}, C::U(C::Acc()), trkH);
        static bool s_sheetSliderActive = false;
        if (!g_sheet.closing && io.MouseClicked[0] && fabsf(io.MousePos.y - trkY) < 56.f
            && io.MousePos.x >= trkX - 24.f && io.MousePos.x <= trkX + trkW + 24.f)
            s_sheetSliderActive = true;
        if (!io.MouseDown[0]) s_sheetSliderActive = false;
        bool near = s_sheetSliderActive && !g_sheet.closing && io.MouseDown[0];
        fg->AddCircleFilled({kx + 0.7f, trkY + 1.5f}, kR2, IM_COL32(0, 0, 0, 40));
        fg->AddCircleFilled({kx, trkY}, kR2, IM_COL32(255, 255, 255, 255));
        if (near) fg->AddCircle({kx, trkY}, kR2 + 4.f, C::UA(C::Light::Acc, 0.85f), 48, 3.f);
        if (near) { float dx = io.MousePos.x - trkX; *v = mn + Clamp01(dx / trkW) * (mx - mn); }
        char minB[16], maxB[16];
        snprintf(minB, 16, g_sheet.slFmt, mn); snprintf(maxB, 16, g_sheet.slFmt, mx);
        fg->AddText(fn, fs * 0.85f, {trkX, trkY + kR2 + 8.f}, C::U(C::Dim()), minB);
        auto mxSz = fn->CalcTextSizeA(fs * 0.85f, FLT_MAX, 0, maxB);
        fg->AddText(fn, fs * 0.85f, {trkX + trkW - mxSz.x, trkY + kR2 + 8.f}, C::U(C::Dim()), maxB);
    } else if (g_sheet.type == 2) {
        auto dSz = fn->CalcTextSizeA(fs * 1.15f, FLT_MAX, 0, XS("Приложение будет закрыто."));
        fg->AddText(fn, fs * 1.15f, {sheetX + sheetW * 0.5f - dSz.x * 0.5f, cY + 16.f},
            C::U(C::Dim()), XS("Приложение будет закрыто."));
    }

    {
        float btnH = 62.f, btnY = sheetY + sheetH - btnH - 24.f;
        if (g_sheet.type == 2) {
            float bW  = (sheetW - padX * 2.f - 12.f) * 0.5f;
            float b1X = sheetX + padX, b2X = b1X + bW + 12.f;
            DrawExitButtons(fg, fn, fs, 1.f,
                b1X, b2X, bW, btnY, btnH,
                g_sheet.closing,
                io.MousePos, io.MouseClickedPos[0],
                io.MouseReleased[0], true);
        } else {
            float bW = sheetW - padX * 2.f, bX = sheetX + padX;
            fg->AddRectFilled({bX, btnY}, {bX + bW, btnY + btnH}, C::U(C::Acc()), R::Btn);
            auto bSz = fn->CalcTextSizeA(fs * 1.15f, FLT_MAX, 0, XS("Готово"));
            fg->AddText(fn, fs * 1.15f, {bX + bW * 0.5f - bSz.x * 0.5f, btnY + (btnH - fs * 1.15f) * 0.5f},
                IM_COL32(255, 255, 255, 255), XS("Готово"));
            if (!g_sheet.closing && io.MouseClicked[0]
                && io.MousePos.x >= bX && io.MousePos.x <= bX + bW
                && io.MousePos.y >= btnY && io.MousePos.y <= btnY + btnH)
                SheetClose();
        }
    }
    fg->PopClipRect();
}
