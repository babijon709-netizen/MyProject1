// SPDX-License-Identifier: GPL-2.0
/*
 * rwmem - cross-process memory transport for Android/Linux.
 *
 * The userspace wire protocol and maps ioctls are compatible with
 * https://github.com/ri-char/rwMem (commit 6ea91bb).  Memory copying uses
 * access_process_vm() rather than walking PTEs and mapping physical RAM: this
 * correctly handles swapped/COW pages and avoids retaining unlocked PTE
 * pointers.  The module is intentionally limited to the read/write/maps API
 * used by this project; rwMem's breakpoint kernel patch is not required.
 */

#include <linux/cdev.h>
#include <linux/dcache.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/limits.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/pid.h>
#include <linux/sched/mm.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "rwmem_uapi.h"

#define RWMEM_MAX_MAP_NAME 4096U
#define RWMEM_MAX_MAP_BUFFER (64U * 1024U * 1024U)

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 8, 0)
#define mmap_read_lock(mm) down_read(&(mm)->mmap_sem)
#define mmap_read_unlock(mm) up_read(&(mm)->mmap_sem)
#endif

static dev_t rwmem_devno;
static struct cdev rwmem_cdev;
static struct class *rwmem_class;

static struct task_struct *rwmem_get_task(__u64 raw_pid)
{
	struct pid *pid;
	struct task_struct *task;

	if (!raw_pid || raw_pid > 0x7fffffffULL)
		return NULL;

	pid = find_get_pid((pid_t)raw_pid);
	if (!pid)
		return NULL;
	task = get_pid_task(pid, PIDTYPE_PID);
	put_pid(pid);
	return task;
}

static ssize_t rwmem_transfer(struct task_struct *task, __u64 address,
			      void __user *user, size_t count,
			      bool write, bool force)
{
	unsigned int gup_flags = write ? FOLL_WRITE : 0;
	char *bounce;
	size_t done = 0;

	if (!count)
		return 0;
	if (address > ULONG_MAX || count > ULONG_MAX - (unsigned long)address)
		return -EOVERFLOW;
	if (force)
		gup_flags |= FOLL_FORCE;

	bounce = (char *)__get_free_page(GFP_KERNEL);
	if (!bounce)
		return -ENOMEM;

	while (done < count) {
		size_t chunk = min_t(size_t, PAGE_SIZE, count - done);
		int copied;

		if (write && copy_from_user(bounce, (char __user *)user + done,
					    chunk)) {
			if (!done)
				done = (size_t)-EFAULT;
			break;
		}

		copied = access_process_vm(task, (unsigned long)address + done,
					   bounce, (int)chunk, gup_flags);
		if (copied <= 0) {
			if (!done)
				done = (size_t)-EFAULT;
			break;
		}

		if (!write && copy_to_user((char __user *)user + done, bounce,
						 (size_t)copied)) {
			if (!done)
				done = (size_t)-EFAULT;
			break;
		}

		done += (size_t)copied;
		if ((size_t)copied != chunk)
			break;
		cond_resched();
	}

	free_page((unsigned long)bounce);
	return (ssize_t)done;
}

static ssize_t rwmem_read(struct file *file, char __user *buffer, size_t count,
			  loff_t *position)
{
	struct rwmem_request_header request;
	struct task_struct *task;
	ssize_t result;

	(void)file;
	(void)position;
	if (!count)
		return 0;
	if (copy_from_user(&request, buffer, sizeof(request)))
		return -EFAULT;

	task = rwmem_get_task(request.pid);
	if (!task)
		return -ESRCH;
	result = rwmem_transfer(task, request.address, buffer, count, false,
				request.force != 0);
	put_task_struct(task);
	return result;
}

static ssize_t rwmem_write(struct file *file, const char __user *buffer,
			   size_t count, loff_t *position)
{
	struct rwmem_request_header request;
	struct task_struct *task;
	ssize_t result;

	(void)file;
	(void)position;
	if (!count)
		return 0;
	if (count > SIZE_MAX - sizeof(request))
		return -EOVERFLOW;
	if (copy_from_user(&request, buffer, sizeof(request)))
		return -EFAULT;

	task = rwmem_get_task(request.pid);
	if (!task)
		return -ESRCH;
	result = rwmem_transfer(task, request.address,
				(void __user *)(buffer + sizeof(request)), count, true,
				request.force != 0);
	put_task_struct(task);
	return result;
}

