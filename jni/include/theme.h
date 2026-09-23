#pragma once
// Тема: палитры Light/Dark, радиусы, габариты строк, ApplyTheme().
// Вынесено из main.cpp — цветами и габаритами пользуются все модули гуи.
//
// Дизайн «Глубина»: три слоя поверхности (окно → панель → карточка),
// волосяные рамки вместо тяжёлых обводок и один акцент — циан, который
// в градиентах уходит в индиго. Светлая тема — та же лестница,
// перевёрнутая в светло-серое.

#include "imgui.h"
#include "ui_util.h"   // Lerpf / EaseInOut в Lerp4

// Радиусы: карточки крупнее, элементы управления мягче.
namespace R {
    static constexpr float Card  = 18.f;
    static constexpr float Pill  = 15.f;
    static constexpr float Sheet = 22.f;
    static constexpr float Btn   = 13.f;
}

// Тёмная тема по умолчанию — не слепит поверх игры; светлая включается
// в Настройках как раньше.
extern bool  g_darkTheme;
extern float g_themeT;
extern float g_menuFadeIn;
// Акцент на выбор пользователя (палитра в «Опциях»): включён — весь GUI
// красится им; выключен — циан из палитры темы.
extern ImVec4 g_accent;
extern bool   g_accentOn;

namespace C {
    // Dark — лестница «окно → панель → карточка», каждая ступень светлее
    // предыдущей; Light — обратная лестница от светло-серого к белому.
    namespace Light {
        static constexpr ImVec4 Bg     = {0.906f, 0.910f, 0.929f, 1};
        static constexpr ImVec4 LeftBg = {0.859f, 0.865f, 0.890f, 1};
        static constexpr ImVec4 Card   = {0.973f, 0.975f, 0.982f, 1};
        static constexpr ImVec4 Acc    = {0.024f, 0.576f, 0.741f, 1};
        static constexpr ImVec4 AccDk  = {0.012f, 0.459f, 0.604f, 1};
        static constexpr ImVec4 Acc2   = {0.310f, 0.353f, 0.878f, 1};
        static constexpr ImVec4 Red    = {0.906f, 0.235f, 0.220f, 1};
        static constexpr ImVec4 Txt    = {0.086f, 0.094f, 0.125f, 1};
        static constexpr ImVec4 Dim    = {0.467f, 0.482f, 0.537f, 1};
        static constexpr ImVec4 TrkOff = {0.827f, 0.833f, 0.859f, 1};
        static constexpr ImVec4 Sep    = {0.816f, 0.822f, 0.847f, 1};
    }
    namespace Dark {
        static constexpr ImVec4 Bg     = {0.075f, 0.081f, 0.110f, 1};
        static constexpr ImVec4 LeftBg = {0.055f, 0.061f, 0.086f, 1};
        static constexpr ImVec4 Card   = {0.110f, 0.118f, 0.157f, 1};
        static constexpr ImVec4 Acc    = {0.129f, 0.765f, 0.898f, 1};
        static constexpr ImVec4 AccDk  = {0.067f, 0.522f, 0.647f, 1};
        static constexpr ImVec4 Acc2   = {0.388f, 0.427f, 0.973f, 1};
        static constexpr ImVec4 Red    = {1.000f, 0.318f, 0.349f, 1};
        static constexpr ImVec4 Txt    = {0.933f, 0.941f, 0.961f, 1};
        static constexpr ImVec4 Dim    = {0.557f, 0.576f, 0.639f, 1};
        static constexpr ImVec4 TrkOff = {0.157f, 0.165f, 0.212f, 1};
        static constexpr ImVec4 Sep    = {0.196f, 0.204f, 0.259f, 1};
    }

    static inline ImVec4 Lerp4(ImVec4 a, ImVec4 b) {
        float t = EaseInOut(g_themeT);
        return {Lerpf(a.x,b.x,t), Lerpf(a.y,b.y,t), Lerpf(a.z,b.z,t), 1.f};
    }
    // Как Lerp4, но альфа тоже интерполируется (для полупрозрачных токенов).
    static inline ImVec4 Lerp4A(ImVec4 a, ImVec4 b) {
        float t = EaseInOut(g_themeT);
        return {Lerpf(a.x,b.x,t), Lerpf(a.y,b.y,t), Lerpf(a.z,b.z,t), Lerpf(a.w,b.w,t)};
    }

    static inline ImVec4 Bg()     { return Lerp4(Light::Bg,     Dark::Bg);     }
    static inline ImVec4 LeftBg() { return Lerp4(Light::LeftBg, Dark::LeftBg); }
    static inline ImVec4 Card()   { return Lerp4(Light::Card,   Dark::Card);   }
    static inline ImVec4 AccLight() { return g_accentOn ? g_accent : Light::Acc; }
    static inline ImVec4 AccDark()  { return g_accentOn ? g_accent : Dark::Acc; }
    static inline ImVec4 AccDkOf(ImVec4 v) { return {v.x*0.60f, v.y*0.66f, v.z*0.72f, 1.f}; }
    static inline ImVec4 Acc()    { return Lerp4(AccLight(), AccDark()); }
    static inline ImVec4 AccDk()  { return g_accentOn ? AccDkOf(g_accent) : Lerp4(Light::AccDk, Dark::AccDk); }
    static inline ImVec4 Red()    { return Lerp4(Light::Red,    Dark::Red);    }
    static inline ImVec4 Txt()    { return Lerp4(Light::Txt,    Dark::Txt);    }
    static inline ImVec4 Dim()    { return Lerp4(Light::Dim,    Dark::Dim);    }
    static inline ImVec4 TrkOff() { return Lerp4(Light::TrkOff, Dark::TrkOff); }
    static inline ImVec4 Sep()    { return Lerp4(Light::Sep,    Dark::Sep);    }

    // Второй стоп градиента акцента: у выбранного пользователем акцента
    // получается смещённый в индиго оттенок, у циана — глубокий синий.
    static inline ImVec4 Acc2() {
        ImVec4 a = Acc();
        return {(a.x + 0.36f) * 0.5f, (a.y + 0.40f) * 0.5f, (a.z + 0.95f) * 0.5f, 1.f};
    }
    // Заливка «элемента» — кнопки-призраки, светофор, дно дока: чуть
    // светлее дорожек, чтобы кнопка читалась на карточке как «выпуклая».
    static inline ImVec4 El()     { return Lerp4({0.847f, 0.851f, 0.875f, 1.f},
                                                 {0.176f, 0.184f, 0.235f, 1.f}); }
    // Волосяная рамка карточек: едва заметная светлая (тёмная тема) или
    // тёмная (светлая) линия по контуру.
    static inline ImVec4 Hair()   { return Lerp4A({0.05f, 0.06f, 0.10f, 0.10f}, {0.85f, 0.90f, 1.00f, 0.075f}); }
    // Подложка под текстом на акценте: тёмно-синий на циане, белый в светлой.
    static inline ImVec4 OnAcc()  { return Lerp4({1.f, 1.f, 1.f, 1.f}, {0.016f, 0.055f, 0.090f, 1.f}); }

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
