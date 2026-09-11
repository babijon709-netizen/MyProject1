#pragma once
// ============================================================================
//  driver.h — режимы доступа к памяти игры: NONKERNEL / KERNEL.
//
//  KERNEL    — чтение/запись ТОЛЬКО прямыми syscalls process_vm_readv/
//              writev: копирование между адресными пространствами выполняет
//              само ядро, без файлов /proc/<pid>/mem и без запасных путей.
//              Работает от root; ядрровой FT-драйвер прошивается вручную
//              (см. DRIVER_MODE.md) и снимает ограничения на уровне ядра.
//
//  NONKERNEL — прежнее поведение: process_vm, при отказе — /proc/<pid>/mem.
//
//  Порядок использования:
//    driver::set_mode(...)      — выбор режима (из стартового окна);
//    driver::readv/writev(...)  — весь обмен с памятью игры идёт только
//                                 через эти функции.
// ============================================================================
#include <sys/types.h>
#include <sys/uio.h>
#include <cstddef>
#include <cstdint>

namespace driver {

// ---- Режим доступа ---------------------------------------------------------
enum class Mode : int {
    NonKernel = 0,   // process_vm_* + запасной /proc/<pid>/mem
    Kernel    = 1,   // только process_vm_* напрямую через ядро
};

// ---- Режим -----------------------------------------------------------------
void        set_mode(Mode m);
Mode        mode();
const char* mode_name();          // "NONKERNEL" / "KERNEL"
const char* kernel_version();     // uname().release телефона

// ---- Root -------------------------------------------------------------------
// Если софт запущен не от root и есть su — перезапускает себя через su
// (root-копия продолжает работу, текущий процесс завершается). Вызывать
// ДО создания оверлея. Возвращает false, если root не дали (продолжаем
// как есть — память игры, скорее всего, будет недоступна).
bool        try_escalate_root(int argc, char* argv[]);

// ---- Доступ к памяти (единственная точка входа для game.cpp) ---------------
// iovec-семантика process_vm_readv/writev. KERNEL — только прямые syscalls
// (без запасных путей); NONKERNEL — process_vm, при отказе /proc/<pid>/mem.
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

// Счётчики чтений/записей по путям (для лога).
struct Stats {
    uint64_t pv_reads, pv_writes;      // process_vm_* (успешные)
    uint64_t mem_reads, mem_writes;    // /proc/<pid>/mem (успешные)
    uint64_t failed_reads, failed_writes;
};
Stats stats();

} // namespace driver
