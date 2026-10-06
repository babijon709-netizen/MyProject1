#pragma once

#include "imgui.h"
#include "imgui_internal.h"
#include <cmath>
#include <vector>
#include <string>
#include <map>
#include <unordered_map>
#include <algorithm>
#include <cstdlib>

// ==========================================
// 1. SNOWFLAKE PARTICLE SYSTEM
// ==========================================
namespace Snowflake {
    struct vec3 {
        float x, y, z;
        vec3() : x(0.f), y(0.f), z(0.f) {}
        vec3(float tX, float tY, float tZ) : x(tX), y(tY), z(tZ) {}
        vec3(float tX, float tY) : x(tX), y(tY), z(0.f) {}

        vec3 operator+(const vec3& t) const { return vec3(x + t.x, y + t.y, z + t.z); }
        vec3& operator+=(const vec3& t) { x += t.x; y += t.y; z += t.z; return *this; }
        vec3& operator*=(float t) { x *= t; y *= t; z *= t; return *this; }
        vec3& operator/=(float t) { x /= t; y /= t; z /= t; return *this; }
        bool operator==(const vec3& t) const { return x == t.x && y == t.y && z == t.z; }

        float MagSq() const { return x * x + y * y + z * z; }
        float Mag() const { return std::sqrt(MagSq()); }
        vec3 Limit(float max) {
            float mSq = MagSq();
            if (mSq > max * max) {
                *this /= std::sqrt(mSq);
                *this *= max;
            }
            return *this;
        }
    };

    class Snowflake {
    private:
        vec3 velocity;
        vec3 accelaretion;
        float radius;
        ImU32 color;
        float minSize;
        float maxSize;

    public:
        int width;
        int height;
        int windowX;
        int windowY;
        vec3 pos;

        Snowflake(float _minSize, float _maxSize, int _windowX, int _windowY, int _width, int _height, ImU32 _color) {
            minSize = _minSize;
            maxSize = _maxSize;
            windowX = _windowX;
            windowY = _windowY;
            width = _width;
            height = _height;
            color = _color;
            float rx = ((float)rand() / (float)RAND_MAX) * width + windowX;
            float ry = ((float)rand() / (float)RAND_MAX) * 90.f + (windowY - 100.f);
            pos = vec3(rx, ry);
            velocity = vec3(0.f, 0.f);
            accelaretion = vec3();
            float r = std::pow(((float)rand() / (float)RAND_MAX), 3);
            radius = std::max(minSize, std::min(r * maxSize, maxSize));
        }

        void ApplyForce(vec3 force) {
            vec3 f = force;
            f *= radius;
            accelaretion += f;
        }

        void Update() {
            velocity += accelaretion;
            velocity.Limit(radius * 0.2f);
            pos += velocity;
            accelaretion *= 0.f;
            if (OffScreen()) Randomize();
        }

        void Render() {
            ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(pos.x, pos.y), radius, color);
        }

        bool OffScreen() const {
            return (pos.y > windowY + height + radius || pos.x < windowX - radius || pos.x > windowX + width + radius);
        }

        void Randomize() {
            float rx = ((float)rand() / (float)RAND_MAX) * width + windowX;
            float ry = ((float)rand() / (float)RAND_MAX) * 90.f + (windowY - 100.f);
            pos = vec3(rx, ry);
            velocity = vec3(0.f, 0.f);
            accelaretion = vec3();
            float r = std::pow(((float)rand() / (float)RAND_MAX), 3);
            radius = std::max(minSize, std::min(r * maxSize, maxSize));
        }

        bool operator==(const Snowflake& target) const {
            return pos == target.pos && velocity == target.velocity && accelaretion == target.accelaretion && radius == target.radius;
        }
    };

    static inline vec3 g_gravity(0.f, 0.005f);

    inline void CreateSnowFlakes(std::vector<Snowflake>& snow, uint64_t limit, float _minSize, float _maxSize, int _windowX, int _windowY, int _width, int _height, vec3 _gravity, ImU32 _color) {
        g_gravity = _gravity;
        snow.clear();
        for (uint64_t i = 0; i < limit; i++)
            snow.push_back(Snowflake(_minSize, _maxSize, _windowX, _windowY, _width, _height, _color));
    }

    inline void Update(std::vector<Snowflake>& snow, vec3 mouse, vec3 windowPos) {
        mouse.x -= windowPos.x;
        mouse.y -= windowPos.y;

        for (Snowflake& flake : snow) {
            float xOff = flake.pos.x / (flake.windowX + flake.width + 1.f);
            float yOff = flake.pos.y / (flake.windowY + flake.height + 1.f);
            float n = (mouse.x - flake.windowX) / (float)(flake.width > 0 ? flake.width : 1);
            float wx = (n * 0.004f) - 0.002f;
            vec3 wind(wx + (xOff * .002f), (yOff * .002f));
            wind *= .5f;

            flake.ApplyForce(g_gravity);
            flake.ApplyForce(wind);
            flake.Update();
            flake.Render();
        }
    }
}

