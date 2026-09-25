LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := benzware

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

LOCAL_C_INCLUDES += $(LOCAL_PATH)/include $(LOCAL_PATH)/include/ImGui $(LOCAL_PATH)/include/ImGui/backends $(LOCAL_PATH)/src \
    $(LOCAL_PATH)/src/third_party/mbedtls/include \
    $(LOCAL_PATH)/src/third_party/mbedtls/library \
    $(LOCAL_PATH)/src/third_party/mbedtls/3rdparty/everest/include \
    $(LOCAL_PATH)/src/third_party/mbedtls/3rdparty/everest/include/everest \
    $(LOCAL_PATH)/src/third_party/mbedtls/3rdparty/everest/include/everest/kremlib \
    $(LOCAL_PATH)/src/third_party/mbedtls/3rdparty/p256-m/p256-m/include \
    $(LOCAL_PATH)/src/third_party/mbedtls/3rdparty/p256-m/p256-m/include/p256-m \
    $(LOCAL_PATH)/src/third_party/mbedtls/3rdparty/p256-m/p256-m_driver_interface

LOCAL_SRC_FILES := \
    src/main.cpp src/remote_gate.cpp src/auth_http.cpp src/device_identity.cpp \
    src/process.cpp src/audio.cpp src/theme.cpp src/hud.cpp \
    src/esp_draw.cpp src/aim.cpp src/silent_aim.cpp src/farm.cpp \
    src/widgets.cpp src/menu.cpp src/config.cpp \
    src/game.cpp src/game_viewmodel.cpp src/game_markers.cpp src/game_farm.cpp src/logfile.cpp \
    src/offset_resolver.cpp \
    src/lang.cpp \
    src/Android_draw/draw.cpp src/Android_touch/TouchHelperA.cpp \
    src/VidAvatar.cpp \
    src/third_party/tjpgd/tjpgd.c \
    src/Blur/Blur.cpp \
    src/ImGui/imgui.cpp src/ImGui/imgui_draw.cpp src/ImGui/imgui_tables.cpp \
    src/ImGui/imgui_widgets.cpp src/ImGui/backends/imgui_impl_android.cpp \
    src/ImGui/backends/imgui_impl_opengl3.cpp

_MBEDTLS := $(LOCAL_PATH)/src/third_party/mbedtls
LOCAL_SRC_FILES += $(patsubst $(LOCAL_PATH)/%,%,$(wildcard $(_MBEDTLS)/library/*.c)) \
    src/third_party/mbedtls/3rdparty/everest/library/everest.c \
    src/third_party/mbedtls/3rdparty/everest/library/x25519.c \
    src/third_party/mbedtls/3rdparty/everest/library/Hacl_Curve25519_joined.c \
    src/third_party/mbedtls/3rdparty/p256-m/p256-m_driver_entrypoints.c \
    src/third_party/mbedtls/3rdparty/p256-m/p256-m/p256-m.c

LOCAL_LDLIBS := -llog -landroid -lEGL -lGLESv3 -lOpenSLES -lz -ldl

include $(BUILD_EXECUTABLE)


# ---- Съёмщик памяти игры (автономный бинарь, не чит) -----------------------
include $(CLEAR_VARS)
LOCAL_MODULE    := benzhack_dump
LOCAL_CPPFLAGS  := -O2 -march=armv8-a -std=c++17 -fPIE -fno-rtti -fexceptions
LOCAL_LDFLAGS   := -pie -Wl,--gc-sections,-s,--strip-all
LOCAL_SRC_FILES := dumper/dumper.cpp
include $(BUILD_EXECUTABLE)
