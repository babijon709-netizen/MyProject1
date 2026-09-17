#include "hud.h"
#include "theme.h"       // C:: палитра
#include "main.h"        // displayInfo, native_window_screen_*
#include "game.h"        // esp_nearby_player_count
#include "process.h"     // g_esp_attached
#include "esp_draw.h"    // FrameBoxes
#include "ui_util.h"     // EaseOut3, TickSlideAnim
#include "str.h"         // XS()
#include "text_utf8.h"   // CopyTextUtf8

struct ToastState {
    char  msg[128]     = {};
    char  nextMsg[128] = {};
    bool  hasNext      = false;
    float slidePos = 0.f, slideVel = 0.f;
    float life = 0.f;
    float textA = 0.f, textAVel = 0.f;
    float barW = 0.f, barVel = 0.f;
    float widthAnim = 200.f, widthVel = 0.f;
    bool  visible = false, closing = false;
};
static ToastState g_toast;

void ShowToast(const char* msg) {
    if (!g_toast.visible || g_toast.closing) {
        CopyTextUtf8(g_toast.msg, sizeof(g_toast.msg), msg);
        g_toast.life = 0.f; g_toast.closing = false; g_toast.visible = true;
        g_toast.slidePos = 0.f; g_toast.slideVel = 0.f;
        g_toast.textA = 0.f; g_toast.textAVel = 0.f;
        g_toast.barW = 1.f; g_toast.barVel = 0.f;
        g_toast.widthVel = 0.f;
        g_toast.hasNext = false;
    } else {
        CopyTextUtf8(g_toast.nextMsg, sizeof(g_toast.nextMsg), msg);
        g_toast.hasNext = true; g_toast.life = 0.f;
    }
}


