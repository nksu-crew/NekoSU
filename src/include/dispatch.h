#pragma once
#include <linux/types.h>

typedef long (*nksu_handler_t)(struct pt_regs *regs);

int nksu_dispatch_init(void);
void nksu_dispatch_exit(void);
int nksu_redirect_syscall(int real_nr);
int nksu_register_handler(u32 nr, nksu_handler_t fn);

/*
 * Bypass the profile gate while the temporary boot-stage watcher is
 * installed: init/zygote are not in the profile, but must still be seen.
 */
void nksu_dispatch_set_unconditional(bool on);