#pragma once
#include <cstdio>
enum { ANDROID_LOG_INFO=4, ANDROID_LOG_ERROR=6, ANDROID_LOG_DEBUG=3, ANDROID_LOG_WARN=5, ANDROID_LOG_VERBOSE=2, ANDROID_LOG_UNKNOWN=0, ANDROID_LOG_DEFAULT=1, ANDROID_LOG_FATAL=7 };
static inline int __android_log_print(int, const char*, const char*, ...) { return 0; }
static inline int __android_log_vprint(int, const char*, const char*, ...) { return 0; }