// Global snowflake container
inline std::vector<Snowflake::Snowflake> g_nike_snow;

// ==========================================
// 2. THEME & LINEAR INTERPOLATION SYSTEM
// ==========================================
struct ColorStyle {
    ImVec4 windowBg;
    ImVec4 border;
    ImVec4 text;
    ImVec4 button;
    ImVec4 buttonHovered;
    ImVec4 buttonActive;
    ImVec4 sliderGrab;
    ImVec4 sliderGrabActive;
    ImVec4 scrollbarBg;
    ImVec4 scrollbarGrab;
    ImVec4 scrollbarGrabActive;
    ImVec4 checkMark;
    ImVec4 frameBg;
    ImVec4 frameBgHovered;
    ImVec4 frameBgActive;
    ImVec4 header;
    ImVec4 headerHovered;
    ImVec4 headerActive;
    ImVec4 titleBg;
    ImVec4 titleBgActive;
    ImVec4 titleBgCollapsed;
    ImVec4 menuBarBg;
    ImVec4 popupBg;
    ImVec4 childBg;
};

inline const ColorStyle LIGHT_STYLE = {
    ImVec4(40.0f/255.0f, 41.0f/255.0f, 45.0f/255.0f, 255.0f/255.0f),
    ImVec4(60.0f/255.0f, 63.0f/255.0f, 67.0f/255.0f, 255.0f/255.0f),
    ImVec4(240.0f/255.0f, 240.0f/255.0f, 240.0f/255.0f, 255.0f/255.0f),
    ImVec4(72.0f/255.0f, 120.0f/255.0f, 249.0f/255.0f, 255.0f/255.0f),
    ImVec4(82.0f/255.0f, 132.0f/255.0f, 255.0f/255.0f, 255.0f/255.0f),
    ImVec4(62.0f/255.0f, 112.0f/255.0f, 249.0f/255.0f, 255.0f/255.0f),
    ImVec4(72.0f/255.0f, 120.0f/255.0f, 249.0f/255.0f, 255.0f/255.0f),
    ImVec4(82.0f/255.0f, 132.0f/255.0f, 255.0f/255.0f, 255.0f/255.0f),
    ImVec4(60.0f/255.0f, 63.0f/255.0f, 67.0f/255.0f, 255.0f/255.0f),
    ImVec4(80.0f/255.0f, 84.0f/255.0f, 88.0f/255.0f, 255.0f/255.0f),
    ImVec4(70.0f/255.0f, 73.0f/255.0f, 77.0f/255.0f, 255.0f/255.0f),
    ImVec4(240.0f/255.0f, 240.0f/255.0f, 240.0f/255.0f, 255.0f/255.0f),
    ImVec4(48.0f/255.0f, 50.0f/255.0f, 54.0f/255.0f, 255.0f/255.0f),
    ImVec4(53.0f/255.0f, 55.0f/255.0f, 59.0f/255.0f, 255.0f/255.0f),
    ImVec4(58.0f/255.0f, 60.0f/255.0f, 64.0f/255.0f, 255.0f/255.0f),
    ImVec4(72.0f/255.0f, 120.0f/255.0f, 249.0f/255.0f, 255.0f/255.0f),
    ImVec4(82.0f/255.0f, 132.0f/255.0f, 255.0f/255.0f, 255.0f/255.0f),
    ImVec4(82.0f/255.0f, 132.0f/255.0f, 255.0f/255.0f, 255.0f/255.0f),
    ImVec4(28.0f/255.0f, 29.0f/255.0f, 32.0f/255.0f, 255.0f/255.0f),
    ImVec4(33.0f/255.0f, 35.0f/255.0f, 38.0f/255.0f, 255.0f/255.0f),
    ImVec4(38.0f/255.0f, 39.0f/255.0f, 43.0f/255.0f, 255.0f/255.0f),
    ImVec4(28.0f/255.0f, 29.0f/255.0f, 32.0f/255.0f, 255.0f/255.0f),
    ImVec4(58.0f/255.0f, 59.0f/255.0f, 63.0f/255.0f, 255.0f/255.0f),
    ImVec4(58.0f/255.0f, 59.0f/255.0f, 63.0f/255.0f, 255.0f/255.0f)
};

