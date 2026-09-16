#pragma once
// app/lifecycle.h — Признаки жизни процесса (main_thread_flag, g_frame_done).
//
// Всё, что модуль отдаёт наружу: сюда смотрят те модули, которым нужны эти
// типы, данные и функции. Реализация — в app/lifecycle.cpp.
#include "app/common.h"

// ---- Данные модуля ----

extern std::atomic<bool> main_thread_flag;

extern std::atomic<bool> g_frame_done;

// ---- Типы модуля ----

namespace prot {
static void Init() {}
}
