// ui/widgets.cpp — Строки-переключатели, слайдеры, карточки.
//
// Модуль разрезан из прежнего монолита jni/src/main.cpp;
// что здесь лежит и кто это зовёт — в шапке ui/widgets.h и в docs/CODE_MAP.md.


#include "app/common.h"
#include "ui/layout.h"
#include "ui/popover.h"
#include "ui/scroll.h"
#include "ui/sheet.h"
#include "ui/tabs.h"
#include "ui/theme.h"
#include "ui/toast.h"
#include "ui/widgets.h"

void DrawToggle(ImDrawList* dl, float ax, float cy, float t) {
    const float tW = 72.f, tH = 42.f, tR = tH * 0.5f, kR = tR - 3.5f;
    float tx = ax, ty = cy - tH * 0.5f;
    dl->AddRectFilled({tx, ty}, {tx + tW, ty + tH}, C::Mix(C::TrkOff(), C::Acc(), t), tR);
    float kx = Lerpf(tx + tR + 2.f, tx + tW - tR - 2.f, t);
    dl->AddCircleFilled({kx + 0.5f, cy + 1.5f}, kR + 1.f, IM_COL32(0, 0, 0, 35));
    dl->AddCircleFilled({kx, cy}, kR, IM_COL32(255, 255, 255, 255));
}

void RenderToggleRowVisuals(ImDrawList* dl, ImFont* fn, float fs,
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

void RenderSliderVisuals(ImDrawList* dl, ImFont* fn, float fs,
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

void TickSliderAnim(AppState::SliderAnim& anim, float target, bool act, float dt) {
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

bool TickSlideAnim(float& anim, float& vel, bool closing, float dt);

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

bool TickSlideAnim(float& anim, float& vel, bool closing, float dt) {
    if (dt > 0.032f) dt = 0.032f;
    const float stiff = 120.f, damp = 22.f;
    if (!closing) {
        float acc = (1.f - anim) * stiff - vel * damp;
        vel  += acc * dt;
        anim += vel * dt;
        if (anim >= 0.9998f) { anim = 1.f; vel = 0.f; }
        anim = ImClamp(anim, 0.f, 1.f);
        return true;
    } else {
        float acc = (0.f - anim) * stiff - vel * damp;
        vel  += acc * dt;
        anim += vel * dt;
        if (anim <= 0.001f) { anim = 0.f; vel = 0.f; return false; }
        anim = ImClamp(anim, 0.f, 1.f);
        return true;
    }
}
