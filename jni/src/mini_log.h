// mini_log.h — мини-лог на устройстве: почему чит не работает там, где меню
// открылось, а функции молчат.
//
// Зачем: на части устройств процесс игры не читается (политика ядра, SELinux,
// «прятки» root-менеджеров), а на других всё работает. По тостам видно только
// «нет доступа», без причины и без места. Этот лог пишет ровно то, что нужно для
// разбора, и ничего больше:
//   * привязка: найден ли процесс игры, отображён ли libil2cpp.so, его базовый
//     адрес, открылся ли /proc/<pid>/mem и с какой ошибкой, прошла ли проба
//     доступа по ELF-заголовку;
//   * счётчики чтений памяти: сколько чтений, сколько отказов, сколько раз
//     переоткрывали дескриптор (по ним видно «читается, но плохо» и «не
//     читается совсем»);
//   * перезагрузки мира и первые боксы после них (жалоба «боксы не сразу»);
//   * состояние аима: выучен коэффициент или взят из чувствительности, сколько
//     раз за такт менялось направление шага (по этому видно «дёргает»).
//
// Это НЕ прежняя подсистема логов автофарма (она вырезана в dca20ff): записей
// мало, все — по событиям и по таймеру (не чаще одного раза в несколько секунд),
// никакой телеметрии по кадрам. Файл лежит рядом с конфигами, обрезается на
// 256 КБ с одним архивом .1, поэтому место не съест.
//
// Вывод в консоль не делается вовсе: только файл.

#pragma once

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

namespace mlog {

namespace detail {

constexpr size_t kMaxBytes  = 256u * 1024u;
constexpr size_t kRateSlots = 8;
constexpr size_t kPathMax   = 240;

inline pthread_mutex_t& lock() { static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER; return m; }
inline FILE*&           file() { static FILE* f = nullptr; return f; }
inline size_t&          written() { static size_t n = 0; return n; }
inline double&          opened_at() { static double t = 0.0; return t; }
inline char*            path() { static char p[kPathMax] = {}; return p; }

struct RateSlot { char key[16]; double at; int used; };
inline RateSlot* rate() { static RateSlot slots[kRateSlots] = {}; return slots; }

}  // namespace detail

// Монотонные секунды: ими же пользуется код, которому нужны интервалы.
inline double seconds() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

// Открыть файл на дозапись (в конец). Зовётся один раз при старте.
inline void init(const char* path_in) {
    if (!path_in || !path_in[0]) return;
    pthread_mutex_lock(&detail::lock());
    snprintf(detail::path(), detail::kPathMax, "%s", path_in);
    detail::file() = fopen(detail::path(), "ab");
    if (detail::file()) {
        fseek(detail::file(), 0, SEEK_END);
        long size = ftell(detail::file());
        detail::written() = (size > 0) ? (size_t)size : 0;
        detail::opened_at() = seconds();
        if (detail::written() > detail::kMaxBytes) {
            // Один архив: старые записи не жалко, важны последние.
            fclose(detail::file());
            detail::file() = nullptr;
            char old[detail::kPathMax + 8];
            snprintf(old, sizeof(old), "%s.1", detail::path());
            remove(old);
            rename(detail::path(), old);
            detail::file() = fopen(detail::path(), "ab");
            detail::written() = 0;
        }
    }
    pthread_mutex_unlock(&detail::lock());
}

inline bool ready() { return detail::file() != nullptr; }

namespace detail {

inline void stamp(char* out, size_t out_size) {
    struct timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm local{};
    localtime_r(&ts.tv_sec, &local);
    const double up = seconds() - opened_at();
    snprintf(out, out_size, "%02d-%02d %02d:%02d:%02d.%03d (+%.1f) ",
             local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec,
             (int)(ts.tv_nsec / 1000000), up);
}

}  // namespace detail

// Одна строка. Потокобезопасно, со сбросом на диск сразу (падение не съест).
inline void line(const char* fmt, ...) {
    if (!detail::file()) return;
    char text[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);

    char head[64];
    pthread_mutex_lock(&detail::lock());
    if (detail::file()) {
        detail::stamp(head, sizeof(head));
        detail::written() += (size_t)fprintf(detail::file(), "%s%s\n", head, text);
        fflush(detail::file());
        if (detail::written() > detail::kMaxBytes) {
            fclose(detail::file());
            detail::file() = nullptr;
            char old[detail::kPathMax + 8];
            snprintf(old, sizeof(old), "%s.1", detail::path());
            remove(old);
            rename(detail::path(), old);
            detail::file() = fopen(detail::path(), "ab");
            detail::written() = 0;
            if (detail::file()) {
                detail::stamp(head, sizeof(head));
                fprintf(detail::file(), "%sлог обрезан (256 КБ), продолжение в новом файле\n", head);
                fflush(detail::file());
            }
        }
    }
    pthread_mutex_unlock(&detail::lock());
}

// То же, но не чаще, чем раз в `period` секунд (для сводок по таймеру).
inline void every(const char* key, double period, const char* fmt, ...) {
    if (!detail::file() || !key) return;
    const double now = seconds();
    detail::RateSlot* slots = detail::rate();
    detail::RateSlot* slot = nullptr;
    for (size_t i = 0; i < detail::kRateSlots; ++i) {
        if (!slots[i].used) { if (!slot) slot = &slots[i]; continue; }
        if (strncmp(slots[i].key, key, sizeof(slots[i].key)) == 0) { slot = &slots[i]; break; }
    }
    if (!slot) return;
    if (slot->used && (now - slot->at) < period) return;
    if (!slot->used) {
        slot->used = 1;
        snprintf(slot->key, sizeof(slot->key), "%s", key);
    }
    slot->at = now;

    char text[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    line("%s", text);
}

}  // namespace mlog
