#include "kdriver.h"
#include <sys/uio.h>
#include <sys/syscall.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <atomic>
#include <mutex>

static ssize_t proc_vm_readv(pid_t pid, const struct iovec* local_iov, unsigned long liovcnt, const struct iovec* remote_iov, unsigned long riovcnt, unsigned long flags) {
    return syscall(__NR_process_vm_readv, pid, local_iov, liovcnt, remote_iov, riovcnt, flags);
}
static ssize_t proc_vm_writev(pid_t pid, const struct iovec* local_iov, unsigned long liovcnt, const struct iovec* remote_iov, unsigned long riovcnt, unsigned long flags) {
    return syscall(__NR_process_vm_writev, pid, local_iov, liovcnt, remote_iov, riovcnt, flags);
}

struct CopyMemory {
    pid_t pid;
    uintptr_t addr;
    void* buffer;
    size_t size;
};
struct ModuleBase {
    pid_t pid;
    char* name;
    uintptr_t base;
};

enum {
    OP_INIT_KEY   = 0x800,
    OP_READ_MEM   = 0x801,
    OP_WRITE_MEM  = 0x802,
    OP_MODULE_BASE = 0x803,
    // BeiFall socket hook uses 601/602/603 (same as above -0x200)
    OP_READ_MEM_SOCK = 601,
    OP_WRITE_MEM_SOCK = 602,
    OP_MODULE_BASE_SOCK = 603,
};

static pid_t g_kdrv_pid = -1;
static int g_kdrv_fd = -1;
static KDrvBackend g_kdrv_backend = KDRV_NONE;
static char g_kdrv_dev_path[128] = {};
static std::mutex g_kdrv_mutex;
static bool g_kdrv_has_module_base = false;
static int g_kdrv_read_cmd = OP_READ_MEM;
static int g_kdrv_write_cmd = OP_WRITE_MEM;
static int g_kdrv_base_cmd = OP_MODULE_BASE;

static bool try_ioctl_read(int fd, int cmd, pid_t pid, uintptr_t addr, void* out, size_t size) {
    CopyMemory cm{};
    cm.pid = pid;
    cm.addr = addr;
    cm.buffer = out;
    cm.size = size;
    // Some drivers expect buffer to be kernel pointer? But JiangNight uses user pointer directly.
    // We call ioctl and check return.
    int ret = ioctl(fd, cmd, &cm);
    return ret == 0;
}

static bool try_ioctl_write(int fd, int cmd, pid_t pid, uintptr_t addr, const void* in, size_t size) {
    // For write, driver reads from user buffer (cm.buffer)
    CopyMemory cm{};
    cm.pid = pid;
    cm.addr = addr;
    cm.buffer = const_cast<void*>(in);
    cm.size = size;
    int ret = ioctl(fd, cmd, &cm);
    return ret == 0;
}

static bool test_driver_fd(int fd, int read_cmd, int write_cmd, pid_t self_pid) {
    // Test read self memory
    volatile int secret = 0x5A3C7E11;
    int out = 0;
    if (!try_ioctl_read(fd, read_cmd, self_pid, (uintptr_t)&secret, &out, sizeof(out))) return false;
    if (out != secret) return false;
    // Test write self memory
    volatile int target = 0;
    int new_val = 0x12345678;
    if (!try_ioctl_write(fd, write_cmd, self_pid, (uintptr_t)&target, &new_val, sizeof(new_val))) return false;
    if (target != new_val) return false;
    return true;
}

