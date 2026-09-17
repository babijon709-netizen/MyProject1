#include "widgets.h"
#include "theme.h"
#include "ui_util.h"
#include "cfg.h"
#include "str.h"
#include "hud.h"                 // ShowToast
#include "audio.h"               // PlaySound
#include "config.h"
#include "menu.h"                // g_calibMode (калибровка зон)
#include "Blur/Blur.h"
#include "lang.h"
#include <cfloat>
#include <cmath>

ScrollState g_scrollMain;
ScrollState g_scrollPop;
WindowState g_win;
Sheet g_sheet;
Popover g_pop;


// Позиция «пилюли» активной вкладки на нижней панели (экранный X) + пружина.


void ScrollTick(ScrollState& s, bool mIn, bool blocked, float& maxScroll, float dt) {
    auto& io = ImGui::GetIO();

    if (!io.MouseDown[0]) {
        s.drag     = false;
        s.dragging = false;
        s.axis     = 0;
    }

    if (!blocked && mIn && io.MouseDown[0]) {
        if (io.MouseClicked[0]) {
            s.lastTy = io.MousePos.y;
            s.vel    = 0.f;
            s.axis   = 0;
        }

        if (s.axis == 0) {
            float dx = fabsf(io.MousePos.x - io.MouseClickedPos[0].x);
            float dy = fabsf(io.MousePos.y - io.MouseClickedPos[0].y);
            if (dx > 22.f || dy > 22.f)
                s.axis = (dy >= dx) ? 1 : 2;
            s.lastTy = io.MousePos.y;
        }

        if (s.axis == 1) {
            if (!s.drag) {
                s.drag     = true;
                s.dragging = true;
                s.lastTy   = io.MousePos.y;
                s.vel      = 0.f;
            }
            float dy = io.MousePos.y - s.lastTy;
            s.lastTy = io.MousePos.y;
            s.off    = ImClamp(s.off - dy, 0.f, maxScroll);
            float targetV = -dy * 18.f;
            s.vel = s.vel * 0.8f + targetV * 0.2f;
        }

        if (s.axis == 2) {
            s.lastTy = io.MousePos.y;
            s.vel    = 0.f;
        }
    }

    if (mIn && !blocked && io.MouseWheel != 0.f) {
        s.vel = 0.f;
        s.off = ImClamp(s.off - io.MouseWheel * 50.f, 0.f, maxScroll);
    }

    if (!s.drag && fabsf(s.vel) > 0.3f) {
        s.off += s.vel * dt;
        s.vel *= 0.88f;
    }

    s.off = ImClamp(s.off, 0.f, maxScroll);
    if (s.off <= 0.f || s.off >= maxScroll) s.vel = 0.f;

    bool isScrolling = s.drag || fabsf(s.vel) > 0.5f;
    if (isScrolling) s.sb_idle = 0.f;
    else s.sb_idle += dt;
    float sbTarget = (isScrolling || s.sb_idle < 0.8f) ? 1.f : 0.f;
    float sbSpd = sbTarget > s.sb_alpha ? 6.f : 3.5f;
    s.sb_alpha += (sbTarget - s.sb_alpha) * sbSpd * dt;
    s.sb_alpha = ImClamp(s.sb_alpha, 0.f, 1.f);
}







