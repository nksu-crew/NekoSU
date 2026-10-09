// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * nksu -- KernelSU-style init.rc injection, adapted for a daemon-less module.
 *
 * KernelSU hooks init's read() of /system/etc/init/hw/init.rc through the
 * syscall table (__NR_read / __NR_fstat) and appends a static rc that execs
 * its userspace daemon ksud at the well-defined boot stages.  nksu has no
 * daemon, so instead the appended rc execs a small script that nksu writes
 * into a temporary directory it creates:
 *
 *   on post-fs-data
 *       exec u:r:nksu:s0 root -- /system/bin/sh /dev/nksu/stage.sh post-fs-data
 *   on property:vold.decrypt=trigger_restart_framework
 *       exec u:r:nksu:s0 root -- /system/bin/sh /dev/nksu/stage.sh services
 *   on property:sys.boot_completed=1
 *       exec u:r:nksu:s0 root -- /system/bin/sh /dev/nksu/stage.sh boot-completed
 *
 * Running in the unconfined nksu domain (see selinux/policy.c), the script
 * writes the stage name into /dev/nksu/, which the kernel watcher polls.  That
 * is the daemon-less replacement for `ksud post-fs-data`: init itself tells us
 * when the boot stage is reached, at exactly the point KernelSU relies on.
 *
 * The read proxy is a straight port of ksu_install_rc_hook(): the first read()
 * of /system/etc/init/hw/init.rc replaces the file's file_operations with a
 * proxy that, once the original read hits EOF, appends the static rc.  fstat
 * is hooked too so the reported file size includes the appended text.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/dcache.h>
#include <linux/namei.h>
#include <linux/uaccess.h>
#include <linux/uio.h>
#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/delay.h>
#include <linux/version.h>
#include <linux/sched.h>
#include <linux/cred.h>
#include <linux/stat.h>
#include <linux/fcntl.h>
#include <linux/printk.h>
#include <asm/current.h>
#include <asm/ptrace.h>
#include <asm/unistd.h>

#include <fmac.h>
#include "init_rc.h"
#include "syscall.h"
#include "nksu_module.h"

/*
 * The injected rc and the staging files it references.  The directory lives
 * on /dev (tmpfs, mounted by init's first stage before we probe), so it exists
 * long before `on post-fs-data` runs.
 */
#define NKSU_RC_DIR "/dev/nksu"
#define NKSU_RC_SCRIPT NKSU_RC_DIR "/stage.sh"
#define NKSU_RC_MARK_POST_FS_DATA NKSU_RC_DIR "/post-fs-data"
#define NKSU_RC_MARK_SERVICES NKSU_RC_DIR "/services"
#define NKSU_RC_MARK_BOOT_COMPLETED NKSU_RC_DIR "/boot-completed"
#define NKSU_RC_INIT_PATH "/system/etc/init/hw/init.rc"
#define NKSU_RC_INIT_PATH_LEGACY "/system/etc/init.rc"

/* nksu domain; see selinux/selinux.h */
#define NKSU_RC_CONTEXT DOMAIN_CTX

static const char nksu_rc[] =
    "\n"
    "on post-fs-data\n"
    "    exec " NKSU_RC_CONTEXT " root -- /system/bin/sh " NKSU_RC_SCRIPT " post-fs-data\n"
    "\n"
    "on nonencrypted\n"
    "    exec " NKSU_RC_CONTEXT " root -- /system/bin/sh " NKSU_RC_SCRIPT " services\n"
    "\n"
    "on property:vold.decrypt=trigger_restart_framework\n"
    "    exec " NKSU_RC_CONTEXT " root -- /system/bin/sh " NKSU_RC_SCRIPT " services\n"
    "\n"
    "on property:sys.boot_completed=1\n"
    "    exec " NKSU_RC_CONTEXT " root -- /system/bin/sh " NKSU_RC_SCRIPT " boot-completed\n"
    "\n";

