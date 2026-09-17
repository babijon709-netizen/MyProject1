// Лог в Загрузки: см. jni/include/logfile.h.
//
// Пишем обычным fopen/fprintf: путь тот же внешний носитель, куда уже
// складываются конфиги (/storage/emulated/0/...), — значит, доступ к записи у
// приложения есть. Загрузки пробуем первыми: оттуда файл проще всего вытащить
// («Файлы» -> «Загрузки»), а если каталог не пишется — откатываемся в benzhack.
#include "logfile.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <mutex>
#include <thread>

namespace {

const char* kPaths[] = {
    "/storage/emulated/0/Download/benzware_aim.log",
    "/storage/emulated/0/benzhack/aim_debug.log",
};

// Потолок файла: дальше перезаписываем с начала. За сутки разбора набирается
// единицы мегабайт, но если лог забыли выключить — он не должен съесть всё
// место на устройстве.
constexpr long kMaxBytes = 6L * 1024 * 1024;

FILE*                    g_file = nullptr;
const char*              g_path = nullptr;
std::mutex               g_mutex;
bool                     g_opened = false;
long                     g_written = 0;
struct timespec          g_started{};
std::atomic<int>         g_stage{kStageNone};
std::atomic<unsigned long> g_frames{0};
std::atomic<bool>        g_watchdog_on{false};

double uptime_seconds() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)(ts.tv_sec - g_started.tv_sec) +
           (double)(ts.tv_nsec - g_started.tv_nsec) * 1e-9;
}

const char* stage_name(int stage) {
    switch (stage) {
        case kStageFrameBegin: return "начало кадра";
        case kStageEsp:        return "ESP";
        case kStageAimSelect:  return "выбор цели";
        case kStageAimRead:    return "чтение памяти режима";
        case kStageAimWrite:   return "запись в память игры";
        case kStageAimTouch:   return "инъекция касания";
        case kStageFarm:       return "автофарм";
        case kStageMenu:       return "меню";
        case kStageFreecam:    return "фрикам";
        case kStageFrameEnd:   return "конец кадра";
        default:               return "вне кадра";
    }
}

}  // namespace

void LogOpen() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_opened) return;
    clock_gettime(CLOCK_MONOTONIC, &g_started);
    for (const char* path : kPaths) {
        FILE* f = fopen(path, "a");
        if (!f) continue;
        g_file = f;
        g_path = path;
        g_opened = true;
        g_written = ftell(f);
        // Первая строка — чтобы было видно, какой это запуск и куда пишем.
        // Пишем напрямую: LogLine берёт тот же мьютекс, а он уже наш — иначе
        // тут же и встанем (std::mutex не рекурсивный).
        fprintf(g_file, "--- лог открыт: %s (было %ld байт)\n", path, g_written);
        fflush(g_file);
        return;
    }
    // Не открылся ни один путь: молчим (меню и ESP обязаны работать и без лога).
}

void LogClose() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_opened) return;
    fclose(g_file);
    g_file = nullptr;
    g_opened = false;
}

void LogLine(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_opened) return;

    char text[512];
    va_list args;
    va_start(args, fmt);
    const int n = vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    if (n <= 0) return;
    // Каждая запись — с своей строки: в форматах перевод не пишем, он здесь.
    if (n >= (int)sizeof(text) - 1) {
        text[sizeof(text) - 2] = '\n';
        text[sizeof(text) - 1] = 0;
    } else if (text[n - 1] != '\n') {
        text[n] = '\n';
        text[n + 1] = 0;
    }

    time_t now = time(nullptr);
    struct tm local{};
    localtime_r(&now, &local);
    struct timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);

    const int len = fprintf(g_file, "%02d-%02d %02d:%02d:%02d.%03ld (+%.1f) %s",
                            local.tm_mon + 1, local.tm_mday,
                            local.tm_hour, local.tm_min, local.tm_sec,
                            (long)(ts.tv_nsec / 1000000L),
                            uptime_seconds(), text);
    if (len > 0) g_written += len;
    // Сбрасываем сразу: при зависании (или если приложение прибьют) всё, что
    // осталось в буфере stdio, уже никто не прочитает.
    fflush(g_file);

    if (g_written > kMaxBytes && g_path) {
        // По кругу: освобождаем место, помечая, что часть лога потеряна.
        fclose(g_file);
        g_file = fopen(g_path, "w");
        g_written = 0;
        if (g_file) {
            fprintf(g_file, "--- лог перезаписан по кругу (было больше %ld байт)\n", kMaxBytes);
            fflush(g_file);
        }
    }
}

void LogStage(int stage) { g_stage.store(stage); }

void LogFrameBeat(unsigned long frame) { g_frames.store(frame); }

void LogWatchdogStart() {
    if (g_watchdog_on.exchange(true)) return;   // уже запущен
    std::thread([]() {
        unsigned long last = g_frames.load();
        struct timespec last_change{};
        clock_gettime(CLOCK_MONOTONIC, &last_change);
        bool reported = false;
        while (g_watchdog_on.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            const unsigned long now = g_frames.load();
            struct timespec ts{};
            clock_gettime(CLOCK_MONOTONIC, &ts);
            if (now != last) {
                last = now;
                last_change = ts;
                if (reported) {
                    reported = false;
                    LogLine("сторож: кадры пошли снова (кадр %lu)", now);
                }
                continue;
            }
            const double still = (double)(ts.tv_sec - last_change.tv_sec) +
                                 (double)(ts.tv_nsec - last_change.tv_nsec) * 1e-9;
            // Кадр не идёт больше полутора секунд — пишем, где его застало.
            // Дальше — раз в секунду, пока не пойдёт.
            if (still > 1.5 && (!reported || still > 3.0)) {
                reported = true;
                LogLine("СТОПОР: кадр %lu не идёт %.1f с, стадия: %s",
                        last, still, stage_name(g_stage.load()));
            }
        }
    }).detach();
}