inline const ColorStyle DARK_STYLE = {
    ImVec4(20.0f/255.0f, 20.0f/255.0f, 24.0f/255.0f, 255.0f/255.0f),
    ImVec4(40.0f/255.0f, 40.0f/255.0f, 48.0f/255.0f, 255.0f/255.0f),
    ImVec4(240.0f/255.0f, 240.0f/255.0f, 240.0f/255.0f, 255.0f/255.0f),
    ImVec4(72.0f/255.0f, 120.0f/255.0f, 249.0f/255.0f, 255.0f/255.0f),
    ImVec4(85.0f/255.0f, 135.0f/255.0f, 255.0f/255.0f, 255.0f/255.0f),
    ImVec4(60.0f/255.0f, 105.0f/255.0f, 230.0f/255.0f, 255.0f/255.0f),
    ImVec4(72.0f/255.0f, 120.0f/255.0f, 249.0f/255.0f, 255.0f/255.0f),
    ImVec4(85.0f/255.0f, 135.0f/255.0f, 255.0f/255.0f, 255.0f/255.0f),
    ImVec4(30.0f/255.0f, 30.0f/255.0f, 35.0f/255.0f, 255.0f/255.0f),
    ImVec4(60.0f/255.0f, 60.0f/255.0f, 70.0f/255.0f, 255.0f/255.0f),
    ImVec4(80.0f/255.0f, 80.0f/255.0f, 90.0f/255.0f, 255.0f/255.0f),
    ImVec4(240.0f/255.0f, 240.0f/255.0f, 240.0f/255.0f, 255.0f/255.0f),
    ImVec4(28.0f/255.0f, 28.0f/255.0f, 34.0f/255.0f, 255.0f/255.0f),
    ImVec4(35.0f/255.0f, 35.0f/255.0f, 42.0f/255.0f, 255.0f/255.0f),
    ImVec4(42.0f/255.0f, 42.0f/255.0f, 50.0f/255.0f, 255.0f/255.0f),
    ImVec4(72.0f/255.0f, 120.0f/255.0f, 249.0f/255.0f, 255.0f/255.0f),
    ImVec4(85.0f/255.0f, 135.0f/255.0f, 255.0f/255.0f, 255.0f/255.0f),
    ImVec4(85.0f/255.0f, 135.0f/255.0f, 255.0f/255.0f, 255.0f/255.0f),
    ImVec4(18.0f/255.0f, 18.0f/255.0f, 22.0f/255.0f, 255.0f/255.0f),
    ImVec4(24.0f/255.0f, 24.0f/255.0f, 28.0f/255.0f, 255.0f/255.0f),
    ImVec4(24.0f/255.0f, 24.0f/255.0f, 28.0f/255.0f, 255.0f/255.0f),
    ImVec4(18.0f/255.0f, 18.0f/255.0f, 22.0f/255.0f, 255.0f/255.0f),
    ImVec4(32.0f/255.0f, 32.0f/255.0f, 38.0f/255.0f, 255.0f/255.0f),
    ImVec4(32.0f/255.0f, 32.0f/255.0f, 38.0f/255.0f, 255.0f/255.0f)
};

struct ThemeTransitionState {
    bool active = false;
    float startTime = 0.0f;
    float duration = 0.3f;
    float progress = 0.0f;
    ImVec4 fromColors[ImGuiCol_COUNT];
    ImVec4 toColors[ImGuiCol_COUNT];
};

inline ThemeTransitionState g_themeTransition;

inline ImVec4 LerpColors(const ImVec4& a, const ImVec4& b, float t) {
    return ImVec4(
        a.x + (b.x - a.x) * t,
        a.y + (b.y - a.y) * t,
        a.z + (b.z - a.z) * t,
        a.w + (b.w - a.w) * t
    );
}

inline void 二代线性插值() {
    if (!g_themeTransition.active) return;
    float cur = ImGui::GetTime();
    g_themeTransition.progress = (cur - g_themeTransition.startTime) / g_themeTransition.duration;
    ImGuiStyle& style = ImGui::GetStyle();
    if (g_themeTransition.progress >= 1.0f) {
        for (int i = 0; i < ImGuiCol_COUNT; i++)
            style.Colors[i] = g_themeTransition.toColors[i];
        g_themeTransition.active = false;
    } else {
        for (int i = 0; i < ImGuiCol_COUNT; i++)
            style.Colors[i] = LerpColors(g_themeTransition.fromColors[i], g_themeTransition.toColors[i], g_themeTransition.progress);
    }
}

