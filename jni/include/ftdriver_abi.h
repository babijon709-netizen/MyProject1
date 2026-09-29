#pragma once

#include <stdint.h>
#include <sys/ioctl.h>

// User/kernel ABI for the Kernel transport. Keep this header identical in the
// driver-side shim. All addresses are target-process virtual addresses except
// buffer, which is a pointer in the calling client process.
struct FTDriverMemoryRequest {
    int32_t pid;
    uint32_t flags;
    uint64_t address;
    uint64_t buffer;
    uint64_t size;
};

#define FTDRIVER_IOCTL_ATTACH _IOW('F', 0x00, int32_t)
#define FTDRIVER_IOCTL_READ   _IOWR('F', 0x01, FTDriverMemoryRequest)
#define FTDRIVER_IOCTL_WRITE  _IOWR('F', 0x02, FTDriverMemoryRequest)
#define FTDRIVER_IOCTL_DETACH _IOW('F', 0x03, int32_t)
