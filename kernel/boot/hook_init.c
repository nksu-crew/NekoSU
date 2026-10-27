// SPDX-License-Identifier: GPL-3.0-or-later
#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/delay.h>
#include <linux/completion.h>
#include <linux/cred.h>
#include <linux/string.h>
#include <linux/tracepoint.h>
#include <linux/binfmts.h>
#include "hook/syscall.h"
#include "hook/dispatch.h"
#include "hook/hook.h"
#include "hook/tools.h"
#include "selinux/selinux.h"
#include "nksu.h"
#include "boot/init_rc.h"
#include "klog.h"

enum init_boot_stage {
	INIT_FIRST_STAGE,
	INIT_SELINUX_SETUP,
	INIT_SECOND_STAGE,
	INIT_STAGE_UNKNOWN,
};

int boot_stage = INIT_FIRST_STAGE;
// vendor ko load on first_stage, see line 446 on https://android.googlesource.com/platform/system/core/+/refs/heads/main/init/first_stage_init.cpp

/*
 * First-stage (vendor_boot) load: the module probes at init time, long
 * before SELinux policy, /data and the zygote exist.  A temporary syscall
 * watcher follows init through its boot stages and brings the real
 * components up at the right moment:
 *
 *   execve(id, "selinux_setup")  -> logged; the policy is loaded next
 *   execve(id, "second_stage")   -> SELinux Hook (domain + rules); the
 *                                   init.rc proxy is installed from the
 *                                   first init.rc read (see kernel/boot/init_rc.c)
 *   execve(app_process -Xzygote) -> feature components (profile, tracepoint,
 *                                   manager scan...).  Module loading is not
 *                                   started here: it is driven entirely by
 *                                   the injected init.rc, which execs the
 *                                   userspace loader at post-fs-data and
 *                                   late_start.  Then drop the watcher: it
 *                                   exits on its own once the last stage is
 *                                   handled (see stop_init_thread() for the
 *                                   early-exit path at module unload).
 *
 * Zygote ordering: Android init triggers `post-fs-data` *before*
 * `zygote-start` (both from `late-init` in the AOSP init.rc), and this device
 * follows that order, so /data is always mounted by the time zygote runs.
 * That is why the feature components (which need /data) are brought up on
 * zygote detection here, and not at second_stage, which runs before vold
 * mounts and decrypts /data.
 *
 * Late load just initializes everything at once (see core.c).
 */

static struct task_struct *init_thread;
static DECLARE_WAIT_QUEUE_HEAD(stage_wq);
static DECLARE_COMPLETION(init_done);
static bool stage_pending;
static bool init_exiting;
static bool zygote_seen;
static bool early_loaded;
static bool selinux_loaded;
static bool features_loaded;

static void notify_stage(void)
{
	if (init_thread) {
		WRITE_ONCE(stage_pending, true);
		wake_up_interruptible(&stage_wq);
	}
}

/* argv[1] of the current execve/execveat; returns length or < 0. */
static int read_argv1(struct pt_regs *regs, unsigned int argv_argno, char *buf, size_t len)
{
	return get_argvx(regs, argv_argno, 1, buf, len);
}

/*
 * Boot-stage exec targets, matched on the exec *path* before any argv is
 * read:
 *   - init (pid 1) re-execs itself (execve, not execveat) to advance its own
 *     stages; pid 1 is handled directly, so it needs no path filter;
 *   - the first zygote is /system/bin/app_process{,32,64} with "-Xzygote".
 *
 * Reading argv is the expensive part (a get_user for the pointer plus a
 * strncpy_from_user of up to MAX_ARG_LEN), and it would otherwise run for
 * every process until zygote appears.  Matching the path first -- a single
 * bounded copy of at most 32 bytes -- keeps that cost to the two exec
 * programs that can actually matter.
 */
#define NKSU_APP_PROCESS_PATH "/system/bin/app_process"
#define NKSU_EXEC_PATH_MAX 32