inline void ApplyFullTheme(const ColorStyle& style) {
    ImGuiStyle& imguiStyle = ImGui::GetStyle();
    imguiStyle.Colors[ImGuiCol_WindowBg] = style.windowBg;
    imguiStyle.Colors[ImGuiCol_Border] = style.border;
    imguiStyle.Colors[ImGuiCol_Text] = style.text;
    imguiStyle.Colors[ImGuiCol_Button] = style.button;
    imguiStyle.Colors[ImGuiCol_ButtonHovered] = style.buttonHovered;
    imguiStyle.Colors[ImGuiCol_ButtonActive] = style.buttonActive;
    imguiStyle.Colors[ImGuiCol_SliderGrab] = style.sliderGrab;
    imguiStyle.Colors[ImGuiCol_SliderGrabActive] = style.sliderGrabActive;
    imguiStyle.Colors[ImGuiCol_ScrollbarBg] = style.scrollbarBg;
    imguiStyle.Colors[ImGuiCol_ScrollbarGrab] = style.scrollbarGrab;
    imguiStyle.Colors[ImGuiCol_ScrollbarGrabActive] = style.scrollbarGrabActive;
    imguiStyle.Colors[ImGuiCol_CheckMark] = style.checkMark;
    imguiStyle.Colors[ImGuiCol_FrameBg] = style.frameBg;
    imguiStyle.Colors[ImGuiCol_FrameBgHovered] = style.frameBgHovered;
    imguiStyle.Colors[ImGuiCol_FrameBgActive] = style.frameBgActive;
    imguiStyle.Colors[ImGuiCol_Header] = style.header;
    imguiStyle.Colors[ImGuiCol_HeaderHovered] = style.headerHovered;
    imguiStyle.Colors[ImGuiCol_HeaderActive] = style.headerActive;
    imguiStyle.Colors[ImGuiCol_TitleBg] = style.titleBg;
    imguiStyle.Colors[ImGuiCol_TitleBgActive] = style.titleBgActive;
    imguiStyle.Colors[ImGuiCol_TitleBgCollapsed] = style.titleBgCollapsed;
    imguiStyle.Colors[ImGuiCol_MenuBarBg] = style.menuBarBg;
    imguiStyle.Colors[ImGuiCol_PopupBg] = style.popupBg;
    imguiStyle.Colors[ImGuiCol_ChildBg] = style.childBg;
}

inline void SwitchTheme(bool isLight) {
    if (g_themeTransition.active) return;
    g_themeTransition.active = true;
    g_themeTransition.startTime = ImGui::GetTime();
    ImGuiStyle& style = ImGui::GetStyle();
    for (int i = 0; i < ImGuiCol_COUNT; i++) {
        g_themeTransition.fromColors[i] = style.Colors[i];
    }
    const ColorStyle& target = isLight ? LIGHT_STYLE : DARK_STYLE;
    for (int i = 0; i < ImGuiCol_COUNT; i++) {
        if (i == ImGuiCol_WindowBg) g_themeTransition.toColors[i] = target.windowBg;
        else if (i == ImGuiCol_Border) g_themeTransition.toColors[i] = target.border;
        else if (i == ImGuiCol_Text) g_themeTransition.toColors[i] = target.text;
        else if (i == ImGuiCol_Button) g_themeTransition.toColors[i] = target.button;
        else if (i == ImGuiCol_ButtonHovered) g_themeTransition.toColors[i] = target.buttonHovered;
        else if (i == ImGuiCol_ButtonActive) g_themeTransition.toColors[i] = target.buttonActive;
        else if (i == ImGuiCol_SliderGrab) g_themeTransition.toColors[i] = target.sliderGrab;
        else if (i == ImGuiCol_SliderGrabActive) g_themeTransition.toColors[i] = target.sliderGrabActive;
        else if (i == ImGuiCol_ScrollbarBg) g_themeTransition.toColors[i] = target.scrollbarBg;
        else if (i == ImGuiCol_ScrollbarGrab) g_themeTransition.toColors[i] = target.scrollbarGrab;
        else if (i == ImGuiCol_ScrollbarGrabActive) g_themeTransition.toColors[i] = target.scrollbarGrabActive;
        else if (i == ImGuiCol_CheckMark) g_themeTransition.toColors[i] = target.checkMark;
        else if (i == ImGuiCol_FrameBg) g_themeTransition.toColors[i] = target.frameBg;
        else if (i == ImGuiCol_FrameBgHovered) g_themeTransition.toColors[i] = target.frameBgHovered;
        else if (i == ImGuiCol_FrameBgActive) g_themeTransition.toColors[i] = target.frameBgActive;
        else if (i == ImGuiCol_Header) g_themeTransition.toColors[i] = target.header;
        else if (i == ImGuiCol_HeaderHovered) g_themeTransition.toColors[i] = target.headerHovered;
        else if (i == ImGuiCol_HeaderActive) g_themeTransition.toColors[i] = target.headerActive;
        else if (i == ImGuiCol_TitleBg) g_themeTransition.toColors[i] = target.titleBg;
        else if (i == ImGuiCol_TitleBgActive) g_themeTransition.toColors[i] = target.titleBgActive;
        else if (i == ImGuiCol_TitleBgCollapsed) g_themeTransition.toColors[i] = target.titleBgCollapsed;
        else if (i == ImGuiCol_MenuBarBg) g_themeTransition.toColors[i] = target.menuBarBg;
        else if (i == ImGuiCol_PopupBg) g_themeTransition.toColors[i] = target.popupBg;
        else if (i == ImGuiCol_ChildBg) g_themeTransition.toColors[i] = target.childBg;
        else g_themeTransition.toColors[i] = target.windowBg;
    }
}

inline void window_set() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 14.0f;
    style.ChildRounding = 12.0f;
    style.FrameRounding = 8.0f;
    style.PopupRounding = 10.0f;
    style.ScrollbarRounding = 9.0f;
    style.GrabRounding = 8.0f;
    style.TabRounding = 10.0f;
    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.WindowPadding = ImVec2(10.0f, 10.0f);
    style.FramePadding = ImVec2(8.0f, 4.0f);
    style.ItemSpacing = ImVec2(10.0f, 10.0f);
}

