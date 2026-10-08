/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NKSU_UMH_H
#define NKSU_UMH_H

#include <linux/types.h>

/*
 * Kernel-side launch of a userspace program.
 *
 * This is a self-contained re-implementation of the idea behind
 * kernel/umh.c (call_usermodehelper): a kernel thread prepares credentials
 * and then calls kernel_execve() to turn itself into the target program.
 *
 * It deliberately does *not* use the kernel's usermode helper machinery:
 * no subprocess_info, no UMH workqueue, no usermodehelper_disabled state,
 * no CONFIG_STATIC_USERMODEHELPER special case and no exported
 * call_usermodehelper* symbols.  The few primitives it needs that are not
 * part of the GKI export table (kernel_thread, kernel_execve, kernel_wait,
 * flush_signal_handlers) are resolved at load time through
 * symbol_compat.c.
 *
 * The launch runs in two stages, exactly like umh.c does:
 *
 *   caller -> kthread "nksu-umh" (supervisor)
 *                 |
 *                 +-- kernel_thread(nksu_umh_exec_async) -> kernel_execve()
 *
 * The supervisor exists so the caller never becomes the parent of the
 * helper: its own parent is kthreadd, so an unwaited helper can be
 * reparented with CLONE_PARENT (kthreadd auto-reaps) and a waited helper
 * is reaped by the supervisor without touching the caller's signal
 * disposition or children list.
 *
 * Context: process context only (GFP_KERNEL + kthread_run).  Returns the
 * kernel_execve() result for NKSU_UMH_WAIT_EXEC, the wait status for
 * NKSU_UMH_WAIT_PROC, or 0 as soon as the helper has been spawned.
 */
enum {
	NKSU_UMH_NO_WAIT   = 0, /* spawn and return at once */
	NKSU_UMH_WAIT_EXEC = 1, /* wait until execve() succeeded or failed */
	NKSU_UMH_WAIT_PROC = 2, /* wait until the program exits */
};

struct nksu_umh_args {
	const char *path;   /* required, kernel memory */
	char *const *argv;  /* NULL-terminated kernel strings, may be NULL */
	char *const *envp;  /* NULL-terminated kernel strings, may be NULL */
	const char *domain; /* SELinux domain for the helper, or NULL to
	                     * keep the kernel (init) domain */
	int wait;           /* one of NKSU_UMH_* */
};

int nksu_umh_exec(const struct nksu_umh_args *args);

#endif /* NKSU_UMH_H */
