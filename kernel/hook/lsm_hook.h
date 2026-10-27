/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <linux/lsm_hooks.h>
#include <linux/stddef.h>
#include <linux/version.h>

/*
 * Runtime patching of an existing LSM hook slot.
 *
 * Ported from KernelSU's hook/lsm_hook.c.  The SELinux hiding feature needs to
 * intercept security_setprocattr()'s SELinux implementation, which the kernel
 * reaches through the LSM hook table rather than the syscall table, so the
 * syscall dispatcher cannot see it.  There is no exported API to register an
 * out-of-tree LSM, so we find the security_hook_list entry whose handler is
 * `target_name` and overwrite just that slot, keeping the original aside.
 *
 * The slot is located by scanning the table and matching the function pointer,
 * never by a compile-time head/field offset, so it does not depend on the
 * (possibly randomized) struct layout.  6.12 turned the table into static
 * calls; both the security_hook_list slot and the static call are updated there.
 */

struct nksu_lsm_hook {
	const char *head_name;
	const char *target_name;
	size_t hook_offset;
	void *replacement;
	void *original;
	struct security_hook_list *entry;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
	struct lsm_static_call *scall;
#endif
};

/*
 * member is the LSM hook name (also used as the head name in logs),
 * target_symbol the kernel function currently registered for that hook,
 * replacement_fn the function to install.
 */
#define NKSU_LSM_HOOK_INIT(member, target_symbol, replacement_fn)                \
	{                                                                        \
		.head_name = #member,                                            \
		.target_name = target_symbol,                                    \
		.hook_offset = offsetof(struct security_hook_list, hook.member), \
		.replacement = (void *)(replacement_fn),                         \
	}

/*
 * Replace the LSM hook handler that currently points to hook->target_name with
 * hook->replacement, saving the original into hook->original.  Returns 0,
 * -ENOENT if the target is not registered, or a negative errno on failure.
 * Call nksu_lsm_unhook() to restore it.
 */
int nksu_lsm_hook(struct nksu_lsm_hook *hook);

/* Restore the slot patched by nksu_lsm_hook() and clear hook->entry. */
void nksu_lsm_unhook(struct nksu_lsm_hook *hook);
