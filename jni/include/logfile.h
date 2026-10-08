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
    kStageHud,
    kStageMarkers,
    kStageFreecam,
    kStageFrameEnd,
    kStageCount
};

// The primary destination is /storage/emulated/0/Download/benzware.log.
// A private shared-storage fallback is used only when Download is unavailable.
void LogOpen();
void LogClose();
void LogStage(int stage);
void LogFrameBeat(unsigned long frame);
void LogWatchdogStart();
void LogLine(const char* format, ...) __attribute__((format(printf, 1, 2)));

// Короткая заметка «что именно делаем» для crash_handler: stage отвечает «в
// какой фазе кадра», note — «на каком объекте». Без неё падение внутри фазы
// нечем локализовать: в логе 11:54:21 край дал stage=menu(8), а menu — это
// ещё и hud-пилюля со сканом маркеров, и по стадии не понять, где именно.
// Пишет только render-поток; обработчик сигнала читает без мьютекса.
void crash_note(const char* format, ...) __attribute__((format(printf, 1, 2)));
