// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * ncore tracepoint marking.
 *
 * ncore and everything it forks must stay visible to the kernel after exec.
 * Instead of hooking prctl and granting the control fd to every root process,
 * mark the executed ncore image and all of its descendants with
 * TIF_SYSCALL_TRACEPOINT.  A private NKSU_TIF_NCORE bit distinguishes this
 * group from zygote/app processes, which also carry the tracepoint flag.
 * A sched_process_exec probe recognises the ncore binary, marks it and
 * schedules a task_work that attaches the control fd before returning to
 * userspace.  sched_process_fork propagates both marks to children, so module
 * scripts started by ncore stay in the same kernel-visible group.
 *
 * The probe is registered at second_stage, before init execs ncore at
 * post-fs-data, and stays registered for the lifetime of the module.
 */
#include <linux/binfmts.h>
#include <linux/cred.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pid.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/task_work.h>
#include <linux/tracepoint.h>
#include <linux/types.h>
#include <linux/uidgid.h>

#include "klog.h"
#include "hook/handle.h"
#include "manager/ioctl.h"
#include "nksu_task.h"
#include "symbol/symbol_compat.h"

/*
 * task_work_add is reached through the kallsyms-resolved pointer; the module's
 * CFI type hash for that indirect call does not match the running kernel, so
 * disable CFI for this file (same as manager.c / symbol_compat.c).
 */
#if defined(__clang__)
#pragma clang attribute push(__attribute__((no_sanitize("cfi"))), apply_to = function)
#endif

static struct tracepoint *tp_sched_exec;
static struct tracepoint *tp_sched_fork;
static bool nksu_daemon_hooked;

/* task_work runs just before returning to ncore userspace, after exec. */
struct nksu_daemon_work {
	struct callback_head cb;
};

static void nksu_daemon_attach(struct callback_head *cb)
{
	struct nksu_daemon_work *work = container_of(cb, struct nksu_daemon_work, cb);
	int ret;

	ret = fmac_ctlfd_get();
	if (ret < 0)
		pr_err("[daemon] cannot attach control fd to pid %d: %d\n", current->pid, ret);
	kfree(work);
	module_put(THIS_MODULE);
}

static bool nksu_daemon_path(const char *filename)
{
	const char *base;

	if (!filename)
		return false;

	base = strrchr(filename, '/');
	base = base ? base + 1 : filename;

	return !strcmp(base, "ncore") || !strcmp(base, "libncore.so");
}

static void nksu_daemon_exec(void *data, struct task_struct *p, pid_t old_pid,
			     struct linux_binprm *bprm)
{
	struct nksu_daemon_work *work;

	if (!uid_eq(task_euid(p), GLOBAL_ROOT_UID))
		return;
	if (!nksu_daemon_path(bprm ? bprm->filename : NULL))
		return;

	set_tsk_thread_flag(p, TIF_SYSCALL_TRACEPOINT);
	set_tsk_thread_flag(p, NKSU_TIF_NCORE);
	nksu_path_install_task(p);
	pr_info("[daemon] marked %s (pid %d) as ncore\n", bprm ? bprm->filename : "?", p->pid);

	if (!try_module_get(THIS_MODULE)) {
		pr_warn("[daemon] cannot pin module for pid %d task_work\n", p->pid);
		return;
	}

	work = kzalloc(sizeof(*work), GFP_ATOMIC);
	if (!work) {
		module_put(THIS_MODULE);
		return;
	}
	init_task_work(&work->cb, nksu_daemon_attach);
	if (task_work_add(p, &work->cb, TWA_RESUME)) {
		module_put(THIS_MODULE);
		kfree(work);
	}
}

static void nksu_daemon_fork(void *data, struct task_struct *parent, struct task_struct *child)
{
	if (test_tsk_thread_flag(parent, TIF_SYSCALL_TRACEPOINT))
		set_tsk_thread_flag(child, TIF_SYSCALL_TRACEPOINT);
	if (nksu_task_is_ncore(parent))
		set_tsk_thread_flag(child, NKSU_TIF_NCORE);
}

/* Used by the manager scan to mark every thread of zygote. */
void nksu_mark_threads_by_pid(pid_t pid)
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

struct nksu_tp_find_ctx {
	const char *name;
	struct tracepoint **out;
};

static void nksu_tp_find_cb(struct tracepoint *tp, void *priv)
{
	struct nksu_tp_find_ctx *ctx = priv;

	if (*ctx->out)
		return;
	if (!strcmp(tp->name, ctx->name))
		*ctx->out = tp;
}

static struct tracepoint *nksu_find_tracepoint(const char *name)
{
	struct tracepoint *result = NULL;
	struct nksu_tp_find_ctx ctx = { .name = name, .out = &result };

	for_each_kernel_tracepoint(nksu_tp_find_cb, &ctx);
	return result;
}

int nksu_daemon_fd_init(void)
{
	int ret;

	if (nksu_daemon_hooked)
		return 0;

	tp_sched_exec = nksu_find_tracepoint("sched_process_exec");
	tp_sched_fork = nksu_find_tracepoint("sched_process_fork");
	if (!tp_sched_exec || !tp_sched_fork) {
		pr_err("[daemon] sched tracepoints not found\n");
		return -ENOENT;
	}

	ret = tracepoint_probe_register(tp_sched_exec, nksu_daemon_exec, NULL);
	if (ret) {
		pr_err("[daemon] register sched_process_exec failed: %d\n", ret);
		return ret;
	}

	ret = tracepoint_probe_register(tp_sched_fork, nksu_daemon_fork, NULL);
	if (ret) {
		pr_err("[daemon] register sched_process_fork failed: %d\n", ret);
		tracepoint_probe_unregister(tp_sched_exec, nksu_daemon_exec, NULL);
		return ret;
	}

	nksu_daemon_hooked = true;
	pr_info("[daemon] ncore tracepoint marking active\n");
	return 0;
}

void nksu_daemon_fd_exit(void)
{
	if (!nksu_daemon_hooked)
		return;

	tracepoint_probe_unregister(tp_sched_exec, nksu_daemon_exec, NULL);
	tracepoint_probe_unregister(tp_sched_fork, nksu_daemon_fork, NULL);
	tracepoint_synchronize_unregister();
	nksu_daemon_hooked = false;
	pr_info("[daemon] ncore tracepoint marking stopped\n");
}

#if defined(__clang__)
#pragma clang attribute pop
#endif
