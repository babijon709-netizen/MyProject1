#pragma once

#include <cstdarg>

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

// The primary destination is /storage/emulated/0/Download/xvcen.log.
// A private shared-storage fallback is used only when Download is unavailable.
void LogOpen();
void LogClose();
void LogStage(int stage);
void LogFrameBeat(unsigned long frame);
void LogWatchdogStart();
void LogLine(const char* format, ...) __attribute__((format(printf, 1, 2)));
