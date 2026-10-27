/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <linux/types.h>

typedef long (*nksu_handler_t)(struct pt_regs *regs);

/*
 * Single-syscall control: redirect/hook one slot and remove it again by hand.
 * There is no group init/exit -- each owner (the temporary boot watcher, the
 * steady-state hook, the daemon fd grant) installs and removes its own slots
 * at the time it needs them.
 */
int nksu_redirect_syscall(int real_nr);
int nksu_register_handler(u32 nr, nksu_handler_t fn);
void nksu_unregister_handler(u32 nr);

/* Restore one redirected syscall and drop its handler. */
void nksu_unhook_syscall(int nr);

/*
 * Bypass the profile gate while the temporary boot-stage watcher is
 * installed: init/zygote are not in the profile, but must still be seen.
 */
void nksu_dispatch_set_unconditional(bool on);
