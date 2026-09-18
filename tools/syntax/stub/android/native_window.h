#pragma once
#include <cstdint>
#include <cstddef>
struct ANativeWindow;
struct ANativeWindow_Buffer { int32_t width, height, stride, format; void* bits; uint32_t reserved[6]; };
inline int32_t ANativeWindow_acquire(ANativeWindow*) { return 0; }
inline int32_t ANativeWindow_release(ANativeWindow*) { return 0; }
inline int32_t ANativeWindow_setBuffersGeometry(ANativeWindow*, int32_t, int32_t, int32_t) { return 0; }
inline int32_t ANativeWindow_getWidth(ANativeWindow*) { return 0; }
inline int32_t ANativeWindow_getHeight(ANativeWindow*) { return 0; }
inline int32_t ANativeWindow_setFrameRate(ANativeWindow*, float, int8_t) { return 0; }
inline int32_t ANativeWindow_setFrameRateWithChangeStrategy(ANativeWindow*, float, int8_t, int8_t) { return 0; }
inline int32_t ANativeWindow_setBufferCount(ANativeWindow*, size_t) { return 0; }
inline int32_t ANativeWindow_lock(ANativeWindow*, ANativeWindow_Buffer*, void*) { return 0; }
inline int32_t ANativeWindow_unlockAndPost(ANativeWindow*) { return 0; }
inline int32_t ANativeWindow_setSwapInterval(ANativeWindow*, int32_t) { return 0; }
