#pragma once
// Тема: палитры Light/Dark, радиусы, габариты строк, ApplyTheme().
// Вынесено из main.cpp — цветами и габаритами пользуются все модули гуи.

#include "imgui.h"
#include "ui_util.h"   // Lerpf / EaseInOut в Lerp4

// g_darkTheme — целевое состояние (меню/конфиг), g_themeT — сглаженный
// переход 0..1, g_menuFadeIn — затухание меню при входе.
namespace R {
    static constexpr float Card  = 24.f;
    static constexpr float Pill  = 16.f;
    static constexpr float Sheet = 26.f;
    static constexpr float Btn   = 18.f;
}

// Тёмная тема по умолчанию — лучше сочетается с фиолетовым акцентом
// и не слепит поверх игры; светлая включается в Настройках как раньше.
extern bool  g_darkTheme;
extern float g_themeT;
extern float g_menuFadeIn;

namespace C {
    // "Graphite & Violet": спокойный графитовый фон + фиолетовый акцент.
    // Light — мягкий светло-серый с лёгким лавандовым оттенком,
    // Dark  — глубокий сине-графитовый (почти чёрный), карточки чуть светлее.
    namespace Light {
        static constexpr ImVec4 Bg     = {0.929f, 0.933f, 0.953f, 1};
        static constexpr ImVec4 LeftBg = {0.957f, 0.957f, 0.973f, 1};
        static constexpr ImVec4 Card   = {1.000f, 1.000f, 1.000f, 1};
        static constexpr ImVec4 Acc    = {0.424f, 0.361f, 0.906f, 1};
        static constexpr ImVec4 AccDk  = {0.333f, 0.275f, 0.784f, 1};
        static constexpr ImVec4 Red    = {1.000f, 0.271f, 0.227f, 1};
        static constexpr ImVec4 Txt    = {0.078f, 0.082f, 0.102f, 1};
        static constexpr ImVec4 Dim    = {0.541f, 0.553f, 0.596f, 1};
        static constexpr ImVec4 TrkOff = {0.788f, 0.796f, 0.839f, 1};
        static constexpr ImVec4 Sep    = {0.851f, 0.859f, 0.898f, 1};
    }
    namespace Dark {
        static constexpr ImVec4 Bg     = {0.063f, 0.071f, 0.094f, 1};
        static constexpr ImVec4 LeftBg = {0.047f, 0.055f, 0.075f, 1};
        static constexpr ImVec4 Card   = {0.106f, 0.118f, 0.153f, 1};
        static constexpr ImVec4 Acc    = {0.545f, 0.486f, 1.000f, 1};
        static constexpr ImVec4 AccDk  = {0.424f, 0.361f, 0.906f, 1};
        static constexpr ImVec4 Red    = {1.000f, 0.361f, 0.322f, 1};
        static constexpr ImVec4 Txt    = {0.949f, 0.953f, 0.969f, 1};
        static constexpr ImVec4 Dim    = {0.604f, 0.616f, 0.663f, 1};
        static constexpr ImVec4 TrkOff = {0.180f, 0.196f, 0.251f, 1};
        static constexpr ImVec4 Sep    = {0.196f, 0.212f, 0.271f, 1};
    }

    static inline ImVec4 Lerp4(ImVec4 a, ImVec4 b) {
        float t = EaseInOut(g_themeT);
        return {Lerpf(a.x,b.x,t), Lerpf(a.y,b.y,t), Lerpf(a.z,b.z,t), 1.f};
    }

    static inline ImVec4 Bg()     { return Lerp4(Light::Bg,     Dark::Bg);     }
    static inline ImVec4 LeftBg() { return Lerp4(Light::LeftBg, Dark::LeftBg); }
    static inline ImVec4 Card()   { return Lerp4(Light::Card,   Dark::Card);   }
    static inline ImVec4 Acc()    { return Lerp4(Light::Acc,    Dark::Acc);    }
    static inline ImVec4 AccDk()  { return Lerp4(Light::AccDk,  Dark::AccDk);  }
    static inline ImVec4 Red()    { return Lerp4(Light::Red,    Dark::Red);    }
    static inline ImVec4 Txt()    { return Lerp4(Light::Txt,    Dark::Txt);    }
    static inline ImVec4 Dim()    { return Lerp4(Light::Dim,    Dark::Dim);    }
    static inline ImVec4 TrkOff() { return Lerp4(Light::TrkOff, Dark::TrkOff); }
    static inline ImVec4 Sep()    { return Lerp4(Light::Sep,    Dark::Sep);    }

    static ImU32 Mix(ImVec4 a, ImVec4 b, float t) {
        return IM_COL32(
            int(Lerpf(a.x, b.x, t) * 255), int(Lerpf(a.y, b.y, t) * 255),
            int(Lerpf(a.z, b.z, t) * 255), int(Lerpf(a.w, b.w, t) * 255));
    }
    static ImU32 U(ImVec4 v)               { return ImGui::ColorConvertFloat4ToU32(v); }
    static ImU32 UA(ImVec4 v, float alpha) { v.w = alpha; return ImGui::ColorConvertFloat4ToU32(v); }
}


namespace Layout {
    static constexpr float RowH      = 78.f;
    static constexpr float SliderH   = 108.f;
    static constexpr float HeaderH   = 66.f;
    static constexpr float Inset     = 16.f;
    static constexpr float PadX      = 20.f;
    static constexpr float BtnH      = 62.f;
    // Нижняя панель вкладок: строка по центру, иконка + подпись.
    // Кнопки крупные: панель выше и ячейки шире (~2x от первоначальных).
    static constexpr float BottomH   = 150.f;
    static constexpr float TabW      = 136.f;
    // Левая панель вкладок: вертикальный столбец по центру.
    static constexpr float RailW     = 128.f;
    static constexpr float TabHV     = 108.f;
}

void ApplyTheme();
