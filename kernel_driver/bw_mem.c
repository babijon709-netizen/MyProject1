// SPDX-License-Identifier: GPL-2.0
/*
 * bw_mem — мост чтения памяти для benzware (Linux 5.10, arm64).
 *
 * Интерфейс: misc-устройство /dev/<name> (по умолчанию /dev/bwmem).
 *   ioctl(BW_IOCTL_PING)   — версия и статус резолва ядерных символов
 *   ioctl(BW_IOCTL_ATTACH) — привязать целевой pid
 *   ioctl(BW_IOCTL_READ)   — прочитать память процесса (возврат = байты)
 *
 * access_process_vm и find_task_by_vpid НЕ экспортируются в модули с 5.x —
 * их адреса резолвятся через kallsyms_lookup_name, адрес которого берётся
 * kprobe-самозагрузкой (register_kprobe экспортируем). Если KPROBES выключен
 * или заблокирован — PING сообщит kln_ok=0, а READ будет возвращать -ENOSYS.
 *
 * Сборка: против linux-5.10.255 (arm64 defconfig) с kernel.org.
 * Загрузка на устройстве — finit_module(..., MODULE_INIT_IGNORE_MODVERSIONS |
 * MODULE_INIT_IGNORE_VERMAGIC), т.к. vermagic/CRC кастомного ядра не совпадают
 * со стоком. Импортируются только стабильно экспортируемые символы
 * (register_kprobe, misc_register, kmalloc, copy_*_user и т.п.).
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/kprobes.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/pid.h>
#include <linux/mm.h>
#include <linux/types.h>
#include <linux/ioctl.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/sizes.h>

#include <bwmem_ioctl.h>

/* ---- резолв неэкспортируемых ядерных функций --------------------------- */

static unsigned long (*kln_ptr)(const char *name);
static int (*p_access_process_vm)(struct task_struct *tsk, unsigned long addr,
                                  void *buf, int len, unsigned int gup_flags);
static struct task_struct *(*p_find_task_by_vpid)(pid_t nr);

static int bw_kln_bootstrap(void)
{
    struct kprobe kp = { .symbol_name = "kallsyms_lookup_name" };
    int ret;

    ret = register_kprobe(&kp);
    if (ret < 0) {
        pr_err("bwmem: kprobe bootstrap failed: %d "
               "(CONFIG_KPROBES off или заблокирован)\n", ret);
        return ret;
    }
    kln_ptr = (unsigned long (*)(const char *))kp.addr;
    unregister_kprobe(&kp);
    return 0;
}

static void bw_resolve_syms(void)
{
    if (!kln_ptr)
        return;
    p_access_process_vm  = (int (*)(struct task_struct *, unsigned long,
                                    void *, int, unsigned int))
                           kln_ptr("access_process_vm");
    p_find_task_by_vpid  = (struct task_struct * (*)(pid_t))
                           kln_ptr("find_task_by_vpid");
}

/* ---- ioctl ---------------------------------------------------------------- */

static u32 bw_pid;
static DEFINE_MUTEX(bw_lock);

static long bw_do_read(u32 pid, u64 addr, u64 ubuf, u32 len)
{
    struct task_struct *tsk;
    void *kbuf;
    int ret;

    if (!pid)
        return -ESRCH;
    if (len == 0 || len > SZ_1M)
        return -EINVAL;
    if (!p_access_process_vm || !p_find_task_by_vpid)
        return -ENOSYS;

    rcu_read_lock();
    tsk = p_find_task_by_vpid((pid_t)pid);
    if (tsk)
        get_task_struct(tsk);
    rcu_read_unlock();
    if (!tsk)
        return -ESRCH;

    kbuf = kmalloc(len, GFP_KERNEL);
    if (!kbuf) {
        put_task_struct(tsk);
        return -ENOMEM;
    }

    /* gup_flags = 0: без FOLL_WRITE — только чтение */
    ret = p_access_process_vm(tsk, (unsigned long)addr, kbuf, (int)len, 0);
    put_task_struct(tsk);

    if (ret > 0) {
        if (copy_to_user((void __user *)(unsigned long)ubuf, kbuf, ret))
            ret = -EFAULT;
    }
    kfree(kbuf);
    return ret; /* >0 = прочитано байт */
}

static long bw_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    void __user *uarg = (void __user *)arg;

    switch (cmd) {
    case BW_IOCTL_PING: {
        struct bw_ping p = {
            .ver     = BW_PROTO_VER,
            .kln_ok  = (kln_ptr && p_access_process_vm && p_find_task_by_vpid)
                       ? 1u : 0u,
            .kln_addr = kln_ptr ? (u64)(unsigned long)kln_ptr : 0,
        };
        return copy_to_user(uarg, &p, sizeof(p)) ? -EFAULT : 0;
    }
    case BW_IOCTL_ATTACH: {
        struct bw_attach a;
        if (copy_from_user(&a, uarg, sizeof(a)))
            return -EFAULT;
        if (!a.pid)
            return -EINVAL;
        mutex_lock(&bw_lock);
        bw_pid = a.pid;
        mutex_unlock(&bw_lock);
        return 0;
    }
    case BW_IOCTL_READ: {
        struct bw_read r;
        u32 pid;
        long ret;

        if (copy_from_user(&r, uarg, sizeof(r)))
            return -EFAULT;
        mutex_lock(&bw_lock);
        pid = r.pid ? r.pid : bw_pid;
        mutex_unlock(&bw_lock);
        ret = bw_do_read(pid, r.addr, r.buf, r.len);
        return ret;
    }
    default:
        return -ENOTTY;
    }
}

static const struct file_operations bw_fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = bw_ioctl,
#ifdef CONFIG_COMPAT
    .compat_ioctl   = bw_ioctl, /* раскладка структур одинакова в ILP32 */
#endif
};

static struct miscdevice bw_misc = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = BW_MEM_DEV_DEFAULT,
    .fops  = &bw_fops,
    .mode  = 0600, /* только root — софт и так работает от root */
};

/* ---- init/exit ----------------------------------------------------------- */

static char *bw_name = BW_MEM_DEV_DEFAULT;
module_param_named(name, bw_name, charp, 0444);
MODULE_PARM_DESC(name, "имя misc-устройства (по умолчанию bwmem)");

static int __init bw_init(void)
{
    int ret;

    bw_kln_bootstrap();
    bw_resolve_syms();

    pr_info("bwmem: kln=%pS access_process_vm=%pS find_task_by_vpid=%pS\n",
            kln_ptr, p_access_process_vm, p_find_task_by_vpid);
    if (kln_ptr && (!p_access_process_vm || !p_find_task_by_vpid))
        pr_err("bwmem: не все символы найдены — READ не будет работать\n");

    bw_misc.name = bw_name;
    ret = misc_register(&bw_misc);
    if (ret) {
        pr_err("bwmem: misc_register: %d\n", ret);
        return ret;
    }
    pr_info("bwmem: готов, устройство /dev/%s\n", bw_name);
    return 0;
}

static void __exit bw_exit(void)
{
    misc_deregister(&bw_misc);
    pr_info("bwmem: выгружен\n");
}

module_init(bw_init);
module_exit(bw_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("benzware memory bridge (process memory read via ioctl)");
MODULE_AUTHOR("benzware");
