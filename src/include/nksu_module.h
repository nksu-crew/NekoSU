/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NKSU_MODULE_H
#define NKSU_MODULE_H

#include <linux/types.h>

/*
 * Magisk/KernelSU-style module loading, driven from the kernel.
 *
 * Enumerates /data/adb/modules and runs each enabled module's boot hooks
 * through nksu_spawn(), so bringing modules up does not depend on a userspace
 * daemon (ksud/ncore) walking the directory and forking the scripts.  The
 * Magisk-compatible /data/adb/post-fs-data.d and /data/adb/service.d script
 * directories are executed as part of the matching stage.
 *
 * The boot stages are kept apart:
 *   nksu_modules_post_fs_data()  post-fs-data.d/*.sh
 *                                + post-fs-data.sh  (init second_stage)
 *                                + metamodule metamount.sh
 *   nksu_modules_service()       service.d/*.sh
 *                                + service.sh       (late_start / zygote)
 * The late-load path serves post-fs-data before the feature stage.
 *
 * Both entry points are non-blocking: the work is queued to an internal
 * kthread and the caller returns immediately, so a slow module script cannot
 * stall init, the boot watcher or a late insmod.
 *
 * Mounting is delegated to the metamodule (module.prop "metamodule=1"): its
 * lifecycle scripts run first and its metamount.sh performs the mount.
 *
 * Since there is no userspace daemon, the manager gets the module list from
 * the kernel: nksu_modules_emit_json() renders it as a JSON array that
 * IOC_LIST_MODULES hands to userspace.
 */
#define NKSU_MODULES_JSON_MAX (256 * 1024)

void nksu_modules_post_fs_data(void);
int nksu_modules_service(void);
void nksu_modules_exit(void);

/*
 * Write a JSON array describing every module under /data/adb/modules into
 * @buf (NUL-terminated, always within @size).  Returns the number of bytes
 * written excluding the terminator.
 */
size_t nksu_modules_emit_json(char *buf, size_t size);

#endif /* NKSU_MODULE_H */
