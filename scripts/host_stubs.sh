#!/bin/bash
# Хост-заглушки Android/EGL/GLES/SLES для g++ -fsyntax-only правленных TU
# (без устройства и NDK). Каталог .stubs/ gitignored и исчезает между
# сессиями — запускать перед проверками:  bash scripts/host_stubs.sh
set -e
cd "$(dirname "$0")/.."
mkdir -p .stubs/EGL .stubs/GLES3 .stubs/KHR .stubs/android .stubs/sys .stubs/SLES
cat > .stubs/KHR/khrplatform.h <<'EOF'
#pragma once
#include <stdint.h>
#include <stddef.h>
typedef int8_t khronos_int8_t;
typedef uint8_t khronos_uint8_t;
typedef int16_t khronos_int16_t;
typedef uint16_t khronos_uint16_t;
typedef int32_t khronos_int32_t;
typedef uint32_t khronos_uint32_t;
typedef int64_t khronos_int64_t;
typedef uint64_t khronos_uint64_t;
typedef float khronos_float_t;
typedef intptr_t khronos_intptr_t;
typedef size_t khronos_ssize_t;
typedef khronos_uint32_t khronos_uintptr_t;
EOF
cat > .stubs/EGL/egl.h <<'EOF'
#pragma once
#include <KHR/khrplatform.h>
typedef khronos_int32_t EGLint;
typedef khronos_uint32_t EGLBoolean;
typedef khronos_uint32_t EGLenum;
typedef khronos_int32_t EGLAttrib;
typedef void* EGLDisplay; typedef void* EGLSurface; typedef void* EGLConfig;
typedef void* EGLContext; typedef void* EGLImageKHR;
typedef intptr_t EGLNativeDisplayType; typedef void* EGLNativeWindowType;
#define EGL_DEFAULT_DISPLAY ((EGLNativeDisplayType)0)
#define EGL_NO_DISPLAY ((EGLDisplay)0)
#define EGL_NO_SURFACE ((EGLSurface)0)
#define EGL_NO_CONTEXT ((EGLContext)0)
#define EGL_SURFACE_TYPE 0x3033
#define EGL_WINDOW_BIT 0x0004
#define EGL_PBUFFER_BIT 0x0001
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES3_BIT 0x0008
#define EGL_OPENGL_ES2_BIT 0x0004
#define EGL_RED_SIZE 0x3024
#define EGL_GREEN_SIZE 0x3023
#define EGL_BLUE_SIZE 0x3022
#define EGL_ALPHA_SIZE 0x3021
#define EGL_DEPTH_SIZE 0x3025
#define EGL_STENCIL_SIZE 0x3026
#define EGL_SAMPLES 0x3031
#define EGL_NONE 0x3038
#define EGL_FALSE 0
#define EGL_TRUE 1
#define EGL_BUFFER_SIZE 0x3020
#define EGL_NATIVE_VISUAL_ID 0x302E
#define EGL_WIDTH 0x3057
#define EGL_HEIGHT 0x3056
#define EGL_GL_COLORSPACE_KHR 0x3092
#define EGL_GL_COLORSPACE_SRGB_KHR 0x3089
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_VENDOR 0x3053
#define EGL_VERSION 0x3054
#define EGL_EXTENSIONS 0x3055
extern "C" {
EGLDisplay eglGetDisplay(EGLNativeDisplayType);
EGLBoolean eglInitialize(EGLDisplay, EGLint*, EGLint*);
EGLBoolean eglChooseConfig(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*);
EGLSurface eglCreateWindowSurface(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint*);
EGLContext eglCreateContext(EGLDisplay, EGLConfig, EGLContext, const EGLint*);
EGLBoolean eglMakeCurrent(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
EGLBoolean eglSwapBuffers(EGLDisplay, EGLSurface);
EGLBoolean eglSwapInterval(EGLDisplay, EGLint);
EGLBoolean eglDestroyContext(EGLDisplay, EGLContext);
EGLBoolean eglDestroySurface(EGLDisplay, EGLSurface);
EGLBoolean eglTerminate(EGLDisplay);
EGLBoolean eglGetConfigAttrib(EGLDisplay, EGLConfig, EGLint, EGLint*);
void* eglGetProcAddress(const char*);
}
EOF
cat > .stubs/EGL/eglext.h <<'EOF'
#pragma once
#include <EGL/egl.h>
#define EGL_NATIVE_BUFFER_ANDROID 0x3140
EOF
cat > .stubs/GLES3/gl3.h <<'EOF'
#pragma once
#include <KHR/khrplatform.h>
#define GL_APIENTRY
typedef char GLchar;
typedef khronos_uint32_t GLbitfield;
typedef khronos_float_t GLfloat;
typedef khronos_int32_t GLint;
typedef khronos_uint32_t GLuint;
typedef khronos_int32_t GLsizei;
typedef khronos_uint32_t GLenum;
typedef khronos_uint8_t GLboolean;
typedef khronos_int8_t GLbyte;
typedef khronos_uint8_t GLubyte;
typedef khronos_int16_t GLshort;
typedef khronos_uint16_t GLushort;
typedef khronos_int64_t GLint64;
typedef khronos_uint64_t GLuint64;
typedef khronos_ssize_t GLsizeiptr;
typedef khronos_intptr_t GLintptr;
typedef khronos_int32_t GLfixed;
typedef void GLvoid;
typedef struct __GLsync *GLsync;
typedef void (GL_APIENTRY *GLDEBUGPROC)(GLenum, GLenum, GLuint, GLenum, GLsizei, const GLchar*, const void*);
typedef void (GL_APIENTRY *PFNGLGENVERTEXARRAYSPROC)(GLsizei, GLuint*);
typedef void (GL_APIENTRY *PFNGLBINDVERTEXARRAYPROC)(GLuint);
#define GL_DEPTH_BUFFER_BIT 0x00000100
#define GL_STENCIL_BUFFER_BIT 0x00000400
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_FALSE 0
#define GL_TRUE 1
#define GL_POINTS 0x0000
#define GL_LINES 0x0001
#define GL_LINE_LOOP 0x0002
#define GL_LINE_STRIP 0x0003
#define GL_TRIANGLES 0x0004
#define GL_TRIANGLE_STRIP 0x0005
#define GL_TRIANGLE_FAN 0x0006
#define GL_ZERO 0
#define GL_ONE 1
#define GL_SRC_COLOR 0x0300
#define GL_ONE_MINUS_SRC_COLOR 0x0301
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_DST_ALPHA 0x0304
#define GL_ONE_MINUS_DST_ALPHA 0x0305
#define GL_DST_COLOR 0x0306
#define GL_ONE_MINUS_DST_COLOR 0x0307
#define GL_BLEND 0x0BE2
#define GL_CULL_FACE 0x0B44
#define GL_DEPTH_TEST 0x0B71
#define GL_SCISSOR_TEST 0x0C11
#define GL_STENCIL_TEST 0x0B90
#define GL_VIEWPORT 0x0BA2
#define GL_TEXTURE_2D 0x0DE1
#define GL_BYTE 0x1400
#define GL_UNSIGNED_BYTE 0x1401
#define GL_SHORT 0x1402
#define GL_UNSIGNED_SHORT 0x1403
#define GL_INT 0x1404
#define GL_UNSIGNED_INT 0x1405
#define GL_FLOAT 0x1406
#define GL_HALF_FLOAT 0x140B
#define GL_UNSIGNED_INT_24_8 0x84FA
#define GL_RGBA 0x1908
#define GL_BGRA 0x80E1
#define GL_RGB 0x1907
#define GL_LUMINANCE 0x1909
#define GL_RGB8 0x8051
#define GL_RGBA4 0x8056
#define GL_RGBA8 0x8058
#define GL_RGB5_A1 0x8057
#define GL_DEPTH_COMPONENT 0x1902
#define GL_DEPTH_COMPONENT16 0x81A5
#define GL_DEPTH_COMPONENT24 0x81A6
#define GL_DEPTH_COMPONENT32 0x81A7
#define GL_STENCIL_INDEX8 0x8D48
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_NEAREST_MIPMAP_NEAREST 0x2700
#define GL_LINEAR_MIPMAP_NEAREST 0x2701
#define GL_NEAREST_MIPMAP_LINEAR 0x2702
#define GL_LINEAR_MIPMAP_LINEAR 0x2703
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_TEXTURE0 0x84C0
#define GL_ACTIVE_TEXTURE 0x84E0
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_FRONT 0x0404
#define GL_BACK 0x0405
#define GL_FRONT_AND_BACK 0x0408
#define GL_CULL_FACE_MODE 0x0B45
#define GL_NEVER 0x0200
#define GL_LESS 0x0201
#define GL_EQUAL 0x0202
#define GL_LEQUAL 0x0203
#define GL_GREATER 0x0204
#define GL_NOTEQUAL 0x0205
#define GL_GEQUAL 0x0206
#define GL_ALWAYS 0x0207
#define GL_DEPTH_FUNC 0x0B74
#define GL_KEEP 0x1E00
#define GL_REPLACE 0x1E01
#define GL_INCR 0x1E02
#define GL_DECR 0x1E03
#define GL_STENCIL_REF 0x0B97
#define GL_STENCIL_VALUE_MASK 0x0B93
#define GL_INVERT 0x150A
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_STATIC_DRAW 0x88E4
#define GL_DYNAMIC_DRAW 0x88E8
#define GL_STREAM_DRAW 0x88E0
#define GL_PIXEL_UNPACK_BUFFER 0x88EC
#define GL_READ_ONLY 0x88B8
#define GL_WRITE_ONLY 0x88B9
#define GL_READ_WRITE 0x88BA
#define GL_BUFFER_SIZE 0x8764
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_CURRENT_PROGRAM 0x8B8D
#define GL_TEXTURE_BINDING_2D 0x8069
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#define GL_UNPACK_SKIP_PIXELS 0x0CF4
#define GL_UNPACK_SKIP_ROWS 0x0CF3
#define GL_FILL 0x1B02
#define GL_MULTISAMPLE 0x809D
#define GL_POLYGON_MODE 0x0B37
#define GL_SCISSOR_BOX 0x0C10
#define GL_BLEND_SRC_RGB 0x80C9
#define GL_BLEND_DST_RGB 0x80CA
#define GL_BLEND_SRC_ALPHA 0x80CB
#define GL_BLEND_DST_ALPHA 0x80CC
#define GL_BLEND_EQUATION_RGB 0x8009
#define GL_BLEND_EQUATION_ALPHA 0x883D
#define GL_FUNC_ADD 0x8006
#define GL_FUNC_SUBTRACT 0x800A
#define GL_FUNC_REVERSE_SUBTRACT 0x800B
#define GL_MIN 0x8007
#define GL_MAX 0x8008
#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
extern "C" {
void glActiveTexture(GLenum);
void glAttachShader(GLuint, GLuint);
void glBindBuffer(GLenum, GLuint);
void glBindTexture(GLenum, GLuint);
void glBindVertexArray(GLuint);
void glBlendEquation(GLenum);
void glBlendEquationSeparate(GLenum, GLenum);
void glBlendFunc(GLenum, GLenum);
void glBlendFuncSeparate(GLenum, GLenum, GLenum, GLenum);
void glBlitFramebuffer(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
void glFramebufferTexture2D(GLenum, GLenum, GLenum, GLuint, GLint);
void glGetBooleanv(GLenum, GLboolean*);
void glBufferData(GLenum, GLsizeiptr, const void*, GLenum);
void glBufferSubData(GLenum, GLintptr, GLsizeiptr, const void*);
void glClear(GLbitfield);
void glClearColor(GLfloat, GLfloat, GLfloat, GLfloat);
void glClearStencil(GLint);
void glClearDepthf(GLfloat);
void glCompileShader(GLuint);
GLuint glCreateProgram(void);
GLuint glCreateShader(GLenum);
void glCullFace(GLenum);
void glDeleteBuffers(GLsizei, const GLuint*);
void glDeleteProgram(GLuint);
void glDeleteShader(GLuint);
void glDeleteTextures(GLsizei, const GLuint*);
void glDeleteVertexArrays(GLsizei, const GLuint*);
void glDepthFunc(GLenum);
void glDepthMask(GLboolean);
void glDisable(GLenum);
void glDisableVertexAttribArray(GLuint);
void glDrawElements(GLenum, GLsizei, GLenum, const void*);
void glDrawArrays(GLenum, GLint, GLsizei);
void glEnable(GLenum);
void glEnableVertexAttribArray(GLuint);
void glGenBuffers(GLsizei, GLuint*);
void glGenTextures(GLsizei, GLuint*);
void glGenVertexArrays(GLsizei, GLuint*);
GLint glGetAttribLocation(GLuint, const GLchar*);
void glGetIntegerv(GLenum, GLint*);
GLint glGetUniformLocation(GLuint, const GLchar*);
GLboolean glIsEnabled(GLenum);
void glLinkProgram(GLuint);
void glPixelStorei(GLenum, GLint);
void glPolygonMode(GLenum, GLenum);
void glReadPixels(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);
void glScissor(GLint, GLint, GLsizei, GLsizei);
void glShaderSource(GLuint, GLsizei, const GLchar* const*, const GLint*);
void glStencilFunc(GLenum, GLint, GLuint);
void glStencilMask(GLuint);
void glStencilOp(GLenum, GLenum, GLenum);
void glTexImage2D(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
void glTexSubImage2D(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*);
void glTexParameteri(GLenum, GLenum, GLint);
void glUniform1i(GLint, GLint);
void glUniform1f(GLint, GLfloat);
void glUniformMatrix4fv(GLint, GLsizei, GLboolean, const GLfloat*);
void glUseProgram(GLuint);
void glVertexAttribPointer(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
void glViewport(GLint, GLint, GLsizei, GLsizei);
void glGetShaderiv(GLuint, GLenum, GLint*);
void glGetProgramiv(GLuint, GLenum, GLint*);
void glGetShaderInfoLog(GLuint, GLsizei, GLsizei*, GLchar*);
void glGetProgramInfoLog(GLuint, GLsizei, GLsizei*, GLchar*);
void glBindFramebuffer(GLenum, GLuint);
void glFinish(void);
void glFlush(void);
void glGetError(void);
void glLineWidth(GLfloat);
void glGenFramebuffers(GLsizei, GLuint*);
void glDeleteFramebuffers(GLsizei, const GLuint*);
const GLubyte* glGetString(GLenum);
}
EOF
cat > .stubs/GLES3/gl32.h <<'EOF'
#pragma once
#include <GLES3/gl3.h>
EOF
cat > .stubs/android/keycodes.h <<'EOF'
#pragma once
#define AKEYCODE_UNKNOWN 0
#define AKEYCODE_SOFT_LEFT 1
#define AKEYCODE_SOFT_RIGHT 2
#define AKEYCODE_HOME 3
#define AKEYCODE_BACK 4
#define AKEYCODE_CALL 5
#define AKEYCODE_ENDCALL 6
#define AKEYCODE_0 7
#define AKEYCODE_1 8
#define AKEYCODE_2 9
#define AKEYCODE_3 10
#define AKEYCODE_4 11
#define AKEYCODE_5 12
#define AKEYCODE_6 13
#define AKEYCODE_7 14
#define AKEYCODE_8 15
#define AKEYCODE_9 16
#define AKEYCODE_STAR 17
#define AKEYCODE_POUND 18
#define AKEYCODE_DPAD_CENTER 19
#define AKEYCODE_DPAD_UP 20
#define AKEYCODE_DPAD_DOWN 21
#define AKEYCODE_DPAD_LEFT 22
#define AKEYCODE_DPAD_RIGHT 23
#define AKEYCODE_CENTER 24
#define AKEYCODE_VOLUME_UP 25
#define AKEYCODE_VOLUME_DOWN 26
#define AKEYCODE_POWER 27
#define AKEYCODE_CAMERA 28
#define AKEYCODE_CLEAR 29
#define AKEYCODE_A 30
#define AKEYCODE_B 31
#define AKEYCODE_C 32
#define AKEYCODE_D 33
#define AKEYCODE_E 34
#define AKEYCODE_F 35
#define AKEYCODE_G 36
#define AKEYCODE_H 37
#define AKEYCODE_I 38
#define AKEYCODE_J 39
#define AKEYCODE_K 40
#define AKEYCODE_L 41
#define AKEYCODE_M 42
#define AKEYCODE_N 43
#define AKEYCODE_O 44
#define AKEYCODE_P 45
#define AKEYCODE_Q 46
#define AKEYCODE_R 47
#define AKEYCODE_S 48
#define AKEYCODE_T 49
#define AKEYCODE_U 50
#define AKEYCODE_V 51
#define AKEYCODE_W 52
#define AKEYCODE_X 53
#define AKEYCODE_Y 54
#define AKEYCODE_Z 55
#define AKEYCODE_COMMA 56
#define AKEYCODE_PERIOD 57
#define AKEYCODE_ALT_LEFT 58
#define AKEYCODE_SHIFT_LEFT 59
#define AKEYCODE_SPACE 60
#define AKEYCODE_TAB 61
#define AKEYCODE_SEMICOLON 62
#define AKEYCODE_APOSTROPHE 63
#define AKEYCODE_SLASH 64
#define AKEYCODE_AT 65
#define AKEYCODE_ENTER 66
#define AKEYCODE_DEL 67
#define AKEYCODE_GRAVE 68
#define AKEYCODE_MINUS 69
#define AKEYCODE_EQUALS 70
#define AKEYCODE_LEFT_BRACKET 71
#define AKEYCODE_RIGHT_BRACKET 72
#define AKEYCODE_BACKSLASH 73
#define AKEYCODE_ALT_RIGHT 74
#define AKEYCODE_SHIFT_RIGHT 75
#define AKEYCODE_NUM 76
#define AKEYCODE_PLUS 81
#define AKEYCODE_MENU 82
#define AKEYCODE_NOTIFICATION 83
#define AKEYCODE_SEARCH 84
#define AKEYCODE_MEDIA_PLAY_PAUSE 85
#define AKEYCODE_MEDIA_PLAY 86
#define AKEYCODE_MEDIA_STOP 87
#define AKEYCODE_MEDIA_NEXT 88
#define AKEYCODE_MEDIA_PREVIOUS 89
#define AKEYCODE_MEDIA_REWIND 90
#define AKEYCODE_MEDIA_FAST_FORWARD 91
#define AKEYCODE_MUTE 92
#define AKEYCODE_PAGE_UP 93
#define AKEYCODE_PAGE_DOWN 94
#define AKEYCODE_PICTSYMBOLS 95
#define AKEYCODE_SWITCH_CHARSET 96
#define AKEYCODE_FORWARD_DEL 112
#define AKEYCODE_CTRL_LEFT 129
#define AKEYCODE_CTRL_RIGHT 130
#define AKEYCODE_CAPS_LOCK 115
#define AKEYCODE_SCROLL_LOCK 116
#define AKEYCODE_ESCAPE 111
#define AKEYCODE_SYSRQ 100
#define AKEYCODE_BREAK 103
#define AKEYCODE_MOVE_HOME 122
#define AKEYCODE_MOVE_END 123
#define AKEYCODE_INSERT 124
#define AKEYCODE_F1 131
#define AKEYCODE_F2 132
#define AKEYCODE_F3 133
#define AKEYCODE_F4 134
#define AKEYCODE_F5 135
#define AKEYCODE_F6 136
#define AKEYCODE_F7 137
#define AKEYCODE_F8 138
#define AKEYCODE_F9 139
#define AKEYCODE_F10 140
#define AKEYCODE_F11 141
#define AKEYCODE_F12 142
#define AKEYCODE_NUM_LOCK 143
#define AKEYCODE_META_LEFT 147
#define AKEYCODE_META_RIGHT 148
#define AKEYCODE_NUMPAD_0 158
#define AKEYCODE_NUMPAD_1 159
#define AKEYCODE_NUMPAD_2 160
#define AKEYCODE_NUMPAD_3 161
#define AKEYCODE_NUMPAD_4 162
#define AKEYCODE_NUMPAD_5 163
#define AKEYCODE_NUMPAD_6 164
#define AKEYCODE_NUMPAD_7 165
#define AKEYCODE_NUMPAD_8 166
#define AKEYCODE_NUMPAD_9 167
#define AKEYCODE_NUMPAD_DIVIDE 168
#define AKEYCODE_NUMPAD_MULTIPLY 169
#define AKEYCODE_NUMPAD_SUBTRACT 170
#define AKEYCODE_NUMPAD_ADD 171
#define AKEYCODE_NUMPAD_DOT 172
#define AKEYCODE_NUMPAD_COMMA 173
#define AKEYCODE_NUMPAD_ENTER 174
#define AKEYCODE_NUMPAD_EQUALS 175
EOF
cat > .stubs/android/input.h <<'EOF'
#pragma once
#include <cstdint>
#include <android/keycodes.h>

struct AInputEvent;
typedef struct AInputEvent AInputEvent;
struct AInputQueue;
typedef struct AInputQueue AInputQueue;
struct AKeyEvent;
typedef struct AKeyEvent AKeyEvent;
struct AMotionEvent;
typedef struct AMotionEvent AMotionEvent;

enum {
    AINPUT_EVENT_TYPE_KEY = 1,
    AINPUT_EVENT_TYPE_MOTION = 2,
};

#define AKEY_EVENT_ACTION_DOWN 0
#define AKEY_EVENT_ACTION_UP 1

enum {
    AMOTION_EVENT_ACTION_DOWN = 0,
    AMOTION_EVENT_ACTION_UP = 1,
    AMOTION_EVENT_ACTION_MOVE = 2,
    AMOTION_EVENT_ACTION_CANCEL = 3,
    AMOTION_EVENT_ACTION_OUTSIDE = 4,
    AMOTION_EVENT_ACTION_POINTER_DOWN = 5,
    AMOTION_EVENT_ACTION_POINTER_UP = 6,
    AMOTION_EVENT_ACTION_HOVER_MOVE = 7,
    AMOTION_EVENT_ACTION_SCROLL = 8,
    AMOTION_EVENT_ACTION_HOVER_ENTER = 9,
    AMOTION_EVENT_ACTION_HOVER_EXIT = 10,
    AMOTION_EVENT_ACTION_BUTTON_PRESS = 5,
    AMOTION_EVENT_ACTION_BUTTON_RELEASE = 6,
    AMOTION_EVENT_ACTION_MASK = 0xff,
    AMOTION_EVENT_ACTION_POINTER_INDEX_MASK = 0xff00,
    AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT = 8,
};

#define AMOTION_EVENT_BUTTON_PRIMARY (1 << 0)
#define AMOTION_EVENT_BUTTON_SECONDARY (1 << 1)
#define AMOTION_EVENT_BUTTON_TERTIARY (1 << 2)
#define AMOTION_EVENT_BUTTON_NAV (1 << 3)
#define AMOTION_EVENT_BUTTON_CIRCLE (1 << 4)
#define AMOTION_EVENT_BUTTON_SQUARE (1 << 5)

enum {
    AMOTION_EVENT_TOOL_TYPE_UNKNOWN = 0,
    AMOTION_EVENT_TOOL_TYPE_FINGER = 1,
    AMOTION_EVENT_TOOL_TYPE_STYLUS = 2,
    AMOTION_EVENT_TOOL_TYPE_MOUSE = 3,
    AMOTION_EVENT_TOOL_TYPE_ERASER = 4,
};

#define AMOTION_EVENT_AXIS_X 0
#define AMOTION_EVENT_AXIS_Y 1
#define AMOTION_EVENT_AXIS_PRESSURE 2
#define AMOTION_EVENT_AXIS_SIZE 3
#define AMOTION_EVENT_AXIS_ORIENTATION 4
#define AMOTION_EVENT_AXIS_TOOL_MAJOR 5
#define AMOTION_EVENT_AXIS_TOOL_MINOR 6
#define AMOTION_EVENT_AXIS_FINGER_MAJOR 7
#define AMOTION_EVENT_AXIS_FINGER_MINOR 8
#define AMOTION_EVENT_AXIS_HSCROLL 9
#define AMOTION_EVENT_AXIS_VSCROLL 10

#define AMETA_SHIFT_ON 0x0001
#define AMETA_SYM_ON 0x0004
#define AMETA_ALT_ON 0x0200
#define AMETA_META_ON 0x2000
#define AMETA_CTRL_ON 0x1000

static inline int AInputEvent_getType(const AInputEvent*) { return 0; }
static inline int AKeyEvent_getAction(const AInputEvent*) { return 0; }
static inline int AKeyEvent_getKeyCode(const AInputEvent*) { return 0; }
static inline int AKeyEvent_getMetaState(const AInputEvent*) { return 0; }
static inline int AKeyEvent_getScanCode(const AInputEvent*) { return 0; }
static inline int AMotionEvent_getAction(const AInputEvent*) { return 0; }
static inline int AMotionEvent_getToolType(const AInputEvent*, int32_t) { return 0; }
static inline float AMotionEvent_getX(const AInputEvent*, int32_t) { return 0.f; }
static inline float AMotionEvent_getY(const AInputEvent*, int32_t) { return 0.f; }
static inline int AMotionEvent_getButtonState(const AInputEvent*) { return 0; }
static inline float AMotionEvent_getAxisValue(const AInputEvent*, int32_t, int32_t) { return 0.f; }
EOF
cat > .stubs/android/rect.h <<'EOF'
#pragma once
#include <stdint.h>
typedef struct ARect { int32_t left, top, right, bottom; } ARect;
EOF
cat > .stubs/android/log.h <<'EOF'
#pragma once
#include <stdarg.h>
enum { ANDROID_LOG_UNKNOWN = 0, ANDROID_LOG_DEFAULT, ANDROID_LOG_VERBOSE,
       ANDROID_LOG_DEBUG, ANDROID_LOG_INFO, ANDROID_LOG_WARN,
       ANDROID_LOG_ERROR, ANDROID_LOG_FATAL, ANDROID_LOG_SILENT };
extern "C" int __android_log_print(int prio, const char* tag, const char* fmt, ...)
    __attribute__((format(printf, 3, 4)));
extern "C" int __android_log_write(int prio, const char* tag, const char* text);
extern "C" void __android_log_vprint(int prio, const char* tag, const char* fmt, va_list ap);
EOF
cat > .stubs/android/asset_manager.h <<'EOF'
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#ifdef __cplusplus
struct AAssetManager; struct AAssetDir;
struct AAsset { int dummy_; };
extern "C" {
AAssetDir* AAssetManager_openDir(AAssetManager*, const char*);
AAsset* AAssetManager_open(AAssetManager*, const char*, int);
const char* AAssetDir_getNextFileName(AAssetDir*);
void AAssetDir_close(AAssetDir*);
int AAsset_read(AAsset*, void*, size_t);
off64_t AAsset_getLength64(AAsset*);
off_t AAsset_getLength(AAsset*);
off64_t AAsset_getRemainingLength64(AAsset*);
off_t AAsset_getRemainingLength(AAsset*);
void AAsset_close(AAsset*);
}
#endif
EOF
cat > .stubs/android/native_window.h <<'EOF'
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <android/rect.h>
#ifdef __cplusplus
struct ANativeWindow;
enum ANativeWindowFrameRateChangeStrategy { ANATIVEWINDOW_FRAME_RATE_CHANGE_STRATEGY_DEFAULT = 0 };
extern "C" {
void ANativeWindow_acquire(ANativeWindow*);
void ANativeWindow_release(ANativeWindow*);
int32_t ANativeWindow_getWidth(ANativeWindow*);
int32_t ANativeWindow_getHeight(ANativeWindow*);
int ANativeWindow_setBuffersGeometry(ANativeWindow*, int32_t, int32_t, int32_t);
int ANativeWindow_setBufferCount(ANativeWindow*, size_t);
int ANativeWindow_setFrameRate(ANativeWindow*, float, int8_t);
int ANativeWindow_setFrameRateWithChangeStrategy(ANativeWindow*, float, int8_t, int);
}
#endif
EOF
cat > .stubs/android/native_activity.h <<'EOF'
#pragma once
#include <android/native_window.h>
#include <android/asset_manager.h>
struct ANativeActivity { ANativeWindow* nativeWindow; AAssetManager* assetManager; };
EOF
cat > .stubs/sys/system_properties.h <<'EOF'
#pragma once
#include <stdint.h>
#define PROP_VALUE_MAX 92
extern "C" int __system_property_get(const char* name, char* value);
EOF
cat > .stubs/SLES/OpenSLES.h <<'EOF'
#pragma once
#include <stdint.h>
#include <stddef.h>
typedef uint8_t SLboolean; typedef uint32_t SLuint32; typedef uint32_t SLresult;
typedef int32_t SLint32; typedef uint16_t SLuint16; typedef uint32_t SLmilliHertz;
typedef uint16_t SLmillibel; typedef uint32_t SLpermille;
typedef struct SLInterfaceID_ { uint32_t time_low; uint16_t time_mid; uint16_t time_hi_and_version; uint16_t clock_seq; uint8_t node[6]; } *SLInterfaceID;
typedef const struct SLObjectItf_ * const * SLObjectItf;
typedef const struct SLEngineItf_ * const * SLEngineItf;
typedef const struct SLPlayItf_ * const * SLPlayItf;
typedef const struct SLAndroidSimpleBufferQueueItf_ * const * SLAndroidSimpleBufferQueueItf;
typedef const struct SLVolumeItf_ * const * SLVolumeItf;
#define SL_BOOLEAN_FALSE ((SLboolean)0)
#define SL_BOOLEAN_TRUE ((SLboolean)1)
#define SL_RESULT_SUCCESS 0x00000000
#define SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE 0x800007CC
#define SL_DATALOCATOR_OUTPUTMIX 0x800007D4
#define SL_DATAFORMAT_PCM 0x800007D6
#define SL_SPEAKER_FRONT_LEFT ((SLuint32)0x00000001)
#define SL_SPEAKER_FRONT_RIGHT ((SLuint32)0x00000002)
#define SL_SPEAKER_FRONT_CENTER ((SLuint32)0x00000004)
#define SL_BYTEORDER_LITTLEENDIAN 0x00000000
#define SL_ENGINEOPTION_THREADSAFE ((SLuint32)0x00000001)
#define SL_IID_ENGINE ((SLInterfaceID)&SL_IID_ENGINE_id)
#define SL_IID_PLAY ((SLInterfaceID)&SL_IID_PLAY_id)
#define SL_IID_BUFFERQUEUE ((SLInterfaceID)&SL_IID_BUFFERQUEUE_id)
#define SL_IID_VOLUME ((SLInterfaceID)&SL_IID_VOLUME_id)
#define SL_PLAYSTATE_STOPPED 0x00000001
#define SL_PLAYSTATE_PAUSED 0x00000002
#define SL_PLAYSTATE_PLAYING 0x00000003
extern "C" {
extern const struct SLInterfaceID_ SL_IID_ENGINE_id;
extern const struct SLInterfaceID_ SL_IID_PLAY_id;
extern const struct SLInterfaceID_ SL_IID_BUFFERQUEUE_id;
extern const struct SLInterfaceID_ SL_IID_VOLUME_id;
}
typedef struct SLEngineOption { SLuint32 feature; SLuint32 data; } SLEngineOption;
typedef struct SLDataLocator_AndroidSimpleBufferQueue { SLuint32 locatorType; SLuint32 numBuffers; } SLDataLocator_AndroidSimpleBufferQueue;
typedef struct SLDataLocator_OutputMix { SLuint32 locatorType; SLObjectItf outputMix; } SLDataLocator_OutputMix;
typedef struct SLDataFormat_PCM { SLuint32 formatType; SLuint32 numChannels; SLuint32 samplesPerSec; SLuint32 bitsPerSample; SLuint32 containerSize; SLuint32 channelMask; SLuint32 endianness; } SLDataFormat_PCM;
typedef struct SLDataSource { void* pLocator; void* pFormat; } SLDataSource;
typedef struct SLDataSink { void* pLocator; void* pFormat; } SLDataSink;
struct SLObjectItf_ {
    SLresult (*Realize)(SLObjectItf, SLboolean);
    SLresult (*Resume)(SLObjectItf);
    SLresult (*GetState)(SLObjectItf, SLuint32*);
    SLresult (*GetInterface)(SLObjectItf, SLInterfaceID, void*);
    SLresult (*Destroy)(SLObjectItf);
};
struct SLEngineItf_ {
    SLresult (*CreateOutputMix)(SLEngineItf, SLObjectItf*, SLuint32, const SLInterfaceID*, const SLboolean*);
    SLresult (*CreateAudioPlayer)(SLEngineItf, SLObjectItf*, const SLDataSource*, const SLDataSink*, SLuint32, const SLInterfaceID*, const SLboolean*);
};
struct SLPlayItf_ {
    SLresult (*SetPlayState)(SLPlayItf, SLuint32);
    SLresult (*GetPlayState)(SLPlayItf, SLuint32*);
};
struct SLAndroidSimpleBufferQueueItf_ {
    SLresult (*Enqueue)(SLAndroidSimpleBufferQueueItf, const void*, SLuint32);
    SLresult (*Clear)(SLAndroidSimpleBufferQueueItf);
    SLresult (*GetState)(SLAndroidSimpleBufferQueueItf, SLuint32*);
    SLresult (*RegisterCallback)(SLAndroidSimpleBufferQueueItf, void (*)(SLAndroidSimpleBufferQueueItf, void*), void*);
};
struct SLVolumeItf_ {
    SLresult (*SetVolumeLevel)(SLVolumeItf, SLmillibel);
    SLresult (*GetVolumeLevel)(SLVolumeItf, SLmillibel*);
};
extern "C" SLresult slCreateEngine(SLObjectItf*, SLuint32, const SLEngineOption*, SLuint32, const SLInterfaceID*, const SLboolean*);
EOF
cat > .stubs/SLES/OpenSLES_Android.h <<'EOF'
#pragma once
#include <SLES/OpenSLES.h>
#define SL_ANDROID_RECORDING_PRESET_NONE ((SLuint32)0)
EOF
echo "stubs ready"
