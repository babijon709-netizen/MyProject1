#pragma once
// Тема: палитры Light/Dark, радиусы, габариты строк, ApplyTheme().
// Вынесено из main.cpp — цветами и габаритами пользуются все модули гуи.
//
// Дизайн «legit.gui» (референс — CheatMenu.cpp): почти чёрный
// фиолетовый фон, неоновая маджента в акцентах и рамках, жёлтые
// заголовки секций, зелёная точка «онлайн». Светлая тема — та же
// схема, перевёрнутая в светло-серое.

#include "imgui.h"
#include "ui_util.h"   // Lerpf / EaseInOut в Lerp4

// Радиусы: карточки крупнее, элементы управления мягче.
namespace R {
    static constexpr float Card  = 11.f;
    static constexpr float Pill  = 11.f;
    static constexpr float Sheet = 14.f;
    static constexpr float Btn   = 8.f;
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
        static constexpr ImVec4 Bg     = {0.925f, 0.918f, 0.929f, 1};
        static constexpr ImVec4 LeftBg = {0.878f, 0.867f, 0.886f, 1};
        static constexpr ImVec4 Card   = {0.976f, 0.973f, 0.980f, 1};
        static constexpr ImVec4 Acc    = {0.830f, 0.180f, 0.530f, 1};
        static constexpr ImVec4 AccDk  = {0.690f, 0.110f, 0.435f, 1};
        static constexpr ImVec4 Red    = {0.870f, 0.220f, 0.208f, 1};
        static constexpr ImVec4 Txt    = {0.110f, 0.094f, 0.125f, 1};
        static constexpr ImVec4 Dim    = {0.490f, 0.463f, 0.510f, 1};
        static constexpr ImVec4 TrkOff = {0.866f, 0.848f, 0.873f, 1};
        static constexpr ImVec4 Sep    = {0.847f, 0.833f, 0.856f, 1};
    }
    namespace Dark {
        static constexpr ImVec4 Bg     = {0.050f, 0.030f, 0.061f, 1};
        static constexpr ImVec4 LeftBg = {0.070f, 0.043f, 0.086f, 1};
        static constexpr ImVec4 Card   = {0.080f, 0.050f, 0.100f, 1};
        static constexpr ImVec4 Acc    = {0.920f, 0.280f, 0.660f, 1};
        static constexpr ImVec4 AccDk  = {0.750f, 0.150f, 0.500f, 1};
        static constexpr ImVec4 Red    = {0.960f, 0.240f, 0.280f, 1};
        static constexpr ImVec4 Txt    = {0.920f, 0.880f, 0.920f, 1};
        static constexpr ImVec4 Dim    = {0.550f, 0.500f, 0.580f, 1};
        static constexpr ImVec4 TrkOff = {0.120f, 0.080f, 0.140f, 1};
        static constexpr ImVec4 Sep    = {0.160f, 0.090f, 0.180f, 1};
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
    // Заливка «элемента» — кнопки, светофор, дно дока (ImGuiCol_Button
    // референса: фиолетово-розовый чуть светлее фона карточки).
    static inline ImVec4 El()     { return Lerp4({0.886f, 0.871f, 0.890f, 1.f},
                                                 {0.160f, 0.090f, 0.180f, 1.f}); }
    // Неоновая кромка панелей — маджента ~32% (ImGuiCol_Border референса).
    static inline ImVec4 Hair()   { return Lerp4A({0.55f, 0.18f, 0.42f, 0.28f}, {0.85f, 0.25f, 0.65f, 0.32f}); }
    // Жёлтый заголовков секций («== Aimbot ==» в референсе).
    static inline ImVec4 Hdr()    { return Lerp4({0.580f, 0.480f, 0.050f, 1.f}, {0.950f, 0.850f, 0.400f, 1.f}); }
    // Зелёный индикатор «* online».
    static inline ImVec4 Green()  { return Lerp4({0.180f, 0.580f, 0.270f, 1.f}, {0.450f, 0.900f, 0.550f, 1.f}); }
    // Текст на заливке акцентом: белый на мадженте (обе темы).
    static inline ImVec4 OnAcc()  { return {1.f, 1.f, 1.f, 1.f}; }

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
