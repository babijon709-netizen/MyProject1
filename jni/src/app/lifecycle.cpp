// app/lifecycle.cpp — Признаки жизни процесса (main_thread_flag, g_frame_done).
//
// Модуль разрезан из прежнего монолита jni/src/main.cpp;
// что здесь лежит и кто это зовёт — в шапке app/lifecycle.h и в docs/CODE_MAP.md.


#include "app/common.h"
#include "esp/esp_time.h"   // mono_seconds: время последнего завершённого кадра
#include "ui/sheet.h"
#include "app/lifecycle.h"

std::atomic<bool> main_thread_flag{true};

std::atomic<bool> g_frame_done{true};

std::atomic<const char*> g_frame_stage{"старт"};

std::atomic<unsigned long long> g_frame_count{0};

std::atomic<double> g_frame_heartbeat{0.0};

pthread_t g_main_thread = 0;

std::atomic<int> g_main_tid{0};

void FrameStage(const char* stage) { g_frame_stage.store(stage); }

void FrameDone() {
    g_frame_heartbeat.store(mono_seconds());
    g_frame_count.fetch_add(1);
}
