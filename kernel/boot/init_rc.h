/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NKSU_INIT_RC_H
#define NKSU_INIT_RC_H

#include <linux/types.h>

/*
 * KernelSU-style init.rc injection.
 *
 * nksu hooks init's read() of /system/etc/init/hw/init.rc through the syscall
 * table (__NR_read / __NR_fstat) and appends a static rc that execs ncore,
 * nksu's userspace module runtime, at the well-defined boot stages:
 *
 *   on post-fs-data
 *       exec u:r:nksu:s0 root -- /data/adb/nksu/ncore post-fs-data
 *   on nonencrypted
 *   on property:vold.decrypt=trigger_restart_framework
 *       exec u:r:nksu:s0 root -- /data/adb/nksu/ncore services
 *   on property:sys.boot_completed=1
 *       exec u:r:nksu:s0 root -- /data/adb/nksu/ncore boot-completed
 *
 * /data/adb/nksu/ncore is installed by the manager (it is the same binary the
 * app ships as libncore.so).  ncore is a KernelSU-compatible userspace module
 * runtime: it enumerates /data/adb/modules, runs the boot hooks, drives the
 * metamodule mount, and pushes each module's sepolicy.rule through the control
 * fd (IOC_SET_SEPOLICY).
 *
 * init runs `exec` synchronously, so the post-fs-data hooks (metamodule mount
 * included) finish before init continues and modules are mounted before
 * zygote/system_server start.
 *
 * The module is loaded from init's first stage (modules.load), so this is a
 * boot-time feature: a late load injects nothing here and brings no modules up.
 */
#define NKSU_NCORE_PATH "/data/adb/nksu/ncore"

int nksu_init_rc_init(void);

#endif /* NKSU_INIT_RC_H */
