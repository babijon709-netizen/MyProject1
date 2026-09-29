#pragma once

#include <stdint.h>
#include <sys/ioctl.h>

// PROVISIONAL ADAPTER CONTRACT — NOT THE ABI OF THE SUPPLIED FTDriver
// PAYLOADS. The payloads are opaque and provide no public device/ioctl
// definition. Keep this header only for a separately supplied driver-side shim;
// replace it when the real endpoint and protocol for the selected kernel
// payload are confirmed. Do not present these values as native FTDriver ABI.
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