inline void zhu_set() {
    window_set();
}

// ==========================================
// 3. LYRICS SYSTEM (歌词)
// ==========================================
inline std::vector<std::string> g_lyrics = {
    "一只飞鸟正轻啼着\nHey, my little bird is singing",
    "在无垠无名的蓝天上寻觅我\nSearching for me in the boundless blue sky",
    "在温柔织就的摇篮里\nIn the cradle woven with tenderness",
    "一同向明天进发吧\nLet's move forward to tomorrow together",
    "无论晴空万里或是狂风暴雨\nWhether it's sunny or stormy",
    "我会始终将你守护\nI'll always be by your side, protecting you",
    "你是无可替代的宝物\nYou're my irreplaceable treasure",
    "无名之花正静静沉睡\nThe unnamed flower is sleeping quietly",
    "张开被赠予的翅膀\nSpread your given wings",
    "永远别放弃飞翔 让我们约定吧\nLet's make a promise that we'll never give up flying"
};

struct LyricItem {
    std::string text;
    float yPos;
    float targetY;
    float scale;
    float targetScale;
    ImColor color;
    float alpha;
    bool isCurrent;
};

inline std::vector<LyricItem> g_lyricItems;
inline int g_currentLyricLine = 0;
inline float g_lyricTimer = 0.0f;
inline const float LYRIC_LINE_HEIGHT = 48.0f;
inline const float LYRIC_BASE_Y = 10.0f;
inline const float LYRIC_FONT_SIZE = 18.0f;
inline const int LYRIC_MAX_VISIBLE = 3;

inline void InitLyricSystem() {
    g_lyricItems.clear();
    g_currentLyricLine = 0;
    int linesToLoad = std::min(LYRIC_MAX_VISIBLE, (int)g_lyrics.size());
    for (int i = 0; i < linesToLoad; i++) {
        if (g_currentLyricLine + i >= (int)g_lyrics.size()) break;
        LyricItem item;
        item.text = g_lyrics[g_currentLyricLine + i];
        item.yPos = LYRIC_BASE_Y + i * LYRIC_LINE_HEIGHT;
        item.targetY = LYRIC_BASE_Y + i * LYRIC_LINE_HEIGHT;
        item.scale = 1.0f;
        item.targetScale = 1.0f;
        item.isCurrent = (i == 0);
        item.alpha = 255.0f;
        if (item.isCurrent) {
            item.color = ImColor(72, 120, 249, 255);
            item.targetScale = 1.15f;
        } else {
            item.color = ImColor(180, 180, 180, 220);
        }
        g_lyricItems.push_back(item);
    }
}

inline void UpdateLyricSystem() {
    if (g_lyricItems.empty() || g_lyrics.empty()) return;
    float dt = ImGui::GetIO().DeltaTime;
    g_lyricTimer += dt;
    if (g_lyricTimer >= 2.5f && g_currentLyricLine + LYRIC_MAX_VISIBLE < (int)g_lyrics.size()) {
        g_lyricTimer = 0.0f;
        g_currentLyricLine++;
        if (!g_lyricItems.empty()) g_lyricItems.erase(g_lyricItems.begin());
        for (auto& item : g_lyricItems) item.targetY -= LYRIC_LINE_HEIGHT;
        int nextIdx = g_currentLyricLine + LYRIC_MAX_VISIBLE - 1;
        if (nextIdx < (int)g_lyrics.size()) {
            LyricItem newItem;
            newItem.text = g_lyrics[nextIdx];
            newItem.yPos = LYRIC_BASE_Y + LYRIC_MAX_VISIBLE * LYRIC_LINE_HEIGHT;
            newItem.targetY = LYRIC_BASE_Y + (LYRIC_MAX_VISIBLE - 1) * LYRIC_LINE_HEIGHT;
            newItem.scale = 1.0f;
            newItem.targetScale = 1.0f;
            newItem.isCurrent = false;
            newItem.color = ImColor(180, 180, 180, 220);
            newItem.alpha = 255.0f;
            g_lyricItems.push_back(newItem);
        }
        for (size_t i = 0; i < g_lyricItems.size(); i++) {
            g_lyricItems[i].isCurrent = (i == 0);
            if (g_lyricItems[i].isCurrent) {
                g_lyricItems[i].targetScale = 1.15f;
                g_lyricItems[i].color = ImColor(72, 120, 249, 255);
            } else {
                g_lyricItems[i].targetScale = 1.0f;
                g_lyricItems[i].color = ImColor(180, 180, 180, 220);
            }
        }
    }
    for (auto& item : g_lyricItems) {
        item.yPos += (item.targetY - item.yPos) * (5.0f * dt);
        item.scale += (item.targetScale - item.scale) * (8.0f * dt);
    }
}

