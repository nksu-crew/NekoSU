/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NKSU_SPAWN_H
#define NKSU_SPAWN_H

/*
 * Launch a userspace program from kernel context.
 *
 * This is a self-contained replacement for the kernel's usermode-helper
 * implementation.  It deliberately does not touch call_usermodehelper*(),
 * subprocess_info, system_unbound_wq, usermodehelper_disabled or
 * CONFIG_STATIC_USERMODEHELPER -- none of them is usable from an
 * out-of-tree module and all of them can be disabled by the system.
 *
 *   caller -> kthread "nksu-spawn" (supervisor)
 *                 |
 *                 +-- kernel_thread(nksu_spawn_child) -> kernel_execve()
 *
 * The supervisor is a child of kthreadd, so the caller never becomes the
 * parent of the helper: an unwaited helper is reparented to kthreadd with
 * CLONE_PARENT (auto-reaped) and a waited one is reaped by the supervisor.
 * The caller's signal disposition and children list are never touched.
 *
 * kernel_thread(), kernel_execve(), kernel_wait() and flush_signal_handlers()
 * are not in the GKI export table; they are resolved at load time through
 * symbol_compat.c, so the implementation has no direct relocation for them.
 *
 * Strings are copied into kernel memory, so callers may pass temporary
 * buffers.  Context: process context only (GFP_KERNEL + kthread_run).
 */

enum nksu_spawn_wait {
	NKSU_SPAWN_NOWAIT    = 0, /* spawn and return at once */
	NKSU_SPAWN_WAIT_EXEC = 1, /* wait until execve() succeeded or failed */
	NKSU_SPAWN_WAIT_PROC = 2, /* wait until the program exits */
};

struct nksu_spawn_args {
	const char *path;   /* required, kernel memory */
	char *const *argv;  /* NULL-terminated kernel strings, may be NULL */
	char *const *envp;  /* NULL-terminated kernel strings, may be NULL */
	const char *domain; /* SELinux domain for the helper, or NULL to keep
	                     * the kernel (init) domain */
	int wait;           /* one of enum nksu_spawn_wait */
};

int nksu_spawn(const struct nksu_spawn_args *args);

#endif /* NKSU_SPAWN_H */