/* The stage script writes its argument (the stage name) into the temp dir. */
static const char nksu_rc_script[] =
    "#!/system/bin/sh\n"
    "echo 1 > " NKSU_RC_DIR "/$1\n";

static const size_t nksu_rc_len = sizeof(nksu_rc) - 1;
static ssize_t nksu_rc_pos;
static bool nksu_rc_hooked;
static bool nksu_rc_prepared;

/* f_op proxy state, mirroring KernelSU's fops_proxy */
static struct file_operations nksu_fops_proxy;
static ssize_t (*nksu_orig_read)(struct file *, char __user *, size_t, loff_t *);
static ssize_t (*nksu_orig_read_iter)(struct kiocb *, struct iov_iter *);

/*
 * None of getname_kernel(), do_mkdirat() or putname() is part of the GKI
 * KMI, and vfs_mkdir() changed its first argument across the KMI range we
 * build for, so resolve the three through the module's own kallsyms scanner
 * (like every other unexported symbol).  do_mkdirat() has kept a stable
 * signature across those kernels and hides the vfs_mkdir() differences.
 */
typedef struct filename *(*nksu_getname_kernel_t)(const char *);
typedef long (*nksu_do_mkdirat_t)(int, struct filename *, umode_t);
typedef void (*nksu_putname_t)(struct filename *);

static nksu_getname_kernel_t nksu_getname_kernel;
static nksu_do_mkdirat_t nksu_do_mkdirat;
static nksu_putname_t nksu_putname;

/* saved originals for the two syscall-table hooks */
static syscall_fn_t nksu_orig_read_sys;
static syscall_fn_t nksu_orig_fstat_sys;

static struct task_struct *nksu_rc_watch_thread;

/*
 * These resolve into foreign function pointers and are called with the
 * compiler's CFI type check disabled, like spawn.c.
 */
#if defined(__clang__)
#pragma clang attribute push(__attribute__((no_sanitize("cfi"))), apply_to=function)
#endif

static int nksu_rc_mkdir(const char *path)
{
    struct filename *name;
    long ret;

    if (!nksu_getname_kernel || !nksu_do_mkdirat || !nksu_putname)
        return -ENOSYS;

    name = nksu_getname_kernel(path);
    if (IS_ERR(name))
        return PTR_ERR(name);

    ret = nksu_do_mkdirat(AT_FDCWD, name, 0700);
    nksu_putname(name);

    if (ret == -EEXIST)
        return 0;
    return ret < 0 ? (int)ret : 0;
}

#if defined(__clang__)
#pragma clang attribute pop
#endif

/* Same match as KernelSU's is_init_rc(): only init, only the hw init.rc. */
static bool nksu_rc_is_init_rc(struct file *file)
{
    char buf[256];
    char *dpath;
    const char *short_name;

    if (strcmp(current->comm, "init"))
        return false;

    if (!file->f_path.dentry || !d_is_reg(file->f_path.dentry))
        return false;

    short_name = file->f_path.dentry->d_name.name;
    if (!short_name || strcmp(short_name, "init.rc"))
        return false;

    dpath = d_path(&file->f_path, buf, sizeof(buf));
    if (IS_ERR(dpath))
        return false;

    return !strcmp(dpath, NKSU_RC_INIT_PATH) ||
           !strcmp(dpath, NKSU_RC_INIT_PATH_LEGACY);
}

static int nksu_rc_write_file(const char *path, const char *data, size_t len)
{
    struct file *f;
    loff_t pos = 0;
    ssize_t n;

    f = filp_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0700);
    if (IS_ERR(f))
        return PTR_ERR(f);

    n = kernel_write(f, data, len, &pos);
    filp_close(f, NULL);

    return n < 0 ? (int)n : 0;
}

/*
 * Create the temp directory and the stage script.  Called the first time init
 * is seen reading init.rc, i.e. at second stage, long after /dev exists and
 * well before `on post-fs-data`.
 */