inline void DrawLyrics() {
    if (g_lyricItems.empty()) return;
    ImVec2 winPos = ImGui::GetWindowPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (auto& item : g_lyricItems) {
        if (item.yPos < LYRIC_BASE_Y - LYRIC_LINE_HEIGHT || item.yPos > LYRIC_BASE_Y + LYRIC_MAX_VISIBLE * LYRIC_LINE_HEIGHT)
            continue;
        float fs = LYRIC_FONT_SIZE * item.scale;
        dl->AddText(NULL, fs, ImVec2(winPos.x + 15.0f, winPos.y + item.yPos), item.color, item.text.c_str());
    }
}

// ==========================================
// 4. CUSTOM NIKE WIDGETS
// ==========================================

// DIAN traffic light dots
inline void DIAN(float xOff, float yOff, float gap, float size) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    pos.x += xOff;
    pos.y += yOff;
    dl->AddCircleFilled(ImVec2(pos.x + 1.0f, pos.y + 1.0f), size, IM_COL32(235, 75, 75, 255));
    dl->AddCircleFilled(ImVec2(pos.x + 1.0f + gap, pos.y + 1.0f), size, IM_COL32(235, 185, 50, 255));
    dl->AddCircleFilled(ImVec2(pos.x + 1.0f + gap * 2.0f, pos.y + 1.0f), size, IM_COL32(65, 205, 95, 255));
}

inline void DIAN1(float xOff, float yOff, float size) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    pos.x += xOff;
    pos.y += yOff;
    dl->AddCircleFilled(ImVec2(pos.x + 1.0f, pos.y + 1.0f), size, IM_COL32(65, 205, 95, 255));
}

// Section Title with glow line
inline void title(const char* text, float size = 4.0f, ImVec4 color = ImVec4(72.0f/255.0f, 120.0f/255.0f, 249.0f/255.0f, 1.0f), ImVec4 textcolor = ImVec4(240.0f/255.0f, 240.0f/255.0f, 240.0f/255.0f, 1.0f)) {
    ImGui::TextColored(textcolor, "%s", text);
    ImVec2 cpos = ImGui::GetCursorScreenPos();
    ImVec2 start = ImVec2(cpos.x, cpos.y + 4.0f);
    ImVec2 end = ImVec2(start.x + ImGui::GetContentRegionAvail().x, start.y);
    ImGui::GetWindowDrawList()->AddLine(start, end, ImGui::GetColorU32(color), size);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 10.0f);
}

