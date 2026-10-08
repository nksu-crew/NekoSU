// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * nksu -- launch a userspace program from kernel context.
 *
 * See include/spawn.h for the overall design.  Short version: a short-lived
 * supervisor kthread creates the child with kernel_thread(); the child resets
 * its signal handlers and credentials, waits for the initramfs and turns
 * itself into the target with kernel_execve().
 *
 * Ownership of the work item is decided by a single atomic exchange of its
 * completion pointer, so the waiter, the supervisor and the child can race
 * freely without double-freeing or touching a dead stack frame.
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
#include <linux/version.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 13, 0)
#include <linux/initrd.h>
#endif
#include <linux/fs_struct.h>
#include <linux/signal.h>
#include <linux/binfmts.h>
#include <linux/cred.h>
#include <linux/kthread.h>
#include <linux/completion.h>

#include <fmac.h>
#include "symbol_compat.h"

/*
 * The resolved primitives are invoked through pointers; a foreign function
 * pointer can fail the compiler's CFI type check, so opt this file out.
 */
#if defined(__clang__)
#pragma clang attribute push(__attribute__((no_sanitize("cfi"))), apply_to=function)
#endif

struct nksu_spawn {
	char *path;
	char **argv;
	char **envp;
	const char *domain;
	int wait;
	int ret;
	/* NULL when nobody waits: then the last owner frees the work item. */
	struct completion *done;
};

static void nksu_spawn_free_strv(char **v)
{
	size_t i;

	if (!v)
		return;
	for (i = 0; v[i]; i++)
		kfree(v[i]);
	kfree(v);
}

static void nksu_spawn_free(struct nksu_spawn *work)
{
	if (!work)
		return;

	nksu_spawn_free_strv(work->argv);
	nksu_spawn_free_strv(work->envp);
	kfree(work->path);
	module_put(THIS_MODULE);
	kfree(work);
}

/*
 * Single-owner hand-over: whoever gets a non-NULL completion back owns the
 * wait, and whoever gets NULL frees the work item.
 */
static void nksu_spawn_complete(struct nksu_spawn *work)
{
	struct completion *done = xchg(&work->done, NULL);

	if (done)
		complete(done);
	else
		nksu_spawn_free(work);
}

/* Deep-copy a NULL-terminated vector; NULL input yields a zeroed one. */
static char **nksu_spawn_dup_strv(char *const *src)
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
			nksu_spawn_free_strv(dst);
			return NULL;
		}
	}
	return dst;
}

/* The task that becomes the userspace program. */
static int nksu_spawn_child(void *data)
{
	struct nksu_spawn *work = data;
	bool wait_proc = work->wait == NKSU_SPAWN_WAIT_PROC;
	struct cred *new;
	int ret;

	spin_lock_irq(&current->sighand->siglock);
	flush_signal_handlers(current, 1);
	spin_unlock_irq(&current->sighand->siglock);

	if (current->fs)
		current->fs->umask = 0022;

	set_user_nice(current, 0);

	ret = -ENOMEM;
	new = prepare_kernel_cred(current);
	if (!new)
		goto out;

	if (work->domain) {
		ret = set_domain(work->domain, new);
		if (ret) {
			abort_creds(new);
			goto out;
		}
	}

	commit_creds(new);

	/*
	 * Make sure files provided by the initramfs are visible.
	 * wait_for_initramfs() was introduced in 5.13; older kernels did not
	 * need it in their usermode helper, so the call is compiled out there.
	 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 13, 0)
	wait_for_initramfs();
#endif

	ret = kernel_execve(work->path,
			    (const char *const *)work->argv,
			    (const char *const *)work->envp);
out:
	work->ret = ret;

	/*
	 * WAIT_PROC: the supervisor reaps us and completes the waiter, so it
	 * keeps ownership.  Otherwise the hand-over happens right here.
	 */
	if (!wait_proc)
		nksu_spawn_complete(work);

	/*
	 * On success kernel_execve() has already rebuilt the mm, the stack and
	 * the registers: returning 0 takes this thread straight into userspace,
	 * exactly as the kernel's usermode helper does.
	 */
	if (!ret)
		return 0;
	do_exit(0);
}

