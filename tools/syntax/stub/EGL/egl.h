#pragma once
#include <cstdint>
#include <android/native_window.h>
typedef void* EGLDisplay; typedef void* EGLSurface; typedef void* EGLContext; typedef void* EGLConfig;
typedef int32_t EGLint; typedef uintptr_t EGLNativeDisplayType; typedef uintptr_t EGLNativeWindowType;
typedef unsigned int EGLenum;
#define EGL_NO_DISPLAY ((EGLDisplay)0)
#define EGL_NO_SURFACE ((EGLSurface)0)
#define EGL_NO_CONTEXT ((EGLContext)0)
#define EGL_TRUE 1
#define EGL_FALSE 0
#define EGL_DEFAULT_DISPLAY ((EGLNativeDisplayType)0)
#define EGL_SUCCESS 0x3000
#define EGL_OPENGL_ES3_BIT 0x40
#define EGL_OPENGL_ES2_BIT 0x4
#define EGL_BUFFER_SIZE 0x3020
#define EGL_STENCIL_SIZE 0x3026
#define EGL_NATIVE_VISUAL_ID 0x302E
#define EGL_TRANSPARENT_TYPE 0x3034
#define EGL_SAMPLES 0x3031
#define EGL_SAMPLE_BUFFERS 0x3032
#define EGL_LUMINANCE_SIZE 0x303D
#define EGL_CONFORMANT 0x3042
#define EGL_PBUFFER_BIT 0x1
#define EGL_PIXMAP_BIT 0x2
#define EGL_TRANSPARENT_RGB 0x3052
#define EGL_CONTEXT_MAJOR_VERSION 0x3098
#define EGL_OPENGL_ES_BIT 0x1
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_SURFACE_TYPE 0x3033
#define EGL_WINDOW_BIT 0x4
#define EGL_RED_SIZE 0x3024
#define EGL_GREEN_SIZE 0x3025
#define EGL_BLUE_SIZE 0x3026
#define EGL_ALPHA_SIZE 0x3027
#define EGL_DEPTH_SIZE 0x3025
#define EGL_NONE 0x3038
inline EGLDisplay eglGetDisplay(EGLNativeDisplayType) { return EGL_NO_DISPLAY; }
inline EGLint eglInitialize(EGLDisplay, EGLint*, EGLint*) { return EGL_TRUE; }
inline EGLint eglChooseConfig(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*) { return EGL_TRUE; }
inline EGLint eglGetConfigAttrib(EGLDisplay, EGLConfig, EGLint, EGLint*) { return EGL_TRUE; }
inline EGLContext eglCreateContext(EGLDisplay, EGLConfig, EGLContext, const EGLint*) { return EGL_NO_CONTEXT; }
inline EGLSurface eglCreateWindowSurface(EGLDisplay, EGLConfig, ANativeWindow*, const EGLint*) { return EGL_NO_SURFACE; }
inline EGLint eglMakeCurrent(EGLDisplay, EGLSurface, EGLSurface, EGLContext) { return EGL_TRUE; }
inline EGLint eglSwapBuffers(EGLDisplay, EGLSurface) { return EGL_TRUE; }
inline EGLint eglSwapInterval(EGLDisplay, EGLint) { return EGL_TRUE; }
inline EGLint eglDestroySurface(EGLDisplay, EGLSurface) { return EGL_TRUE; }
inline EGLint eglDestroyContext(EGLDisplay, EGLContext) { return EGL_TRUE; }
inline EGLint eglTerminate(EGLDisplay) { return EGL_TRUE; }
