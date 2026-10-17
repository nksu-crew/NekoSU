#include <linux/string.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <asm/ptrace.h>
#include <linux/compiler.h>

#include "hook/hook.h"
#include "manager/manager.h"
#include "fd/fd.h"
#include "privilege/privilege.h"
#include "selinux/selinux.h"
#include "manager/ioctl.h"
#include <fmac.h>
#include "klog.h"

#ifndef CONFIG_NKSU_SYSCALL
#include <linux/pid.h>
#include <linux/sched/signal.h>
#include <linux/trace_events.h>
#include <linux/tracepoint.h>
#endif

long handle_prctl_hooks(struct pt_regs *regs)
{
#if defined(__aarch64__)
    unsigned long option = regs->regs[0];
#elif defined(__x86_64__)
    unsigned long option = regs->di;
#endif

    if (likely(!is_manager())) {
        return 0;
    }

    switch (option) {
    case NKSU_PRCTL_GET_SHM_FD:
        fmac_anonfd_get();
        return 1;

    case NKSU_PRCTL_GET_CTL_FD:
        fmac_ctlfd_get();
        return 1;

    default:
        return 0;
    }
}

/*
 * The x86-64 ABI reserves the 128 bytes below the stack pointer as the red
 * zone: live scratch that the kernel preserves across signal delivery, so the
 * staged string has to sit below it or it could clobber a caller's red-zone
 * locals.  aarch64 has no red zone, so there is nothing to skip there.
 */
#if defined(__x86_64__)
#define NKSU_STACK_REDZONE 128UL
#else
#define NKSU_STACK_REDZONE 0UL
#endif

/*
 * Stage a replacement path argument on the user stack and return its address,
 * or 0 on failure.
 *
 * The wrapped syscall reads the string synchronously -- we never return to
 * user mode in between -- so it only has to survive for the duration of the
 * call.  The area below the red zone (see above) is already mapped, needs no
 * allocation and cannot clash with live frames; align it like the ABI stack
 * and keep it clear of the guard page.  copy_to_user() performs its own
 * access_ok() check (a separate call is redundant and discouraged), so an
 * address outside the mapped stack simply fails the copy.
 */
static unsigned long push_str(unsigned long sp, const char *str, size_t len)
{
    unsigned long size, addr;

    if (unlikely(!len || len > PAGE_SIZE))
        return 0;

    size = ALIGN(len, 16);
    if (unlikely(sp < NKSU_STACK_REDZONE + size))
        return 0;

    addr = sp - NKSU_STACK_REDZONE - size;
    if (unlikely(copy_to_user((void __user *)addr, str, len)))
        return 0;

    return addr;
}

static unsigned long try_redirect_path(struct pt_regs *regs, unsigned int arg_index, const char *target,
                                       size_t target_len, bool require_target)
{
    char buf[SU_PATH_LEN];
    const char __user *upath;
    unsigned long sp;
    ssize_t ulen;

    if (unlikely(!current->mm))
        return 0;

#if defined(__aarch64__)
    if (unlikely(arg_index > 5))
        return 0;
#else
    if (unlikely(arg_index > 7))
        return 0;
#endif

    if (unlikely(!target || !target_len || target_len > PAGE_SIZE))
        return 0;

    upath = (const char __user *)regs->regs[arg_index];
    if (unlikely(!upath))
        return 0;

    /*
     * Only an exact match matters, so copy at most SU_PATH_LEN bytes: the
     * overwhelmingly common case is a completely different path (every
     * stat/access/exec a managed UID performs), and it must not pay for a
     * full-sized copy.  A longer path is truncated to SU_PATH_LEN and fails
     * the length check below; -EFAULT also fails it.
     * strncpy_from_user() returns the length without the trailing NUL, while
     * SU_PATH_LEN is sizeof(SU_PATH) (NUL included).
     */
    ulen = strncpy_from_user(buf, upath, sizeof(buf));
    if (unlikely(ulen != SU_PATH_LEN - 1) || !path_is_su(buf))
        return 0;

    /*
     * KernelSU gates the su→ksud redirect on ksud existing; without a target
     * there is nothing to exec, so run the real su instead of escalating into
     * a failed exec.  This must not sleep (the tracepoint path runs with
     * preemption disabled), so use the flag the manager scan publishes.
     */
    if (require_target && !is_daemon_ready())
        return 0;

    sp = user_stack_pointer(regs);
    if (unlikely(!sp))
        return 0;

    return push_str(sp, target, target_len);
}

