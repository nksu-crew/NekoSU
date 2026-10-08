#pragma once
#include <linux/types.h>

typedef long (*nksu_handler_t)(struct pt_regs *regs);

int nksu_dispatch_init(void);
void nksu_dispatch_exit(void);
int nksu_redirect_syscall(int real_nr);
int nksu_register_handler(u32 nr, nksu_handler_t fn);