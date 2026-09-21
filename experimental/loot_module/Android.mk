LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE := arena_loot_experimental
LOCAL_SRC_FILES := module.cpp
LOCAL_C_INCLUDES := $(LOCAL_PATH)/../../jni/src
LOCAL_CPPFLAGS := -std=c++17 -O2 -Wall -Wextra -fexceptions -fvisibility=hidden -mbranch-protection=standard
LOCAL_LDFLAGS := -Wl,-z,relro,-z,now,-z,nodelete
include $(BUILD_SHARED_LIBRARY)