// Signature M_SmallCheckbox (iOS / Android Smooth Animated Switch with Diamond Icon)
inline bool M_SmallCheckbox(const char* label, bool* v) {
    ImGuiContext& g = *GImGui;
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return false;

    ImGuiID id = window->GetID(label);
    ImVec2 pos = window->DC.CursorPos;
    ImVec2 label_size = ImGui::CalcTextSize(label, NULL, true);

    float switch_width = 58.0f;
    float switch_height = 30.0f;
    float total_width = ImMin(ImGui::GetContentRegionAvail().x, 260.0f);
    float total_height = 42.0f;

    ImRect total_bb(pos, ImVec2(pos.x + total_width, pos.y + total_height));
    ImGui::ItemSize(total_bb, g.Style.FramePadding.y);
    if (!ImGui::ItemAdd(total_bb, id)) return false;

    bool hovered, held;
    bool pressed = ImGui::ButtonBehavior(total_bb, id, &hovered, &held);
    if (pressed) {
        *v = !(*v);
        ImGui::MarkItemEdited(id);
    }

    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    // Background card for item
    ImU32 card_bg = IM_COL32(45, 45, 52, 100);
    draw_list->AddRectFilled(total_bb.Min, total_bb.Max, card_bg, 8.0f);

    // Diamond marker on the left
    float diamond_size = 6.0f;
    ImVec2 diamond_center = ImVec2(pos.x + 12.0f + diamond_size, pos.y + total_height * 0.5f);
    ImU32 d_outer = IM_COL32(200, 200, 200, 255);
    ImU32 d_inner = (*v) ? IM_COL32(72, 120, 249, 255) : IM_COL32(100, 100, 110, 255);

    ImVec2 out_pts[4] = {
        ImVec2(diamond_center.x, diamond_center.y - diamond_size),
        ImVec2(diamond_center.x + diamond_size, diamond_center.y),
        ImVec2(diamond_center.x, diamond_center.y + diamond_size),
        ImVec2(diamond_center.x - diamond_size, diamond_center.y)
    };
    draw_list->AddConvexPolyFilled(out_pts, 4, d_outer);

    ImVec2 in_pts[4] = {
        ImVec2(diamond_center.x, diamond_center.y - diamond_size * 0.5f),
        ImVec2(diamond_center.x + diamond_size * 0.5f, diamond_center.y),
        ImVec2(diamond_center.x, diamond_center.y + diamond_size * 0.5f),
        ImVec2(diamond_center.x - diamond_size * 0.5f, diamond_center.y)
    };
    draw_list->AddConvexPolyFilled(in_pts, 4, d_inner);

    // Label text
    if (label_size.x > 0.0f) {
        ImVec2 label_pos = ImVec2(diamond_center.x + diamond_size + 8.0f, pos.y + (total_height - label_size.y) * 0.5f);
        draw_list->AddText(label_pos, ImGui::GetColorU32(ImGuiCol_Text), label);
    }

    // Switch capsule
    float switch_x = pos.x + total_width - switch_width - 8.0f;
    float switch_y = pos.y + (total_height - switch_height) * 0.5f;

    static std::unordered_map<ImGuiID, float> anim_times;
    if (anim_times.find(id) == anim_times.end()) {
        anim_times[id] = *v ? 1.0f : 0.0f;
    }

    float target = *v ? 1.0f : 0.0f;
    anim_times[id] += (target - anim_times[id]) * (15.0f * g.IO.DeltaTime);
    float t = ImClamp(anim_times[id], 0.0f, 1.0f);

    ImVec4 bg_off = ImVec4(60/255.0f, 60/255.0f, 68/255.0f, 1.0f);
    ImVec4 bg_on  = ImVec4(72/255.0f, 120/255.0f, 249/255.0f, 1.0f);
    ImVec4 cur_bg = ImVec4(
        ImLerp(bg_off.x, bg_on.x, t),
        ImLerp(bg_off.y, bg_on.y, t),
        ImLerp(bg_off.z, bg_on.z, t),
        1.0f
    );

    draw_list->AddRectFilled(
        ImVec2(switch_x, switch_y),
        ImVec2(switch_x + switch_width, switch_y + switch_height),
        ImGui::GetColorU32(cur_bg),
        switch_height * 0.5f
    );

    float radius = switch_height * 0.40f;
    float min_x = switch_x + 3.0f + radius;
    float max_x = switch_x + switch_width - 3.0f - radius;
    float cx = min_x + t * (max_x - min_x);
    float cy = switch_y + switch_height * 0.5f;

    draw_list->AddCircleFilled(ImVec2(cx, cy), radius, IM_COL32(255, 255, 255, 255));
    if (t > 0.1f) {
        draw_list->AddCircleFilled(ImVec2(cx, cy), radius * 0.35f, ImGui::GetColorU32(cur_bg));
    }

    return pressed;
}

// Custom ImGui::Tabs for sidebar
namespace ImGui {
    inline bool Tabs(bool selected, const char* icon, const char* label, const ImVec2& size_arg, ImGuiButtonFlags flags = 0) {
        ImGuiWindow* window = GetCurrentWindow();
        if (window->SkipItems) return false;
        ImGuiContext& g = *GImGui;
        const ImGuiID id = window->GetID(label);
        const ImVec2 label_size = CalcTextSize(label, NULL, true);
        ImVec2 pos = window->DC.CursorPos;
        ImVec2 size = CalcItemSize(size_arg, label_size.x, label_size.y);
        const ImRect bb(pos, ImVec2(pos.x + size.x, pos.y + size.y));
        ItemSize(size, 0.f);
        if (!ItemAdd(bb, id)) return false;

        bool hovered, held, pressed = ButtonBehavior(bb, id, &hovered, &held, flags);

        struct tab_state { float rect = 0.f; };
        static std::map<ImGuiID, tab_state> anim;
        auto& st = anim[id];
        st.rect = ImLerp(st.rect, selected ? 1.0f : 0.0f, g.IO.DeltaTime * 10.f);

        // Highlight card
        GetWindowDrawList()->AddRectFilled(bb.Min, bb.Max, ImColor(45/255.0f, 45/255.0f, 55/255.0f, st.rect * 0.9f), 10.f);

        // Blue accent bar on the left
        ImVec2 dec_pos(bb.Min.x + 5.0f, bb.Min.y + (size.y - size.y * 0.4f) * 0.5f);
        const ImRect dec_bb(dec_pos, ImVec2(dec_pos.x + 4.0f, dec_pos.y + size.y * 0.4f));
        ImU32 dec_col = selected ? IM_COL32(72, 120, 249, 255) : IM_COL32(80, 80, 90, 80);
        GetWindowDrawList()->AddRectFilled(dec_bb.Min, dec_bb.Max, dec_col, 2.0f);

        // Text
        ImVec4 text_col = selected ? ImVec4(1.0f, 1.0f, 1.0f, 1.0f) : ImVec4(0.7f, 0.7f, 0.75f, 1.0f);
        float width = bb.Max.x - bb.Min.x;
        GetWindowDrawList()->AddText(NULL, 18.0f, ImVec2(bb.Min.x + width / 2.0f - label_size.x * 0.5f, bb.GetCenter().y - label_size.y * 0.5f), GetColorU32(text_col), label);

        return pressed;
    }
}

