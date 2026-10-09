// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * nksu -- KernelSU-style init.rc injection.
 *
 * KernelSU hooks init's read() of /system/etc/init/hw/init.rc through the
 * syscall table (__NR_read / __NR_fstat) and appends a static rc that execs
 * its userspace daemon ksud at the well-defined boot stages.  nksu has no
 * daemon, but it does the same trick: the appended rc execs the module loader
 * nksu writes into /dev/nksu.
 *
 *   on post-fs-data
 *       exec u:r:nksu:s0 root -- /system/bin/sh /dev/nksu/modules.sh post-fs-data
 *   on nonencrypted
 *   on property:vold.decrypt=trigger_restart_framework
 *       exec u:r:nksu:s0 root -- /system/bin/sh /dev/nksu/modules.sh late_start
 *
 * The loader itself does the module enumeration and runs the hooks in
 * userspace (see src/module.c and the script embedded below); the kernel only
 * edits init's view of init.rc.  Because init's `exec` is synchronous, the
 * post-fs-data hooks (metamodule mount included) finish before init continues,
 * so modules are mounted before zygote/system_server start.  This replaces the
 * previous kernel-side loader, whose marker watcher had to poll /dev/nksu and
 * guess when the stages had been reached.
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
#include <linux/version.h>
#include <linux/sched.h>
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

#define NKSU_RC_INIT_PATH "/system/etc/init/hw/init.rc"
#define NKSU_RC_INIT_PATH_LEGACY "/system/etc/init.rc"

/* nksu domain; see selinux/selinux.h */
#define NKSU_RC_CONTEXT DOMAIN_CTX

/* The rc that init parses; it only has to reach the loader at each stage. */
static const char nksu_rc[] =
    "\n"
    "on post-fs-data\n"
    "    exec " NKSU_RC_CONTEXT " root -- /system/bin/sh " NKSU_LOADER_SCRIPT " post-fs-data\n"
    "\n"
    "on nonencrypted\n"
    "    exec " NKSU_RC_CONTEXT " root -- /system/bin/sh " NKSU_LOADER_SCRIPT " late_start\n"
    "\n"
    "on property:vold.decrypt=trigger_restart_framework\n"
    "    exec " NKSU_RC_CONTEXT " root -- /system/bin/sh " NKSU_LOADER_SCRIPT " late_start\n"
    "\n";

/*
 * The module loader.  It is written into /dev/nksu before `on post-fs-data`
 * runs and does all the work KernelSU's ksud would do here: apply each
 * module's sepolicy.rule through /proc/nksu/sepolicy, run post-fs-data.d /
 * service.d, run the metamodule and regular hooks, and drive the metamodule
 * mount.  The two stages are guarded by marker files so both `on nonencrypted`
 * and `on property:vold.decrypt=...` cannot run late_start twice.
 *
 * (Generated from userspace/loader/modules.sh; keep the two in sync.)
 */
