// app/lifecycle.cpp — Признаки жизни процесса (main_thread_flag, g_frame_done).
//
// Модуль разрезан из прежнего монолита jni/src/main.cpp;
// что здесь лежит и кто это зовёт — в шапке app/lifecycle.h и в docs/CODE_MAP.md.


#include "app/common.h"
#include "ui/sheet.h"
#include "app/lifecycle.h"

std::atomic<bool> main_thread_flag{true};

std::atomic<bool> g_frame_done{true};
