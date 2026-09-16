#pragma once
// managed.h — Чтение managed-строк и коллекций.
//
// Всё, что модуль отдаёт наружу: сюда смотрят те модули, которым нужны эти
// типы, данные и функции. Реализация — в managed.cpp.
#include "esp/common.h"

// ---- Функции, которые видят другие модули ----

std::string read_remote_string(uint64_t address, bool* readable = nullptr);

bool read_managed_string_ex(uint64_t str_obj, char* out, size_t cap, int32_t max_chars);

bool read_managed_string(uint64_t str_obj, char* out, size_t cap);
