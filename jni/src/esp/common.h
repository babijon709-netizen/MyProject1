#pragma once
// Общая шапка модулей ESP (jni/src/esp/*).
//
// Раньше это был верх файла-монолита game.cpp: одни и те же включения и
// `using namespace go_active` на весь файл. Теперь каждый модуль ESP
// подключает этот заголовок первым, а дальше — только свой.
//
// Модуль добавляет к этому ровно то, что ему нужно:
//   esp/mem.h            — чтение/запись памяти игры (rd/wr)
//   esp/il2cpp.h         — поиск баз, классов и статических полей
//   esp/transform.h      — позиция через иерархию Transform
//   esp/camera.h         — камера: матрицы, углы, чувствительность
//   esp/math.h           — вектора/матрицы, мировое→экранное
//   esp/managed.h        — managed-строки и коллекции
//   esp/names.h          — ники игроков
//   esp/weapons.h        — метка оружия в руках
//   esp/skeleton_cache.h — кеш скелетов, включение костей
//   esp/skeleton_names.h — имена костей, поиск скелета, KCC/ragdoll
//   esp/skeleton_build.h — сборка скелета
//   esp/player_pose.h    — позиция игрока: трек, скачки, «сидит/на маунте»
//   esp/aim_points.h     — точки прицела и локальный ADS
//   esp/melee.h          — дальность удара ближнего боя
//   esp/frame.h          — состояние кадра ESP
//   esp/boxes.h          — esp_get_boxes
//   esp/markers.h        — маркеры мира
//   esp/marker_labels.h  — подписи маркеров
//   esp/farm_scan.h      — скан узлов автофарма
//   esp/farm_target.h    — выбор узла автофарма
//   esp/game_patch.h     — запись в игру: X-ray, «всегда день»

#include "game.h"                 // публичный API ESP (его зовёт main.cpp)
#include "mem_io.h"               // /proc/<pid>/mem + кэш блоков
#include "maps_lookup.h"          // базовый адрес библиотеки по /proc/<pid>/maps
#include "game_offsets_active.h"  // активные оффсеты: релиз или бета (go::SelectBuild)
#include "Vector.h"
#include "lang.h"                 // РУ/EN: подписи визуалов
#include "text_utf8.h"            // копия подписи в буфер без разрезания UTF-8

#include <string.h>
#include <strings.h>   // strncasecmp
#include <sys/uio.h>
#include <stdio.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <chrono>
#include <atomic>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <unistd.h>
#include <sys/syscall.h>

// Оффсеты всех версий игры лежат в go_active/go_beta; активный набор выбирается
// при старте (go::SelectBuild), поэтому модули работают с безымянным go_active.
using namespace go_active;

// Часы ожидания (steady clock, секунды). Раньше жили внутри game.cpp рядом с
// PlayerTrack, а звались из половины файла; теперь определение одно на всех —
// inline в заголовке, чтобы модули не тянули друг друга из-за одной функции.
#include "esp_time.h"