// Custom Segmented Switch Bar (选择条)
inline void 选择条(const std::vector<std::string>& names, float& 参数, float 居中 = 0, float 按钮x = 90, float 按钮y = 36, float 矩形圆角 = 6) {
    if (names.empty()) return;
    float itemW = 按钮x;
    float itemH = 按钮y;
    ImVec2 窗口(itemW * names.size(), itemH);

    struct BarAnim { float beg = 0.f, end = 0.f; int Tab = 0; };
    static std::unordered_map<std::string, BarAnim> bar_anims;
    BarAnim& name = bar_anims[names[0]];

    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.18f, 0.18f, 0.22f, 0.7f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImGui::BeginChild(names[0].c_str(), 窗口, true, ImGuiWindowFlags_NoScrollbar);

    ImVec2 min = ImGui::GetCursorScreenPos();
    ImVec2 max = ImVec2(min.x + itemW, min.y + itemH);

    name.beg += (name.end - name.beg) * 0.2f;

    ImGui::GetWindowDrawList()->AddRectFilled(
        ImVec2(min.x + name.beg + 2.f, min.y + 2.f),
        ImVec2(min.x + name.beg + itemW - 2.f, min.y + itemH - 2.f),
        IM_COL32(72, 120, 249, 230),
        矩形圆角
    );

    for (size_t i = 0; i < names.size(); i++) {
        ImGui::SetCursorPos(ImVec2(i * itemW, 0));
        char btnId[32];
        snprintf(btnId, sizeof(btnId), "##bar_%zu", i);
        if (ImGui::InvisibleButton(btnId, ImVec2(itemW, itemH))) {
            name.Tab = (int)i;
            name.end = itemW * i;
            参数 = (float)i;
        }
        if (name.Tab == (int)i) {
            name.end = itemW * i;
        }
        ImVec2 tsz = ImGui::CalcTextSize(names[i].c_str());
        ImVec2 tpos(min.x + i * itemW + (itemW - tsz.x) * 0.5f, min.y + (itemH - tsz.y) * 0.5f);
        ImGui::GetWindowDrawList()->AddText(tpos, IM_COL32(240, 240, 240, 255), names[i].c_str());
    }

    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

// Custom Theme Switch Bar (选择条2)
inline void 选择条2(const std::vector<std::string>& names, bool& 参数, float 居中 = 0, float 按钮x = 105, float 按钮y = 42, float 矩形圆角 = 8) {
    if (names.size() < 2) return;
    float p = 参数 ? 1.0f : 0.0f;
    选择条(names, p, 居中, 按钮x, 按钮y, 矩形圆角);
    参数 = (p >= 0.5f);
}

// whitecloud segmented control wrapper
inline void whitecloud(const std::vector<std::string>& names, float& 参数, float 居中 = 0, float 按钮x = 80, float 按钮y = 36, float 矩形圆角 = 6) {
    选择条(names, 参数, 居中, 按钮x, 按钮y, 矩形圆角);
}

inline void whitecloud(const std::vector<std::string>& names, int& 参数, float 居中 = 0, float 按钮x = 80, float 按钮y = 36, float 矩形圆角 = 6) {
    float p = (float)参数;
    选择条(names, p, 居中, 按钮x, 按钮y, 矩形圆角);
    参数 = (int)p;
}

// SliderFloat styled widget
inline void SliderFloat(const char* label, float* v, float v_min, float v_max, const char* format = "%.1f", ImGuiSliderFlags flags = 0) {
    ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.20f, 0.20f, 0.25f, 0.8f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.25f, 0.25f, 0.32f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.22f, 0.22f, 0.28f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(72.0f/255.0f, 120.0f/255.0f, 249.0f/255.0f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(90.0f/255.0f, 140.0f/255.0f, 255.0f/255.0f, 1.0f));
    ImGui::Text("%s", label);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 2.0f);
    ImGui::SliderFloat(label, v, v_min, v_max, format, flags);
    ImGui::PopStyleColor(5);
    ImGui::PopStyleVar(2);
}

// Color Palette (白云调色板)
inline void 白云调色板() {
    static const ImColor palette[] = {
        ImColor(72, 120, 249), ImColor(255, 69, 0), ImColor(50, 205, 50),
        ImColor(255, 215, 0), ImColor(160, 32, 240), ImColor(0, 255, 255),
        ImColor(255, 105, 180), ImColor(240, 240, 240)
    };
    ImGui::BeginGroup();
    for (int i = 0; i < 8; i++) {
        if (i > 0) ImGui::SameLine();
        char colId[16];
        snprintf(colId, sizeof(colId), "##col_%d", i);
        ImGui::PushStyleColor(ImGuiCol_Button, palette[i].Value);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, palette[i].Value);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, palette[i].Value);
        if (ImGui::Button(colId, ImVec2(24, 24))) {
            // Selected color
        }
        ImGui::PopStyleColor(3);
    }
    ImGui::EndGroup();
}
