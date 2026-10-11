#pragma once
#include <stdint.h>
#include <stddef.h>
typedef int32_t AInputEventType;
typedef struct AInputEvent AInputEvent;
typedef struct AInputQueue AInputQueue;
typedef int32_t AKeyEventAction;
typedef int32_t AKeyEventFlags;
typedef int32_t AMotionEventAction;
typedef int32_t AMotionEventToolType;
#define AKEY_EVENT_ACTION_DOWN 0
#define AKEY_EVENT_ACTION_UP 1
#define AMOTION_EVENT_ACTION_DOWN 0
#define AMOTION_EVENT_ACTION_UP 1
#define AMOTION_EVENT_ACTION_MOVE 2
#define AMOTION_EVENT_ACTION_CANCEL 3
#define AMOTION_EVENT_ACTION_POINTER_DOWN 5
#define AMOTION_EVENT_ACTION_POINTER_UP 6
#define AINPUT_EVENT_TYPE_KEY 1
#define AINPUT_EVENT_TYPE_MOTION 2
#define AMOTION_EVENT_AXIS_X 0
#define AMOTION_EVENT_AXIS_Y 1
#define AMOTION_EVENT_AXIS_PRESSURE 2
#define AMOTION_EVENT_AXIS_SIZE 3
int32_t AInputEvent_getType(const AInputEvent*);
int32_t AInputEvent_getDeviceId(const AInputEvent*);
int32_t AKeyEvent_getAction(const AInputEvent*);
int32_t AKeyEvent_getKeyCode(const AInputEvent*);
int32_t AKeyEvent_getMetaState(const AInputEvent*);
int32_t AMotionEvent_getAction(const AInputEvent*);
int32_t AMotionEvent_getMetaState(const AInputEvent*);
size_t AMotionEvent_getPointerCount(const AInputEvent*);
int32_t AMotionEvent_getPointerId(const AInputEvent*, size_t);
float AMotionEvent_getX(const AInputEvent*, size_t);
float AMotionEvent_getY(const AInputEvent*, size_t);
float AMotionEvent_getPressure(const AInputEvent*, size_t);
float AMotionEvent_getSize(const AInputEvent*, size_t);
float AMotionEvent_getRawX(const AInputEvent*, size_t);
float AMotionEvent_getRawY(const AInputEvent*, size_t);
int32_t AKeyEvent_getScanCode(const AInputEvent*);
int32_t AKeyEvent_getFlags(const AInputEvent*);
int64_t AKeyEvent_getEventTime(const AInputEvent*);
int64_t AMotionEvent_getEventTime(const AInputEvent*);
int64_t AMotionEvent_getDownTime(const AInputEvent*);
#ifndef AMOTION_EVENT_ACTION_MASK
#define AMOTION_EVENT_ACTION_MASK 0xff
#define AMOTION_EVENT_ACTION_POINTER_INDEX_MASK 0xff00
#define AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT 8
#define AMOTION_EVENT_ACTION_HOVER_MOVE 7
#define AMOTION_EVENT_ACTION_SCROLL 8
#define AMOTION_EVENT_ACTION_HOVER_ENTER 9
#define AMOTION_EVENT_ACTION_HOVER_EXIT 10
#define AMOTION_EVENT_TOOL_TYPE_UNKNOWN 0
#define AMOTION_EVENT_TOOL_TYPE_FINGER 1
#define AMOTION_EVENT_TOOL_TYPE_STYLUS 2
#define AMOTION_EVENT_TOOL_TYPE_MOUSE 3
#define AMOTION_EVENT_TOOL_TYPE_ERASER 4
#endif
AMotionEventToolType AMotionEvent_getToolType(const AInputEvent*, size_t);
int32_t AMotionEvent_getButtonState(const AInputEvent*);
float AMotionEvent_getAxisValue(const AInputEvent*, int32_t, size_t);
size_t AMotionEvent_getHistorySize(const AInputEvent*);
#define AMOTION_EVENT_ACTION_BUTTON_PRESS 11
#define AMOTION_EVENT_ACTION_BUTTON_RELEASE 12
#define AMOTION_EVENT_BUTTON_PRIMARY 1
#define AMOTION_EVENT_BUTTON_SECONDARY 2
#define AMOTION_EVENT_BUTTON_TERTIARY 4
#define AMOTION_EVENT_BUTTON_BACK 8
#define AMOTION_EVENT_BUTTON_FORWARD 16
#define AMOTION_EVENT_AXIS_HSCROLL 10
#define AMOTION_EVENT_AXIS_VSCROLL 9
#define AMOTION_EVENT_AXIS_ORIENTATION 8
#define AMOTION_EVENT_AXIS_TOUCH_MAJOR 4
#define AMOTION_EVENT_AXIS_TOUCH_MINOR 5
#define AMOTION_EVENT_AXIS_TOOL_MAJOR 6
#define AMOTION_EVENT_AXIS_TOOL_MINOR 7
#define AINPUT_SOURCE_CLASS_MASK 0x000000ff
#define AINPUT_SOURCE_CLASS_POINTER 0x00000002
#define AINPUT_SOURCE_MOUSE 0x00002002
#define AINPUT_SOURCE_TOUCHSCREEN 0x00001002
#define AINPUT_SOURCE_KEYBOARD 0x00000101
int32_t AInputEvent_getSource(const AInputEvent*);
