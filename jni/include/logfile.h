#pragma once
// Диагностика на устройстве отключена: не открывать файлы и не создавать лог.
// Сигнатуры оставлены, чтобы проверки формата и существующие точки вызова
// продолжали собираться без вмешательства в логику резолвера/аима/оружия.

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

inline void LogOpen() {}
inline void LogClose() {}
inline void LogStage(int) {}
inline void LogFrameBeat(unsigned long) {}
inline void LogWatchdogStart() {}
inline void LogLine(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
inline void LogLine(const char*, ...) {}
