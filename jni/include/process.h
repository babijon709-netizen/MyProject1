#pragma once
// Поиск процесса release-клиента по точному package name и фоновый поток
// привязки. Вынесено из main.cpp.

#include <sys/types.h>
#include <atomic>

extern pid_t g_target_pid;      // -1 пока не привязались
extern bool  g_esp_attached;    // память игры читается
// Поток привязки хотя бы раз прошёл поиск игры (индикация «игра не найдена»
// в main.cpp не срабатывает до первого скана).
extern std::atomic<bool> g_attach_scanned;

void start_attach_thread();
void stop_attach_thread();
// Разобрать привязку при выходе: кэши игры сбрасываются, поток сможет
// подключиться заново, когда процесс снова будет доступен.
void process_detach();
