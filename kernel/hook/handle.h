/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HANDLE_H
#define HANDLE_H

#include <fmac.h>

struct task_struct;

long handle_prctl_hooks(struct pt_regs *regs);
long hook_path_at(struct pt_regs *regs);
long hook__NR_execveat(struct pt_regs *regs);
long hook__NR_execve(struct pt_regs *regs);
long hook__NR_setresuid(struct pt_regs *regs);

/* Stage the redirect paths and register the task_work mapping probes. */
int nksu_path_init(void);
void nksu_path_exit(void);
void nksu_path_install_task(struct task_struct *task);
void nksu_path_sync_all(void);

#ifndef CONFIG_NKSU_SYSCALL
int load_tracepoint_hook(void);
void unload_tracepoint_hook(void);
#endif

#endif /* HANDLE_H */
