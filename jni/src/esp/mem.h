#pragma once
// mem.h — Доступ к памяти игры и состояние привязки.
//
// Всё, что модуль отдаёт наружу: сюда смотрят те модули, которым нужны эти
// типы, данные и функции. Реализация — в mem.cpp.
#include "esp/common.h"

// ---- Данные, которые видят другие модули ----

extern memio::Reader g_mem;

extern pid_t g_pid;

extern uint64_t g_il2cpp_base;

extern uint64_t g_player_manager_class;

extern uint64_t g_player_manager_static_fields;

extern uint64_t g_game_controller_class;

extern uint64_t g_local_player;

extern bool g_matrix_configuration_validated;

extern bool g_camera_matrix_physical_match;

extern uint64_t g_player_position_offset;

extern EspAttachState g_attach_state;

// ---- Шаблоны и inline-функции ----

template<typename T>
inline T rd(uint64_t addr) {
    T v{};
    g_mem.read(addr, &v, sizeof(T));
    return v;
}

template<typename T>
inline bool rd_exact(uint64_t addr, T& value) {
    value = {};
    if (!addr) return false;
    return g_mem.read(addr, &value, sizeof(T));
}

// ---- Функции, которые видят другие модули ----

bool rd_buf(uint64_t addr, void* out, size_t size);

uint64_t rd_ptr(uint64_t a);

Vec3 rd_v3 (uint64_t a);

Mat4 rd_m4 (uint64_t a);

bool wr_buf(uint64_t addr, const void* in, size_t size);

// Переоткрыть /proc/<pid>/mem и перечитать пробу (см. mem_io.h). Нужно потоку
// привязки: прежде чем считать доступ потерянным, он пробует это.
bool esp_rebind_memory();
