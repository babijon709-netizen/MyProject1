#pragma once
#include <cstdint>
struct ANativeWindow;
typedef struct ANativeWindow ANativeWindow;
static inline int ANativeWindow_acquire(ANativeWindow*) { return 0; }
static inline void ANativeWindow_release(ANativeWindow*) { }
static inline int ANativeWindow_setBuffersGeometry(ANativeWindow*, int, int, int) { return 0; }
static inline int32_t ANativeWindow_getWidth(const ANativeWindow*) { return 0; }
static inline int32_t ANativeWindow_getHeight(const ANativeWindow*) { return 0; }
