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
// Единственный канал, пишущий в файл (помимо CRASH): статус-переходы фич
// с причиной («фича X: ошибка, стадия 5 = FPManager»). Зовётся кодом,
// который УЖЕ дедупит повторы (печатает только при смене состояния) — сам
// LogStatus не фильтрует поток.
void LogStatus(const char* format, ...) __attribute__((format(printf, 1, 2)));
// Мёртвый кадровый канал: ~100 старых вызовов по проекту компилируются и
// ничего не пишут (минимальный режим, см. logfile.cpp).
void LogLine(const char* format, ...) __attribute__((format(printf, 1, 2)));

// Короткая заметка «что именно делаем» для crash_handler: stage отвечает «в
// какой фазе кадра», note — «на каком объекте». Без неё падение внутри фазы
// нечем локализовать: в логе 11:54:21 край дал stage=menu(8), а menu — это
// ещё и hud-пилюля со сканом маркеров, и по стадии не понять, где именно.
// Пишет только render-поток; обработчик сигнала читает без мьютекса.
void crash_note(const char* format, ...) __attribute__((format(printf, 1, 2)));
