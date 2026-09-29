/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _UAPI_RWMEM_H
#define _UAPI_RWMEM_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define RWMEM_DEVICE_NAME "rwmem"
#define RWMEM_API_VERSION 1U
#define RWMEM_IOCTL_MAGIC 100

/* These three commands retain the ri-char/rwMem ABI. */
#define RWMEM_IOCTL_MAPS_COUNT \
	_IOW(RWMEM_IOCTL_MAGIC, 0, size_t)
#define RWMEM_IOCTL_MAPS_LIST \
	_IOWR(RWMEM_IOCTL_MAGIC, 1, char *)
#define RWMEM_IOCTL_CHECK_PROCESS_ADDR_PHY \
	_IOWR(RWMEM_IOCTL_MAGIC, 2, char *)

/* Extension used only to identify this maintained read/write-only variant. */
#define RWMEM_IOCTL_GET_API_VERSION \
	_IOR(RWMEM_IOCTL_MAGIC, 6, __u32)

/*
 * read(2)/write(2) wire header. The VFS count is the payload byte count.
 * For read(2), callers must provide at least 17 accessible bytes even when
 * count is smaller. For write(2), payload begins at user_buffer + 17.
 */
struct rwmem_request_header {
	__u64 pid;
	__u64 address;
	__u8 force;
} __attribute__((packed));

#endif /* _UAPI_RWMEM_H */
