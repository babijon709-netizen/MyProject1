#pragma once
// Тема: палитры Light/Dark, радиусы, габариты строк, ApplyTheme().
// Вынесено из main.cpp — цветами и габаритами пользуются все модули гуи.

#include "imgui.h"
#include "ui_util.h"   // Lerpf / EaseInOut в Lerp4

// g_darkTheme — целевое состояние (меню/конфиг), g_themeT — сглаженный
// переход 0..1, g_menuFadeIn — затухание меню при входе.
// Стиль «ASCII-терминал Linux»: у терминала нет скруглений — все радиусы
// нулевые, форму задают рамки из символов псевдографики (см. ascii.h).
namespace R {
    static constexpr float Card  = 0.f;
    static constexpr float Pill  = 0.f;
    static constexpr float Sheet = 0.f;
    static constexpr float Btn   = 0.f;
}

// Тёмная тема по умолчанию — лучше сочетается с фиолетовым акцентом
// и не слепит поверх игры; светлая включается в Настройках как раньше.
extern bool  g_darkTheme;
extern float g_themeT;
extern float g_menuFadeIn;
// Акцент на выбор пользователя (палитра в «Опциях»): включён — весь GUI
// красится им вместо фиолетового; выключен — классический Light/Dark.
extern ImVec4 g_accent;
extern bool   g_accentOn;

namespace C {
    // «Green phosphor terminal»: тёмная тема — классический зелёный
    // монохромный фосфор P1 на почти чёрном экране; светлая — «бумажный»
    // терминал xterm по умолчанию (чёрное на белом), акцент — тёмно-зелёный,
    // как цвета ls на светлой консоли.
    namespace Light {
        static constexpr ImVec4 Bg     = {0.925f, 0.925f, 0.906f, 1};
        static constexpr ImVec4 LeftBg = {0.867f, 0.867f, 0.848f, 1};
        static constexpr ImVec4 Card   = {0.965f, 0.965f, 0.949f, 1};
        static constexpr ImVec4 Acc    = {0.047f, 0.451f, 0.161f, 1};
        static constexpr ImVec4 AccDk  = {0.031f, 0.337f, 0.118f, 1};
        static constexpr ImVec4 Red    = {0.780f, 0.137f, 0.098f, 1};
        static constexpr ImVec4 Txt    = {0.086f, 0.106f, 0.086f, 1};
        static constexpr ImVec4 Dim    = {0.376f, 0.416f, 0.376f, 1};
        static constexpr ImVec4 TrkOff = {0.816f, 0.816f, 0.796f, 1};
        static constexpr ImVec4 Sep    = {0.722f, 0.722f, 0.702f, 1};
    }
    namespace Dark {
        static constexpr ImVec4 Bg     = {0.016f, 0.031f, 0.020f, 1};
        static constexpr ImVec4 LeftBg = {0.008f, 0.016f, 0.012f, 1};
        static constexpr ImVec4 Card   = {0.035f, 0.063f, 0.043f, 1};
        static constexpr ImVec4 Acc    = {0.200f, 0.851f, 0.353f, 1};
        static constexpr ImVec4 AccDk  = {0.118f, 0.549f, 0.220f, 1};
        static constexpr ImVec4 Red    = {1.000f, 0.322f, 0.271f, 1};
        static constexpr ImVec4 Txt    = {0.663f, 0.941f, 0.702f, 1};
        static constexpr ImVec4 Dim    = {0.318f, 0.549f, 0.361f, 1};
        static constexpr ImVec4 TrkOff = {0.059f, 0.129f, 0.075f, 1};
        static constexpr ImVec4 Sep    = {0.102f, 0.220f, 0.125f, 1};
    }

    static inline ImVec4 Lerp4(ImVec4 a, ImVec4 b) {
        float t = EaseInOut(g_themeT);
        return {Lerpf(a.x,b.x,t), Lerpf(a.y,b.y,t), Lerpf(a.z,b.z,t), 1.f};
    }

    static inline ImVec4 Bg()     { return Lerp4(Light::Bg,     Dark::Bg);     }
    static inline ImVec4 LeftBg() { return Lerp4(Light::LeftBg, Dark::LeftBg); }
    static inline ImVec4 Card()   { return Lerp4(Light::Card,   Dark::Card);   }
    static inline ImVec4 AccLight() { return g_accentOn ? g_accent : Light::Acc; }
    static inline ImVec4 AccDark()  { return g_accentOn ? g_accent : Dark::Acc; }
    static inline ImVec4 AccDkOf(ImVec4 v) { return {v.x*0.78f, v.y*0.78f, v.z*0.82f, 1.f}; }
    static inline ImVec4 Acc()    { return Lerp4(AccLight(), AccDark()); }
    static inline ImVec4 AccDk()  { return g_accentOn ? AccDkOf(g_accent) : Lerp4(Light::AccDk, Dark::AccDk); }
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