void DrawToast(float dt) {
    if (!g_toast.visible) return;
    g_toast.life += dt;

    if (g_toast.hasNext && g_toast.textA < 0.04f) {
        snprintf(g_toast.msg, sizeof(g_toast.msg), "%s", g_toast.nextMsg);
        g_toast.hasNext = false;
        g_toast.barW = 1.f; g_toast.barVel = 0.f;
    }

    if (g_toast.life > 2.6f && !g_toast.closing && !g_toast.hasNext) g_toast.closing = true;

    if (!TickSlideAnim(g_toast.slidePos, g_toast.slideVel, g_toast.closing, dt)) {
        g_toast.slidePos = 0.f; g_toast.slideVel = 0.f; g_toast.visible = false; return;
    }

    float textTgt = (g_toast.hasNext ? 0.f : 1.f);
    g_toast.textA += (textTgt - g_toast.textA) * 14.f * dt;
    g_toast.textA = ImClamp(g_toast.textA, 0.f, 1.f);

    { float bt = g_toast.hasNext ? 1.f : ImMax(0.f, 1.f - g_toast.life / 2.6f);
      float f = 220.f * (bt - g_toast.barW) - 30.f * g_toast.barVel;
      g_toast.barVel += f * dt; g_toast.barW += g_toast.barVel * dt;
      g_toast.barW = ImClamp(g_toast.barW, 0.f, 1.f); }

    auto* fg = ImGui::GetForegroundDrawList();
    auto* fn = ImGui::GetFont();
    float fs  = ImGui::GetFontSize() * 1.55f;
    float scrW = 0.f, scrH = 0.f;
    VisibleScreen(scrW, scrH);

    const char* sep = strchr(g_toast.msg, 124);
    char np[128] = {}, sp[128] = {};
    bool isOn = false, isOff = false;
    if (sep) {
        char head[128] = {};
        CopyTextUtf8(head, sizeof(head), g_toast.msg);
        if (char* cut = strchr(head, 124)) *cut = '\0';
        CopyTextUtf8(np, sizeof(np), head);
        // Буфер значения был на 8 байт — «Русский» в него не влезал и рвался.
        CopyTextUtf8(sp, sizeof(sp), sep + 1);
        // Вкл/Выкл — по своей строке, а не по «ON»: в русском это «Вкл», в
        // английском — «On» из таблицы переводов.
        isOn  = (strcmp(sp, XS("Вкл")) == 0);
        isOff = (strcmp(sp, XS("Выкл")) == 0);
    } else {
        CopyTextUtf8(np, sizeof(np), g_toast.msg);
    }

    auto nsz = fn->CalcTextSizeA(fs, FLT_MAX, 0, np);
    auto ssz = fn->CalcTextSizeA(fs, FLT_MAX, 0, XS(" - "));
    auto asz = fn->CalcTextSizeA(fs, FLT_MAX, 0, sp);

    float padH = 28.f, padV = 20.f;
    float tWTarget = padH + nsz.x + (sep ? ssz.x + asz.x : 0.f) + padH;
    float tH = nsz.y + padV * 2.f + 10.f;

    if (g_toast.widthAnim < 8.f) { g_toast.widthAnim = tWTarget; g_toast.widthVel = 0.f; }
    {
        float dw = tWTarget - g_toast.widthAnim;
        g_toast.widthVel += (dw * 220.f - g_toast.widthVel * 28.f) * dt;
        g_toast.widthAnim += g_toast.widthVel * dt;
        if (fabsf(dw) < 0.6f && fabsf(g_toast.widthVel) < 4.f) {
            g_toast.widthAnim = tWTarget; g_toast.widthVel = 0.f;
        }
    }
    float tW = g_toast.widthAnim;

    float ease  = EaseInOut(ImClamp(g_toast.slidePos, 0.f, 1.f));
    float offY  = (1.f - ease) * (-(tH + 20.f));
    float alpha = ease;

    float tx = scrW - tW - 16.f;
    float ty = 16.f + offY;

    ImVec4 card = C::Card(), txt = C::Txt(), dim = C::Dim(), acc = C::Acc();

    fg->AddRectFilled({tx + 2.f, ty + 4.f}, {tx + tW + 2.f, ty + tH + 4.f},
        IM_COL32(0, 0, 0, int(26 * alpha)), 26.f);
    fg->AddRectFilled({tx, ty}, {tx + tW, ty + tH},
        IM_COL32(int(card.x*255), int(card.y*255), int(card.z*255), int(alpha * 245)), 26.f);
    fg->AddRect({tx, ty}, {tx + tW, ty + tH},
        C::UA(acc, 0.32f * alpha), 26.f, 0, 1.3f);

    fg->PushClipRect({tx + 1.f, ty + 1.f}, {tx + tW - 1.f, ty + tH - 1.f}, true);

    float cx0 = tx + padH;
    float textY = ty + padV;
    float ta = g_toast.textA * alpha;

    fg->AddText(fn, fs, {cx0, textY},
        IM_COL32(int(txt.x*255), int(txt.y*255), int(txt.z*255), int(ta*255)), np);
    cx0 += nsz.x;

    if (sep) {
        fg->AddText(fn, fs, {cx0, textY},
            IM_COL32(int(dim.x*255), int(dim.y*255), int(dim.z*255), int(ta*170)), XS(" - "));
        cx0 += ssz.x;
        // Зелёный — включено, красный — выключено, нейтральный — всё прочее
        // (например, выбранный язык: он не «выключен», красным ему не место).
        ImVec4 stCol = isOn  ? ImVec4{0.196f, 0.843f, 0.294f, 1.f}
                     : isOff ? ImVec4{1.f, 0.231f, 0.188f, 1.f}
                             : ImVec4{txt.x, txt.y, txt.z, 1.f};
        fg->AddText(fn, fs, {cx0, textY},
            IM_COL32(int(stCol.x*255), int(stCol.y*255), int(stCol.z*255), int(ta*255)), sp);
    }

    {
        float bH  = 10.f, bR = 5.f;
        float bY  = ty + tH - 8.f - bH;
        float bx0 = tx + 22.f, bx1 = tx + tW - 22.f;
        float fx1 = bx0 + g_toast.barW * (bx1 - bx0);
        int ar = int(acc.x*255), ag = int(acc.y*255), ab = int(acc.z*255);
        if (fx1 > bx0 + bR * 2.f)
            fg->AddRectFilled({bx0, bY}, {fx1, bY + bH},
                IM_COL32(ar, ag, ab, int(alpha * 215)), bR);
    }

    fg->PopClipRect();
}

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