void SheetOpen(const char* title, int type, bool* bp, float* ap,
               float* sp, float mn, float mx, const char* fmt) {
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


static ImVec2 g_popOpenClickPos = {-9999.f, -9999.f};
static ImVec4* g_colP = nullptr;
static bool    g_palSelReset = false;


static const ImVec4 kColorPalette[] = {
    {1.00f, 0.20f, 0.20f, 1.f}, {1.00f, 0.50f, 0.50f, 1.f}, {0.80f, 0.05f, 0.05f, 1.f},
    {1.00f, 0.60f, 0.10f, 1.f}, {1.00f, 0.75f, 0.30f, 1.f}, {0.90f, 0.40f, 0.00f, 1.f},
    {1.00f, 0.95f, 0.10f, 1.f}, {1.00f, 0.85f, 0.50f, 1.f}, {0.85f, 0.75f, 0.00f, 1.f},
    {0.20f, 0.85f, 0.35f, 1.f}, {0.50f, 1.00f, 0.50f, 1.f}, {0.05f, 0.55f, 0.15f, 1.f},
    {0.10f, 0.90f, 0.90f, 1.f}, {0.40f, 0.85f, 1.00f, 1.f}, {0.00f, 0.60f, 0.70f, 1.f},
    {0.10f, 0.50f, 1.00f, 1.f}, {0.50f, 0.70f, 1.00f, 1.f}, {0.00f, 0.25f, 0.75f, 1.f},
    {0.65f, 0.20f, 1.00f, 1.f}, {0.80f, 0.55f, 1.00f, 1.f}, {0.45f, 0.05f, 0.70f, 1.f},
    {1.00f, 0.30f, 0.80f, 1.f}, {1.00f, 0.60f, 0.90f, 1.f}, {0.80f, 0.05f, 0.50f, 1.f},
    {1.00f, 1.00f, 1.00f, 1.f}, {0.70f, 0.70f, 0.70f, 1.f}, {0.35f, 0.35f, 0.35f, 1.f},
    {0.12f, 0.12f, 0.12f, 1.f},
};
static constexpr int kPaletteCount = (int)(sizeof(kColorPalette)/sizeof(kColorPalette[0]));

void PopoverOpenColor(const char* title, ImVec4* cp) {
    if (g_pop.visible) return;
    snprintf(g_pop.title, sizeof(g_pop.title), "%s", title);
    g_pop.sectionId  = 4;
    g_pop.visible    = true;
    g_pop.closing    = false;
    g_pop.anim       = 0.f;
    g_pop.vel        = 0.f;
    g_pop.openFrames = 0;
    g_scrollPop      = {};
    g_colP = cp;
    g_palSelReset = true;
    g_popOpenClickPos = ImGui::GetIO().MousePos;
    auto& _io = ImGui::GetIO();
    Blur::Freeze((int)_io.DisplaySize.x, (int)_io.DisplaySize.y,
                 (int)g_win.pos.x, (int)g_win.pos.y,
                 (int)g_win.w,     (int)g_win.h);
}

void PopoverOpen(const char* title, int sid) {
    // Переоткрытие поверх ЗАКРЫВАЮЩЕГОСЯ окна должно работать: калибровка
    // зон запускается тапом из окна (оно уходит в closing), рисование меню
    // замирает на время калибровки, и к её завершению окно всё ещё
    // "visible+closing". Старый ранний выход здесь молча съедал реоткрытие —
    // пользователя выкидывало на голую вкладку «Разное».
    if (g_pop.visible) {
        if (!g_pop.closing) return;
        g_pop.visible = false; // прервать закрытие и открыть заново
    }
    snprintf(g_pop.title, sizeof(g_pop.title), "%s", title);
    g_colP = nullptr; // обычное окно, не палитра
    g_pop.sectionId  = sid;
    g_pop.visible    = true;
    g_pop.closing    = false;
    g_pop.anim       = 0.f;
    g_pop.vel        = 0.f;
    g_pop.openFrames = 0;
    g_scrollPop      = {};
    auto& _io = ImGui::GetIO();
    Blur::Freeze((int)_io.DisplaySize.x, (int)_io.DisplaySize.y,
                 (int)g_win.pos.x, (int)g_win.pos.y,
                 (int)g_win.w,     (int)g_win.h);
}

void PopoverClose() {
    if (!g_pop.visible || g_pop.closing) return;
    if (g_pop.openFrames < 1) return;
    g_pop.closing = true;
    g_input.touchConsumed = true;
}

void DrawToggle(ImDrawList* dl, float ax, float cy, float t) {
    const float tW = 72.f, tH = 42.f, tR = tH * 0.5f, kR = tR - 3.5f;
    float tx = ax, ty = cy - tH * 0.5f;
    dl->AddRectFilled({tx, ty}, {tx + tW, ty + tH}, C::Mix(C::TrkOff(), C::Acc(), t), tR);
    float kx = Lerpf(tx + tR + 2.f, tx + tW - tR - 2.f, t);
    dl->AddCircleFilled({kx + 0.5f, cy + 1.5f}, kR + 1.f, IM_COL32(0, 0, 0, 35));
    dl->AddCircleFilled({kx, cy}, kR, IM_COL32(255, 255, 255, 255));
}

static void RenderToggleRowVisuals(ImDrawList* dl, ImFont* fn, float fs,
                                    float rowX, float rowY, float rowW,
                                    const char* lbl, float animT, float alpha,
                                    bool showSep, bool last) {
    const float inset = Layout::Inset, padX = Layout::PadX;
    float cX = rowX + inset, cW = rowW - inset * 2.f;
    const float rowH = Layout::RowH;
    float textX    = cX + padX;
    float textMaxX = cX + cW - 72.f - padX - 16.f;
    float textY    = rowY + (rowH - fs * 1.15f) * 0.5f;

    dl->PushClipRect({textX, rowY}, {textMaxX, rowY + rowH}, true);
    dl->AddText(fn, fs * 1.15f, {textX, textY}, C::UA(C::Txt(), alpha), lbl);
    dl->PopClipRect();

    DrawToggle(dl, cX + cW - 72.f - padX, rowY + rowH * 0.5f, EaseInOut(animT));

    if (!last && showSep)
        dl->AddLine({cX + padX, rowY + rowH - 0.5f},
                    {cX + cW - padX, rowY + rowH - 0.5f}, C::UA(C::Sep(), alpha), 0.8f);
}

static void RenderSliderVisuals(ImDrawList* dl, ImFont* fn, float fs,
                                 float rowX, float rowY, float rowW, float rowH,
                                 const char* lbl, const char* fmt, float v, float animPos,
                                 bool active, float alpha, bool showSep, bool last) {
    const float inset = Layout::Inset, padX = Layout::PadX;
    const float tH = 11.f, kR = 22.f, padH = 38.f;
    float cX = rowX + inset, cW = rowW - inset * 2.f;

    char valBuf[24]; snprintf(valBuf, 24, fmt, v);
    auto vsz   = fn->CalcTextSizeA(fs, FLT_MAX, 0, valBuf);
    float textY = rowY + padH * 0.5f;

    float sX = cX + padX + kR, sW = cW - padX * 2.f - kR * 2.f;
    float tY = rowY + rowH - padH;
    float kx = sX + animPos * sW;

    dl->PushClipRect({cX + padX, rowY}, {cX + cW - vsz.x - padX * 2.f, rowY + rowH * 0.55f}, true);
    dl->AddText(fn, fs * 1.15f, {cX + padX, textY}, C::UA(C::Txt(), alpha), lbl);
    dl->PopClipRect();
    dl->AddText(fn, fs * 1.15f, {cX + cW - vsz.x - padX, textY}, C::UA(C::Acc(), alpha), valBuf);

    dl->AddRectFilled({sX, tY - tH * 0.5f}, {sX + sW, tY + tH * 0.5f}, C::UA(C::TrkOff(), alpha), tH);
    dl->AddRectFilled({sX, tY - tH * 0.5f}, {kx,      tY + tH * 0.5f}, C::UA(C::Acc(),    alpha), tH);

    const float knobR = 16.f;
    dl->AddCircleFilled({kx + 0.5f, tY + 1.5f}, knobR + 1.f, IM_COL32(0, 0, 0, int(30 * alpha)));
    dl->AddCircleFilled({kx, tY}, knobR, IM_COL32(255, 255, 255, int(255 * alpha)));
    if (active)
        dl->AddCircle({kx, tY}, knobR + 4.f, C::UA(C::Light::Acc, 0.85f * alpha), 48, 3.f);

    if (!last && showSep)
        dl->AddLine({cX + padX, rowY + rowH - 0.5f},
                    {cX + cW - padX, rowY + rowH - 0.5f}, C::UA(C::Sep(), alpha), 0.8f);
}

bool ToggleRow(const char* id, const char* lbl, bool* v, float& anim,
               bool last, bool first) {
    auto* dl  = ImGui::GetWindowDrawList();
    float avW = ImGui::GetContentRegionAvail().x;
    const float rowH = Layout::RowH;
    auto pos = ImGui::GetCursorScreenPos();

    ImGui::InvisibleButton(id, {avW, rowH});
    bool popBlocking = (g_pop.visible && !g_pop.closing) || g_sheet.visible;

    bool triggered = WasTappedHere() && !popBlocking && !IsScrollDragging() && !g_input.touchConsumed;
    if (triggered) { *v=!*v; char _b[160]; snprintf(_b,sizeof(_b),XS("%s|%s"),lbl,*v?XS("Вкл"):XS("Выкл")); ShowToast(_b); PlaySound(SND_CLICK); }

    RenderToggleRowVisuals(dl, ImGui::GetFont(), ImGui::GetFontSize(),
                           pos.x, pos.y, avW, lbl, anim, 1.f,
                           g_state.ui_show_sep, last);
    return triggered;
}

static void TickSliderAnim(AppState::SliderAnim& anim, float target, bool act, float dt) {
    if (anim.pos < 0.f) { anim.pos = target; anim.vel = 0.f; }
    const float stiff = act ? 900.f : 280.f;
    const float damp  = act ?  60.f :  28.f;
    float force = stiff * (target - anim.pos) - damp * anim.vel;
    anim.vel += force * dt;
    anim.pos += anim.vel * dt;
    anim.pos  = ImClamp(anim.pos, 0.f, 1.f);
    if (fabsf(target - anim.pos) < 0.0002f && fabsf(anim.vel) < 0.001f) {
        anim.pos = target; anim.vel = 0.f;
    }
}

bool SliderRow(const char* id, const char* lbl, float* v, float mn, float mx,
               const char* fmt, bool last, bool first,
               AppState::SliderAnim& anim, float dt) {
    auto* dl  = ImGui::GetWindowDrawList();
    auto& io  = ImGui::GetIO();
    float avW = ImGui::GetContentRegionAvail().x;
    const float rowH = Layout::SliderH, kR = 22.f;
    const float inset = Layout::Inset, padX = Layout::PadX;
    auto pos = ImGui::GetCursorScreenPos();

    ImGui::InvisibleButton(id, {avW, rowH});
    bool itemActive = ImGui::IsItemActive();

    int scrollAxis = g_scrollMain.axis;
    bool scrollVertical = g_scrollMain.dragging || (itemActive && scrollAxis == 1);
    bool axisUndecided  = itemActive && scrollAxis == 0 && io.MouseDown[0];

    bool act = itemActive && !scrollVertical && !axisUndecided;

    if (act)
        *v = mn + Clamp01((io.MousePos.x - (pos.x + inset + padX + kR)) / (avW - inset * 2.f - padX * 2.f - kR * 2.f)) * (mx - mn);

    float target = Clamp01((*v - mn) / (mx - mn));

    TickSliderAnim(anim, target, act, dt);

    RenderSliderVisuals(dl, ImGui::GetFont(), ImGui::GetFontSize(),
                        pos.x, pos.y, avW, rowH,
                        lbl, fmt, *v, anim.pos, act, 1.f,
                        g_state.ui_show_sep, last);
    return act;
}

void CardBg(float h, ImDrawFlags flags) {
    auto* dl  = ImGui::GetWindowDrawList();
    auto  p0  = ImGui::GetCursorScreenPos();
    float w   = ImGui::GetContentRegionAvail().x;
    const float inset = Layout::Inset;
    ImVec2 bMin = {p0.x + inset, p0.y};
    ImVec2 bMax = {p0.x + w - inset, p0.y + h};
    dl->AddRectFilled(bMin, bMax, C::U(C::Card()), R::Card, flags);
    if (g_state.ui_show_sep)
        dl->AddRect(bMin, bMax, C::U(C::Sep()), R::Card, flags, 1.2f);
}


void SHdr(const char* t, float top) {
    ImGui::Dummy({1.f, top});
    if (t && t[0]) {
        auto*  dl  = ImGui::GetWindowDrawList();
        auto*  fn  = ImGui::GetFont();
        float  fs  = ImGui::GetFontSize() * 1.15f;
        auto   pos = ImGui::GetCursorScreenPos();
        auto   tsz = fn->CalcTextSizeA(fs, FLT_MAX, 0, t);
        // Акцентная «капсула» слева от заголовка секции — визуально
        // связывает секции с индикатором активной вкладки в рейле.
        dl->AddRectFilled({pos.x + 18.f, pos.y + tsz.y * 0.14f},
                          {pos.x + 24.f, pos.y + tsz.y * 0.86f}, C::U(C::Acc()), 3.f);
        dl->AddText(fn, fs, {pos.x + 34.f, pos.y}, C::U(C::Dim()), t);
        ImGui::Dummy({1.f, tsz.y});
    }
    ImGui::Dummy({1.f, 8.f});
}


bool CollapsibleHeader(const char* id, const char* lbl, int secId) {
    auto* dl  = ImGui::GetWindowDrawList();
    float avW = ImGui::GetContentRegionAvail().x;
    const float rowH = Layout::RowH, inset = Layout::Inset, padX = Layout::PadX;
    auto pos = ImGui::GetCursorScreenPos();

    dl->AddRectFilled({pos.x + inset, pos.y}, {pos.x + avW - inset, pos.y + rowH}, C::U(C::Card()), R::Card);
    if (g_state.ui_show_sep)
        dl->AddRect({pos.x + inset, pos.y}, {pos.x + avW - inset, pos.y + rowH}, C::U(C::Sep()), R::Card, 0, 1.2f);

    ImGui::InvisibleButton(id, {avW, rowH});
    bool clicked = WasTappedHere() && !IsScrollDragging() && !g_input.touchConsumed;

    float fs    = ImGui::GetFontSize() * 1.15f;
    float textY = pos.y + (rowH - fs) * 0.5f;
    dl->AddText(ImGui::GetFont(), fs, {pos.x + inset + padX, textY}, C::U(C::Txt()), lbl);

    const float cBtnR = 22.f;
    float cx2 = pos.x + avW - inset - cBtnR - padX * 0.5f, cy2 = pos.y + rowH * 0.5f;
    dl->AddCircleFilled({cx2 + 0.5f, cy2 + 1.5f}, cBtnR + 1.f, IM_COL32(0, 0, 0, 28), 48);
    dl->AddCircleFilled({cx2, cy2}, cBtnR, C::U(C::Acc()), 48);
    {
        float t = EaseInOut(g_themeT);
        ImU32 ringCol = IM_COL32(
            int(Lerpf(85,  255, t)),
            int(Lerpf(70,  255, t)),
            int(Lerpf(200, 255, t)),
            int(Lerpf(130,  60, t))
        );
        dl->AddCircle({cx2, cy2}, cBtnR + 2.5f, ringCol, 48, 1.5f);
    }

    float chs = 10.f, thk = 3.f;
    dl->AddLine({cx2 - chs * 0.45f, cy2 - chs * 0.8f}, {cx2 + chs * 0.55f, cy2}, IM_COL32(255, 255, 255, 250), thk);
    dl->AddLine({cx2 + chs * 0.55f, cy2}, {cx2 - chs * 0.45f, cy2 + chs * 0.8f}, IM_COL32(255, 255, 255, 250), thk);

    if (clicked && !(g_pop.visible && !g_pop.closing)) {
        PopoverOpen(lbl, secId);
        g_popOpenClickPos = ImGui::GetIO().MousePos;
        PlaySound(SND_CLICK);
    }
    return clicked;
}


static void DrawExitButtons(ImDrawList* fg, ImFont* fn, float fs, float alpha,
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

static float DrawPopoverContentFG(ImDrawList* fg, ImFont* fn, float fs, int secId, float dt,
                                   float cX, float cW, float curY, float alpha,
                                   bool blocked, const ImVec2& mousePos, const ImVec2& clickedPos,
                                   bool mouseReleased, bool mouseDown)
{
    const float inset = Layout::Inset, padX = Layout::PadX;
    const float rH = Layout::RowH;

    auto FgSHdr = [&](const char* t, float topPad = 18.f) {
        curY += topPad;
        if (t && t[0]) {
            auto tsz = fn->CalcTextSizeA(fs * 1.15f, FLT_MAX, 0, t);
            fg->AddText(fn, fs * 1.15f, {cX + 32.f, curY}, C::UA(C::Dim(), alpha), t);
            curY += tsz.y + 8.f;
        } else {
            curY += 8.f;
        }
    };

    auto FgCardBg = [&](float h, ImDrawFlags flags = ImDrawFlags_RoundCornersAll) {
        fg->AddRectFilled({cX + inset, curY}, {cX + cW - inset, curY + h}, C::UA(C::Card(), alpha), R::Card, flags);
        if (g_state.ui_show_sep)
            fg->AddRect({cX + inset, curY}, {cX + cW - inset, curY + h}, C::UA(C::Sep(), alpha), R::Card, flags, 1.2f);
    };

    auto FgToggleRow = [&](const char* lbl, bool* v, float& anim, bool last) {
        Tick(anim, *v, dt, 12.f);
        RenderToggleRowVisuals(fg, fn, fs, cX, curY, cW, lbl, anim, alpha,
                               g_state.ui_show_sep, last);
        if (!blocked && !g_scrollPop.dragging && mouseReleased
            && mousePos.x >= cX + inset && mousePos.x <= cX + cW - inset
            && mousePos.y >= curY && mousePos.y <= curY + rH
            && clickedPos.x >= cX + inset && clickedPos.x <= cX + cW - inset
            && clickedPos.y >= curY && clickedPos.y <= curY + rH) {
            *v = !*v;
            char _b[160]; snprintf(_b, sizeof(_b), XS("%s|%s"), lbl, *v ? XS("Вкл") : XS("Выкл")); ShowToast(_b);
            PlaySound(SND_CLICK);
        }
        curY += rH;
    };

    auto FgRadioRow = [&](int idx, int* cur, AppState::RadioAnim& ra, float* fillAnim,
                           const char* lbl, bool last) {
        bool isSelected = (*cur == idx);
        Tick(*fillAnim, isSelected, dt, 10.f);
        float fill = EaseInOut(*fillAnim);

        if (!blocked && mouseReleased
            && mousePos.x >= cX + inset && mousePos.x <= cX + cW - inset
            && mousePos.y >= curY && mousePos.y <= curY + rH
            && clickedPos.y >= curY && clickedPos.y <= curY + rH
            && !isSelected) {
            ra.scaleVel = -8.f;
            ra.ring = 0.f;
            ra.ringVel = 5.5f;
            *cur = idx;
            PlaySound(SND_CLICK);
        }
        SpringTick(ra.scale, ra.scaleVel, 1.0f, dt);
        ra.scale = ImMax(0.6f, ra.scale);
        SpringTick(ra.ring, ra.ringVel, 3.5f, dt);

        const float rR = 22.f;
        float textX = cX + inset + padX, textY = curY + (rH - fs * 1.15f) * 0.5f;
        float textMaxX = cX + cW - inset - rR * 2.f - padX - 16.f;
        fg->PushClipRect({textX, curY}, {textMaxX, curY + rH}, true);
        fg->AddText(fn, fs * 1.15f, {textX, textY}, C::UA(C::Txt(), alpha), lbl);
        fg->PopClipRect();

        float cx2 = cX + cW - inset - rR - padX, cy2 = curY + rH * 0.5f;
        float r2 = rR * ra.scale;

        if (ra.ring > 0.05f && ra.ring < 3.4f) {
            float ringR  = rR + ra.ring * 10.f;
            float ringA  = (1.f - ra.ring / 3.5f) * 0.55f * alpha;
            fg->AddCircle({cx2, cy2}, ringR, C::UA(C::Acc(), ringA), 48, 2.5f);
        }

        ImU32 radioCol = IM_COL32(
            int(Lerpf(C::TrkOff().x, C::Acc().x, fill)*255),
            int(Lerpf(C::TrkOff().y, C::Acc().y, fill)*255),
            int(Lerpf(C::TrkOff().z, C::Acc().z, fill)*255),
            int(alpha*255));
        fg->AddCircleFilled({cx2, cy2}, r2, radioCol, 40);

        if (fill > 0.01f)
            fg->AddCircleFilled({cx2, cy2}, r2 * 0.46f * fill,
                IM_COL32(255,255,255,int(255*fill*alpha)), 32);

        if (fill < 0.99f)
            fg->AddCircle({cx2, cy2}, r2, C::UA(C::Sep(), (1.f-fill)*alpha), 40, 1.8f);

        if (!last && g_state.ui_show_sep)
            fg->AddLine({cX + inset + padX, curY + rH - 0.5f},
                        {cX + cW - inset - padX, curY + rH - 0.5f}, C::UA(C::Sep(), alpha), 0.8f);
        curY += rH;
    };

    auto FgSliderRow = [&](const char* lbl, float* v, float mn, float mx, const char* fmt,
                            bool last, AppState::SliderAnim& anim) {
        const float rowH = Layout::SliderH;
        float cX2 = cX + inset, cW2 = cW - inset * 2.f;

        bool act = !blocked && mouseDown && !g_scrollPop.dragging
            && clickedPos.x >= cX2 && clickedPos.x <= cX2 + cW2
            && clickedPos.y >= curY && clickedPos.y <= curY + rowH;

        if (act)
            *v = mn + Clamp01((mousePos.x - (cX2 + padX + 22.f)) / (cW2 - padX * 2.f - 44.f)) * (mx - mn);

        float target = Clamp01((*v - mn) / (mx - mn));
        TickSliderAnim(anim, target, act, dt);

        RenderSliderVisuals(fg, fn, fs, cX, curY, cW, rowH,
                            lbl, fmt, *v, anim.pos, act, alpha,
                            g_state.ui_show_sep, last);
        curY += rowH;
    };

    if (secId == 0) {
        FgSHdr(XS("Куда целиться"));
        FgCardBg(rH * 3);
        FgRadioRow(0, &g_state.aim_bone, g_state.ra_aim_head,   &g_state.a_aim_head,   XS("Голова"), false);
        FgRadioRow(1, &g_state.aim_bone, g_state.ra_aim_chest,  &g_state.a_aim_chest,  XS("Шея"), false);
        FgRadioRow(2, &g_state.aim_bone, g_state.ra_aim_pelvis, &g_state.a_aim_pelvis, XS("Тело"), true);

        FgSHdr(XS("Выбор цели"));
        FgCardBg(rH * 3);
        FgRadioRow(0, &g_state.aim_priority, g_state.ra_aim_pr0, &g_state.a_aim_pr0, XS("Умный"), false);
        FgRadioRow(1, &g_state.aim_priority, g_state.ra_aim_pr1, &g_state.a_aim_pr1, XS("Ближе к прицелу"), false);
        FgRadioRow(2, &g_state.aim_priority, g_state.ra_aim_pr2, &g_state.a_aim_pr2, XS("Ближе ко мне"), true);

    } else if (secId == 1) {
        FgSHdr(XS("Линии и боксы"));
        FgCardBg(Layout::SliderH);
        FgSliderRow(XS("Толщина"),   &g_state.esp_thick, 0.5f, 5.f,   "%.1f",     true, g_state.sl_esp_thick);

    } else if (secId == 5) {
        // Автофарм: всё управление ботом в одном окне.
        // (secId 4 занят палитрой цветов — там скролл выключен.)
        FgSHdr(XS("Автофарм"));
        FgCardBg(rH * 1);
        FgToggleRow(XS("Автофарм"), &g_state.farm_on, g_state.a_farm_on, true);

        FgSHdr(XS("Что добывать"));
        FgCardBg(rH * 4);
        FgToggleRow(XS("Дерево"), &g_state.farm_wood,   g_state.a_farm_wood,   false);
        FgToggleRow(XS("Камень"), &g_state.farm_stone,  g_state.a_farm_stone,  false);
        FgToggleRow(XS("Металл"), &g_state.farm_metal,  g_state.a_farm_metal,  false);
        FgToggleRow(XS("Сера"),   &g_state.farm_sulfur, g_state.a_farm_sulfur, true);

        FgSHdr(XS("Дальность"));
        FgCardBg(Layout::SliderH);
        FgSliderRow(XS("Искать до"), &g_state.farm_range, 10.f, 300.f,
                    XS("%.0f м"), true, g_state.sl_farm_range);

        // Зоны бота: куда жать джойстик движения и кнопку огня.
        FgSHdr(XS("Зоны бота"));
        {
            struct ZoneRow {
                const char* lbl;
                int   calib;         // g_calibMode для этой зоны
                float zx, zy;        // сохранённые доли экрана (-1 = нет)
            };
            const ZoneRow zrows[2] = {
                {XS("Зона джойстика"), 1, g_state.farm_joy_x,  g_state.farm_joy_y},
                {XS("Зона огня"),      2, g_state.farm_fire_x, g_state.farm_fire_y},
            };

            FgCardBg(rH * 2);
            for (int zi = 0; zi < 2; ++zi) {
                const ZoneRow& z = zrows[zi];

                if (!blocked && !g_scrollPop.dragging && mouseReleased
                    && mousePos.x >= cX + inset && mousePos.x <= cX + cW - inset
                    && mousePos.y >= curY && mousePos.y <= curY + rH
                    && clickedPos.x >= cX + inset && clickedPos.x <= cX + cW - inset
                    && clickedPos.y >= curY && clickedPos.y <= curY + rH) {
                    g_calibMode = z.calib;
                    PopoverClose();
                    PlaySound(SND_CLICK);
                }

                float cy2 = curY + rH * 0.5f;
                fg->AddText(fn, fs * 1.15f,
                    {cX + inset + padX, cy2 - fs * 1.15f * 0.5f},
                    C::UA(C::Txt(), alpha), z.lbl);

                char st[32];
                bool set = z.zx >= 0.f;
                if (set) snprintf(st, sizeof(st), "%d%% %d%%", (int)(z.zx * 100.f), (int)(z.zy * 100.f));
                else     snprintf(st, sizeof(st), "%s", XS("Задать"));
                auto stsz = fn->CalcTextSizeA(fs * 1.0f, FLT_MAX, 0, st);
                float stx = cX + cW - inset - padX - stsz.x;
                fg->AddText(fn, fs * 1.0f, {stx, cy2 - stsz.y * 0.5f},
                    set ? C::UA(C::Acc(), alpha) : C::UA(C::Dim(), alpha), st);
                if (set)
                    fg->AddCircleFilled({stx - 16.f, cy2}, 5.f, C::UA(C::Acc(), alpha), 16);

                if (zi == 0 && g_state.ui_show_sep)
                    fg->AddLine({cX + inset + padX, curY + rH - 0.5f},
                                {cX + cW - inset - padX, curY + rH - 0.5f},
                                C::UA(C::Sep(), alpha), 0.8f);
                curY += rH;
            }

            // Сброс зон к дефолту (если наставил мимо).
            if (g_state.farm_joy_x >= 0.f || g_state.farm_fire_x >= 0.f) {
                curY += 8.f;
                FgCardBg(rH * 1);
                if (!blocked && !g_scrollPop.dragging && mouseReleased
                    && mousePos.x >= cX + inset && mousePos.x <= cX + cW - inset
                    && mousePos.y >= curY && mousePos.y <= curY + rH
                    && clickedPos.x >= cX + inset && clickedPos.x <= cX + cW - inset
                    && clickedPos.y >= curY && clickedPos.y <= curY + rH) {
                    g_state.farm_joy_x = g_state.farm_joy_y = -1.f;
                    g_state.farm_fire_x = g_state.farm_fire_y = -1.f;
                    ShowToast(XS("Зоны сброшены"));
                    PlaySound(SND_CLICK);
                }
                const char* rt = XS("Сбросить зоны");
                auto rsz = fn->CalcTextSizeA(fs * 1.05f, FLT_MAX, 0, rt);
                fg->AddText(fn, fs * 1.05f,
                    {cX + (cW - rsz.x) * 0.5f, curY + (rH - rsz.y) * 0.5f},
                    C::UA(C::Dim(), alpha), rt);
                curY += rH;
            }
        }

    } else if (secId == 2) {
        FgSHdr(nullptr, 12.f);
        auto dSz = fn->CalcTextSizeA(fs * 1.15f, FLT_MAX, 0, XS("Приложение будет закрыто."));
        fg->AddText(fn, fs * 1.15f,
            {cX + (cW - dSz.x) * 0.5f, curY},
            C::UA(C::Dim(), alpha), XS("Приложение будет закрыто."));
        curY += dSz.y + 28.f;

        const float btnH = 62.f, bPad = 28.f;
        float bW  = (cW - inset * 2.f - bPad * 2.f - 12.f) * 0.5f;
        float b1X = cX + inset + bPad, b2X = b1X + bW + 12.f;
        DrawExitButtons(fg, fn, fs, alpha,
            b1X, b2X, bW, curY, btnH,
            blocked, mousePos, clickedPos,
            mouseReleased, false);
        curY += btnH;
    } else if (secId == 3) {
        const char* cfgName = (g_configToDelete >= 0 && g_configToDelete < g_configCount)
            ? g_configs[g_configToDelete].name : XS("Конфиг");

        FgSHdr(nullptr, 40.f);

        {
            char nameBuf[96];
            snprintf(nameBuf, sizeof(nameBuf), XS("«%s»"), cfgName);
            float nameFS = fs * 1.55f;
            auto  nameSz = fn->CalcTextSizeA(nameFS, FLT_MAX, 0, nameBuf);
            fg->AddText(fn, nameFS,
                {cX + (cW - nameSz.x) * 0.5f, curY},
                C::UA(C::Txt(), alpha), nameBuf);
            curY += nameSz.y + 12.f;
        }

        {
            const char* sub = XS("Будет удалён безвозвратно");
            auto sSz = fn->CalcTextSizeA(fs * 1.05f, FLT_MAX, 0, sub);
            fg->AddText(fn, fs * 1.05f,
                {cX + (cW - sSz.x) * 0.5f, curY},
                C::UA(C::Red(), alpha * 0.85f), sub);
            curY += sSz.y + 36.f;
        }

        {
            const float btnH = 58.f, bPad = 28.f, gap = 12.f;
            float bW  = (cW - inset * 2.f - bPad * 2.f - gap) * 0.5f;
            float b1X = cX + inset + bPad;
            float b2X = b1X + bW + gap;

            fg->AddRectFilled({b1X, curY}, {b1X + bW, curY + btnH}, C::UA(C::Card(), alpha), R::Btn);
            fg->AddRect({b1X, curY}, {b1X + bW, curY + btnH}, C::UA(C::Sep(), alpha), R::Btn, 0, 1.5f);
            {
                const char* ct = XS("Отмена");
                auto ctsz = fn->CalcTextSizeA(fs * 1.1f, FLT_MAX, 0, ct);
                fg->AddText(fn, fs * 1.1f,
                    {b1X + bW * 0.5f - ctsz.x * 0.5f, curY + (btnH - fs * 1.1f) * 0.5f},
                    C::UA(C::Txt(), alpha), ct);
            }

            fg->AddRectFilled({b2X, curY}, {b2X + bW, curY + btnH}, C::UA(C::Red(), alpha * 0.9f), R::Btn);
            fg->AddRect({b2X, curY}, {b2X + bW, curY + btnH}, C::UA(C::Red(), alpha), R::Btn, 0, 1.5f);
            {
                const char* dt2 = XS("Удалить");
                auto dtsz = fn->CalcTextSizeA(fs * 1.1f, FLT_MAX, 0, dt2);
                fg->AddText(fn, fs * 1.1f,
                    {b2X + bW * 0.5f - dtsz.x * 0.5f, curY + (btnH - fs * 1.1f) * 0.5f},
                    IM_COL32(255, 255, 255, int(255 * alpha)), dt2);
            }

            if (!blocked && mouseReleased) {
                if (mousePos.x >= b1X && mousePos.x <= b1X + bW
                 && mousePos.y >= curY && mousePos.y <= curY + btnH
                 && clickedPos.x >= b1X && clickedPos.x <= b1X + bW
                 && clickedPos.y >= curY && clickedPos.y <= curY + btnH)
                    PopoverClose();

                if (mousePos.x >= b2X && mousePos.x <= b2X + bW
                 && mousePos.y >= curY && mousePos.y <= curY + btnH
                 && clickedPos.x >= b2X && clickedPos.x <= b2X + bW
                 && clickedPos.y >= curY && clickedPos.y <= curY + btnH) {
                    ConfigDelete(g_configToDelete);
                    g_configToDelete = -1;
                    PopoverClose();
                }
            }
            curY += btnH;
        }
    }

    curY += 12.f;
    return curY;
}

void DrawPopover(float dt, ImVec2 menuPos, float WW, float WH) {
    if (!g_pop.visible) return;

    g_pop.openFrames++;

    if (!TickSlideAnim(g_pop.anim, g_pop.vel, g_pop.closing, dt)) {
        g_pop.anim = 0.f; g_pop.vel = 0.f;
        g_pop.visible = false; g_pop.closing = false;
        g_popOpenClickPos = {-9999.f, -9999.f};
        g_scrollPop = {};
        Blur::Unfreeze();
        return;
    }

    auto& io    = ImGui::GetIO();
    float easeT = EaseInOut(g_pop.anim);
    auto* fn    = ImGui::GetFont();
    float fs    = ImGui::GetFontSize();

    float titleFS  = fs * 1.45f;
    float headerH  = 28.f + titleFS + 16.f + 8.f;

    float popW = WW * 0.92f, popH = WH * 0.88f;

    float sX     = menuPos.x + (WW - popW) * 0.5f;
    float sYFull = menuPos.y + (WH - popH) * 0.5f;
    float sY     = Lerpf(menuPos.y + WH, sYFull, easeT);
    float sW = popW, sH = popH;

    float contentY = sY + headerH;
    float areaH    = sH - headerH;
    float lineY    = sY + 28.f + titleFS + 16.f;

    auto* fg = ImGui::GetForegroundDrawList();

    Blur::Draw(fg, g_win.pos, {g_win.pos.x+g_win.w, g_win.pos.y+g_win.h}, easeT, R::Card);

    fg->PushClipRect({menuPos.x, menuPos.y}, {menuPos.x + WW, menuPos.y + WH}, true);

    fg->AddRectFilled({sX, sY}, {sX + sW, sY + sH}, C::U(C::Bg()), R::Sheet);

    {
        float dhW = 72.f, dhH = 7.f;
        float dhX = sX + sW * 0.5f - dhW * 0.5f, dhY = sY + 14.f;
        fg->AddRectFilled({dhX, dhY}, {dhX + dhW, dhY + dhH},
            C::UA(C::Dim(), 0.45f * easeT), dhH * 0.5f);
        if (io.MouseClicked[0]
            && io.MousePos.x >= sX && io.MousePos.x <= sX + sW
            && io.MousePos.y >= sY && io.MousePos.y <= sY + 44.f) {
            g_win.dragging   = true;
            g_win.touchStart = io.MousePos;
            g_win.posStart   = g_win.pos;
        }
    }

    bool inPop = io.MousePos.x >= sX && io.MousePos.x <= sX + sW
              && io.MousePos.y >= contentY && io.MousePos.y <= sY + sH - 4.f;
    bool clickInPop = io.MouseClickedPos[0].x >= sX && io.MouseClickedPos[0].x <= sX + sW
                   && io.MouseClickedPos[0].y >= contentY && io.MouseClickedPos[0].y <= sY + sH - 4.f;
    bool mIn = inPop && clickInPop;

    static float s_popMaxScroll = 0.f;
    bool colorPop = (g_pop.sectionId == 4) && g_colP != nullptr;
    if (!colorPop) ScrollTick(g_scrollPop, mIn, g_pop.closing, s_popMaxScroll, dt);
    else { g_scrollPop.off = 0.f; g_scrollPop.vel = 0.f; s_popMaxScroll = 0.f; }

    if (colorPop && g_colP) {

        const float PAD   = 16.f;
        const float btnH  = 62.f, btnMar = 10.f;
        float X  = sX + PAD, W = sW - PAD * 2.f;
        float bY = sY + sH - btnH - btnMar;

        const int   COLS     = 7;
        const float CELL_GAP = 10.f;
        int   ROWS   = (kPaletteCount + COLS - 1) / COLS;
        float availH = bY - contentY - 16.f;
        float cellW = (W - CELL_GAP * (COLS - 1)) / (float)COLS;
        float cellH = (availH - CELL_GAP * (ROWS - 1)) / (float)ROWS;
        float cellSz = cellW < cellH ? cellW : cellH;
        if (cellSz < 22.f) cellSz = 22.f;
        float gridW  = COLS * cellSz + (COLS - 1) * CELL_GAP;
        float gridH  = ROWS * cellSz + (ROWS - 1) * CELL_GAP;
        float gridX  = X + (W - gridW) * 0.5f;
        float gridY  = contentY + (bY - contentY - gridH) * 0.5f;
        if (gridY + gridH > bY - 8.f) gridY = bY - 8.f - gridH;
        if (gridY < contentY + 8.f) gridY = contentY + 8.f;
        fg->PushClipRect({sX + 8.f, contentY}, {sX + sW - 8.f, bY - 6.f}, true);

        const float CELL_R = 10.f;

        static int   s_palSel  = -1;
        static float s_selOffX = 0.f, s_selOffY = 0.f;
        static float s_selVelX = 0.f, s_selVelY = 0.f;
        static float s_selSz   = 0.f, s_selVelSz = 0.f;
        if (g_colP) {
            float bestDist = 1e9f;
            int   bestIdx  = -1;
            for (int pi = 0; pi < kPaletteCount; pi++) {
                float dr = kColorPalette[pi].x - g_colP->x;
                float dg = kColorPalette[pi].y - g_colP->y;
                float db = kColorPalette[pi].z - g_colP->z;
                float d  = dr*dr + dg*dg + db*db;
                if (d < bestDist) { bestDist = d; bestIdx = pi; }
            }
            if (bestDist < 0.02f) s_palSel = bestIdx; else s_palSel = -1;
        }

        auto cellCenter = [&](int pi) -> ImVec2 {
            int r = pi / COLS, c = pi % COLS;
            return { c * (cellSz + CELL_GAP) + cellSz * 0.5f,
                     r * (cellSz + CELL_GAP) + cellSz * 0.5f };
        };

        if (s_palSel >= 0) {
            ImVec2 tc = cellCenter(s_palSel);
            if (s_selSz < 0.001f || g_palSelReset) {
                s_selOffX = tc.x; s_selOffY = tc.y;
                s_selVelX = 0.f;  s_selVelY = 0.f;
                if (g_palSelReset) { s_selSz = 1.f; s_selVelSz = 0.f; }
            }
            SpringTick(s_selOffX, s_selVelX, tc.x, dt);
            SpringTick(s_selOffY, s_selVelY, tc.y, dt);
            SpringTick(s_selSz, s_selVelSz, 1.f, dt);
        } else {
            SpringTick(s_selSz, s_selVelSz, 0.f, dt);
        }
        g_palSelReset = false;

        for (int pi = 0; pi < kPaletteCount; pi++) {
            int row = pi / COLS, col = pi % COLS;
            float cx0 = gridX + col * (cellSz + CELL_GAP);
            float cy0 = gridY + row * (cellSz + CELL_GAP);
            float cx1 = cx0 + cellSz, cy1 = cy0 + cellSz;

            ImVec4 cv = kColorPalette[pi];
            ImU32  fill = IM_COL32(int(cv.x*255), int(cv.y*255), int(cv.z*255), 255);

            fg->AddRectFilled({cx0+1.5f, cy0+1.5f}, {cx1+1.5f, cy1+1.5f},
                IM_COL32(0,0,0, int(18*easeT)), CELL_R);
            fg->AddRectFilled({cx0, cy0}, {cx1, cy1}, fill, CELL_R);

            if (!g_pop.closing && io.MouseReleased[0]) {
                ImVec2 mp = io.MousePos, cp = io.MouseClickedPos[0];
                bool hit = mp.x >= cx0 && mp.x <= cx1 && mp.y >= cy0 && mp.y <= cy1
                        && cp.x >= cx0 && cp.x <= cx1 && cp.y >= cy0 && cp.y <= cy1;
                if (hit) {
                    *g_colP = kColorPalette[pi];
                    s_palSel = pi;
                    g_input.touchConsumed = true;
                }
            }
        }

        if (s_selSz > 0.001f) {
            float ax = gridX + s_selOffX, ay = gridY + s_selOffY;
            float r = (cellSz * 0.5f + 5.f) * s_selSz;
            fg->AddRect({ax - r, ay - r}, {ax + r, ay + r},
                IM_COL32(0,0,0, int(220*easeT*s_selSz)), CELL_R + 5.f * s_selSz, 0, 4.f);
            fg->AddRect({ax - r + 2.5f, ay - r + 2.5f}, {ax + r - 2.5f, ay + r - 2.5f},
                IM_COL32(255,255,255, int(255*easeT*s_selSz)), CELL_R + 2.5f * s_selSz, 0, 1.5f);
        }
        fg->PopClipRect();

        fg->AddRectFilled({X, bY}, {X+W, bY+btnH}, C::UA(C::Acc(), easeT), R::Btn);
        auto bsz = fn->CalcTextSizeA(fs*1.2f, FLT_MAX, 0, XS("Готово"));
        fg->AddText(fn, fs*1.2f,
            {X + W*0.5f - bsz.x*0.5f, bY + (btnH-bsz.y)*0.5f},
            IM_COL32(255,255,255,int(255*easeT)), XS("Готово"));

        if (io.MouseDown[0] || io.MouseReleased[0])
            g_input.touchConsumed = true;

        if (!g_pop.closing && io.MouseReleased[0]) {
            ImVec2 mp = io.MousePos, cp = io.MouseClickedPos[0];
            bool doneTap = mp.x >= X && mp.x <= X+W && mp.y >= bY && mp.y <= bY+btnH
                        && cp.x >= X && cp.x <= X+W && cp.y >= bY && cp.y <= bY+btnH;
            if (doneTap) PopoverClose();
        }

    } else {
        fg->PushClipRect({sX, contentY}, {sX + sW, sY + sH - 4.f}, true);

        float drawY = contentY - g_scrollPop.off;
        // mIn: и нажатие, и отпускание внутри области контента (ниже шапки).
        // Без этого строки, уехавшие при прокрутке под шапку, ловили тапы
        // «сквозь» крестик закрытия — рисование клипается, хит-тест нет.
        float endY  = DrawPopoverContentFG(fg, fn, fs, g_pop.sectionId, dt,
                                            sX, sW, drawY, easeT,
                                            g_pop.closing,
                                            io.MousePos, io.MouseClickedPos[0],
                                            io.MouseReleased[0] && !g_scrollPop.dragging && mIn,
                                            io.MouseDown[0] && mIn);

        float contentTotalH = endY - drawY;
        s_popMaxScroll = ImMax(0.f, contentTotalH - (areaH - 4.f));

        fg->PopClipRect();
    }

    {
        auto tsz = fn->CalcTextSizeA(titleFS, FLT_MAX, 0, g_pop.title);
        fg->AddText(fn, titleFS,
            {sX + sW * 0.5f - tsz.x * 0.5f, sY + 28.f},
            C::UA(C::Txt(), easeT), g_pop.title);
    }
    {
        float cR = 26.f, bcx = sX + sW - cR - 16.f, bcy = sY + 28.f + titleFS * 0.5f;
        fg->AddCircleFilled({bcx, bcy}, cR, C::UA(C::Sep(), easeT), 32);
        float cs = 9.f;
        fg->AddLine({bcx-cs, bcy-cs}, {bcx+cs, bcy+cs}, C::UA(C::Dim(), easeT), 3.2f);
        fg->AddLine({bcx+cs, bcy-cs}, {bcx-cs, bcy+cs}, C::UA(C::Dim(), easeT), 3.2f);

        if (!g_pop.closing && io.MouseReleased[0]
            && fabsf(io.MousePos.x - bcx) < cR + 12.f
            && fabsf(io.MousePos.y - bcy) < cR + 12.f) {
            PopoverClose();
            g_input.touchConsumed = true;
        }
    }
    fg->AddLine({sX + 18.f, lineY}, {sX + sW - 18.f, lineY}, C::UA(C::Sep(), easeT), 2.5f);

    fg->PopClipRect();
}
