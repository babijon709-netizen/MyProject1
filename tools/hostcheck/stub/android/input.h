// Заглушка android/input.h для hostcheck (на устройстве — NDK-заголовок).
// Значения и сигнатуры совпадают с NDK r26: геттеры AKeyEvent_*/AMotionEvent_*
// принимают const AInputEvent* (именно поэтому imgui_impl_android.cpp передаёт
// input_event напрямую), а HOVER_MOVE=7 / SCROLL=8 — разные значения.
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

// Meta key state (AMetaKey)
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
