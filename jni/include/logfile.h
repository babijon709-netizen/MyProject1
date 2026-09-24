#pragma once
// Диагностический лог в файл (папка Загрузок). Включён: резолвер оффсетов,
// привязка и смотритель кадра пишут по несколько строк в секунду — по файлу
// на устройстве видно то, что с хоста не разглядеть. Файл ротируется сам
// (старый становится .old), так что место не кончается.
//
// Реализация целиком в заголовке (inline): стенды собирают один свой
// перевод-юнит и не линкуют модули приложения — так лог доступен всем без
// возни со стабами. На хосте каталог не откроется, и лог молча не пишется.

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <mutex>

enum LogStageId {
    kStageNone = 0,
    kStageFrameBegin,
    kStageEsp,
    kStageAimSelect,
    kStageAimRead,
    kStageAimWrite,
    kStageAimTouch,
    kStageFarm,
    kStageMenu,
    kStageFreecam,
    kStageFrameEnd,
    kStageCount
};

// Открыть/закрыть лог. Открытие ленивое: первый LogLine сам откроет файл,
// эти точки вызова оставлены для совместимости.
inline void LogOpen() {}
inline void LogClose() {}
inline void LogStage(int) {}
inline void LogFrameBeat(unsigned long) {}
inline void LogWatchdogStart() {}

namespace logdetail {

constexpr const char* kLogPath    = "/storage/emulated/0/Download/benzhack_log.txt";
constexpr const char* kLogPathOld = "/storage/emulated/0/Download/benzhack_log.txt.old";
constexpr long kLogMaxBytes = 2L << 20;   // 2 МБ

inline std::mutex& mutex()       { static std::mutex m; return m; }
inline FILE*&      file()        { static FILE* f = nullptr; return f; }
inline bool&       open_failed() { static bool b = false; return b; }

inline double now_seconds() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    static struct timespec t0 = ts;
    return (double)(ts.tv_sec - t0.tv_sec) + (double)(ts.tv_nsec - t0.tv_nsec) * 1e-9;
}

inline void open_once() {
    if (file() || open_failed()) return;
    // Ротация: большой файл уезжает в .old, новый начинается с чистого листа.
    struct stat st;
    if (stat(kLogPath, &st) == 0 && st.st_size >= kLogMaxBytes) {
        remove(kLogPathOld);
        rename(kLogPath, kLogPathOld);
    }
    file() = fopen(kLogPath, "a");
    if (!file()) { open_failed() = true; return; }
    setvbuf(file(), nullptr, _IOLBF, 4096);
    time_t now = time(nullptr);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char wall[32];
    strftime(wall, sizeof(wall), "%Y-%m-%d %H:%M:%S", &tmv);
    // В шапку пишем, КАКАЯ это сборка: время компиляции модуля. Без него в
    // логе не видно, старый бинарник его писал или новый, — и разбор каждый
    // раз начинался с вопроса «а ты новую сборку поставил?». Время сборки
    // сопоставляется с прогоном CI и даёт точный ответ.
    fprintf(file(), "=== лог начат %s | сборка %s %s ===\n", wall, __DATE__, __TIME__);
}

}  // namespace logdetail

// Строка в лог. Потокобезопасно; время — секунды с момента первой записи.
inline void LogLine(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
inline void LogLine(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(logdetail::mutex());
    logdetail::open_once();
    FILE* f = logdetail::file();
    if (!f) return;
    char body[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    fprintf(f, "%9.3f %s\n", logdetail::now_seconds(), body);
    // Ротация «на ходу»: если разрослись посреди сессии — начать заново.
    if (ftell(f) >= logdetail::kLogMaxBytes) {
        fclose(f);
        logdetail::file() = nullptr;
        remove(logdetail::kLogPathOld);
        rename(logdetail::kLogPath, logdetail::kLogPathOld);
        logdetail::file() = fopen(logdetail::kLogPath, "a");
        if (logdetail::file()) setvbuf(logdetail::file(), nullptr, _IOLBF, 4096);
    }
}
