#ifndef NATIVESURFACE_DRAW_H
#define NATIVESURFACE_DRAW_H

#include <iostream>
#include <thread>
#include <chrono>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <android/native_window.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES3/gl32.h>

#include "native_surface/ANativeWindowCreator.h"
#include "ImGui/imgui.h"
#include "ImGui/imgui_internal.h"
#include "ImGui/backends/imgui_impl_android.h"
#include "ImGui/backends/imgui_impl_opengl3.h"
#include "Android_touch/TouchHelperA.h"

using namespace std;
using namespace std::chrono_literals;

extern anwc::ANativeWindowCreator::DisplayInfo displayInfo;
extern bool g_Initialized;
extern ImGuiWindow *g_window;
extern int native_window_screen_x, native_window_screen_y;

bool init_egl(uint32_t _screen_x, uint32_t _screen_y, bool log = false);
bool initGUI_draw(uint32_t _screen_x, uint32_t _screen_y, bool log = false);
bool ImGui_init();
void screen_config();
void drawBegin();
void drawEnd();
void shutdown();

// Живучесть окна оверлея (см. большой комментарий в Android_draw/draw.cpp).
// Оверлей — своя поверхность и свой EGL-контекст: если система его уничтожила
// (сменилось разрешение, панель ушла в сон, слой прибит композитором), кадры
// начинают падать, и раньше это было НАВСЕГДА — «чит сам выключился». Теперь
// окно собирается заново на ходу; меню и настройки при этом не теряются.
void draw_request_surface_rebuild(const char* why);

// Сколько раз окно пересоздавалось (для журнала здоровья).
int draw_rebuild_count();
float overlay_fps();
// Темп оверлея и диагностика (см. kOverlayPaceHz в draw.cpp): сколько последний
// кадр проспал до своего темпа, пик панели и сам темп — для шапки лога.
double overlay_pace_sleep_ms();
float overlay_peak_hz();
float overlay_pace_hz();

#endif