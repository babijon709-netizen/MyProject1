#pragma once
#include <cstdint>
typedef void* EGLDisplay; typedef void* EGLSurface; typedef void* EGLContext;
typedef void* EGLConfig; typedef void* EGLNativeWindowType; typedef int EGLint; typedef unsigned int EGLBoolean;
#define EGL_TRUE 1
#define EGL_NO_DISPLAY ((EGLDisplay)0)
#define EGL_NO_SURFACE ((EGLSurface)0)
#define EGL_NO_CONTEXT ((EGLContext)0)
#define EGL_ALPHA_SIZE 0x3021
#define EGL_BLUE_SIZE 0x3022
#define EGL_GREEN_SIZE 0x3023
#define EGL_RED_SIZE 0x3024
#define EGL_DEPTH_SIZE 0x3025
#define EGL_STENCIL_SIZE 0x3026
#define EGL_SURFACE_TYPE 0x3033
#define EGL_WINDOW_BIT 0x0004
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES2_BIT 0x0004
#define EGL_NONE 0x3038
#define EGL_DEFAULT_DISPLAY ((EGLDisplay)0)
#define EGL_BUFFER_SIZE 0x3058
#define EGL_NATIVE_VISUAL_ID 0x3000
#define EGL_WIDTH 0x3057
#define EGL_HEIGHT 0x3056
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
static inline EGLDisplay eglGetDisplay(void*) { return nullptr; }
static inline EGLBoolean eglInitialize(EGLDisplay, EGLint*, EGLint*) { return 0; }
static inline EGLBoolean eglChooseConfig(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*) { return 0; }
static inline EGLSurface eglCreateWindowSurface(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint*) { return nullptr; }
static inline EGLContext eglCreateContext(EGLDisplay, EGLConfig, EGLContext, const EGLint*) { return nullptr; }
static inline EGLBoolean eglMakeCurrent(EGLDisplay, EGLSurface, EGLSurface, EGLContext) { return 0; }
static inline EGLBoolean eglSwapBuffers(EGLDisplay, EGLSurface) { return 0; }
static inline EGLBoolean eglQuerySurface(EGLDisplay, EGLSurface, EGLint, EGLint*) { return 0; }
static inline EGLBoolean eglDestroySurface(EGLDisplay, EGLSurface) { return 0; }
static inline EGLBoolean eglDestroyContext(EGLDisplay, EGLContext) { return 0; }
static inline EGLBoolean eglTerminate(EGLDisplay) { return 0; }
static inline void eglSwapInterval(EGLDisplay, EGLint) { }
static inline EGLBoolean eglGetConfigAttrib(EGLDisplay, EGLConfig, EGLint, EGLint*) { return 0; }
