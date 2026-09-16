// app/media.cpp — Иконки вкладок: загрузка GL-текстур.
//
// Модуль разрезан из прежнего монолита jni/src/main.cpp;
// что здесь лежит и кто это зовёт — в шапке app/media.h и в docs/CODE_MAP.md.

// Иконки вкладок лежат в media/icons.h (вшиты в бинарник), а распаковка
// PNG — на stb_image. Реализация stb_image должна быть ровно одна на
// программу, поэтому #define ниже живёт только в этом файле.
#if __has_include("media/icons.h")
#  include "media/icons.h"
#  define ICONS_AVAILABLE
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include "stb_image/stb_image.h"

#include "app/common.h"
#include "ui/scroll.h"
#include "ui/tabs.h"
#include "ui/window.h"
#include "app/media.h"

GLuint g_tabIcons[kTabCount] = {};

static GLuint LoadTexFromMemory(const unsigned char* data, int len) {
    int w, h, ch;
    unsigned char* px = stbi_load_from_memory(data, len, &w, &h, &ch, 4);
    if (!px) return 0;
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    stbi_image_free(px);
    return tex;
}

void LoadTabIcons() {
#ifdef ICONS_AVAILABLE
    g_tabIcons[0] = LoadTexFromMemory(main_tab_png, (int)main_tab_png_len);
    g_tabIcons[1] = LoadTexFromMemory(aimbot_png,   (int)aimbot_png_len);
    g_tabIcons[2] = LoadTexFromMemory(visuals_png,  (int)visuals_png_len);
    // Tab order: 3=Разное, 4=Конфиги, 5=Опции. У «Разное» своя векторная
    // иконка (рисуется кодом), поэтому текстура ему не нужна — иначе она
    // дублировала бы иконку «Конфиги» (misc_png).
    g_tabIcons[3] = 0;
    g_tabIcons[4] = LoadTexFromMemory(misc_png,     (int)misc_png_len);
    g_tabIcons[5] = LoadTexFromMemory(settings_png, (int)settings_png_len);
#endif
}

// Танцующий спрайт-аватар убран из интерфейса; текстура больше не грузится.
void LoadAnimeImage() {}
