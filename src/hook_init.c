#include <linux/kthread.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include "syscall.h"
#include "dispatch.h"
#include "tools.h"
#include "selinux/selinux.h"
#include "klog.h"

enum init_boot_stage {
    INIT_FIRST_STAGE,
    INIT_SELINUX_SETUP,
    INIT_SECOND_STAGE,
    INIT_STAGE_UNKNOWN,
};

int boot_stage = INIT_FIRST_STAGE;
// vendor ko load on first_stage, see line 446 on https://android.googlesource.com/platform/system/core/+/refs/heads/main/init/first_stage_init.cpp

static struct task_struct *unload_thread;
static DECLARE_COMPLETION(second_stage);

void request_unload(void)
{
    if (unload_thread)
        complete(&second_stage);
}

long handle__NR_execveat(struct pt_regs *regs)
{
    char argv1[MAX_ARG_LEN];
    int a1;

    if (current->pid != 1)
        return 0;

    a1 = get_argvx(regs, 2, 1, argv1, sizeof(argv1));

    if (a1 < 0) {
        pr_warn("nksu: get_argvx failed: %d\n", a1);
        return 0;
    }
    if (argv_eq(argv1, a1, sizeof(argv1), "second_stage")) {
        WRITE_ONCE(boot_stage, INIT_SECOND_STAGE);
        request_unload();
    }
    return 0;
}

struct syscall_hook {
    int nr;
    long (*handler)(struct pt_regs *);
};

static const struct syscall_hook syscall_hooks[] = {
    { __NR_execveat, handle__NR_execveat },
};

int load_temp_syscall(void)
{
    int ret, i;
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

static int unload_thread_fn(void *data)
{
    wait_for_completion(&second_stage);

    if (kthread_should_stop())
        return 0;

    pr_info("nksu: init run on second stage, load SELinux hook\n");
    init_selinux_hook();
    nksu_dispatch_exit();

    return 0;
}

int start_unload_thread(void)
{
    unload_thread = kthread_run(unload_thread_fn, NULL, "nksu_unload");
    if (IS_ERR(unload_thread)) {
        int ret = PTR_ERR(unload_thread);
        unload_thread = NULL;
        pr_err("nksu: create unload thread failed: %d\n", ret);
        return ret;
    }
    return 0;
}

static void stop_unload_thread(void)
{
    if (unload_thread) {
        complete(&second_stage);
        kthread_stop(unload_thread);
        unload_thread = NULL;
    }
}

int hook_init(void)
{
    int ret =0;
    ret = nksu_dispatch_init();
    if (ret)
        return ret;

    ret = load_temp_syscall();
    if (ret)
        goto err_dispatch;

    ret = start_unload_thread();
    if (ret)
        goto err_syscall;

    return 0;

err_syscall:
err_dispatch:
    nksu_dispatch_exit();
    return ret;
}

void hook_exit(void)
{
    stop_unload_thread();
    selinux_exit();
}