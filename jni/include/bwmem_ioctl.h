#pragma once
// ============================================================================
// Протокол драйвера bw_mem (benzware memory bridge).
// Один источник правды для обеих сторон:
//   ядро:      kernel_driver/bw_mem.c  (Linux 5.10 arm64)
//   софт:      jni/src/main.cpp (--loadmod/--unloadmod, самотест)
//
// Устройство: /dev/<name>, name по умолчанию "bwmem"
// (имя можно поменять при загрузке: --loadmod bw.ko name=xyz).
//
// Все структуры выровнены так, чтобы раскладка совпадала в LP64 и ILP32.
// ============================================================================

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/ioctl.h>
#else
#include <stdint.h>
#define __u64 uint64_t
#define __u32 uint32_t
#endif

#define BW_MEM_DEV_DEFAULT "bwmem"   // имя misc-устройства
#define BW_MOD_NAME        "bw_mem"  // имя модуля (файл bw_mem.ko)
#define BW_IOCTL_MAGIC     0x77
#define BW_PROTO_VER       1u

// Состояние драйвера.
struct bw_ping {
    __u32 ver;      // BW_PROTO_VER
    __u32 kln_ok;   // 1 = kallsyms-резолв удался, чтение доступно
    __u64 kln_addr; // адрес kallsyms_lookup_name (для диагностики)
};

// Привязка целевого процесса (pid сохраняется до следующего ATTACH).
struct bw_attach {
    __u32 pid;
    __u32 pad;
};

// Чтение памяти. pid != 0 -> читать указанный pid, иначе привязанный ATTACH-ом.
// Возврат ioctl: >0 = сколько байт прочитано, <=0 = ошибка.
struct bw_read {
    __u64 addr;     // адрес в целевом процессе
    __u64 buf;      // пользовательский буфер для данных
    __u32 len;      // сколько читать (1..1 МБ)
    __u32 pid;      // 0 = использовать ATTACH-нутый pid
};

#define BW_IOCTL_PING   _IOR (BW_IOCTL_MAGIC, 1, struct bw_ping)
#define BW_IOCTL_ATTACH _IOW (BW_IOCTL_MAGIC, 2, struct bw_attach)
#define BW_IOCTL_READ   _IOWR(BW_IOCTL_MAGIC, 3, struct bw_read)
