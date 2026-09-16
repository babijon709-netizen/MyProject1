// ui/watermark.cpp — Пилюли-подписи поверх игры.
//
// Модуль разрезан из прежнего монолита jni/src/main.cpp;
// что здесь лежит и кто это зовёт — в шапке ui/watermark.h и в docs/CODE_MAP.md.


#include "app/common.h"
#include "app/attach.h"
#include "app/screen.h"
#include "ui/esp_overlay.h"
#include "ui/layout.h"
#include "ui/theme.h"
#include "ui/window.h"
#include "ui/watermark.h"

static float wm_ring   = 2.f;

bool  menu_open = true;

void DrawWatermark(float dt) {
    auto& io  = ImGui::GetIO();
    auto* fn  = ImGui::GetFont();
    auto* fg  = ImGui::GetForegroundDrawList();

    wm_ring += dt * 3.5f;

    ImVec4 acc   = C::Acc();
    ImU32 bgCol  = C::U(C::Card());
    ImU32 brdCol = C::UA(acc, 0.6f);

    float scrW = 0.f, scrH = 0.f;
    VisibleScreen(scrW, scrH);

    // ---- Некликабельная пилюля с названием чита (слева сверху) ---------
    // Тапы по ней не обрабатываются вовсе, так что она «прозрачна» для
    // кликов и ничему не мешает.
    {
        const float fs = 38.f, padX = 24.f, padY = 14.f;
        const char* name = XS("t.me/benzware");
        auto nSz = fn->CalcTextSizeA(fs, FLT_MAX, 0, name);
        float bW = padX * 2.f + nSz.x;
        float bH = padY * 2.f + nSz.y;
        const float bX = 12.f, bY = 12.f;
        fg->AddRectFilled({bX, bY}, {bX + bW, bY + bH}, C::UA(C::Card(), 0.8f), bH * 0.5f);
        fg->AddRect      ({bX, bY}, {bX + bW, bY + bH}, C::UA(acc, 0.5f), bH * 0.5f, 0, 1.5f);
        fg->AddText(fn, fs, {bX + padX, bY + padY}, C::UA(C::Txt(), 0.95f), name);
    }

    // ---- Пилюля-счётчик противников (по центру верха экрана) -----------
    // Показывает, сколько игроков видит ESP; тап по ней открывает/закрывает
    // меню.
    {
        int enemies = 0;
        if (g_esp_attached) {
            // Обновляем снимок кадра (кэшируется на кадр) и берём число
            // игроков вокруг на все 360° — не только тех, кто попал на экран.
            FrameBoxes(scrW, scrH);
            enemies = esp_nearby_player_count();
        }

        const float fs = 40.f, pad = 22.f, bR = 46.f;
        const char* lbl = XS("Противники");
        char cntBuf[16]; snprintf(cntBuf, sizeof(cntBuf), "%d", enemies);
        auto lSz = fn->CalcTextSizeA(fs, FLT_MAX, 0, lbl);
        auto cSz = fn->CalcTextSizeA(fs, FLT_MAX, 0, cntBuf);

        // Точка-индикатор + подпись + число.
        const float dotR = 7.f;
        float bW = pad + dotR * 2.f + 12.f + lSz.x + 14.f + cSz.x + pad;
        float bH = cSz.y + pad;
        float bX = (scrW - bW) * 0.5f;
        const float bY = 12.f;

        bool in = (io.MousePos.x >= bX && io.MousePos.x <= bX + bW &&
                   io.MousePos.y >= bY && io.MousePos.y <= bY + bH);
        if (in && io.MouseClicked[0]) { menu_open = !menu_open; wm_ring = 0.f; }

        if (wm_ring < 1.f) {
            float r = EaseOut3(wm_ring), ex = r * 22.f;
            fg->AddRectFilled({bX - ex, bY - ex}, {bX + bW + ex, bY + bH + ex},
                C::UA(acc, 0.18f * (1.f - r)), bR + ex);
        }

        fg->AddRectFilled({bX + 2.f, bY + 3.f}, {bX + bW + 2.f, bY + bH + 3.f},
            IM_COL32(0, 0, 0, 20), bR);
        fg->AddRectFilled({bX, bY}, {bX + bW, bY + bH}, bgCol, bR);
        fg->AddRect      ({bX, bY}, {bX + bW, bY + bH}, brdCol, bR, 0, 1.5f);

        // Зелёная точка, когда противники рядом есть; серая — когда никого.
        ImVec4 dotCol = enemies > 0 ? ImVec4{0.24f, 0.78f, 0.42f, 1.f} : C::Dim();
        float dcy = bY + bH * 0.5f;
        fg->AddCircleFilled({bX + pad + dotR, dcy}, dotR, C::U(dotCol), 24);
        if (enemies > 0)
            fg->AddCircle({bX + pad + dotR, dcy}, dotR + 3.f,
                C::UA(dotCol, 0.5f + 0.3f * sinf((float)ImGui::GetTime() * 5.f)), 24, 1.8f);

        float tY = bY + (bH - cSz.y) * 0.5f;
        float lX = bX + pad + dotR * 2.f + 12.f;
        fg->AddText(fn, fs, {lX, tY}, C::UA(C::Txt(), 0.85f), lbl);
        fg->AddText(fn, fs, {lX + lSz.x + 14.f, tY}, C::U(acc), cntBuf);
    }
}
