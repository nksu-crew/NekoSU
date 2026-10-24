#pragma once

#include <asm/syscall.h>
#include <linux/version.h>
#include "kernel_compat.h"

/*
 * Kernel-side syscall-table edits.  Two things:
 *
 *   syscall_table_resolve()  find sys_call_table
 *   syscall_slot_hook()      replace one slot, handing back the old handler
 *   syscall_slot_unhook()    put one replaced slot back
 *
 * Each caller owns the slots it hooks and restores them itself; there is no
 * group restore.  The temporary boot watcher removes its redirects (and asks
 * init_rc.c to remove its read/fstat proxies) explicitly when it is dropped at
 * the zygote stage -- see boot/hook_init.c.
 */
extern syscall_fn_t *syscall_table;

int syscall_table_resolve(void);

int syscall_slot_hook(int nr, syscall_fn_t fn, syscall_fn_t *previous, const char *tag);

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 17, 0)
/*
 * Pre-4.17 kernels store the raw sys_* functions in sys_call_table.  Hook one
 * slot with its native signature instead of the pt_regs dispatcher.
 */
int syscall_slot_hook_native(int nr, void *fn, void **previous, const char *tag);
#endif

/* Put a single previously hooked slot back and drop its record. */
int syscall_slot_unhook(int nr);