/* "/system/bin/app_process" with an optional 32/64 ABI suffix. */
static bool exec_path_is_app_process(const char *path, int n)
{
	const size_t base = sizeof(NKSU_APP_PROCESS_PATH) - 1;

	if (unlikely(n < (int)base))
		return false;
	if (memcmp(path, NKSU_APP_PROCESS_PATH, base) != 0)
		return false;

	return path[base] == '\0' || strcmp(path + base, "32") == 0 ||
	       strcmp(path + base, "64") == 0;
}

/*
 * init re-execs itself with execve() (not execveat): first "selinux_setup"
 * once the policy is about to be loaded, later "second_stage".  The first
 * zygote is exec'd even later, when /data is ready.
 */
static void watch_exec(struct pt_regs *regs, unsigned int argv_argno)
{
	char arg1[MAX_ARG_LEN];
	char path[NKSU_EXEC_PATH_MAX];
	int n;

	if (unlikely(current->pid == 1)) {
		n = read_argv1(regs, argv_argno, arg1, sizeof(arg1));
		if (n < 0) {
			pr_warn("nksu: get_argvx failed: %d\n", n);
			return;
		}

		if (argv_eq(arg1, n, sizeof(arg1), "selinux_setup")) {
			WRITE_ONCE(boot_stage, INIT_SELINUX_SETUP);
			pr_info("nksu: init entered selinux_setup\n");
			notify_stage();
		} else if (argv_eq(arg1, n, sizeof(arg1), "second_stage")) {
			WRITE_ONCE(boot_stage, INIT_SECOND_STAGE);
			pr_info("nksu: init entered second_stage\n");
			notify_stage();
		}
		return;
	}

	if (READ_ONCE(zygote_seen))
		return;

	/*
     * The zygote is the first app_process* exec'd by init while still root
     * (its service has `user root`; it only drops uid once it forks apps).
     * Reject everything else before touching user memory: most boot execs
     * are non-root services, and the rest fail the path match.
     */
	if (unlikely(!uid_eq(current_euid(), GLOBAL_ROOT_UID)))
		return;

	n = get_path_arg(regs, argv_argno - 1, path, sizeof(path));
	if (n < 0 || !exec_path_is_app_process(path, n))
		return;

	n = read_argv1(regs, argv_argno, arg1, sizeof(arg1));
	if (n > 0 && argv_eq(arg1, n, sizeof(arg1), "-Xzygote")) {
		WRITE_ONCE(zygote_seen, true);
		pr_info("nksu: zygote is starting\n");
		notify_stage();
	}
}

static long handle__NR_execve(struct pt_regs *regs)
{
	watch_exec(regs, 1);
	return 0;
}

static long handle__NR_execveat(struct pt_regs *regs)
{
	watch_exec(regs, 2);
	return 0;
}

/*
 * Stage-period-only watcher.  Unlike handle.c's steady-state hooks (which use
 * the sys_enter tracepoint + TIF_SYSCALL_TRACEPOINT), this one is driven from
 * the syscall table because it must be live from the first-stage (vendor_boot)
 * load, before any managed thread exists.  It follows init through
 * execve/execveat (to detect the boot stages); the slots are restored by
 * unload_temp_syscall() at the zygote stage.  The control-fd prctl grant lives
 * in manager/daemon.c and is armed earlier, at second_stage.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 17, 0)
static const struct syscall_hook boot_syscall_hooks[] = {
	{ __NR_execve, handle__NR_execve },
	{ __NR_execveat, handle__NR_execveat },
};

static bool temp_syscall_loaded;

int load_temp_syscall(void)
{
	int ret, i;

	/*
     * init and zygote are not in the profile, so the dispatcher must run
     * handlers unconditionally while the temporary watcher is installed.
     */
	nksu_dispatch_set_unconditional(true);

	for (i = 0; i < ARRAY_SIZE(boot_syscall_hooks); i++) {
		ret = nksu_redirect_syscall(boot_syscall_hooks[i].nr);
		if (ret) {
			pr_err("[hook]: can't redirect syscall %d: %d\n", boot_syscall_hooks[i].nr,
			       ret);
			goto err;
		}
	}

	for (i = 0; i < ARRAY_SIZE(boot_syscall_hooks); i++) {
		ret = nksu_register_handler(boot_syscall_hooks[i].nr,
					    boot_syscall_hooks[i].handler);
		if (ret) {
			pr_err("[hook]: can't register handler for syscall %d: %d\n",
			       boot_syscall_hooks[i].nr, ret);
			goto err;
		}
	}
	temp_syscall_loaded = true;
	return 0;