bool kdrv_init(pid_t target_pid) {
    std::lock_guard<std::mutex> lock(g_kdrv_mutex);
    if (g_kdrv_fd >= 0) {
        close(g_kdrv_fd);
        g_kdrv_fd = -1;
    }
    g_kdrv_pid = target_pid;
    g_kdrv_backend = KDRV_NONE;
    g_kdrv_dev_path[0] = '\0';
    g_kdrv_has_module_base = false;
    g_kdrv_read_cmd = OP_READ_MEM;
    g_kdrv_write_cmd = OP_WRITE_MEM;
    g_kdrv_base_cmd = OP_MODULE_BASE;

    pid_t self_pid = getpid();

    // --- Try device file backends ---
    const char* dev_paths[] = {
        "/dev/JiangNight",
        "/dev/jiangnight",
        "/dev/BeiFall",
        "/dev/beifall",
        "/dev/Bei_Fall",
        "/dev/ft",
        "/dev/ft_device",
        "/dev/ft_device_2",
        "/dev/ftdrv",
        "/dev/ft_dev",
        "/dev/FT",
        "/dev/FT_dev",
        "/dev/mtg_bypass",
        "/dev/mtg",
        "/dev/mtk",
        "/dev/kdriver",
        "/dev/kmem",
        "/dev/kmem_driver",
        "/dev/mem_driver",
        "/dev/driver",
        "/dev/ice_binder",
        "/dev/xxx",
        "/dev/xyz",
        "/dev/ksg",
        "/dev/kgs",
        "/dev/vivo",
        "/dev/mirage",
        "/dev/5s",
        "/dev/5s_driver",
        "/dev/capcom",
        "/dev/cheat",
        "/dev/game",
        "/dev/gm",
        "/dev/hack",
        "/dev/kernel",
        "/dev/khack",
        "/dev/rw",
        "/dev/rw_mem",
        "/dev/memory",
        "/dev/mem_rw",
        "/dev/proc_mem",
        nullptr
    };

    // For each device, try open and test both 0x801 and 601 command sets
    for (int i = 0; dev_paths[i]; ++i) {
        int fd = open(dev_paths[i], O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        // Try JiangNight style 0x801/0x802
        if (test_driver_fd(fd, OP_READ_MEM, OP_WRITE_MEM, self_pid)) {
            g_kdrv_fd = fd;
            g_kdrv_backend = KDRV_DEV_FILE;
            strncpy(g_kdrv_dev_path, dev_paths[i], sizeof(g_kdrv_dev_path)-1);
            g_kdrv_read_cmd = OP_READ_MEM;
            g_kdrv_write_cmd = OP_WRITE_MEM;
            g_kdrv_base_cmd = OP_MODULE_BASE;
            g_kdrv_has_module_base = true;
            return true;
        }
        // Try socket hook style 601/602
        if (test_driver_fd(fd, OP_READ_MEM_SOCK, OP_WRITE_MEM_SOCK, self_pid)) {
            g_kdrv_fd = fd;
            g_kdrv_backend = KDRV_DEV_FILE;
            strncpy(g_kdrv_dev_path, dev_paths[i], sizeof(g_kdrv_dev_path)-1);
            g_kdrv_read_cmd = OP_READ_MEM_SOCK;
            g_kdrv_write_cmd = OP_WRITE_MEM_SOCK;
            g_kdrv_base_cmd = OP_MODULE_BASE_SOCK;
            g_kdrv_has_module_base = true;
            return true;
        }
        // Try FT variant 0x1001/0x1002 (some FT drivers)
        if (test_driver_fd(fd, 0x1001, 0x1002, self_pid)) {
            g_kdrv_fd = fd;
            g_kdrv_backend = KDRV_DEV_FILE;
            strncpy(g_kdrv_dev_path, dev_paths[i], sizeof(g_kdrv_dev_path)-1);
            g_kdrv_read_cmd = 0x1001;
            g_kdrv_write_cmd = 0x1002;
            g_kdrv_base_cmd = 0x1003;
            g_kdrv_has_module_base = false;
            return true;
        }
        close(fd);
    }

    // --- Try socket hook backend (inet_ioctl hook) ---
    // This driver hooks inet_ioctl, so any socket fd works.
    // Open an inet socket and try ioctl 601.
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd >= 0) {
        if (test_driver_fd(sock_fd, OP_READ_MEM_SOCK, OP_WRITE_MEM_SOCK, self_pid)) {
            g_kdrv_fd = sock_fd;
            g_kdrv_backend = KDRV_SOCKET_HOOK;
            strncpy(g_kdrv_dev_path, "socket:inet_ioctl", sizeof(g_kdrv_dev_path)-1);
            g_kdrv_read_cmd = OP_READ_MEM_SOCK;
            g_kdrv_write_cmd = OP_WRITE_MEM_SOCK;
            g_kdrv_base_cmd = OP_MODULE_BASE_SOCK;
            g_kdrv_has_module_base = true;
            return true;
        }
        // Try 0x801 on socket as well (some hooks check both)
        if (test_driver_fd(sock_fd, OP_READ_MEM, OP_WRITE_MEM, self_pid)) {
            g_kdrv_fd = sock_fd;
            g_kdrv_backend = KDRV_SOCKET_HOOK;
            strncpy(g_kdrv_dev_path, "socket:inet_ioctl", sizeof(g_kdrv_dev_path)-1);
            g_kdrv_read_cmd = OP_READ_MEM;
            g_kdrv_write_cmd = OP_WRITE_MEM;
            g_kdrv_base_cmd = OP_MODULE_BASE;
            g_kdrv_has_module_base = true;
            return true;
        }
        close(sock_fd);
    }

    // --- Fallback to proc_vm ---
    g_kdrv_backend = KDRV_PROC_VM;
    g_kdrv_fd = -1;
    return true;
}

