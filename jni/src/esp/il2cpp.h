#pragma once
// il2cpp.h — Il2Cpp: базовые адреса, классы, статические поля.
//
// Всё, что модуль отдаёт наружу: сюда смотрят те модули, которым нужны эти
// типы, данные и функции. Реализация — в il2cpp.cpp.
#include "esp/common.h"

// ---- Функции, которые видят другие модули ----

int get_base_candidates(const char* lib, uint64_t* out, int max);

bool class_looks_alive(uint64_t klass);

int class_identity(uint64_t klass, const char* expected_name, const char* expected_ns);

bool base_resolves_game(uint64_t base);

bool player_list_contains(uint64_t list, uint64_t player);

uint64_t get_class_static_fields(uint64_t klass);

uint64_t resolve_runtime_player_list();

uint64_t resolve_local_player();

uint64_t resolve_native_transform(uint64_t transform);

bool object_class_name_is(uint64_t obj, const char* expected, bool accept_when_unreadable = false);