long hook_path_at(struct pt_regs *regs)
{
    unsigned long new_uaddr = try_redirect_path(regs, 1, SH_PATH, SH_PATH_LEN, false);
    if (new_uaddr > 0) {
        regs->regs[1] = new_uaddr;
    }
    return 0;
}

long hook__NR_execve(struct pt_regs *regs)
{
    unsigned long new_uaddr = try_redirect_path(regs, 0, REDIRECT_TARGET, REDIRECT_TARGET_LEN, true);
    if (new_uaddr > 0) {
        regs->regs[0] = new_uaddr;
        privilege_escalate_from_profile(NULL);
    }
    return 0;
}

long hook__NR_execveat(struct pt_regs *regs)
{
    unsigned long new_uaddr = try_redirect_path(regs, 1, REDIRECT_TARGET, REDIRECT_TARGET_LEN, true);
    if (new_uaddr > 0) {
        regs->regs[1] = new_uaddr;
        privilege_escalate_from_profile(NULL);
    }
    return 0;
}

/*
 * Root from native code, without spawning `su`.
 *
 * An authorized app may ask the kernel for root directly from JNI/native
 * code.  Only setresuid() is usable for that: Android's app seccomp policy
 * allows setresuid(2) but kills the process with SIGSYS on setuid(2) /
 * setreuid(2), and all callers go through setresuid (see jni readFile).
 *
 * When the call targets root, escalate from the profile just before the real
 * syscall runs; the caller is root by then, so the original call succeeds
 * instead of failing with EPERM.
 *
 * Unauthorized processes never reach here: both dispatchers gate on a
 * managed UID (see probe_sys_enter and nksu_dispatch_fast), and
 * privilege_escalate_from_profile() re-checks the profile before touching
 * credentials.  This mirrors exactly what the su path already grants.
 */
long hook__NR_setresuid(struct pt_regs *regs)
{
    uid_t root_uid;

    /* setresuid(ruid, euid, suid): only euid == 0 actually grants privilege. */
    if (regs->regs[1] != 0)
        return 0;

    if (privilege_escalate_from_profile(&root_uid) == 0) {
        /*
         * The profile decides the resulting uid.  Point the real syscall at
         * that uid as well; otherwise a profile whose root uid is non-zero
         * would be undone the moment the original setresuid(..., 0, ...) runs
         * (the raised capabilities would let it succeed).
         */
        regs->regs[0] = root_uid;
        regs->regs[1] = root_uid;
        regs->regs[2] = root_uid;
    }
    return 0;
}

/*
 * Single source of truth for the syscalls we intercept.  The tracepoint
 * dispatcher (probe_sys_enter) expands it into a switch; the syscall-table
 * build (init_syscall_hook) expands it into the table below.
 */
#define NKSU_SYSCALL_HANDLERS(X)                                            \
	X(__NR_prctl, handle_prctl_hooks)                                   \
	X(__NR_execve, hook__NR_execve)                                     \
	X(__NR_execveat, hook__NR_execveat)                                 \
	X(__NR_setresuid, hook__NR_setresuid)                               \
	X(__NR_faccessat, hook_path_at)                                     \
	X(__NR_newfstatat, hook_path_at)

#define NKSU_HOOK_TABLE(nr, fn) { nr, fn },

static const struct syscall_hook syscall_hooks[] = {
	NKSU_SYSCALL_HANDLERS(NKSU_HOOK_TABLE)
};

#ifdef CONFIG_NKSU_SYSCALL

int init_syscall_hook(void)
{
	int ret, i;

	/* Steady state is profile-gated; the temporary watcher may have left
	 * this on. */
	nksu_dispatch_set_unconditional(false);

	for (i = 0; i < ARRAY_SIZE(syscall_hooks); i++) {
		ret = nksu_redirect_syscall(syscall_hooks[i].nr);
		if (ret) {
			pr_err("[hook]: can't redirect syscall %d: %d\n",
			       syscall_hooks[i].nr, ret);
			goto err;
		}
	}

	for (i = 0; i < ARRAY_SIZE(syscall_hooks); i++) {
		ret = nksu_register_handler(syscall_hooks[i].nr,
					    syscall_hooks[i].handler);
		if (ret) {
			pr_err("[hook]: can't register handler for syscall %d: %d\n",
			       syscall_hooks[i].nr, ret);
			goto err;
		}
	}

	pr_info("[hook]: loaded syscall hook\n");
	return 0;

err:
	/* Undo a partial install. */
	for (i = 0; i < ARRAY_SIZE(syscall_hooks); i++)
		nksu_unhook_syscall(syscall_hooks[i].nr);
	return ret;
}