static void nksu_rc_prepare(void)
{
    int ret;

    if (nksu_rc_prepared)
        return;
    nksu_rc_prepared = true;

    ret = nksu_rc_mkdir(NKSU_RC_DIR);
    if (ret && ret != -EEXIST)
        pr_warn("nksu: cannot create %s: %d\n", NKSU_RC_DIR, ret);

    ret = nksu_rc_write_file(NKSU_RC_SCRIPT, nksu_rc_script,
                             sizeof(nksu_rc_script) - 1);
    if (ret)
        pr_warn("nksu: cannot write %s: %d\n", NKSU_RC_SCRIPT, ret);
}

/* append the rc once the original read has reached EOF (KernelSU read_proxy) */
static ssize_t nksu_rc_read_proxy(struct file *file, char __user *buf, size_t count, loff_t *pos)
{
    ssize_t ret = 0;
    size_t append_count;

    if (nksu_rc_pos && nksu_rc_pos < (ssize_t)nksu_rc_len)
        goto append_rc;

    ret = nksu_orig_read(file, buf, count, pos);
    if (ret != 0)
        return ret;
    if (nksu_rc_pos >= (ssize_t)nksu_rc_len)
        return ret;

append_rc:
    if (nksu_rc_pos < (ssize_t)nksu_rc_len) {
        append_count = nksu_rc_len - (size_t)nksu_rc_pos;
        if (append_count > count - (size_t)ret)
            append_count = count - (size_t)ret;
        if (copy_to_user(buf + ret, nksu_rc + nksu_rc_pos, append_count))
            return ret;
        nksu_rc_pos += append_count;
        ret += append_count;
    }

    return ret;
}

static ssize_t nksu_rc_read_iter_proxy(struct kiocb *iocb, struct iov_iter *to)
{
    ssize_t ret = 0;
    size_t append_count;

    if (nksu_rc_pos && nksu_rc_pos < (ssize_t)nksu_rc_len)
        goto append_rc;

    ret = nksu_orig_read_iter(iocb, to);
    if (ret != 0)
        return ret;
    if (nksu_rc_pos >= (ssize_t)nksu_rc_len)
        return ret;

append_rc:
    if (nksu_rc_pos < (ssize_t)nksu_rc_len) {
        append_count = copy_to_iter(nksu_rc + nksu_rc_pos,
                                    nksu_rc_len - (size_t)nksu_rc_pos, to);
        if (!append_count)
            return ret;
        nksu_rc_pos += append_count;
        ret += append_count;
    }

    return ret;
}

/* install the read proxy on the first init.rc read (KernelSU ksu_install_rc_hook) */
static void nksu_rc_install(struct file *file)
{
    if (!nksu_rc_is_init_rc(file))
        return;

    if (nksu_rc_hooked)
        return;
    nksu_rc_hooked = true;

    nksu_rc_prepare();

    memcpy(&nksu_fops_proxy, file->f_op, sizeof(struct file_operations));

    nksu_orig_read = file->f_op->read;
    if (nksu_orig_read)
        nksu_fops_proxy.read = nksu_rc_read_proxy;

    nksu_orig_read_iter = file->f_op->read_iter;
    if (nksu_orig_read_iter)
        nksu_fops_proxy.read_iter = nksu_rc_read_iter_proxy;

    file->f_op = &nksu_fops_proxy;

    pr_info("nksu: init.rc hooked, appending %zu bytes\n", nksu_rc_len);
}

/* __NR_read: detect init reading init.rc before the real read runs */
static long nksu_sys_read(const struct pt_regs *regs)
{
    unsigned int fd = (unsigned int)regs->regs[0];
    struct file *file = fget(fd);

    if (file) {
        nksu_rc_install(file);
        fput(file);
    }

    return nksu_orig_read_sys(regs);
}

