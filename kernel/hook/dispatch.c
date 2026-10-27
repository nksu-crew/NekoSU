// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Per-syscall redirection through a single shared entry point.
 *
 * nksu_dispatch_fast() is installed into the slots nksu wants to observe; it
 * looks up the real handler and the registered handler for the incoming syscall
 * number and applies the profile gate.  There is no group init/exit: callers
 * use nksu_redirect_syscall() / nksu_unhook_syscall() and manage their own
 * lifetimes.
 */
#include "klog.h"
#include <linux/cred.h>
#include <linux/kallsyms.h>
#include <linux/sched.h>
#include <linux/syscalls.h>
#include <linux/nospec.h>

#include <fmac.h>
#include "hook/syscall.h"
#include "hook/dispatch.h"

syscall_fn_t nksu_orig_table[__NR_syscalls] ____cacheline_aligned;
nksu_handler_t virt_table[__NR_syscalls] ____cacheline_aligned;

static bool dispatch_unconditional;

void nksu_dispatch_set_unconditional(bool on)
{
	WRITE_ONCE(dispatch_unconditional, on);
}

static int hook_and_save(int nr, syscall_fn_t new_fn, const char *tag)
{
	syscall_fn_t orig = NULL;
	int ret;

	if ((unsigned int)nr >= (unsigned int)__NR_syscalls)
		return -EINVAL;

	if (!syscall_table) {
		ret = syscall_table_resolve();
		if (ret)
			return ret;
	}

	ret = syscall_slot_hook(nr, new_fn, &orig, tag);
	if (ret)
		return ret;

	WRITE_ONCE(nksu_orig_table[nr], orig);

	pr_info("[syscall]: slot %d hooked: orig=%ps new=%ps\n", nr, orig, new_fn);
	return 0;
}

int nksu_register_handler(u32 nr, nksu_handler_t fn)
{
	if (nr >= __NR_syscalls || !fn)
		return -EINVAL;

	if (READ_ONCE(virt_table[nr]) != NULL)
		return -EEXIST;

	smp_store_release(&virt_table[nr], fn);
	return 0;
}

void nksu_unregister_handler(u32 nr)
{
	if (nr < __NR_syscalls)
		WRITE_ONCE(virt_table[nr], NULL);
}

asmlinkage long nksu_dispatch_fast(const struct pt_regs *regs)
{
	unsigned int nr = (unsigned int)regs->regs[8];
	syscall_fn_t orig;

	if (unlikely(nr >= (unsigned int)__NR_syscalls))
		return -ENOSYS;

	nr = array_index_nospec(nr, __NR_syscalls);

	orig = READ_ONCE(nksu_orig_table[nr]);

	if (likely(!READ_ONCE(dispatch_unconditional) && !nksu_profile_has_uid(current_uid().val)))
		return orig ? orig(regs) : -ENOSYS;

	{
		nksu_handler_t handler = READ_ONCE(virt_table[nr]);
		if (unlikely(handler)) {
			long ret = handler((struct pt_regs *)regs);
			if (ret)
				return ret;
		}
	}

	return orig ? orig(regs) : -ENOSYS;
}

int nksu_redirect_syscall(int real_nr)
{
	return hook_and_save(real_nr, nksu_dispatch_fast, "nksu_redirect");
}

/*
 * Restore one redirected syscall and forget its handler.  Idempotent enough
 * for the owners: syscall_slot_unhook() reports a slot it never recorded.
 */
void nksu_unhook_syscall(int nr)
{
	if ((unsigned int)nr >= (unsigned int)__NR_syscalls)
		return;

	WRITE_ONCE(virt_table[nr], NULL);
	WRITE_ONCE(nksu_orig_table[nr], NULL);
	syscall_slot_unhook(nr);
}
