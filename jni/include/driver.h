#pragma once
// ============================================================================
//  driver.h — режимы доступа к памяти игры: NONKERNEL / KERNEL.
//
//  NONKERNEL — текущее поведение: прямые syscalls process_vm_readv/writev
//              (работает там, где процессу уже разрешён ptrace-доступ).
//
//  KERNEL    — работа «от ядра» телефона: перед подключением к игре
//              запускается FT-драйвер (FTDriver.zip, скрипт под конкретную
//              версию ядра). Драйвер патчит ядро, после чего чтение/запись
//              памяти игры обслуживаются на уровне ядра: тот же syscall
//              проходит без ptrace-ограничений, а недоступный путь
//              автоматически заменяется на /proc/<pid>/mem (pread/pwrite).
//
//  Порядок использования:
//    driver::set_mode(...)            — выбор режима (из стартового окна);
//    driver::start_kernel_driver(pkg) — поднять FT-драйвер (KERNEL);
//    driver::readv/writev(...)        — весь обмен с памятью игры идёт
//                                       только через эти функции.
// ============================================================================
#include <sys/types.h>
#include <sys/uio.h>
#include <cstddef>
#include <cstdint>

namespace driver {

// ---- Режим доступа ---------------------------------------------------------
enum class Mode : int {
    NonKernel = 0,   // process_vm_* напрямую (как раньше)
    Kernel    = 1,   // через FT-драйвер ядра
};

// ---- Состояние загрузки ядрового драйвера ----------------------------------
enum class KernelState : int {
    Idle       = 0,  // режим не выбран / драйвер не запускался
    Detecting  = 1,  // определяем версию ядра и подбираем скрипт
    NoScript   = 2,  // под ядро телефона нет скрипта в FTDriver
    NoRoot     = 3,  // нет su — драйвер не запустить
    Launching  = 4,  // скрипт драйвера запущен через su
    Waiting    = 5,  // ждём, пока память игры станет доступной
    Ready      = 6,  // драйвер поднялся (память читается)
    Failed     = 7,  // не поднялся (таймаут / su отказал)
};

// ---- Режим -----------------------------------------------------------------
void        set_mode(Mode m);
Mode        mode();
const char* mode_name();          // "NONKERNEL" / "KERNEL"

// ---- Ядровой драйвер -------------------------------------------------------
// Подбирает скрипт под uname() телефона, запускает его через su и в фоне
// ждёт готовности (память игры <pkg> начинает читаться). Неблокирующий:
// статус смотреть в kernel_state(). Повторный вызов игнорируется, пока
// драйвер в процессе загрузки.
bool        start_kernel_driver(const char* game_package);
KernelState kernel_state();
const char* kernel_state_text();          // человеческое описание состояния
const char* kernel_version();             // uname().release телефона
const char* driver_script();              // имя выбранного скрипта ("5.10.sh")
const char* su_path();                    // найденный su ("" если нет)
bool        kernel_verified();            // память игры проверена чтением?
const char* last_error();                 // последняя ошибка ("" если нет)
// Последние строки вывода скрипта драйвера (ftdrv.log) для экрана загрузки.
const char* driver_log_tail();

// ---- Root -------------------------------------------------------------------
// Если софт запущен не от root и есть su — перезапускает себя через su
// (root-копия продолжает работу, текущий процесс завершается). Вызывать
// ДО создания оверлея. Возвращает false, если root не дали (продолжаем
// как есть — память игры, скорее всего, будет недоступна).
bool        try_escalate_root(int argc, char* argv[]);

// ---- Доступ к памяти (единственная точка входа для game.cpp) ---------------
// iovec-семантика process_vm_readv/writev. В KERNEL-режиме при сбое
// process_vm автоматически используется /proc/<pid>/mem.
ssize_t     readv (pid_t pid,
                   const struct iovec* local_iov, unsigned long liovcnt,
                   const struct iovec* remote_iov, unsigned long riovcnt,
                   unsigned long flags);
ssize_t     writev(pid_t pid,
                   const struct iovec* local_iov, unsigned long liovcnt,
                   const struct iovec* remote_iov, unsigned long riovcnt,
                   unsigned long flags);

// Утилиты поверх iovec-интерфейса (по одному диапазону за вызов).
bool        read_process_memory (pid_t pid, uint64_t addr, void* out, size_t len);
bool        write_process_memory(pid_t pid, uint64_t addr, const void* in, size_t len);

// Диагностика для меню: сколько чтений/записей прошло через каждый путь.
struct Stats {
    uint64_t pv_reads, pv_writes;      // process_vm_* (успешные)
    uint64_t mem_reads, mem_writes;    // /proc/<pid>/mem (успешные)
    uint64_t failed_reads, failed_writes;
};
Stats stats();

} // namespace driver
