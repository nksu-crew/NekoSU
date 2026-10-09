#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/delay.h>
#include "syscall.h"
#include "dispatch.h"
#include "tools.h"
#include "selinux/selinux.h"
#include "nksu.h"
#include "nksu_module.h"
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
 *   execve(id, "second_stage")   -> SELinux Hook
 *   execve(app_process -Xzygote) -> feature components (profile, tracepoint,
 *                                   manager scan...), then drop the watcher
 *
 * Late load just initializes everything at once (see nksu.c).
 */

static struct task_struct *init_thread;
static DECLARE_WAIT_QUEUE_HEAD(stage_wq);
static bool stage_pending;
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

struct syscall_hook {
    int nr;
    long (*handler)(struct pt_regs *);
};

static const struct syscall_hook syscall_hooks[] = {
    { __NR_execve, handle__NR_execve },
    { __NR_execveat, handle__NR_execveat },
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
     * The loop must never return on its own: a kthread that exits while
     * the module still holds its task_struct makes the later kthread_stop()
     * dereference freed memory.  It only leaves when kthread_stop() asks.
     */
    while (!kthread_should_stop()) {
        wait_event_interruptible(stage_wq,
                                 READ_ONCE(stage_pending) ||
                                 kthread_should_stop());
        if (kthread_should_stop())
            break;

        WRITE_ONCE(stage_pending, false);

        if (READ_ONCE(boot_stage) == INIT_SECOND_STAGE && !selinux_loaded) {
            if (nksu_init_selinux_components() == 0) {
                selinux_loaded = true;
                /*
                 * The nksu SELinux domain now exists.  /data is *not* mounted
                 * yet on FBE devices (that happens during second_stage), so
                 * the module loader waits for the tree itself before reading
                 * it.
                 */
                nksu_modules_post_fs_data();
            } else {
                pr_err("nksu: SELinux hook init failed\n");
            }
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
        }
    }
    return 0;
}

static int start_init_thread(void)
{
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
    if (init_thread) {
        kthread_stop(init_thread); /* sets KTHREAD_SHOULD_STOP and wakes us */
        init_thread = NULL;
    }
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

    return 0;

err_dispatch:
    nksu_dispatch_exit();
    return ret;
}

void hook_exit(void)
{
    stop_init_thread();

    if (READ_ONCE(features_loaded))
        nksu_exit_feature_components();
    else
        /* post-fs-data may already have started the module loader. */
        nksu_modules_exit();

    if (READ_ONCE(selinux_loaded))
        nksu_exit_selinux_components();

    nksu_dispatch_exit();
}
