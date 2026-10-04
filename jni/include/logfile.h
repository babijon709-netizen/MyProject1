#pragma once
// Лог выключен: вызовы остаются в коде как no-op, файл не пишется.

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
#define LogLine(...) ((void)0)