static long rwmem_maps_count(unsigned long argument)
{
	struct task_struct *task;
	struct mm_struct *mm;
	long count;

	task = rwmem_get_task((__u64)argument);
	if (!task)
		return -ESRCH;
	mm = get_task_mm(task);
	put_task_struct(task);
	if (!mm)
		return -EINVAL;

	mmap_read_lock(mm);
	count = (long)mm->map_count;
	mmap_read_unlock(mm);
	mmput(mm);
	return count;
}

struct rwmem_maps_request {
	__u64 pid;
	__u64 name_length;
	__u64 buffer_size;
};

static void rwmem_vma_name(struct vm_area_struct *vma, struct mm_struct *mm,
			   char *path_buffer, size_t path_buffer_size,
			   char *output, size_t output_size)
{
	char *path;

	if (!output_size)
		return;
	output[0] = '\0';
	if (vma->vm_file) {
		path = d_path(&vma->vm_file->f_path, path_buffer,
			      path_buffer_size);
		if (!IS_ERR(path))
			strscpy(output, path, output_size);
		return;
	}
	if (vma->vm_start <= mm->brk && vma->vm_end >= mm->start_brk)
		strscpy(output, "[heap]", output_size);
	else if (vma->vm_start <= mm->start_stack &&
		 vma->vm_end >= mm->start_stack)
		strscpy(output, "[stack]", output_size);
}

static int rwmem_emit_vma(struct vm_area_struct *vma, struct mm_struct *mm,
			  unsigned char **cursor, size_t *remaining,
			  size_t name_length, char *path_buffer,
			  unsigned char *entry)
{
	const size_t entry_size = 20 + name_length;
	__u64 start = (__u64)vma->vm_start;
	__u64 end = (__u64)vma->vm_end;

	if (*remaining < entry_size)
		return 1;

	memset(entry, 0, entry_size);
	memcpy(entry, &start, sizeof(start));
	memcpy(entry + 8, &end, sizeof(end));
	entry[16] = (vma->vm_flags & VM_READ) != 0;
	entry[17] = (vma->vm_flags & VM_WRITE) != 0;
	entry[18] = (vma->vm_flags & VM_EXEC) != 0;
	entry[19] = (vma->vm_flags & VM_MAYSHARE) != 0;
	rwmem_vma_name(vma, mm, path_buffer, PAGE_SIZE,
			(char *)entry + 20, name_length);

	memcpy(*cursor, entry, entry_size);
	*cursor += entry_size;
	*remaining -= entry_size;
	return 0;
}

