#pragma once
// Barbatos-inspired dark graphite theme with a violet accent and rounded surfaces.

#include "imgui.h"
#include "ui_util.h"

namespace R {
    static constexpr float Card  = 20.f;
    static constexpr float Pill  = 18.f;
    static constexpr float Sheet = 24.f;
    static constexpr float Btn   = 15.f;
}

extern bool  g_darkTheme;
extern float g_themeT;
extern float g_menuFadeIn;
extern ImVec4 g_accent;
extern bool   g_accentOn;

namespace C {
    // Both palettes intentionally match: old light-theme configs must keep the
    // same dark Barbatos presentation.
    namespace Light {
        static constexpr ImVec4 Bg     = {0.027f, 0.029f, 0.043f, 1};
        static constexpr ImVec4 LeftBg = {0.043f, 0.047f, 0.067f, 1};
        static constexpr ImVec4 Card   = {0.075f, 0.078f, 0.110f, 1};
        static constexpr ImVec4 Acc    = {0.545f, 0.486f, 1.000f, 1};
        static constexpr ImVec4 AccDk  = {0.388f, 0.325f, 0.820f, 1};
        static constexpr ImVec4 Red    = {1.000f, 0.361f, 0.322f, 1};
        static constexpr ImVec4 Txt    = {0.949f, 0.953f, 0.976f, 1};
        static constexpr ImVec4 Dim    = {0.558f, 0.573f, 0.655f, 1};
        static constexpr ImVec4 TrkOff = {0.137f, 0.145f, 0.200f, 1};
        static constexpr ImVec4 Sep    = {0.180f, 0.188f, 0.259f, 1};
    }
    namespace Dark {
        static constexpr ImVec4 Bg     = {0.027f, 0.029f, 0.043f, 1};
        static constexpr ImVec4 LeftBg = {0.043f, 0.047f, 0.067f, 1};
        static constexpr ImVec4 Card   = {0.075f, 0.078f, 0.110f, 1};
        static constexpr ImVec4 Acc    = {0.545f, 0.486f, 1.000f, 1};
        static constexpr ImVec4 AccDk  = {0.388f, 0.325f, 0.820f, 1};
        static constexpr ImVec4 Red    = {1.000f, 0.361f, 0.322f, 1};
        static constexpr ImVec4 Txt    = {0.949f, 0.953f, 0.976f, 1};
        static constexpr ImVec4 Dim    = {0.558f, 0.573f, 0.655f, 1};
        static constexpr ImVec4 TrkOff = {0.137f, 0.145f, 0.200f, 1};
        static constexpr ImVec4 Sep    = {0.180f, 0.188f, 0.259f, 1};
    }

    static inline ImVec4 Lerp4(ImVec4 a, ImVec4 b) {
        const float t = EaseInOut(g_themeT);
        return {Lerpf(a.x,b.x,t), Lerpf(a.y,b.y,t), Lerpf(a.z,b.z,t), 1.f};
    }

    static inline ImVec4 Bg()     { return Lerp4(Light::Bg,     Dark::Bg);     }
    static inline ImVec4 LeftBg() { return Lerp4(Light::LeftBg, Dark::LeftBg); }
    static inline ImVec4 Card()   { return Lerp4(Light::Card,   Dark::Card);   }
    static inline ImVec4 AccLight() { return g_accentOn ? g_accent : Light::Acc; }
    static inline ImVec4 AccDark()  { return g_accentOn ? g_accent : Dark::Acc; }
    static inline ImVec4 AccDkOf(ImVec4 v) { return {v.x*0.72f, v.y*0.68f, v.z*0.84f, 1.f}; }
    static inline ImVec4 Acc()    { return Lerp4(AccLight(), AccDark()); }
    static inline ImVec4 AccDk()  { return g_accentOn ? AccDkOf(g_accent) : Lerp4(Light::AccDk, Dark::AccDk); }
    static inline ImVec4 Red()    { return Lerp4(Light::Red,    Dark::Red);    }
    static inline ImVec4 Txt()    { return Lerp4(Light::Txt,    Dark::Txt);    }
    static inline ImVec4 Dim()    { return Lerp4(Light::Dim,    Dark::Dim);    }
    static inline ImVec4 TrkOff() { return Lerp4(Light::TrkOff, Dark::TrkOff); }
    static inline ImVec4 Sep()    { return Lerp4(Light::Sep,    Dark::Sep);    }

    static inline ImVec4 Raised() {
        const ImVec4 card = Card();
        const ImVec4 acc = Acc();
        return {Lerpf(card.x, acc.x, 0.12f), Lerpf(card.y, acc.y, 0.12f),
                Lerpf(card.z, acc.z, 0.12f), 1.f};
    }

    static ImU32 Mix(ImVec4 a, ImVec4 b, float t) {
        return IM_COL32(
            int(Lerpf(a.x, b.x, t) * 255), int(Lerpf(a.y, b.y, t) * 255),
            int(Lerpf(a.z, b.z, t) * 255), int(Lerpf(a.w, b.w, t) * 255));
    }
    static ImU32 U(ImVec4 v)               { return ImGui::ColorConvertFloat4ToU32(v); }
    static ImU32 UA(ImVec4 v, float alpha) { v.w = alpha; return ImGui::ColorConvertFloat4ToU32(v); }
}

namespace Layout {
    static constexpr float RowH      = 72.f;
    static constexpr float SliderH   = 102.f;
    static constexpr float HeaderH   = 74.f;
    static constexpr float Inset     = 18.f;
    static constexpr float PadX      = 22.f;
    static constexpr float BtnH      = 58.f;
    static constexpr float BottomH   = 116.f;
    static constexpr float TabW      = 126.f;
    static constexpr float RailW     = 226.f;
    static constexpr float TabHV     = 78.f;
}

void ApplyTheme();
