#pragma once
#include <cstdio>
#include <cstdarg>
#define ANDROID_LOG_ERROR 6
#define ANDROID_LOG_WARN 5
#define ANDROID_LOG_INFO 4
#define ANDROID_LOG_DEBUG 3
inline int __android_log_print(int, const char*, const char*, ...) { return 0; }
inline int __android_log_vprint(int, const char*, const char*, va_list) { return 0; }
inline int __android_log_write(int, const char*, const char*) { return 0; }