/* __NR_fstat: report the appended size (KernelSU ksu_sys_fstat) */
static long nksu_sys_fstat(const struct pt_regs *regs)
{
    unsigned int fd = (unsigned int)regs->regs[0];
    void __user *statbuf = (void __user *)regs->regs[1];
    bool is_rc = false;
    struct file *file = fget(fd);
    long ret;

    if (file) {
        if (nksu_rc_is_init_rc(file)) {
            nksu_rc_prepare();
            is_rc = true;
        }
        fput(file);
    }

    ret = nksu_orig_fstat_sys(regs);

    if (is_rc && ret == 0) {
        void __user *st_size_ptr = statbuf + offsetof(struct stat, st_size);
        long size, new_size;

        if (!copy_from_user(&size, st_size_ptr, sizeof(long))) {
            new_size = size + (long)nksu_rc_len;
            if (copy_to_user(st_size_ptr, &new_size, sizeof(long)))
                pr_warn("nksu: cannot patch init.rc size\n");
        }
    }

    return ret;
}

/* ---- marker watcher ---- */

static bool nksu_rc_marker_seen(const char *path)
{
    struct file *f = filp_open(path, O_RDONLY, 0);

    if (IS_ERR(f))
        return false;
    filp_close(f, NULL);
    return true;
}

static int nksu_rc_watch_fn(void *data)
{
    bool post = false, serv = false, boot = false;
    int i;

    for (i = 0; i < 900 && !kthread_should_stop(); i++) {
        if (!post && nksu_rc_marker_seen(NKSU_RC_MARK_POST_FS_DATA)) {
            post = true;
            pr_info("nksu: init.rc reached post-fs-data\n");
            nksu_modules_post_fs_data();
        }

        if (!serv && nksu_rc_marker_seen(NKSU_RC_MARK_SERVICES)) {
            serv = true;
            pr_info("nksu: init.rc reached services\n");
            nksu_modules_service();
        }

        if (!boot && nksu_rc_marker_seen(NKSU_RC_MARK_BOOT_COMPLETED)) {
            boot = true;
            pr_info("nksu: init.rc reached boot-completed\n");
        }

        if (post && serv && boot)
            break;

        msleep(100);
    }

    return 0;
}

bool nksu_rc_injected(void)
{
    return READ_ONCE(nksu_rc_hooked);
}

int nksu_init_rc_init(void)
{
    int ret;

    nksu_getname_kernel = (nksu_getname_kernel_t)nksu_ksym_lookup("getname_kernel");
    nksu_do_mkdirat = (nksu_do_mkdirat_t)nksu_ksym_lookup("do_mkdirat");
    nksu_putname = (nksu_putname_t)nksu_ksym_lookup("putname");

    if (!nksu_getname_kernel || !nksu_do_mkdirat || !nksu_putname)
        pr_warn("nksu: cannot resolve VFS mkdir helpers, using fallback\n");

    ret = hook_save(__NR_read, nksu_sys_read, &nksu_orig_read_sys, "nksu_rc_read");
    if (ret) {
        pr_err("nksu: cannot hook __NR_read: %d\n", ret);
        return ret;
    }

    ret = hook_save(__NR_fstat, nksu_sys_fstat, &nksu_orig_fstat_sys, "nksu_rc_fstat");
    if (ret) {
        pr_err("nksu: cannot hook __NR_fstat: %d\n", ret);
        return ret;
    }

    nksu_rc_watch_thread = kthread_run(nksu_rc_watch_fn, NULL, "nksu-rc");
    if (IS_ERR(nksu_rc_watch_thread)) {
        ret = PTR_ERR(nksu_rc_watch_thread);
        nksu_rc_watch_thread = NULL;
        pr_err("nksu: cannot start init.rc watcher: %d\n", ret);
        return ret;
    }

    return 0;
}

void nksu_init_rc_exit(void)
{
    if (nksu_rc_watch_thread) {
        kthread_stop(nksu_rc_watch_thread);
        nksu_rc_watch_thread = NULL;
    }

    /*
     * The read/fstat syscall hops are torn down together with the temporary
     * boot watcher (nksu_dispatch_exit -> syscalltable_exit), so there is
     * nothing to unhook here.
     */
}
