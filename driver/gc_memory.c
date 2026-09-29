// SPDX-License-Identifier: GPL-2.0
/*
 * Android arm64 process-memory transport derived from the communication model
 * of gongchuang1089/android_kerneldriver_gongchuang (GPL-2.0).
 *
 * It intentionally exposes no character device. A free protocol family is
 * selected at load time and authenticated by the private socket protocol plus
 * an ABI ping. Only uid 0 may create or operate a session.
 */
#include <linux/cred.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/sched/mm.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <net/sock.h>

#include "include/gc_protocol.h"

#ifndef GC_HIDE_SELF
#define GC_HIDE_SELF 1
#endif

struct gc_sock {
    struct sock sk;
    struct mutex lock;
    pid_t pid;
};

static struct proto gc_proto;
static struct proto_ops gc_proto_ops;
static struct net_proto_family gc_family_ops;
static int gc_family = -1;

static inline struct gc_sock *gc_session(struct socket *socket)
{
    return container_of(socket->sk, struct gc_sock, sk);
}

static bool gc_is_root(void)
{
    return uid_eq(current_euid(), GLOBAL_ROOT_UID);
}

static struct task_struct *gc_task_get(pid_t pid)
{
    struct pid *number;
    struct task_struct *task;

    number = find_get_pid(pid);
    if (!number)
        return NULL;
    task = get_pid_task(number, PIDTYPE_PID);
    put_pid(number);
    return task;
}

static bool gc_pid_alive(pid_t pid)
{
    struct task_struct *task = gc_task_get(pid);
    bool alive = false;

    if (task) {
        alive = pid_alive(task) && task->mm != NULL;
        put_task_struct(task);
    }
    return alive;
}

static int gc_copy_memory(struct gc_sock *session, bool write,
                          const struct gc_request *request)
{
    struct task_struct *task;
    void *buffer;
    size_t done = 0;
    int result = 0;

    if (request->version != GC_ABI_VERSION || request->flags != 0)
        return -EPROTO;
    if (!request->address || !request->buffer || !request->size ||
        request->size > GC_MAX_TRANSFER)
        return -EINVAL;
    if (session->pid <= 0)
        return -ENOTCONN;

    task = gc_task_get(session->pid);
    if (!task)
        return -ESRCH;

    buffer = kmalloc(min_t(size_t, request->size, 64U * 1024U), GFP_KERNEL);
    if (!buffer) {
        put_task_struct(task);
        return -ENOMEM;
    }

    while (done < request->size) {
        const size_t amount = min_t(size_t, request->size - done, 64U * 1024U);
        const unsigned long remote = (unsigned long)(request->address + done);
        void __user *local = u64_to_user_ptr(request->buffer + done);
        int copied;

        if (write && copy_from_user(buffer, local, amount)) {
            result = -EFAULT;
            break;
        }

        copied = access_process_vm(task, remote, buffer, (int)amount,
                                   write ? FOLL_WRITE : 0);
        if (copied != amount) {
            result = copied < 0 ? copied : -EIO;
            break;
        }

        if (!write && copy_to_user(local, buffer, amount)) {
            result = -EFAULT;
            break;
        }
        done += amount;
    }

    kfree(buffer);
    put_task_struct(task);
    return result;
}

static int gc_getsockopt(struct socket *socket, int level, int option,
                         char __user *value, int __user *length_user)
{
    struct gc_sock *session;
    struct gc_request request;
    int length;
    int result = 0;

    if (!gc_is_root())
        return -EACCES;
    if (!socket || !socket->sk)
        return -EINVAL;
    session = gc_session(socket);

    if (option == GC_OP_PING) {
        __u32 version = GC_ABI_VERSION;
        int output_length = sizeof(version);
        if (copy_to_user(value, &version, sizeof(version)) ||
            copy_to_user(length_user, &output_length, sizeof(output_length)))
            return -EFAULT;
        return 0;
    }

    mutex_lock(&session->lock);
    switch (option) {
    case GC_OP_ATTACH:
        if (level <= 0 || !gc_pid_alive((pid_t)level)) {
            result = -ESRCH;
            break;
        }
        session->pid = (pid_t)level;
        if (value) {
            int pid = session->pid;
            int output_length = sizeof(pid);
            if (copy_to_user(value, &pid, sizeof(pid)) ||
                copy_to_user(length_user, &output_length, sizeof(output_length)))
                result = -EFAULT;
        }
        break;

    case GC_OP_ALIVE: {
        int alive = gc_pid_alive(session->pid) ? 1 : 0;
        int output_length = sizeof(alive);
        if (copy_to_user(value, &alive, sizeof(alive)) ||
            copy_to_user(length_user, &output_length, sizeof(output_length)))
            result = -EFAULT;
        break;
    }

    case GC_OP_READ:
    case GC_OP_WRITE:
        if (copy_from_user(&length, length_user, sizeof(length)) ||
            length < sizeof(request) ||
            copy_from_user(&request, value, sizeof(request))) {
            result = -EFAULT;
            break;
        }
        result = gc_copy_memory(session, option == GC_OP_WRITE, &request);
        break;

    default:
        result = -ENOPROTOOPT;
        break;
    }
    mutex_unlock(&session->lock);
    return result;
}

