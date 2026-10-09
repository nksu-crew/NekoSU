/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NKSU_INIT_RC_H
#define NKSU_INIT_RC_H

#include <linux/types.h>

/*
 * KernelSU-style init.rc injection.
 *
 * nksu hooks init's read() of /system/etc/init/hw/init.rc through the syscall
 * table (__NR_read / __NR_fstat) and appends a static rc that execs the
 * userspace module loader at the well-defined boot stages:
 *
 *   on post-fs-data
 *       exec u:r:nksu:s0 root -- /system/bin/sh /dev/nksu/modules.sh post-fs-data
 *   on nonencrypted
 *   on property:vold.decrypt=trigger_restart_framework
 *       exec u:r:nksu:s0 root -- /system/bin/sh /dev/nksu/modules.sh late_start
 *
 * nksu writes the loader (and its directory) into /dev as soon as init first
 * reads init.rc, i.e. at second stage, long before `on post-fs-data` runs.
 *
 * init runs `exec` synchronously, so the post-fs-data hooks (including the
 * metamodule mount) finish before init continues: modules are mounted before
 * zygote/system_server start, and none of the old marker polling or kernel-side
 * timing heuristics are needed.
 *
 * nksu_init_rc_init() hooks __NR_read and __NR_fstat and creates the sepolicy
 * sink; it must run after nksu_dispatch_init().  Because the module is loaded
 * from init's first stage (modules.load), this is a boot-time feature: a late
 * load injects nothing here and brings no modules up.
 */
int nksu_init_rc_init(void);
void nksu_init_rc_exit(void);

/*
 * Idempotently create /dev/nksu and write the module loader into it.  Called
 * from the read proxy, the first time init reads init.rc.
 */
void nksu_rc_prepare_loader(void);

#endif /* NKSU_INIT_RC_H */
