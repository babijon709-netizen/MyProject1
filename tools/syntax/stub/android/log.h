#pragma once
#include <cstdio>
#define ANDROID_LOG_ERROR 6
#define ANDROID_LOG_WARN 5
#define ANDROID_LOG_INFO 4
#define ANDROID_LOG_DEBUG 3
inline int __android_log_print(int,const*,...){return 0;}
inline int __android_log_write(int,const*,const char*){return 0;}
