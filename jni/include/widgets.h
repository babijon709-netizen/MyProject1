#pragma once
// Виджеты меню: скролл, окно, шторка (sheet), поповер, строки тумблеров/
// слайдеров. Состояние (g_sheet, g_pop, g_win, g_scroll*) — extern, его
// читают аим и автофарм («открыто ли окно»).

#include "imgui.h"
#include "app_state.h"

const float WW_MIN = 840.f;
const float WH_MIN = 720.f;

struct ScrollState {
    float off      = 0.f;
    float vel      = 0.f;
    float lastTy   = 0.f;
    bool  drag     = false;
    bool  dragging = false;
    int   axis     = 0;
    float sb_alpha = 0.f;
    float sb_idle  = 0.f;
    float sb_w     = 3.f;
    float sb_y     = 0.f;
    float sb_h     = 48.f;
    bool  sb_hot   = false;
};

struct WindowState {
    ImVec2 pos              = {-1.f, -1.f};
    float  w                = WW_MIN;
    float  h                = WH_MIN;
    bool   dragging         = false;
    ImVec2 touchStart       = {0, 0};
    ImVec2 posStart         = {0, 0};
    bool   resizing         = false;
    ImVec2 resizeTouchStart = {0, 0};
    ImVec2 sizeStart        = {WW_MIN, WH_MIN};
};

struct Sheet {
    bool   visible    = false;
    float  anim       = 0.f;
    float  vel        = 0.f;
    bool   closing    = false;
    int    openFrames = 0;
    char   title[64]  = {};
    int    type       = 0;
    bool*  boolP      = nullptr;
    float* animP      = nullptr;
    float* slP        = nullptr;
    float  slMin      = 0;
    float  slMax      = 1;
    const char* slFmt = "%.1f";
};

struct Popover {
    bool  visible    = false;
    bool  closing    = false;
    float anim       = 0.f;
    float vel        = 0.f;
    int   openFrames = 0;
    int   sectionId  = 0;
    char  title[64]  = {};
};

extern ScrollState g_scrollMain;
extern ScrollState g_scrollPop;
void ScrollTick(ScrollState& s, bool mIn, bool blocked, float& maxScroll, float dt);
inline bool IsScrollDragging() { return g_scrollMain.dragging || g_scrollPop.dragging; }
extern WindowState g_win;
extern Sheet g_sheet;
extern Popover g_pop;

void SheetOpen(const char* title, int type, bool* bp = nullptr, float* ap = nullptr,
               float* sp = nullptr, float mn = 0, float mx = 1, const char* fmt = "%.1f");
void SheetClose();
void PopoverOpenColor(const char* title, ImVec4* cp);
void PopoverOpen(const char* title, int sid);
void PopoverClose();
bool ToggleRow(const char* id, const char* lbl, bool* v, float& anim, bool last = false, bool first = false);
bool SliderRow(const char* id, const char* lbl, float* v, float mn, float mx, const char* fmt,
               bool last, bool first, AppState::SliderAnim& anim, float dt);
void CardBg(float h, ImDrawFlags flags = ImDrawFlags_RoundCornersAll);
void DrawToggle(ImDrawList* dl, float ax, float cy, float t);
void SHdr(const char* t, float top = 18.f);
bool CollapsibleHeader(const char* id, const char* lbl, int secId = 0);
void DrawSheet(float dt, ImVec2 menuPos, float WW, float WH);
void DrawPopover(float dt, ImVec2 menuPos, float WW, float WH);