err:
	/* Undo a partial install; unhooking an unrecorded slot is a no-op. */
	for (i = 0; i < ARRAY_SIZE(boot_syscall_hooks); i++)
		nksu_unhook_syscall(boot_syscall_hooks[i].nr);
	nksu_dispatch_set_unconditional(false);
	return ret;
}

/*
 * Restore every slot the temporary watcher touched.  Called once, at the
 * zygote stage (or on the unload/error path); the flag keeps it idempotent.
 */
static void unload_temp_syscall(void)
{
	int i;

	if (!temp_syscall_loaded)
		return;
	temp_syscall_loaded = false;

	nksu_init_rc_exit();
	for (i = 0; i < ARRAY_SIZE(boot_syscall_hooks); i++)
		nksu_unhook_syscall(boot_syscall_hooks[i].nr);
	nksu_dispatch_set_unconditional(false);
}
#else
/*
 * 4.9 has no pt_regs-based syscall table entries, so the boot watcher uses the
 * sched_process_exec tracepoint instead.  It only has to spot the first zygote
 * (init's stage argv is not needed: the SELinux component waits for the policy
 * itself and is armed from init_thread_fn below).  The init.rc read/fstat
 * proxies are installed with native signatures by boot/init_rc.c.
 */
static struct tracepoint *tp_boot_exec;

struct nksu_boot_tp_ctx {
	const char *name;
	struct tracepoint **out;
};

static void nksu_boot_tp_cb(struct tracepoint *tp, void *priv)
{
	struct nksu_boot_tp_ctx *ctx = priv;

	if (*ctx->out)
		return;
	if (strcmp(tp->name, ctx->name) == 0)
		*ctx->out = tp;
}

static void nksu_boot_exec(void *data, struct task_struct *p, pid_t old_pid,
			   struct linux_binprm *bprm)
{
	const char *base;

	(void)data;
	(void)old_pid;

	if (!bprm || !bprm->filename || READ_ONCE(zygote_seen))
		return;
	if (p->pid == 1 || !uid_eq(task_euid(p), GLOBAL_ROOT_UID))
		return;

	base = strrchr(bprm->filename, '/');
	base = base ? base + 1 : bprm->filename;
	if (strncmp(base, "app_process", 11))
		return;

	WRITE_ONCE(zygote_seen, true);
	pr_info("nksu: zygote is starting\n");
	notify_stage();
}

static bool temp_syscall_loaded;

int load_temp_syscall(void)
{
	struct nksu_boot_tp_ctx ctx = { .name = "sched_process_exec", .out = &tp_boot_exec };
	int ret;

	for_each_kernel_tracepoint(nksu_boot_tp_cb, &ctx);
	if (!tp_boot_exec) {
		pr_err("[hook]: sched_process_exec tracepoint not found\n");
		return -ENOENT;
	}

	ret = tracepoint_probe_register(tp_boot_exec, nksu_boot_exec, NULL);
	if (ret) {
		pr_err("[hook]: cannot register boot exec probe: %d\n", ret);
		tp_boot_exec = NULL;
		return ret;
	}
	temp_syscall_loaded = true;
	return 0;
}

static void unload_temp_syscall(void)
{
	if (!temp_syscall_loaded)
		return;
	temp_syscall_loaded = false;

	nksu_init_rc_exit();
	if (tp_boot_exec) {
		tracepoint_probe_unregister(tp_boot_exec, nksu_boot_exec, NULL);
		tracepoint_synchronize_unregister();
		tp_boot_exec = NULL;
	}
}
#endif

static int init_thread_fn(void *data)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 17, 0)
	/*
	 * No init-stage argv to watch on 4.9: arm the early components and the
	 * SELinux hook right away (the latter waits for the policy itself),
	 * then only wait for zygote to bring the feature group up.
	 */
	if (!early_loaded && nksu_init_early_components() == 0)
		early_loaded = true;
	if (!selinux_loaded && nksu_init_selinux_components() == 0)
		selinux_loaded = true;
