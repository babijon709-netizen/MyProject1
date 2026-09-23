// Реализация ASCII-примитивов «терминала» — см. ascii.h. Здесь нет ничего,
// кроме ImDrawList::AddText с символами псевдографики и пары тонких
// прямоугольников (сканлайны, инверсный фон): так рамки и чекбоксы
// выглядят в точности как в linux-консоли, пиксель в пиксель шрифта.

#include "ascii.h"

#include "imgui_internal.h"   // ImMin/ImMax/ImClamp

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ascii {

namespace {

ImFont* g_smallFont = nullptr;

// Повторить глиф n раз в буфер (UTF-8, глифы псевдографиики трёхбайтовые).
void Repeat(char* dst, size_t cap, const char* g, int n) {
    const size_t gl = strlen(g);
    size_t p = 0;
    for (int i = 0; i < n && p + gl < cap; ++i) { memcpy(dst + p, g, gl); p += gl; }
    dst[p] = '\0';
}

ImFont* ResolvedFont(ImFont* fn) { return fn ? fn : ImGui::GetFont(); }
float   ResolvedSize(float fs)   { return fs > 0.f ? fs : ImGui::GetFontSize(); }

}  // namespace

// Настоящая реализация альфы: ImGui хранит цвет как ABGR.
static ImU32 AlphaMul(ImU32 col, float a) {
    const unsigned ai = (unsigned)(a * 255.f + 0.5f);
    const unsigned a0 = (col >> IM_COL32_A_SHIFT) & 0xFF;
    const unsigned an = (ai * a0 + 127) / 255;
    return (col & ~IM_COL32_A_MASK) | (an << IM_COL32_A_SHIFT);
}

void SetSmallFont(ImFont* f) { g_smallFont = f; }
ImFont* SmallFont()          { return g_smallFont ? g_smallFont : ImGui::GetFont(); }

ImVec2 Cell(ImFont* fn, float fs) {
    fn = ResolvedFont(fn);
    fs = ResolvedSize(fs);
    // Ширина — advance моносимвола, высота — полное знакоместо (блок «█»
    // занимает всю клетку, в отличие от строчной буквы).
    const ImVec2 w = fn->CalcTextSizeA(fs, FLT_MAX, 0.f, "#");
    const ImVec2 h = fn->CalcTextSizeA(fs, FLT_MAX, 0.f, "\xE2\x96\x88");  // █
    return { w.x, ImMax(w.y, h.y) };
}

ImVec2 Cell(float fs) { return Cell(nullptr, fs); }

void Frame(ImDrawList* dl, const ImVec2& mn, const ImVec2& mx, ImU32 col,
           bool dbl, float fs) {
    if (!dl) return;
    ImFont* fn = ImGui::GetFont();
    fs = ResolvedSize(fs);
    const ImVec2 cell = Cell(fn, fs);
    const float w = mx.x - mn.x, h = mx.y - mn.y;
    if (w < cell.x * 2.f || h < cell.y * 2.f) return;

    const int cols = (int)(w / cell.x);
    const int rows = (int)(h / cell.y);

    const char* cH  = dbl ? "\xE2\x95\x90" : "\xE2\x94\x80";  // ═ / ─
    const char* cV  = dbl ? "\xE2\x95\x91" : "\xE2\x94\x82";  // ║ / │
    const char* cTL = dbl ? "\xE2\x95\x94" : "\xE2\x94\x8C";  // ╔ / ┌
    const char* cTR = dbl ? "\xE2\x95\x97" : "\xE2\x94\x90";  // ╗ / ┐
    const char* cBL = dbl ? "\xE2\x95\x9A" : "\xE2\x94\x94";  // ╚ / └
    const char* cBR = dbl ? "\xE2\x95\x9D" : "\xE2\x94\x98";  // ╝ / ┘

    char buf[1024];
    Repeat(buf, sizeof buf, cH, cols - 2);

    dl->AddText(fn, fs, {mn.x, mn.y}, col, cTL);
    dl->AddText(fn, fs, {mn.x + cell.x, mn.y}, col, buf);
    dl->AddText(fn, fs, {mx.x - cell.x, mn.y}, col, cTR);

    const float yB = mx.y - cell.y;
    dl->AddText(fn, fs, {mn.x, yB}, col, cBL);
    dl->AddText(fn, fs, {mn.x + cell.x, yB}, col, buf);
    dl->AddText(fn, fs, {mx.x - cell.x, yB}, col, cBR);

    const float xR = mx.x - cell.x;
    for (int r = 1; r < rows - 1; ++r) {
        const ImVec2 p{mn.x, mn.y + cell.y * (float)r};
        dl->AddText(fn, fs, p, col, cV);
        dl->AddText(fn, fs, {xR, p.y}, col, cV);
    }
}

void HRule(ImDrawList* dl, const char* label, float x0, float x1, float cy,
           ImU32 col, ImU32 labelCol, float fs, bool leftAlign) {
    if (!dl || x1 - x0 < 8.f) return;
    ImFont* fn = ImGui::GetFont();
    fs = ResolvedSize(fs);
    const ImVec2 cell = Cell(fn, fs);
    const float y = cy - cell.y * 0.5f;
    const int total = (int)((x1 - x0) / cell.x);
    if (total < 1) return;

    char buf[1024];

    if (labelCol == 0) labelCol = col;
    if (!label || !label[0]) {
        Repeat(buf, sizeof buf, "\xE2\x94\x80", total);  // ─
        dl->AddText(fn, fs, {x0, y}, col, buf);
        return;
    }

    char lab[160];
    snprintf(lab, sizeof lab, "[ %s ]", label);
    const ImVec2 lsz = fn->CalcTextSizeA(fs, FLT_MAX, 0.f, lab);
    const int labCells = (int)(lsz.x / cell.x + 0.5f);
    if (labCells + 2 > total) {
        // Правило короче подписи — подпись без линеек.
        dl->AddText(fn, fs, {x0, y}, labelCol, lab);
        return;
    }

    if (leftAlign) {
        const int rightCells = total - labCells;
        Repeat(buf, sizeof buf, "\xE2\x94\x80", rightCells);
        dl->AddText(fn, fs, {x0, y}, labelCol, lab);
        dl->AddText(fn, fs, {x0 + labCells * cell.x, y}, col, buf);
    } else {
        const int leftCells = (total - labCells) / 2;
        const int rightCells = total - labCells - leftCells;
        if (leftCells > 0) {
            Repeat(buf, sizeof buf, "\xE2\x94\x80", leftCells);
            dl->AddText(fn, fs, {x0, y}, col, buf);
        }
        dl->AddText(fn, fs, {x0 + leftCells * cell.x, y}, labelCol, lab);
        if (rightCells > 0) {
            Repeat(buf, sizeof buf, "\xE2\x94\x80", rightCells);
            dl->AddText(fn, fs, {x0 + (leftCells + labCells) * cell.x, y}, col, buf);
        }
    }
}

void VRule(ImDrawList* dl, float x, float y0, float y1, ImU32 col, float fs) {
    if (!dl || y1 - y0 < 8.f) return;
    ImFont* fn = ImGui::GetFont();
    fs = ResolvedSize(fs);
    const ImVec2 cell = Cell(fn, fs);
    const int rows = (int)((y1 - y0) / cell.y);
    for (int r = 0; r < rows; ++r)
        dl->AddText(fn, fs, {x, y0 + cell.y * (float)r}, col, "\xE2\x94\x82");  // │
}

void Check(ImDrawList* dl, const ImVec2& pos, float t, ImU32 onCol, ImU32 offCol, float fs) {
    if (!dl) return;
    t = ImClamp(t, 0.f, 1.f);
    ImFont* fn = ImGui::GetFont();
    fs = ResolvedSize(fs);
    const ImVec2 cell = Cell(fn, fs);
    const bool on = t >= 0.5f;
    const ImU32 br = on ? onCol : offCol;
    dl->AddText(fn, fs, pos, br, "[");
    if (t > 0.02f)
        dl->AddText(fn, fs, {pos.x + cell.x, pos.y}, AlphaMul(onCol, t), "x");
    dl->AddText(fn, fs, {pos.x + cell.x * 2.f, pos.y}, br, "]");
}

void Radio(ImDrawList* dl, const ImVec2& c, bool on, ImU32 onCol, ImU32 offCol, float fs) {
    if (!dl) return;
    ImFont* fn = ImGui::GetFont();
    fs = ResolvedSize(fs);
    const ImVec2 cell = Cell(fn, fs);
    const ImVec2 sz = fn->CalcTextSizeA(fs, FLT_MAX, 0.f, "( )");
    const ImVec2 p{c.x - sz.x * 0.5f, c.y - cell.y * 0.5f};
    dl->AddText(fn, fs, p, on ? onCol : offCol, on ? "(\xE2\x97\x8F)" : "( )");  // (●)
}

void Bar(ImDrawList* dl, const ImVec2& pos, float frac, int cells,
         ImU32 onCol, ImU32 offCol, float fs) {
    if (!dl || cells < 3) return;
    frac = ImClamp(frac, 0.f, 1.f);
    ImFont* fn = ImGui::GetFont();
    fs = ResolvedSize(fs);
    const ImVec2 cell = Cell(fn, fs);

    const float filled = frac * (float)cells;
    const int  full    = (int)filled;
    const bool partial = (filled - (float)full) >= 0.3f;
    const int  used    = ImMin(cells, full + (partial ? 1 : 0));
    const int  rest    = cells - used;

    char s1[512], s2[512];
    size_t p = 0;
    s1[p++] = '[';
    for (int i = 0; i < full; ++i) { memcpy(s1 + p, "\xE2\x96\x88", 3); p += 3; }      // █
    if (partial && full < cells) { memcpy(s1 + p, "\xE2\x96\x8C", 3); p += 3; }        // ▌
    s1[p] = '\0';

    p = 0;
    for (int i = 0; i < rest; ++i) { memcpy(s2 + p, "\xE2\x96\x91", 3); p += 3; }      // ░
    s2[p++] = ']';
    s2[p] = '\0';

    dl->AddText(fn, fs, pos, onCol, s1);
    // Хвост рисуем от конца занятых знакомест, чтобы скобки не разъезжались.
    dl->AddText(fn, fs, {pos.x + (float)(used + 1) * cell.x, pos.y}, offCol, s2);
}

void Scanlines(ImDrawList* dl, const ImVec2& mn, const ImVec2& mx, float alpha) {
    if (!dl || alpha <= 0.001f) return;
    const ImU32 col = IM_COL32(0, 0, 0, (int)(alpha * 255.f + 0.5f));
    for (float y = mn.y; y < mx.y; y += 4.f)
        dl->AddRectFilled({mn.x, y}, {mx.x, y + 1.f}, col);
}

bool BlinkOn() {
    const float t = fmodf((float)ImGui::GetTime(), 1.06f);
    return t < 0.53f;
}

void Cursor(ImDrawList* dl, const ImVec2& pos, ImU32 col, float fs) {
    if (!dl) return;
    dl->AddText(ImGui::GetFont(), ResolvedSize(fs), pos, col, "\xE2\x96\x8C");  // ▌
}

void InverseText(ImDrawList* dl, const ImVec2& pos, const char* text, float fs,
                 ImU32 bg, ImU32 fg, float alpha) {
    if (!dl || !text) return;
    ImFont* fn = ImGui::GetFont();
    fs = ResolvedSize(fs);
    const ImVec2 sz = fn->CalcTextSizeA(fs, FLT_MAX, 0.f, text);
    dl->AddRectFilled(pos, {pos.x + sz.x, pos.y + sz.y}, AlphaMul(bg, alpha));
    dl->AddText(fn, fs, pos, AlphaMul(fg, alpha), text);
}

}  // namespace ascii