void exit_syscall_hook(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(syscall_hooks); i++)
		nksu_unhook_syscall(syscall_hooks[i].nr);

	pr_info("[hook]: unloaded syscall hook\n");
}
#endif /* CONFIG_NKSU_SYSCALL */

#ifndef CONFIG_NKSU_SYSCALL

static struct tracepoint *tp_sys_enter;
static struct tracepoint *tp_sched_fork;

/* mark every thread in the process identified by pid */
void mark_threads_by_pid(pid_t pid)
{
	struct task_struct *task, *t;

	rcu_read_lock();

	task = find_task_by_vpid(pid);
	if (!task)
		goto out;

	for_each_thread(task, t)
		set_tsk_thread_flag(t, TIF_SYSCALL_TRACEPOINT);

out:
	rcu_read_unlock();
}

/* sys_enter callback: filter by managed UID, then dispatch.  The cases come
 * from NKSU_SYSCALL_HANDLERS, so the compiler still emits a decision tree /
 * jump table with no loop or array load on this per-syscall path. */
static void probe_sys_enter(void *data, struct pt_regs *regs, long id)
{
#define NKSU_HOOK_CASE(nr, fn) \
	case nr:               \
		fn(regs);      \
		return;

	if (!nksu_profile_has_uid(__kuid_val(task_uid(current))))
		return;

	switch (id) {
		NKSU_SYSCALL_HANDLERS(NKSU_HOOK_CASE)
	}

#undef NKSU_HOOK_CASE
}

/* fork callback: ensure the child of a managed UID also gets traced.  Only the
 * new task needs flagging -- the parent's threads already carry the flag -- so
 * this is O(1) instead of scanning every thread on every fork. */
static void probe_sched_fork(void *data, struct task_struct *parent,
			     struct task_struct *child)
{
	if (!nksu_profile_has_uid(__kuid_val(task_uid(child))))
		return;
	set_tsk_thread_flag(child, TIF_SYSCALL_TRACEPOINT);
}

struct tp_find_ctx {
	const char *name;
	struct tracepoint **out;
};

static void tp_find_cb(struct tracepoint *tp, void *priv)
{
	struct tp_find_ctx *ctx = priv;

	if (*ctx->out)
		return;
	if (strcmp(tp->name, ctx->name) == 0)
		*ctx->out = tp;
}

/* walk the kernel tracepoint table to find one by name */
static struct tracepoint *find_tracepoint(const char *name)
{
	struct tracepoint *result = NULL;
	struct tp_find_ctx ctx = { .name = name, .out = &result };

	for_each_kernel_tracepoint(tp_find_cb, &ctx);
	return result;
}

int load_tracepoint_hook(void)
{
	int ret;

	tp_sys_enter = find_tracepoint("sys_enter");
	if (!tp_sys_enter) {
		pr_err("cannot find sys_enter tracepoint\n");
		return -ENOENT;
	}

	tp_sched_fork = find_tracepoint("sched_process_fork");
	if (!tp_sched_fork) {
		pr_err("cannot find sched_process_fork tracepoint\n");
		return -ENOENT;
	}

	ret = tracepoint_probe_register(tp_sys_enter, probe_sys_enter, NULL);
	if (ret) {
		pr_err("register sys_enter probe failed: %d\n", ret);
		return ret;
	}

	ret = tracepoint_probe_register(tp_sched_fork, probe_sched_fork, NULL);
	if (ret) {
		pr_err("register sched_process_fork probe failed: %d\n", ret);
		tracepoint_probe_unregister(tp_sys_enter, probe_sys_enter, NULL);
		return ret;
	}

	pr_info("tracepoint hooks loaded\n");
	return 0;
}

void unload_tracepoint_hook(void)
{
	if (tp_sys_enter)
		tracepoint_probe_unregister(tp_sys_enter, probe_sys_enter, NULL);
	if (tp_sched_fork)
		tracepoint_probe_unregister(tp_sched_fork, probe_sched_fork, NULL);

	tracepoint_synchronize_unregister();

	pr_info("tracepoint hooks unloaded\n");
}

#endif /* !CONFIG_NKSU_SYSCALL */