#endif

	/*
     * The watcher follows init until the zygote starts; at that point every
     * component is up and there is nothing left to wait for, so it leaves on
     * its own.  hook_exit() asks it to leave earlier by setting init_exiting.
     * Either way it signals init_done before returning so the unload path can
     * wait for it -- see stop_init_thread() for why we never kthread_stop().
     */
	while (!READ_ONCE(init_exiting)) {
		wait_event_interruptible(stage_wq,
					 READ_ONCE(stage_pending) || READ_ONCE(init_exiting));
		if (READ_ONCE(init_exiting))
			break;

		WRITE_ONCE(stage_pending, false);

		/*
         * The policy is loaded during selinux_setup, so by second_stage we
         * can inject the nksu domain and its rules.  The early group (safe
         * mode) needs neither the policy nor /data, so it is armed here too --
         * before post-fs-data, where ncore first queries safe mode.
         * Do NOT start the module stage here: on FBE devices init's
         * second_stage runs *before* vold mounts and decrypts /data, so
         * /data/adb/modules does not exist yet (this is what made every module
         * look absent).
         */
		if (READ_ONCE(boot_stage) == INIT_SECOND_STAGE) {
			if (!early_loaded) {
				if (nksu_init_early_components() == 0)
					early_loaded = true;
				else
					pr_err("nksu: early component init failed\n");
			}
			if (!selinux_loaded) {
				if (nksu_init_selinux_components() == 0)
					selinux_loaded = true;
				else
					pr_err("nksu: SELinux hook init failed\n");
			}
		}

		if (READ_ONCE(zygote_seen) && !features_loaded) {
			/*
             * Drop the temporary boot watcher first: the steady-state hooks
             * (and the daemon fd grant) install into the same slots.
             */
			unload_temp_syscall();

			/* Let the execve() that announced zygote settle. */
			msleep(100);

			if (!selinux_loaded && nksu_init_selinux_components() == 0)
				selinux_loaded = true;

			if (nksu_init_feature_components() == 0)
				features_loaded = true;
			else
				pr_err("nksu: feature init failed\n");

			/* Zygote was the last boot stage to watch for. */
			break;
		}
	}

	complete(&init_done);
	return 0;
}

static int start_init_thread(void)
{
	reinit_completion(&init_done);
	init_thread = kthread_run(init_thread_fn, NULL, "nksu-init");
	if (IS_ERR(init_thread)) {
		int ret = PTR_ERR(init_thread);
		init_thread = NULL;
		pr_err("nksu: create init thread failed: %d\n", ret);
		return ret;
	}
	return 0;
}

static void stop_init_thread(void)
{
	if (!init_thread)
		return;

	/*
     * The watcher may already have exited on its own after the zygote stage,
     * so kthread_stop() would dereference a freed task_struct.  Ask it to
     * leave and wait for it to signal init_done instead; once complete() has
     * run no further module code executes, so it is safe to tear down.
     */
	WRITE_ONCE(init_exiting, true);
	wake_up_interruptible(&stage_wq);
	wait_for_completion(&init_done);
	init_thread = NULL;
}

int hook_init(void)
{
	int ret;

	ret = load_temp_syscall();
	if (ret)
		goto err_dispatch;

	ret = start_init_thread();
	if (ret)
		goto err_dispatch;

	/*
     * Inject the KernelSU-style init.rc rc (read/fstat proxy) and write the
     * userspace module loader.  This is not fatal: without it init.rc is
     * untouched and modules simply do not come up.
     */
	ret = nksu_init_rc_init();
	if (ret) {
		pr_err("nksu: init.rc injection failed: %d\n", ret);
		goto err_thread;
	}

	return 0;

err_thread:
	stop_init_thread();

err_dispatch:
	unload_temp_syscall();
	return ret;
}

void hook_exit(void)
{
	stop_init_thread();

	if (READ_ONCE(features_loaded))
		nksu_exit_feature_components();

	if (READ_ONCE(selinux_loaded))
		nksu_exit_selinux_components();

	if (READ_ONCE(early_loaded))
		nksu_exit_early_components();

	unload_temp_syscall();
}
