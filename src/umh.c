// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * nksu -- launch a userspace program from kernel context.
 *
 * A self-contained take on kernel/umh.c: a supervisor kthread forks a child
 * with kernel_thread(), the child installs its credentials and turns itself
 * into the target program with kernel_execve().  The kernel's usermode
 * helper (call_usermodehelper*, subprocess_info, the UMH workqueue,
 * usermodehelper_disabled, CONFIG_STATIC_USERMODEHELPER) is deliberately
 * not used at all -- see include/umh.h for the layout and the contract.
 *
 * The primitives that are not in the GKI export table (kernel_thread,
 * kernel_execve, kernel_wait, flush_signal_handlers) come in through
 * symbol_compat.c, so this file has no direct relocation against them.
 */

#include <linux/slab.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/atomic.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/fs_struct.h>
#include <linux/signal.h>
#include <linux/binfmts.h>
#include <linux/cred.h>
#include <linux/kthread.h>
#include <linux/completion.h>
#include <linux/initrd.h>

#include <fmac.h>
#include "symbol_compat.h"
/*
 * This file calls resolved unexported kernel functions through pointers.
 * Disable CFI for its functions so those indirect calls are not type-hash
 * checked (the module's build headers may hash differently than the running
 * kernel). Functions the kernel calls back live in other files.
 */
#if defined(__clang__)
#pragma clang attribute push(__attribute__((no_sanitize("cfi"))), apply_to=function)
#endif

struct nksu_umh_info {
	char *path;
	char **argv;
	char **envp;
	const char *domain;
	int wait;
	int retval;
	/* NULL when nobody waits: completing then frees the info itself. */
	struct completion *complete;
};

static void nksu_umh_free_strv(char **v)
{
	size_t i;

	if (!v)
		return;
	for (i = 0; v[i]; i++)
		kfree(v[i]);
	kfree(v);
}

/*
 * Single owner of the info block: whoever gets a NULL completion pointer
 * from nksu_umh_complete() releases it, and with it the module reference
 * taken by nksu_umh_exec().
 */
static void nksu_umh_free(struct nksu_umh_info *info)
{
	if (!info)
		return;

	nksu_umh_free_strv(info->argv);
	nksu_umh_free_strv(info->envp);
	kfree(info->path);
	module_put(THIS_MODULE);
	kfree(info);
}

static void nksu_umh_complete(struct nksu_umh_info *info)
{
	struct completion *comp = xchg(&info->complete, NULL);

	if (comp)
		complete(comp);
	else
		nksu_umh_free(info);
}

/* Deep copy of a NULL-terminated string vector; NULL input yields {NULL}. */
static char **nksu_umh_dup_strv(char *const *src)
{
	size_t n = 0, i;
	char **dst;

	while (src && src[n])
		n++;

	dst = kcalloc(n + 1, sizeof(*dst), GFP_KERNEL);
	if (!dst)
		return NULL;

	for (i = 0; i < n; i++) {
		dst[i] = kstrdup(src[i], GFP_KERNEL);
		if (!dst[i]) {
			nksu_umh_free_strv(dst);
			return NULL;
		}
	}
	return dst;
}

/*
 * The task that becomes the userspace program.
 *
 * kernel_thread() copies the supervisor's fs_struct (no CLONE_FS), and
 * nsproxy is shared (CLONE_VM), so the helper starts in the init mount
 * namespace with init's root -- the same position a UMH helper would get.
 */
static int nksu_umh_exec_async(void *data)
{
	struct nksu_umh_info *info = data;
	const bool wait_proc = info->wait == NKSU_UMH_WAIT_PROC;
	struct cred *new;
	int retval;

	spin_lock_irq(&current->sighand->siglock);
	flush_signal_handlers(current, 1);
	spin_unlock_irq(&current->sighand->siglock);

	if (current->fs)
		current->fs->umask = 0022;

	set_user_nice(current, 0);

	wait_for_initramfs();

	retval = -ENOMEM;
	new = prepare_kernel_cred(current);
	if (!new)
		goto out;

	if (info->domain) {
		int rc = set_domain(info->domain, new);

		if (rc) {
			abort_creds(new);
			retval = rc;
			goto out;
		}
	}

	commit_creds(new);

	retval = kernel_execve(info->path,
			       (const char *const *)info->argv,
			       (const char *const *)info->envp);
out:
	info->retval = retval;

	/*
	 * WAIT_PROC: the supervisor collects the exit status, so it stays the
	 * owner of info.  For NO_WAIT / WAIT_EXEC the hand-over happens here:
	 * it wakes the waiter, or frees info outright when there is none.
	 */
	if (!wait_proc)
		nksu_umh_complete(info);

	/*
	 * On success kernel_execve() has already rebuilt the stack, the mm
	 * and the registers: returning 0 from this thread function takes the
	 * task straight into userspace (ret_from_fork -> exit_to_user_mode),
	 * exactly as umh.c does.
	 */
	if (!retval)
		return 0;

	do_exit(0);
}

