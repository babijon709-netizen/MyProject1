// ui/scroll.cpp — Прокрутка панелей и состояние окна.
//
// Модуль разрезан из прежнего монолита jni/src/main.cpp;
// что здесь лежит и кто это зовёт — в шапке ui/scroll.h и в docs/CODE_MAP.md.


#include "app/common.h"
#include "app/media.h"
#include "app/screen.h"
#include "ui/popover.h"
#include "ui/sheet.h"
#include "ui/tabs.h"
#include "ui/widgets.h"
#include "ui/window.h"
#include "ui/scroll.h"

// Позиция «пилюли» активной вкладки на нижней панели (экранный X) + пружина.
float pill_y   = -1.f;

float pill_vel =  0.f;

TabRect tab_rects[kTabCount];

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

ScrollState g_scrollMain;

ScrollState g_scrollPop;

bool IsScrollDragging() { return g_scrollMain.dragging || g_scrollPop.dragging; }

ImVec2 g_popOpenClickPos = {-9999.f, -9999.f};

const float WW_MIN = 840.f;

const float WH_MIN = 720.f;

WindowState g_win;
