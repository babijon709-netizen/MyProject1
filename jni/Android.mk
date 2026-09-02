LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := xvcen

_CF := -O3 -ffast-math -march=armv8-a+simd \
       -fomit-frame-pointer -ffunction-sections -fdata-sections \
       -fvisibility=hidden -fvisibility-inlines-hidden \
       -DUSE_OPENGL -DIMGUI_IMPL_OPENGL_ES3 -DNDEBUG \
       -fstack-protector-strong \
       -fPIE \
       -fno-unwind-tables \
       -fno-asynchronous-unwind-tables \
       -fstrict-aliasing

LOCAL_CPPFLAGS := $(_CF) -std=gnu++17 -fexceptions -fno-rtti
LOCAL_CFLAGS   := $(_CF) -std=c11

LOCAL_LDFLAGS := -Wl,--gc-sections,-s,--strip-all -Wl,-x -Wl,--build-id=none -pie

LOCAL_C_INCLUDES += $(LOCAL_PATH)/include $(LOCAL_PATH)/include/ImGui $(LOCAL_PATH)/include/ImGui/backends $(LOCAL_PATH)/src

LOCAL_SRC_FILES := \
    src/main.cpp src/game.cpp src/skeleton.cpp src/Android_draw/draw.cpp src/Android_touch/TouchHelperA.cpp \
    src/Blur/Blur.cpp \
    src/ImGui/imgui.cpp src/ImGui/imgui_draw.cpp src/ImGui/imgui_tables.cpp \
    src/ImGui/imgui_widgets.cpp src/ImGui/backends/imgui_impl_android.cpp \
    src/ImGui/backends/imgui_impl_opengl3.cpp

LOCAL_LDLIBS := -llog -landroid -lEGL -lGLESv3 -lOpenSLES -lz -ldl

include $(BUILD_EXECUTABLE)
