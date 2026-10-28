// SPDX-License-Identifier: GPL-3.0-or-later
#include "klog.h"
#include <linux/kallsyms.h>
#include <linux/printk.h>
#include <linux/spinlock.h>
#include <asm/syscall.h>

#include <fmac.h>
#include "hook/patch.h"
#include "hook/syscall.h"
#include "symbol/symbol.h"
/* Last: redirects unexported symbols through resolved pointers. */
#include "symbol/symbol_compat.h"

syscall_fn_t *syscall_table;

#define MAX_HOOKS 256

struct hook_entry {
	unsigned long addr;
	void *previous;
};

static struct hook_entry hook_table[MAX_HOOKS];
static int hook_count = 0;
static DEFINE_SPINLOCK(hook_lock);

/*
 * Replacing a slot goes through hook/patch.c, the single owner of the
 * text-patch primitive (and of init_mm).  Do not add a second patcher here.
 */
static int patch_slot(void *addr, void *newval)
{
	return nksu_patch_text(addr, &newval, sizeof(newval));
}

static void *recorded_previous(unsigned long addr);

static int record_slot(unsigned long addr, void *hook_fn, void **previous)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&hook_lock, flags);
	if (hook_count >= MAX_HOOKS) {
		spin_unlock_irqrestore(&hook_lock, flags);
		return -ENOMEM;
	}
	hook_table[hook_count].addr = addr;
	hook_table[hook_count].previous = *(void **)addr;
	hook_count++;
	spin_unlock_irqrestore(&hook_lock, flags);

	if (previous)
		*previous = recorded_previous(addr);

	ret = patch_slot((void *)addr, hook_fn);
	if (ret) {
		spin_lock_irqsave(&hook_lock, flags);
		hook_count--;
		spin_unlock_irqrestore(&hook_lock, flags);
		pr_err("nksu: patch failed: %d\n", ret);
	}
	return ret;
}

static int restore_slot(unsigned long addr)
{
	unsigned long flags;
	syscall_fn_t previous;
	int i, ret;

	spin_lock_irqsave(&hook_lock, flags);
	/* newest first: a later hook on the same slot wraps the earlier one */
	for (i = hook_count - 1; i >= 0; i--) {
		if (hook_table[i].addr == addr)
			break;
	}
	if (i < 0) {
		spin_unlock_irqrestore(&hook_lock, flags);
		pr_debug("nksu: restore: slot not recorded\n");
		return -ENOENT;
	}
	previous = (syscall_fn_t)hook_table[i].previous;
	hook_table[i] = hook_table[--hook_count];
	spin_unlock_irqrestore(&hook_lock, flags);

	ret = patch_slot((void *)addr, (void *)previous);
	if (ret)
		pr_err("nksu: restore patch failed: %d\n", ret);
	return ret;
}

static void *recorded_previous(unsigned long addr)
{
	unsigned long flags;
	void *previous = NULL;
	int i;

	spin_lock_irqsave(&hook_lock, flags);
	for (i = hook_count - 1; i >= 0; i--) {
		if (hook_table[i].addr == addr) {
			previous = hook_table[i].previous;
			break;
		}
	}
	spin_unlock_irqrestore(&hook_lock, flags);
	return previous;
}

int syscall_slot_hook(int nr, syscall_fn_t fn, syscall_fn_t *previous, const char *tag)
{
	unsigned long addr;
	int ret;

	if (!syscall_table) {
		ret = syscall_table_resolve();
		if (ret)
			return ret;
	}

	addr = (unsigned long)&syscall_table[nr];
	ret = record_slot(addr, (void *)fn, (void **)previous);

	if (ret) {
		pr_err("nksu: failed to hook %s: %d\n", tag, ret);
		return ret;
	}
	pr_info("nksu: hooked %s\n", tag);
	return 0;
}

int syscall_slot_unhook(int nr)
{
	return restore_slot((unsigned long)&syscall_table[nr]);
}

/*
 * 4.9 sys_call_table holds plain sys_* functions (no pt_regs wrapper), so the
 * init.rc proxy hooks a slot with the native C signature through this entry
 * point.  The slot bookkeeping (and restore path) is shared.
 */
int syscall_slot_hook_native(int nr, void *fn, void **previous, const char *tag)
{
	unsigned long addr;
	int ret;

	if (!syscall_table) {
		ret = syscall_table_resolve();
		if (ret)
			return ret;
	}

	addr = (unsigned long)&syscall_table[nr];
	ret = record_slot(addr, fn, previous);
	if (ret)
		pr_err("nksu: failed to hook %s: %d\n", tag, ret);
	else
		pr_info("nksu: hooked %s\n", tag);
	return ret;
}

int syscall_table_resolve(void)
{
	/*
     * init_mm belongs to the patcher (hook/patch.c); failing here keeps the
     * syscall-hook callers from patching a table they cannot reach.
     */
	int ret = nksu_patch_init();
	if (ret) {
		pr_err("nksu: failed to find init_mm\n");
		return ret;
	}

	syscall_table = (syscall_fn_t *)NKSU_KSYM_LOOKUP("sys_call_table");
	if (!syscall_table) {
		pr_err("nksu: failed to find sys_call_table\n");
		return -ENOENT;
	}
	pr_info("nksu: syscall table at %px\n", syscall_table);
	return 0;
}