void kdrv_deinit() {
    std::lock_guard<std::mutex> lock(g_kdrv_mutex);
    if (g_kdrv_fd >= 0) {
        close(g_kdrv_fd);
        g_kdrv_fd = -1;
    }
    g_kdrv_backend = KDRV_NONE;
    g_kdrv_pid = -1;
}

bool kdrv_is_active() {
    return g_kdrv_backend == KDRV_DEV_FILE || g_kdrv_backend == KDRV_SOCKET_HOOK;
}

KDrvBackend kdrv_backend() {
    return g_kdrv_backend;
}

const char* kdrv_backend_name() {
    switch (g_kdrv_backend) {
        case KDRV_NONE: return "none";
        case KDRV_PROC_VM: return "proc_vm";
        case KDRV_DEV_FILE: return "kernel:dev_file";
        case KDRV_SOCKET_HOOK: return "kernel:socket_hook";
        default: return "unknown";
    }
}

const char* kdrv_device_path() {
    return g_kdrv_dev_path;
}

int kdrv_fd() {
    return g_kdrv_fd;
}

bool kdrv_read(uint64_t addr, void* out, size_t size) {
    if (!addr || !out || !size) return false;
    pid_t pid = g_kdrv_pid;
    if (pid <= 0) return false;
    if (g_kdrv_backend == KDRV_DEV_FILE || g_kdrv_backend == KDRV_SOCKET_HOOK) {
        // Driver path
        CopyMemory cm{};
        cm.pid = pid;
        cm.addr = addr;
        cm.buffer = out;
        cm.size = size;
        int ret = ioctl(g_kdrv_fd, g_kdrv_read_cmd, &cm);
        return ret == 0;
    } else {
        // proc_vm fallback
        struct iovec local = {out, size};
        struct iovec remote = {(void*)addr, size};
        return proc_vm_readv(pid, &local, 1, &remote, 1, 0) == (ssize_t)size;
    }
}

bool kdrv_write(uint64_t addr, const void* in, size_t size) {
    if (!addr || !in || !size) return false;
    pid_t pid = g_kdrv_pid;
    if (pid <= 0) return false;
    if (g_kdrv_backend == KDRV_DEV_FILE || g_kdrv_backend == KDRV_SOCKET_HOOK) {
        CopyMemory cm{};
        cm.pid = pid;
        cm.addr = addr;
        cm.buffer = const_cast<void*>(in);
        cm.size = size;
        int ret = ioctl(g_kdrv_fd, g_kdrv_write_cmd, &cm);
        return ret == 0;
    } else {
        struct iovec local = {(void*)in, size};
        struct iovec remote = {(void*)addr, size};
        return proc_vm_writev(pid, &local, 1, &remote, 1, 0) == (ssize_t)size;
    }
}

void kdrv_bulk_read(int n, struct iovec* local, struct iovec* remote) {
    if (n <= 0 || !local || !remote) return;
    if (g_kdrv_backend == KDRV_DEV_FILE || g_kdrv_backend == KDRV_SOCKET_HOOK) {
        // Driver doesn't support bulk, loop one by one.
        // Preserve original semantics: on failure, tail stays zeroed (caller pre-zeroed).
        for (int i = 0; i < n; ++i) {
            if (!kdrv_read((uint64_t)remote[i].iov_base, local[i].iov_base, remote[i].iov_len)) {
                // Stop on first fault, like proc_vm_readv does (partial)
                break;
            }
        }
    } else {
        // Use proc_vm with chunking
        pid_t pid = g_kdrv_pid;
        if (pid <= 0) return;
        int remaining = n;
        struct iovec* l = local;
        struct iovec* r = remote;
        while (remaining > 0) {
            int chunk = remaining > 512 ? 512 : remaining;
            (void)proc_vm_readv(pid, l, chunk, r, chunk, 0);
            l += chunk;
            r += chunk;
            remaining -= chunk;
        }
    }
}

uint64_t kdrv_get_module_base(pid_t pid, const char* name) {
    if (!name || !name[0]) return 0;
    if (pid <= 0) pid = g_kdrv_pid;
    if (pid <= 0) return 0;
    if (g_kdrv_backend == KDRV_DEV_FILE || g_kdrv_backend == KDRV_SOCKET_HOOK) {
        if (!g_kdrv_has_module_base) return 0;
        char buf[256];
        strncpy(buf, name, sizeof(buf)-1);
        buf[sizeof(buf)-1] = '\0';
        ModuleBase mb{};
        mb.pid = pid;
        mb.name = buf;
        mb.base = 0;
        int ret = ioctl(g_kdrv_fd, g_kdrv_base_cmd, &mb);
        if (ret == 0 && mb.base >= 0x10000) return mb.base;
    }
    return 0;
}

uint64_t kdrv_get_module_base_current(const char* name) {
    return kdrv_get_module_base(g_kdrv_pid, name);
}
