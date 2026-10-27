/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Version compatibility shims for older kernels (notably 4.9).
 *
 * The module was originally written against 5.10+ / GKI kernels.  Every entry
 * below is guarded by an explicit version check so newer trees keep using
 * their own definitions.
 */
#ifndef NKSU_KERNEL_COMPAT_H
#define NKSU_KERNEL_COMPAT_H

#include <linux/version.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <asm/ptrace.h>

/*
 * task_work_add() used a bool "notify" argument before 5.8; TWA_RESUME is the
 * equivalent of notify=true (run the callback before returning to userspace).
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 8, 0)
#ifndef TWA_RESUME
#define TWA_RESUME true
#endif
#ifndef TWA_SIGNAL
#define TWA_SIGNAL false
#endif
#endif

/*
 * mmap_lock was introduced in 5.8; older kernels expose mmap_sem directly.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 8, 0)
#ifndef mmap_read_lock
#define mmap_read_lock(mm) down_read(&(mm)->mmap_sem)
#define mmap_read_unlock(mm) up_read(&(mm)->mmap_sem)
#define mmap_read_trylock(mm) down_read_trylock(&(mm)->mmap_sem)
#define mmap_write_lock(mm) down_write(&(mm)->mmap_sem)
#define mmap_write_unlock(mm) up_write(&(mm)->mmap_sem)
#define mmap_write_trylock(mm) down_write_trylock(&(mm)->mmap_sem)
#endif
#endif

/*
 * copy_{to,from}_kernel_nofault replaced probe_kernel_{write,read} in 5.8.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 8, 0)
#ifndef copy_from_kernel_nofault
static inline int nksu_copy_from_kernel_nofault(void *dst, const void *src, size_t size)
{
	return probe_kernel_read(dst, src, size);
}
#define copy_from_kernel_nofault nksu_copy_from_kernel_nofault
#endif
#endif

/*
 * kvmalloc()/kvmalloc_array()/kvcalloc() arrived in 4.12.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 12, 0)
static inline void *nksu_kvmalloc(size_t size, gfp_t flags)
{
	void *p = kmalloc(size, flags | __GFP_NOWARN);

	if (!p)
		p = vmalloc(size);
	return p;
}

static inline void *nksu_kvmalloc_array(size_t n, size_t size, gfp_t flags)
{
	if (size != 0 && n > SIZE_MAX / size)
		return NULL;
	return nksu_kvmalloc(n * size, flags);
}

static inline void *nksu_kvcalloc(size_t n, size_t size, gfp_t flags)
{
	size_t bytes = n * size;
	void *p = nksu_kvmalloc_array(n, size, flags);

	if (p)
		memset(p, 0, bytes);
	return p;
}

#define kvmalloc(size, flags) nksu_kvmalloc((size), (flags))
#define kvmalloc_array(n, size, flags) nksu_kvmalloc_array((n), (size), (flags))
#define kvcalloc(n, size, flags) nksu_kvcalloc((n), (size), (flags))
#endif

/*
 * syscall_fn_t (the pt_regs-based syscall table entry type) only exists from
 * 4.17, when the generic syscall wrappers were introduced.  4.9 arm64 still
 * stores the plain sys_* functions in sys_call_table; the syscall-table hook
 * path is therefore not used there (the tracepoint dispatcher is), but the
 * typedef is needed for the shared declarations.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 17, 0)
typedef long (*syscall_fn_t)(const struct pt_regs *regs);
#endif

/*
 * "notify" semantics for wait_event_interruptible() existed already, but the
 * 4.9 tree has no <linux/sched/signal.h> / <linux/sched/task.h> / <linux/mm.h>
 * split, so callers include <linux/sched.h> instead.
 */

#endif /* NKSU_KERNEL_COMPAT_H */
