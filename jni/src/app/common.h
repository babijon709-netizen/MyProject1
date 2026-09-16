#pragma once
// Общая шапка модулей меню, аима и автофарма (jni/src/app, ui, aim, farm).
//
// Было: верх файла-монолита jni/src/main.cpp, где одним блоком подключалось
// сразу всё — от ImGui и GLES до OpenSLES и stb_image. Теперь каждый модуль
// подключает этот заголовок и дальше только то, что нужно ему самому.
//
// Особые случаи:
//   * stb_image.h и #define STB_IMAGE_IMPLEMENTATION — только в app/media.cpp
//     (реализация должна быть ровно в одной единице трансляции);
//   * аудио (OpenSLES) — app/audio.cpp вместе с app/audio.h;
//   * ubl* таблицы строк UI — ui/xp.h (XS/XS_RU подставляются каждому модулю).
//
// Что где лежит дальше:
//   app/attach.cpp   — поиск процесса игры и поток привязки
//   app/entry.cpp    — main(): инициализация, кадр меню, завершение
//   app/media.cpp    — иконки вкладок (загрузка GL-текстур)
//   app/screen.cpp   — размеры экрана и центрирование окна меню
//   app/audio.cpp    — звуки меню
//   ui/theme.cpp     — палитра, тёмная тема, применение темы
//   ui/settings.cpp  — настройки ESP/аима (cfg::esp, cfg::aim) и alpha бара
//   ui/layout.cpp    — Layout, InputState, AppState, ввод и анимации
//   ui/widgets.cpp   — строки-переключатели, слайдеры, карточки, заголовки
//   ui/toast.cpp     — всплывающие подсказки (тосты)
//   ui/watermark.cpp — пилюли-подписи поверх игры
//   ui/sheet.cpp     — нижняя шторка (Sheet) и кнопки выхода
//   ui/popover.cpp   — всплывающее окно (Popover) и его содержимое
//   ui/scroll.cpp    — прокрутка панелей и состояние окна меню
//   ui/config.cpp    — конфиги: файлы, XOR, слежение за каталогом
//   ui/tabs.cpp      — TabContent: содержимое вкладок
//   ui/window.cpp    — RenderMenu: окно меню целиком
//   ui/esp_overlay.cpp — отрисовка ESP поверх игры
//   aim/controller.cpp — параметры аима, чувствительность, отпускание пальца
//   aim/update.cpp     — UpdateAim: один такт аимбота
//   farm/state.cpp     — g_farm*: что видит окно автофарма
//   farm/controller.cpp — UpdateFarm/UpdateFarmInner: контроллер автофарма

#include "main.h"                 // Android_draw/draw.h → ImGui и окно оверлея
#include "game.h"                 // публичный API ESP (esp_*) и его структуры
#include "game_offsets_active.h"  // активные оффсеты: релиз или бета (go::SelectBuild)
#include "Vector.h"
#include "lang.h"                 // РУ/EN: перевод меню и визуалов
#include "aim_learn.h"            // оценка град/px по реакции прицела
#include "text_utf8.h"            // копия строки с обрезкой по UTF-8
#include "VidAvatar.h"            // аватарка-видео в кружке меню (кадры в VidAvatar.cpp)
#include "Blur/Blur.h"
#include "Android_touch/TouchHelperA.h"   // счётчики Upload (диагностика лага)

#include "ui/xp.h"                // XS/XS_RU: строки интерфейса
#include "app/audio.h"            // SND_* и AudioInit/PlaySound/AudioFree

#include <GLES3/gl3.h>
#include <cmath>
#include <atomic>
#include <chrono>
#include <string>
#include <dirent.h>
#include <fcntl.h>
#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>
#include <fstream>
#include <vector>
#include <algorithm>
#include <random>
#include <sys/inotify.h>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cstdarg>
#include <csignal>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <time.h>

using namespace go_active;
