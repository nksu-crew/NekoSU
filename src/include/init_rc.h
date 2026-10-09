/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NKSU_INIT_RC_H
#define NKSU_INIT_RC_H

#include <linux/types.h>

/*
 * KernelSU-style init.rc injection, adapted for a daemon-less nksu.
 *
 * KernelSU hooks init's read() of /system/etc/init/hw/init.rc and appends a
 * static rc that execs ksud at well-defined boot stages.  nksu has no
 * userspace daemon, so the injected rc instead execs a tiny script that nksu
 * writes into a temporary directory (/dev/nksu).  Running in the unconfined
 * nksu domain, the script writes a stage marker into that directory and the
 * kernel watches for it:
 *
 *   on post-fs-data                                  -> /dev/nksu/post-fs-data
 *   on nonencrypted / vold.decrypt=trigger_restart_framework
 *                                                    -> /dev/nksu/services
 *   on property:sys.boot_completed=1                 -> /dev/nksu/boot-completed
 *
 * This replaces the old "first zygote execve" heuristic, which fired before
 * /data/adb/modules was readable on FBE devices and made every module look
 * absent.
 *
 * nksu_init_rc_init() hooks __NR_read and __NR_fstat (again like KernelSU)
 * and starts the marker watcher.  It must run after the syscall table is
 * initialised (nksu_dispatch_init()).  nksu_rc_injected() reports whether the
 * rc proxy was actually installed, so the zygote path can fall back to the
 * legacy trigger if init never read init.rc through us.
 */
int nksu_init_rc_init(void);
void nksu_init_rc_exit(void);

bool nksu_rc_injected(void);

#endif /* NKSU_INIT_RC_H */
