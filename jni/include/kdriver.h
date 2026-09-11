#pragma once
#include <sys/types.h>
#include <stdint.h>
#include <stddef.h>

// Kernel driver abstraction for memory read/write.
// Supports:
//  - process_vm_readv/writev fallback (no driver)
//  - JiangNight/BeiFall style driver: /dev/JiangNight or socket hook ioctl 601/602/603 (0x801/0x802/0x803)
//  - FT driver 2.3.0 and variants: /dev/ft, /dev/mtg_bypass, etc.
// The driver is optional: if not present, all ops fall back to proc_vm.

enum KDrvBackend {
    KDRV_NONE = 0,
    KDRV_PROC_VM = 1,
    KDRV_DEV_FILE = 2,   // /dev/xxx with ioctl 0x801/0x802
    KDRV_SOCKET_HOOK = 3 // socket(AF_INET) with ioctl 601/602 (inet_ioctl hook)
};

bool kdrv_init(pid_t target_pid);
void kdrv_deinit();
bool kdrv_is_active(); // true if kernel driver is in use (not proc_vm)
KDrvBackend kdrv_backend();
const char* kdrv_backend_name();
const char* kdrv_device_path(); // valid when DEV_FILE

// Low-level ops (pid is the one from kdrv_init)
bool kdrv_read(uint64_t addr, void* out, size_t size);
bool kdrv_write(uint64_t addr, const void* in, size_t size);

// Bulk read: array of local/remote iovecs (like process_vm_readv)
// Returns true if all reads succeeded (or partial as proc_vm does, but we return count via out param if needed)
// For simplicity we implement as best-effort: stops on first failure like bulk_read_v original semantics (tail zeroed by caller)
void kdrv_bulk_read(int n, struct iovec* local, struct iovec* remote);

// Module base via driver (if supported), else 0
uint64_t kdrv_get_module_base(pid_t pid, const char* name);
uint64_t kdrv_get_module_base_current(const char* name); // uses init pid

// For diagnostics
int kdrv_fd();
