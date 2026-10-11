#define EGL_FALSE 0
#define EGL_TRUE 1
#define EGL_BUFFER_SIZE 0x3020
#define EGL_CONFIG_ID 0x3028
#define EGL_NATIVE_VISUAL_ID 0x302E
#define EGL_CONTEXT_MAJOR_VERSION 0x3098
#define EGL_CONTEXT_MINOR_VERSION 0x30FB
#define EGL_OPENGL_ES_API 0x30A0
#define EGL_PLATFORM_ANDROID_KHR 0x3141
#define EGL_NATIVE_VISUAL_TYPE 0x302F
#define EGL_SAMPLE_BUFFERS 0x3032
#define EGL_MAX_SWAP_INTERVAL 0x303C
#define EGL_MIN_SWAP_INTERVAL 0x303D
#define EGL_SUCCESS 0x3000
EGLBoolean eglGetConfigs(EGLDisplay, EGLConfig*, EGLint, EGLint*);
EGLBoolean eglBindAPI(EGLenum);
EGLint eglGetError(void);
EGLSurface eglCreatePbufferSurface(EGLDisplay, EGLConfig, const EGLint*);
EGLNativeDisplayType eglGetNativeDisplay(void);