static const char nksu_rc_script[] =
    "#!/system/bin/sh\n"
    "# nksu userspace module loader.\n"
    "#\n"
    "# This file is embedded verbatim into src/init_rc.c (nksu_rc_script); keep the\n"
    "# two in sync -- the kernel writes this text to /dev/nksu/modules.sh at boot.\n"
    "#\n"
    "# Exec'd by the rc that nksu injects into init.rc (see src/init_rc.c) at the\n"
    "# well-defined boot stages.  It replaces the old kernel-side loader: the\n"
    "# enumeration, the boot-hook ordering and the metamodule mount all happen\n"
    "# here, exactly like KernelSU's ksud but without a resident daemon.\n"
    "#\n"
    "#   post-fs-data   post-fs-data.d, each module's sepolicy.rule, the\n"
    "#                  metamodule's post-fs-data.sh, the regular modules'\n"
    "#                  post-fs-data.sh, then the metamodule's metamount.sh\n"
    "#   late_start     service.d, metamodule service.sh, regular service.sh\n"
    "#                  (all detached: a service.sh may keep a daemon)\n"
    "#\n"
    "# sepolicy.rule cannot be applied from shell, so each file path is handed to\n"
    "# the kernel through /proc/nksu/sepolicy (see selinux/rule_file.c).\n"
    "\n"
    "STAGE=\"$1\"\n"
    "export PATH=/sbin:/system/sbin:/system/bin:/system/xbin\n"
    "export ANDROID_ROOT=/system\n"
    "\n"
    "MODULES_DIR=/data/adb/modules\n"
    "POST_FS_DATA_D=/data/adb/post-fs-data.d\n"
    "SERVICE_D=/data/adb/service.d\n"
    "SEPOLICY_SINK=/proc/nksu/sepolicy\n"
    "STATE_DIR=/dev/nksu\n"
    "\n"
    "# init does not necessarily keep the loader's stdout, so mirror every line to\n"
    "# the kernel log; that is where nksu's other diagnostics show up.\n"
    "log() {\n"
    "    echo \"nksu: $*\"\n"
    "    echo \"nksu: $*\" > /dev/kmsg 2>/dev/null\n"
    "}\n"
    "\n"
    "# A module is skipped when it is disabled or pending removal.\n"
    "enabled() {\n"
    "    [ -d \"$1\" ] || return 1\n"
    "    [ -e \"$1/disable\" ] && return 1\n"
    "    [ -e \"$1/remove\" ] && return 1\n"
    "    return 0\n"
    "}\n"
    "\n"
    "is_metamodule() {\n"
    "    [ -f \"$1/module.prop\" ] || return 1\n"
    "    grep -Eq '^[[:space:]]*metamodule[[:space:]]*=[[:space:]]*(1|true)' \"$1/module.prop\" 2>/dev/null\n"
    "}\n"
    "\n"
    "# /data/adb is mounted with /data, but on FBE devices the module tree only\n"
    "# becomes readable once vold finishes decrypting.  Reading it too early finds\n"
    "# zero modules and (with the .done guard) would skip the whole stage, so wait\n"
    "# (bounded, ~2s) for it before enumerating.  A missing /data/adb means nothing\n"
    "# is installed, so do not stall.\n"
    "wait_modules() {\n"
    "    [ -d /data/adb ] || return 0\n"
    "    n=0\n"
    "    while [ ! -d \"$MODULES_DIR\" ] && [ \"$n\" -lt 20 ]; do\n"
    "        sleep 0.1\n"
    "        n=$((n + 1))\n"
    "    done\n"
    "}\n"
    "\n"
    "apply_sepolicy() {\n"
    "    [ -e \"$SEPOLICY_SINK\" ] || return 0\n"
    "    for d in \"$MODULES_DIR\"/*; do\n"
    "        enabled \"$d\" || continue\n"
    "        [ -f \"$d/sepolicy.rule\" ] && echo \"$d/sepolicy.rule\" > \"$SEPOLICY_SINK\"\n"
    "    done\n"
    "}\n"
    "\n"
    "# $1 directory, $2 wait|nowait\n"
    "run_dir_scripts() {\n"
    "    [ -d \"$1\" ] || return 0\n"
    "    for f in \"$1\"/*.sh; do\n"
    "        [ -f \"$f\" ] || continue\n"
    "        if [ \"$2\" = nowait ]; then sh \"$f\" & else sh \"$f\"; fi\n"
    "    done\n"
    "}\n"
    "\n"
    "# $1 hook, $2 wait|nowait, $3 meta|regular\n"
    "run_hooks() {\n"
    "    for d in \"$MODULES_DIR\"/*; do\n"
    "        enabled \"$d\" || continue\n"
    "        if [ \"$3\" = meta ]; then\n"
    "            is_metamodule \"$d\" || continue\n"
    "        else\n"
    "            is_metamodule \"$d\" && continue\n"
    "        fi\n"
    "        [ -f \"$d/$1\" ] || continue\n"
    "        if [ \"$2\" = nowait ]; then sh \"$d/$1\" & else sh \"$d/$1\"; fi\n"
    "    done\n"
    "}\n"
    "\n"
    "do_post_fs_data() {\n"
    "    wait_modules\n"
    "\n"
    "    count=0\n"
    "    for d in \"$MODULES_DIR\"/*; do\n"
    "        [ -d \"$d\" ] && count=$((count + 1))\n"
    "    done\n"
    "    log \"post-fs-data: $count module dir(s)\"\n"
    "\n"
    "    run_dir_scripts \"$POST_FS_DATA_D\" wait\n"
    "    apply_sepolicy\n"
    "    run_hooks post-fs-data.sh wait meta\n"
    "    run_hooks post-fs-data.sh wait regular\n"
    "    log \"metamodule mount\"\n"
    "    run_hooks metamount.sh wait meta\n"
    "    log \"post-fs-data done\"\n"
    "}\n"
    "\n"
    "do_late_start() {\n"
    "    log \"late_start stage\"\n"
    "    run_dir_scripts \"$SERVICE_D\" nowait\n"
    "    run_hooks service.sh nowait meta\n"
    "    run_hooks service.sh nowait regular\n"
    "    log \"late_start done\"\n"
    "}\n"
    "\n"
    "log \"loader start: stage=$STAGE\"\n"
    "\n"
    "case \"$STAGE\" in\n"
    "    post-fs-data)\n"
    "        [ -e \"$STATE_DIR/.post-fs-data.done\" ] && exit 0\n"
    "        : > \"$STATE_DIR/.post-fs-data.done\"\n"
    "        do_post_fs_data\n"
    "        ;;\n"
    "    late_start)\n"
    "        [ -e \"$STATE_DIR/.late-start.done\" ] && exit 0\n"
    "        : > \"$STATE_DIR/.late-start.done\"\n"
    "        do_late_start\n"
    "        ;;\n"
    "    *)\n"
    "        log \"unknown stage: $STAGE\"\n"
    "        exit 1\n"
    "        ;;\n"
    "esac\n"
    "\n"
    "exit 0\n";

