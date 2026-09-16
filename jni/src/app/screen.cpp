// app/screen.cpp — Размеры экрана и центрирование окна меню.
//
// Модуль разрезан из прежнего монолита jni/src/main.cpp;
// что здесь лежит и кто это зовёт — в шапке app/screen.h и в docs/CODE_MAP.md.


#include "app/common.h"
#include "ui/scroll.h"
#include "ui/toast.h"
#include "ui/watermark.h"
#include "ui/window.h"
#include "app/screen.h"

float g_sw = 1920.f;

float g_sh = 1080.f;

void VisibleScreen(float& w, float& h);

uint32_t g_menu_orient = 0xFFFFFFFFu;

int g_menu_dw = 0, g_menu_dh = 0;

void VisibleScreen(float& w, float& h) {
    float dw = (float)displayInfo.width;
    float dh = (float)displayInfo.height;
    if (dw < 100.f) dw = (float)native_window_screen_x;
    if (dh < 100.f) dh = (float)native_window_screen_y;
    float mx = dw > dh ? dw : dh;
    float mn = dw < dh ? dw : dh;
    if (mx < 100.f) mx = 1080.f;
    if (mn < 100.f) mn = mx;
    bool land = (displayInfo.orientation == 1 || displayInfo.orientation == 3);
    if (dw > dh) land = true;
    else if (dh > dw && (displayInfo.orientation == 0 || displayInfo.orientation == 2)) land = false;
    w = land ? mx : mn;
    h = land ? mn : mx;
}

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