/*
 * Supervisor.  Its parent is kthreadd, so it is the only place where we
 * ever touch SIGCHLD or wait for a child -- the caller's signal
 * disposition and children list stay untouched.
 */
static int nksu_umh_worker(void *data)
{
	struct nksu_umh_info *info = data;
	pid_t pid;
	int status = 0;

	/*
	 * Do_wait() will not fill in a status while SIGCHLD is ignored; the
	 * sighand is private to this kthread (kernel_thread() never passes
	 * CLONE_SIGHAND), so this cannot leak into anybody else.  Same trick
	 * as umh.c.  The supervisor exits right after, no need to restore.
	 */
	kernel_sigaction(SIGCHLD, SIG_DFL);

	if (info->wait == NKSU_UMH_WAIT_PROC) {
		pid = kernel_thread(nksu_umh_exec_async, info, SIGCHLD);
		if (pid < 0) {
			info->retval = pid;
		} else {
			kernel_wait(pid, &status);
			if (!info->retval)
				info->retval = status;
		}
		nksu_umh_complete(info);
		return 0;
	}

	/*
	 * CLONE_PARENT reparents the helper to kthreadd: no zombie left in
	 * our children list and kthreadd auto-reaps it.  For WAIT_EXEC the
	 * helper completes info itself as soon as execve() came back.
	 */
	pid = kernel_thread(nksu_umh_exec_async, info, CLONE_PARENT | SIGCHLD);
	if (pid < 0) {
		info->retval = pid;
		nksu_umh_complete(info);
	}
	return 0;
}

/*
 * Launch a program.  Process context only: this allocates with GFP_KERNEL
 * and creates a kthread, so it cannot be called from atomic context.
 *
 * Returns 0 once the helper is on its way (NKSU_UMH_NO_WAIT), the
 * kernel_execve() result (NKSU_UMH_WAIT_EXEC) or the helper's wait status
 * (NKSU_UMH_WAIT_PROC).  -EINTR means the wait was interrupted and the
 * result was dropped; the helper keeps running.
 */
int nksu_umh_exec(const struct nksu_umh_args *args)
{
	struct nksu_umh_info *info;
	struct task_struct *tsk;
	DECLARE_COMPLETION_ONSTACK(done);
	int ret;

	if (!args || !args->path || !args->path[0])
		return -EINVAL;

	switch (args->wait) {
	case NKSU_UMH_NO_WAIT:
	case NKSU_UMH_WAIT_EXEC:
	case NKSU_UMH_WAIT_PROC:
		break;
	default:
		return -EINVAL;
	}

	/*
	 * The helper task runs module text after we let go of it, so keep the
	 * module busy until the info block is released.  nksu_umh_free() is
	 * the single place that drops the reference again.
	 */
	if (!try_module_get(THIS_MODULE))
		return -ENODEV;

	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (!info)
		goto oom;

	info->path = kstrdup(args->path, GFP_KERNEL);
	if (!info->path)
		goto oom;

	info->argv = nksu_umh_dup_strv(args->argv);
	if (!info->argv)
		goto oom;

	if (!info->argv[0]) {
		info->argv[0] = kstrdup(info->path, GFP_KERNEL);
		if (!info->argv[0])
			goto oom;
	}

	info->envp = nksu_umh_dup_strv(args->envp);
	if (!info->envp)
		goto oom;

	info->domain = args->domain;
	info->wait = args->wait;
	info->retval = 0;
	info->complete = (args->wait == NKSU_UMH_NO_WAIT) ? NULL : &done;

	tsk = kthread_run(nksu_umh_worker, info, "nksu-umh");
	if (IS_ERR(tsk)) {
		ret = PTR_ERR(tsk);
		nksu_umh_free(info);
		return ret;
	}

	if (args->wait == NKSU_UMH_NO_WAIT)
		return 0;

	ret = wait_for_completion_killable(&done);
	if (ret) {
		/*
		 * Interrupted.  Taking the completion pointer back means the
		 * supervisor has not completed yet: from here on it owns info
		 * and will free it (nksu_umh_complete() sees NULL and calls
		 * nksu_umh_free()) without ever touching our stack frame.
		 */
		if (xchg(&info->complete, NULL))
			return -EINTR;
		/* Otherwise it already completed; we still own info. */
		wait_for_completion(&done);
	}

	ret = info->retval;
	nksu_umh_free(info);
	return ret;

oom:
	nksu_umh_free(info);
	return -ENOMEM;
}

#if defined(__clang__)
#pragma clang attribute pop
#endif
