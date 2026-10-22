#ifndef HANDLE_H
#define HANDLE_H

#include <fmac.h>

struct task_struct;

long handle_prctl_hooks(struct pt_regs *regs);
long hook_path_at(struct pt_regs *regs);
long hook__NR_execveat(struct pt_regs *regs);
long hook__NR_execve(struct pt_regs *regs);
long hook__NR_setresuid(struct pt_regs *regs);

#ifndef CONFIG_NKSU_SYSCALL
int load_tracepoint_hook(void);
void unload_tracepoint_hook(void);
#endif

#endif /* HANDLE_H */
