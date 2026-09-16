#pragma once
// app/attach.h — Поиск процесса игры и поток привязки.
//
// Всё, что модуль отдаёт наружу: сюда смотрят те модули, которым нужны эти
// типы, данные и функции. Реализация — в app/attach.cpp.
#include "app/common.h"

// ---- Данные модуля ----

extern pid_t g_target_pid;

extern bool g_esp_attached;

extern bool g_buildPrompt;

// ---- Функции, которые видят другие модули ----

void start_attach_thread();

void stop_attach_thread();