static const size_t nksu_rc_len = sizeof(nksu_rc) - 1;
static ssize_t nksu_rc_pos;
static bool nksu_rc_hooked;
static bool nksu_rc_prepared;

/* f_op proxy state, mirroring KernelSU's fops_proxy */
static struct file_operations nksu_fops_proxy;
static ssize_t (*nksu_orig_read)(struct file *, char __user *, size_t, loff_t *);
static ssize_t (*nksu_orig_read_iter)(struct kiocb *, struct iov_iter *);

/*
 * getname_kernel()/do_mkdirat() are not part of the GKI KMI and vfs_mkdir()
 * changed its first argument across the KMI range we build for, so resolve
 * them through the module's own kallsyms scanner.  do_mkdirat() hides the
 * vfs_*() differences and has kept a stable signature.
 *
 * do_mkdirat() takes ownership of the filename and putname()s it on every
 * path (including the error path), so the caller must NOT put it again.
 */
typedef struct filename *(*nksu_getname_kernel_t)(const char *);
typedef long (*nksu_do_mkdirat_t)(int, struct filename *, umode_t);

static nksu_getname_kernel_t nksu_getname_kernel;
static nksu_do_mkdirat_t nksu_do_mkdirat;

/* saved originals for the two syscall-table hooks */
static syscall_fn_t nksu_orig_read_sys;
static syscall_fn_t nksu_orig_fstat_sys;

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

    if (!nksu_getname_kernel || !nksu_do_mkdirat)
        return -ENOSYS;

    name = nksu_getname_kernel(path);
    if (IS_ERR(name))
        return PTR_ERR(name);

    /* do_mkdirat() owns @name and putname()s it itself. */
    ret = nksu_do_mkdirat(AT_FDCWD, name, 0700);

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
 * Create /dev/nksu and write the loader.  Called the first time init is seen
 * reading init.rc, i.e. at second stage, long after /dev exists and well
 * before `on post-fs-data`.
 */
void nksu_rc_prepare_loader(void)
{
    int ret;

    if (nksu_rc_prepared)
        return;
    nksu_rc_prepared = true;

    if (!nksu_getname_kernel)
        nksu_getname_kernel = (nksu_getname_kernel_t)nksu_ksym_lookup("getname_kernel");
    if (!nksu_do_mkdirat)
        nksu_do_mkdirat = (nksu_do_mkdirat_t)nksu_ksym_lookup("do_mkdirat");

    if (!nksu_getname_kernel || !nksu_do_mkdirat) {
        pr_warn("nksu: cannot resolve VFS mkdir helpers\n");
        return;
    }

    ret = nksu_rc_mkdir(NKSU_LOADER_DIR);
    if (ret && ret != -EEXIST)
        pr_warn("nksu: cannot create %s: %d\n", NKSU_LOADER_DIR, ret);

    ret = nksu_rc_write_file(NKSU_LOADER_SCRIPT, nksu_rc_script,
                             sizeof(nksu_rc_script) - 1);
    if (ret)
        pr_warn("nksu: cannot write %s: %d\n", NKSU_LOADER_SCRIPT, ret);
    else
        pr_info("nksu: wrote module loader (%zu bytes)\n",
                sizeof(nksu_rc_script) - 1);
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

    nksu_rc_prepare_loader();

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
            nksu_rc_prepare_loader();
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

int nksu_init_rc_init(void)
{
    int ret;

    /* The module loader pushes sepolicy.rule through this sink. */
    ret = nksu_sepolicy_sink_init();
    if (ret)
        pr_warn("nksu: cannot create sepolicy sink: %d\n", ret);

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

    return 0;
}

void nksu_init_rc_exit(void)
{
    nksu_sepolicy_sink_exit();

    /*
     * The read/fstat syscall hops are torn down together with the temporary
     * boot watcher (nksu_dispatch_exit -> syscalltable_exit), so there is
     * nothing to unhook here.
     */
}
