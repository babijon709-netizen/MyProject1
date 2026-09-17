#pragma once
// Поиск процесса игры (по пакету или по libil2cpp.so в maps) и фоновый
// поток привязки. Вынесено из main.cpp.

#include <sys/types.h>

extern pid_t g_target_pid;      // -1 пока не привязались
extern bool  g_esp_attached;    // память игры читается
extern bool  g_buildPrompt;     // стартовый экран выбора версии (релиз/бета)

void start_attach_thread();
void stop_attach_thread();
// Разобрать привязку (смена версии клиента / выход): кэши игры сбрасываются,
// поток сам подцепится заново, когда процесс снова будет прочитан.
void process_detach();