/* Supervisor: reaps the helper for WAIT_PROC, otherwise detaches it. */
static int nksu_spawn_supervisor(void *data)
{
	struct nksu_spawn *work = data;
	pid_t pid;
	int status = 0;

	/*
	 * do_wait() will not fill in a status while SIGCHLD is ignored; the
	 * supervisor's sighand is private (kernel_thread() never passes
	 * CLONE_SIGHAND), so restoring the default here cannot leak into the
	 * caller.  Same trick as the kernel's usermode helper.
	 */
	kernel_sigaction(SIGCHLD, SIG_DFL);

	if (work->wait == NKSU_SPAWN_WAIT_PROC) {
		pid = kernel_thread(nksu_spawn_child, work, SIGCHLD);
		if (pid < 0) {
			work->ret = pid;
		} else {
			kernel_wait(pid, &status);
			if (!work->ret)
				work->ret = status;
		}
		nksu_spawn_complete(work);
		return 0;
	}

	/*
	 * CLONE_PARENT reparents the helper to kthreadd: kthreadd ignores
	 * SIGCHLD, so the helper is reaped automatically and never pollutes
	 * our children list.
	 */
	pid = kernel_thread(nksu_spawn_child, work, CLONE_PARENT | SIGCHLD);
	if (pid < 0) {
		work->ret = pid;
		nksu_spawn_complete(work);
	}
	return 0;
}

int nksu_spawn(const struct nksu_spawn_args *args)
{
	struct nksu_spawn *work;
	struct task_struct *tsk;
	DECLARE_COMPLETION_ONSTACK(done);
	int ret;

	if (!args || !args->path || !args->path[0])
		return -EINVAL;

	switch (args->wait) {
	case NKSU_SPAWN_NOWAIT:
	case NKSU_SPAWN_WAIT_EXEC:
	case NKSU_SPAWN_WAIT_PROC:
		break;
	default:
		return -EINVAL;
	}

	/* The helper runs module text after we return, so pin the module. */
	if (!try_module_get(THIS_MODULE))
		return -ENODEV;

	work = kzalloc(sizeof(*work), GFP_KERNEL);
	if (!work) {
		module_put(THIS_MODULE);
		return -ENOMEM;
	}

	work->path = kstrdup(args->path, GFP_KERNEL);
	if (!work->path)
		goto oom;

	work->argv = nksu_spawn_dup_strv(args->argv);
	if (!work->argv)
		goto oom;
	if (!work->argv[0]) {
		work->argv[0] = kstrdup(work->path, GFP_KERNEL);
		if (!work->argv[0])
			goto oom;
	}

	work->envp = nksu_spawn_dup_strv(args->envp);
	if (!work->envp)
		goto oom;

	work->domain = args->domain;
	work->wait = args->wait;
	work->ret = 0;
	work->done = (args->wait == NKSU_SPAWN_NOWAIT) ? NULL : &done;

	tsk = kthread_run(nksu_spawn_supervisor, work, "nksu-spawn");
	if (IS_ERR(tsk)) {
		ret = PTR_ERR(tsk);
		nksu_spawn_free(work);
		return ret;
	}

	if (args->wait == NKSU_SPAWN_NOWAIT)
		return 0;

	ret = wait_for_completion_killable(&done);
	if (ret) {
		/*
		 * Interrupted.  Claim the completion so the helper can no longer
		 * touch our stack frame: if we win the exchange we hand the work
		 * item to the helper (it frees it), otherwise it already
		 * completed and we still own it.
		 */
		if (xchg(&work->done, NULL))
			return -EINTR;
		wait_for_completion(&done);
	}

	ret = work->ret;
	nksu_spawn_free(work);
	return ret;

oom:
	nksu_spawn_free(work);
	return -ENOMEM;
}

#if defined(__clang__)
#pragma clang attribute pop
#endif
