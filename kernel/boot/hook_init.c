#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/delay.h>
#include <linux/completion.h>
#include <linux/cred.h>
#include "hook/syscall.h"
#include "hook/dispatch.h"
#include "hook/tools.h"
#include "manager/ioctl.h"
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
 * Late load just initializes everything at once (see core.c).
 */

static struct task_struct *init_thread;
static DECLARE_WAIT_QUEUE_HEAD(stage_wq);
static DECLARE_COMPLETION(init_done);
static bool stage_pending;
static bool init_exiting;
static bool zygote_seen;
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
static int read_argv1(struct pt_regs *regs, unsigned int argv_argno,
                      char *buf, size_t len)
{
    return get_argvx(regs, argv_argno, 1, buf, len);
}

/*
 * init re-execs itself with execve() (not execveat): first "selinux_setup"
 * once the policy is about to be loaded, later "second_stage".  The first
 * zygote is exec'd even later, when /data is ready.
 */
static void watch_exec(struct pt_regs *regs, unsigned int argv_argno)
{
    char arg1[MAX_ARG_LEN];
    int n;

    if (current->pid == 1) {
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

    if (!READ_ONCE(zygote_seen)) {
        n = read_argv1(regs, argv_argno, arg1, sizeof(arg1));
        if (n > 0 && argv_eq(arg1, n, sizeof(arg1), "-Xzygote")) {
            WRITE_ONCE(zygote_seen, true);
            pr_info("nksu: zygote is starting\n");
            notify_stage();
        }
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
 * Daemon control fd: the boot-time ncore runs as root and cannot call the
 * manager-gated prctl 203, so grant it the [fmac_ctl] fd here.  The temporary
 * watcher dispatches unconditionally, so this reaches the daemon even though
 * it is not in the profile.
 */
static long handle__NR_prctl_daemon(struct pt_regs *regs)
{
#if defined(__aarch64__)
    unsigned long option = regs->regs[0];
#elif defined(__x86_64__)
    unsigned long option = regs->di;
#else
    unsigned long option = 0;
#endif

    if (option != NKSU_PRCTL_GET_DRIVER_FD)
        return 0;

    if (!uid_eq(current_uid(), GLOBAL_ROOT_UID) &&
        !uid_eq(current_euid(), GLOBAL_ROOT_UID))
        return 0;

    fmac_ctlfd_get();
    return 1;
}

struct syscall_hook {
    int nr;
    long (*handler)(struct pt_regs *);
};

static const struct syscall_hook syscall_hooks[] = {
    { __NR_execve, handle__NR_execve },
    { __NR_execveat, handle__NR_execveat },
    { __NR_prctl, handle__NR_prctl_daemon },
};

int load_temp_syscall(void)
{
    int ret, i;

    /*
     * init and zygote are not in the profile, so the dispatcher must run
     * handlers unconditionally while the temporary watcher is installed.
     */
    nksu_dispatch_set_unconditional(true);

    for (i = 0; i < ARRAY_SIZE(syscall_hooks); i++) {
        ret = nksu_redirect_syscall(syscall_hooks[i].nr);
        if (ret) {
            pr_err("[hook]: can't redirect syscall %d: %d\n", syscall_hooks[i].nr, ret);
            return ret;
        }
    }

    for (i = 0; i < ARRAY_SIZE(syscall_hooks); i++) {
        ret = nksu_register_handler(syscall_hooks[i].nr, syscall_hooks[i].handler);
        if (ret) {
            pr_err("[hook]: can't register handler for syscall %d: %d\n", syscall_hooks[i].nr, ret);
            return ret;
        }
    }
    return 0;
}

static int init_thread_fn(void *data)
{
    /*
     * The watcher follows init until the zygote starts; at that point every
     * component is up and there is nothing left to wait for, so it leaves on
     * its own.  hook_exit() asks it to leave earlier by setting init_exiting.
     * Either way it signals init_done before returning so the unload path can
     * wait for it -- see stop_init_thread() for why we never kthread_stop().
     */
    while (!READ_ONCE(init_exiting)) {
        wait_event_interruptible(stage_wq,
                                 READ_ONCE(stage_pending) ||
                                 READ_ONCE(init_exiting));
        if (READ_ONCE(init_exiting))
            break;

        WRITE_ONCE(stage_pending, false);

        /*
         * The policy is loaded during selinux_setup, so by second_stage we
         * can inject the nksu domain and its rules.  Do NOT start the module
         * stage here: on FBE devices init's second_stage runs *before* vold
         * mounts and decrypts /data, so /data/adb/modules does not exist yet
         * (this is what made every module look absent).
         */
        if (READ_ONCE(boot_stage) == INIT_SECOND_STAGE && !selinux_loaded) {
            if (nksu_init_selinux_components() == 0)
                selinux_loaded = true;
            else
                pr_err("nksu: SELinux hook init failed\n");
        }

        if (READ_ONCE(zygote_seen) && !features_loaded) {
            /*
             * Drop the temporary boot watcher first: the real syscall
             * hooks reuse the same dispatcher tables.
             */
            nksu_dispatch_exit();

            /* Let the execve() that announced zygote settle. */
            msleep(100);

            if (!selinux_loaded &&
                nksu_init_selinux_components() == 0)
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

    ret = nksu_dispatch_init();
    if (ret)
        return ret;

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
    nksu_dispatch_exit();
    return ret;
}

void hook_exit(void)
{
    nksu_init_rc_exit();
    stop_init_thread();

    if (READ_ONCE(features_loaded))
        nksu_exit_feature_components();

    if (READ_ONCE(selinux_loaded))
        nksu_exit_selinux_components();

    nksu_dispatch_exit();
}
