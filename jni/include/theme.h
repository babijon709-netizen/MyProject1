#pragma once
// Визуальная система GUI: тёплая редакционная палитра, компактная геометрия
// и общие размеры. Здесь нет неона, свечения и «киберпанк»-декора.

#include "imgui.h"
#include "ui_util.h"

namespace R {
    static constexpr float Card  = 10.f;
    static constexpr float Pill  = 8.f;
    static constexpr float Sheet = 14.f;
    static constexpr float Btn   = 8.f;
}

extern bool  g_darkTheme;
extern float g_themeT;
extern float g_menuFadeIn;
extern ImVec4 g_accent;
extern bool   g_accentOn;

namespace C {
    // «Workshop»: тёплый графит и бумажный светлый режим. Базовый акцент —
    // охра; пользовательская палитра по-прежнему может заменить его целиком.
    namespace Light {
        static constexpr ImVec4 Bg     = {0.929f, 0.914f, 0.875f, 1};
        static constexpr ImVec4 LeftBg = {0.875f, 0.851f, 0.800f, 1};
        static constexpr ImVec4 Card   = {0.976f, 0.965f, 0.933f, 1};
        static constexpr ImVec4 Acc    = {0.690f, 0.396f, 0.157f, 1};
        static constexpr ImVec4 AccDk  = {0.514f, 0.275f, 0.110f, 1};
        static constexpr ImVec4 Red    = {0.698f, 0.235f, 0.192f, 1};
        static constexpr ImVec4 Txt    = {0.122f, 0.118f, 0.102f, 1};
        static constexpr ImVec4 Dim    = {0.408f, 0.396f, 0.349f, 1};
        static constexpr ImVec4 TrkOff = {0.769f, 0.741f, 0.678f, 1};
        static constexpr ImVec4 Sep    = {0.749f, 0.718f, 0.651f, 1};
    }
    namespace Dark {
        static constexpr ImVec4 Bg     = {0.082f, 0.086f, 0.075f, 1};
        static constexpr ImVec4 LeftBg = {0.055f, 0.059f, 0.051f, 1};
        static constexpr ImVec4 Card   = {0.137f, 0.141f, 0.122f, 1};
        static constexpr ImVec4 Acc    = {0.843f, 0.635f, 0.310f, 1};
        static constexpr ImVec4 AccDk  = {0.690f, 0.478f, 0.216f, 1};
        static constexpr ImVec4 Red    = {0.847f, 0.353f, 0.290f, 1};
        static constexpr ImVec4 Txt    = {0.933f, 0.910f, 0.855f, 1};
        static constexpr ImVec4 Dim    = {0.584f, 0.565f, 0.514f, 1};
        static constexpr ImVec4 TrkOff = {0.220f, 0.224f, 0.192f, 1};
        static constexpr ImVec4 Sep    = {0.267f, 0.267f, 0.227f, 1};
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
    static inline ImVec4 AccDkOf(ImVec4 v) { return {v.x*0.72f, v.y*0.72f, v.z*0.68f, 1.f}; }
    static inline ImVec4 Acc()    { return Lerp4(AccLight(), AccDark()); }
    static inline ImVec4 AccDk()  { return g_accentOn ? AccDkOf(g_accent) : Lerp4(Light::AccDk, Dark::AccDk); }
    static inline ImVec4 Red()    { return Lerp4(Light::Red,    Dark::Red);    }
    static inline ImVec4 Txt()    { return Lerp4(Light::Txt,    Dark::Txt);    }
    static inline ImVec4 Dim()    { return Lerp4(Light::Dim,    Dark::Dim);    }
    static inline ImVec4 TrkOff() { return Lerp4(Light::TrkOff, Dark::TrkOff); }
    static inline ImVec4 Sep()    { return Lerp4(Light::Sep,    Dark::Sep);    }

    static inline ImVec4 Raised() {
        const ImVec4 card = Card();
        const ImVec4 txt = Txt();
        const float t = g_darkTheme ? 0.045f : 0.025f;
        return {Lerpf(card.x, txt.x, t), Lerpf(card.y, txt.y, t), Lerpf(card.z, txt.z, t), 1.f};
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
    static constexpr float HeaderH   = 86.f;
    static constexpr float Inset     = 18.f;
    static constexpr float PadX      = 22.f;
    static constexpr float BtnH      = 58.f;
    static constexpr float BottomH   = 116.f;
    static constexpr float TabW      = 126.f;
    static constexpr float RailW     = 226.f;
    static constexpr float TabHV     = 82.f;
}

void ApplyTheme();
