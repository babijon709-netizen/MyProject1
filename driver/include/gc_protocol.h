#ifndef GC_PROTOCOL_H
#define GC_PROTOCOL_H

#include <linux/types.h>

#define GC_ABI_VERSION       ((__u32)0x00020001U)
#define GC_SOCKET_PROTOCOL   0x090419
#define GC_OP_PING           0x4700
#define GC_OP_ATTACH         0x4701
#define GC_OP_ALIVE          0x4702
#define GC_OP_READ           0x4703
#define GC_OP_WRITE          0x4704
#define GC_MAX_TRANSFER      (256U * 1024U)

struct gc_request {
    __u32 version;
    __u32 flags;
    __u64 address;
    __u64 buffer;
    __u64 size;
};

#endif
