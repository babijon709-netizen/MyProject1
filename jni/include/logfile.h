#pragma once
// Диагностический лог в загрузки (/storage/emulated/0/Download/xvcen.log,
// запасной путь — /storage/emulated/0/benzware/xvcen.log). Строки копятся в
// памяти и скидываются пачкой раз в секунду, при записи файл открывается и
// сразу закрывается — долгоживущих fd нет (KERNEL.md §5).

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

void log_open();
void log_close();
void log_stage(int stage);
void log_frame_beat(unsigned long frame);
void log_watchdog_start();
void log_line(const char* fmt, ...);

#define LogOpen()          log_open()
#define LogClose()         log_close()
#define LogStage(s)        log_stage(s)
#define LogFrameBeat(f)    log_frame_beat(f)
#define LogWatchdogStart() log_watchdog_start()
#define LogLine(...)       log_line(__VA_ARGS__)