static int gc_release(struct socket *socket)
{
    struct sock *sk = socket->sk;

    if (!sk)
        return 0;
    sock_orphan(sk);
    socket->sk = NULL;
    sock_put(sk);
    return 0;
}

static int gc_setsockopt(struct socket *socket, int level, int option,
                         sockptr_t value, unsigned int length)
{
    return -ENOPROTOOPT;
}

static __poll_t gc_poll(struct file *file, struct socket *socket,
                        struct poll_table_struct *wait)
{
    return 0;
}

static int gc_ioctl(struct socket *socket, unsigned int command,
                    unsigned long argument)
{
    return -ENOIOCTLCMD;
}

static int gc_sendmsg(struct socket *socket, struct msghdr *message,
                      size_t length)
{
    return -EOPNOTSUPP;
}

static int gc_mmap(struct file *file, struct socket *socket,
                   struct vm_area_struct *area)
{
    return -ENODEV;
}

static int gc_create(struct net *net, struct socket *socket,
                     int protocol, int kernel)
{
    struct sock *sk;
    struct gc_sock *session;

    if (!gc_is_root())
        return -EACCES;
    if (socket->type != SOCK_RAW || protocol != GC_SOCKET_PROTOCOL)
        return -EPROTONOSUPPORT;

    sk = sk_alloc(net, gc_family, GFP_KERNEL, &gc_proto, kernel);
    if (!sk)
        return -ENOMEM;

    socket->state = SS_UNCONNECTED;
    socket->ops = &gc_proto_ops;
    sock_init_data(socket, sk);
    session = container_of(sk, struct gc_sock, sk);
    mutex_init(&session->lock);
    session->pid = -1;
    return 0;
}

static struct proto gc_proto = {
    .name = "gc_mem",
    .owner = THIS_MODULE,
    .obj_size = sizeof(struct gc_sock),
};

static struct proto_ops gc_proto_ops = {
    .family = PF_UNSPEC,
    .owner = THIS_MODULE,
    .release = gc_release,
    .bind = sock_no_bind,
    .connect = sock_no_connect,
    .socketpair = sock_no_socketpair,
    .accept = sock_no_accept,
    .getname = sock_no_getname,
    .poll = gc_poll,
    .ioctl = gc_ioctl,
    .listen = sock_no_listen,
    .shutdown = sock_no_shutdown,
    .setsockopt = gc_setsockopt,
    .getsockopt = gc_getsockopt,
    .sendmsg = gc_sendmsg,
    .recvmsg = sock_no_recvmsg,
    .mmap = gc_mmap,
};

static struct net_proto_family gc_family_ops = {
    .family = PF_UNSPEC,
    .create = gc_create,
    .owner = THIS_MODULE,
};

static int gc_register_family(void)
{
    int family;
    int error = -EAFNOSUPPORT;

    for (family = AF_DECnet; family < NPROTO; ++family) {
        gc_family_ops.family = family;
        error = sock_register(&gc_family_ops);
        if (!error) {
            gc_family = family;
            gc_proto_ops.family = family;
            return 0;
        }
    }
    return error;
}

static int __init gc_init(void)
{
    int error = proto_register(&gc_proto, 1);
    if (error)
        return error;

    error = gc_register_family();
    if (error) {
        proto_unregister(&gc_proto);
        return error;
    }

#if GC_HIDE_SELF
    /* No /dev node exists; remove the remaining standard module listings. */
    list_del_init(&THIS_MODULE->list);
    kobject_del(&THIS_MODULE->mkobj.kobj);
#endif
    return 0;
}

static void __exit gc_exit(void)
{
    if (gc_family >= 0)
        sock_unregister(gc_family);
    proto_unregister(&gc_proto);
}

module_init(gc_init);
module_exit(gc_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("MyProject1; based on gongchuang1089");
MODULE_DESCRIPTION("Android process-memory socket transport");
MODULE_VERSION("2.1");