static long rwmem_maps_list(unsigned long argument)
{
	struct rwmem_maps_request request;
	struct task_struct *task;
	struct mm_struct *mm;
	char __user *user = (char __user *)argument;
	unsigned char *output = NULL;
	unsigned char *cursor;
	char *path_buffer = NULL;
	unsigned char *entry = NULL;
	size_t remaining;
	size_t output_used;
	__u64 emitted = 0;
	int status = 0;

	if (copy_from_user(&request, user, sizeof(request)))
		return -EFAULT;
	if (!request.name_length || request.name_length > RWMEM_MAX_MAP_NAME ||
	    request.buffer_size < 8 ||
	    request.buffer_size > RWMEM_MAX_MAP_BUFFER)
		return -EINVAL;
	task = rwmem_get_task(request.pid);
	if (!task)
		return -ESRCH;
	mm = get_task_mm(task);
	put_task_struct(task);
	if (!mm)
		return -EINVAL;

	path_buffer = (char *)__get_free_page(GFP_KERNEL);
	entry = kzalloc(20 + (size_t)request.name_length, GFP_KERNEL);
	output = kvzalloc((size_t)request.buffer_size, GFP_KERNEL);
	if (!path_buffer || !entry || !output) {
		status = -ENOMEM;
		goto out;
	}

	cursor = output + 8;
	remaining = (size_t)request.buffer_size - 8;
	mmap_read_lock(mm);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
	{
		VMA_ITERATOR(vmi, mm, 0);
		struct vm_area_struct *vma;

		for_each_vma(vmi, vma) {
			status = rwmem_emit_vma(vma, mm, &cursor, &remaining,
						(size_t)request.name_length,
						path_buffer, entry);
			if (status)
				break;
			emitted++;
		}
	}
#else
	{
		struct vm_area_struct *vma;

		for (vma = mm->mmap; vma; vma = vma->vm_next) {
			status = rwmem_emit_vma(vma, mm, &cursor, &remaining,
						(size_t)request.name_length,
						path_buffer, entry);
			if (status)
				break;
			emitted++;
		}
	}
#endif
	mmap_read_unlock(mm);

	if (status >= 0) {
		memcpy(output, &emitted, sizeof(emitted));
		output_used = (size_t)(cursor - output);
		if (copy_to_user(user, output, output_used))
			status = -EFAULT;
	}

out:
	kvfree(output);
	kfree(entry);
	if (path_buffer)
		free_page((unsigned long)path_buffer);
	mmput(mm);
	return status;
}

static long rwmem_ioctl(struct file *file, unsigned int command,
			unsigned long argument)
{
	__u32 version = RWMEM_API_VERSION;

	(void)file;
	switch (command) {
	case RWMEM_IOCTL_MAPS_COUNT:
		return rwmem_maps_count(argument);
	case RWMEM_IOCTL_MAPS_LIST:
		return rwmem_maps_list(argument);
	case RWMEM_IOCTL_GET_API_VERSION:
		return copy_to_user((void __user *)argument, &version,
				    sizeof(version)) ? -EFAULT : 0;
	default:
		return -ENOTTY;
	}
}

static const struct file_operations rwmem_fops = {
	.owner = THIS_MODULE,
	.read = rwmem_read,
	.write = rwmem_write,
	.unlocked_ioctl = rwmem_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = rwmem_ioctl,
#endif
	.llseek = no_llseek,
};

static int __init rwmem_init(void)
{
	struct device *device;
	int error;

	error = alloc_chrdev_region(&rwmem_devno, 0, 1, RWMEM_DEVICE_NAME);
	if (error)
		return error;

	cdev_init(&rwmem_cdev, &rwmem_fops);
	rwmem_cdev.owner = THIS_MODULE;
	error = cdev_add(&rwmem_cdev, rwmem_devno, 1);
	if (error)
		goto unregister_region;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
	rwmem_class = class_create(RWMEM_DEVICE_NAME);
#else
	rwmem_class = class_create(THIS_MODULE, RWMEM_DEVICE_NAME);
#endif
	if (IS_ERR(rwmem_class)) {
		error = PTR_ERR(rwmem_class);
		goto delete_cdev;
	}

	device = device_create(rwmem_class, NULL, rwmem_devno, NULL,
			       RWMEM_DEVICE_NAME);
	if (IS_ERR(device)) {
		error = PTR_ERR(device);
		goto destroy_class;
	}

	pr_info("rwmem: /dev/%s registered (API %u)\n",
		RWMEM_DEVICE_NAME, RWMEM_API_VERSION);
	return 0;

destroy_class:
	class_destroy(rwmem_class);
delete_cdev:
	cdev_del(&rwmem_cdev);
unregister_region:
	unregister_chrdev_region(rwmem_devno, 1);
	return error;
}

static void __exit rwmem_exit(void)
{
	device_destroy(rwmem_class, rwmem_devno);
	class_destroy(rwmem_class);
	cdev_del(&rwmem_cdev);
	unregister_chrdev_region(rwmem_devno, 1);
	pr_info("rwmem: unloaded\n");
}

module_init(rwmem_init);
module_exit(rwmem_exit);

MODULE_DESCRIPTION("rwMem-compatible cross-process memory driver");
MODULE_AUTHOR("ri-char/rwMem contributors; MyProject1 integration");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");
