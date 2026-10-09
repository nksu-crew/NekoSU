/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NKSU_MODULE_H
#define NKSU_MODULE_H

#include <linux/types.h>

/*
 * Magisk/KernelSU-style module loading.
 *
 * Module loading lives in userspace and is driven by the rc nksu injects into
 * init.rc (see src/init_rc.c).  That rc execs the loader nksu writes to
 * /dev/nksu at the well-defined boot stages, and the loader enumerates
 * /data/adb/modules, runs the boot hooks and drives the metamodule mount --
 * like KernelSU's ksud, but without a resident daemon:
 *
 *   post-fs-data   post-fs-data.d scripts
 *                  each module's sepolicy.rule (through /proc/nksu/sepolicy)
 *                  post-fs-data.sh  (metamodule first, then regular)
 *                  metamodule metamount.sh
 *   late_start     service.d scripts
 *                  service.sh       (metamodule first, then regular,
 *                                    detached so a daemon can survive)
 *
 * Because init's `exec` is synchronous, post-fs-data completes before init
 * proceeds, so modules are mounted before zygote/system_server start.  There is
 * no kernel-side marker watcher and no timing heuristic left.
 *
 * This is a boot-time feature: nksu must be loaded by init from modules.load
 * (the vendor_boot path) so that init.rc injection happens.  A late load (an
 * insmod after init already parsed its rc files) has no rc left to fire and
 * therefore does not bring modules up.
 *
 * The kernel only keeps the module list the manager reads through
 * IOC_LIST_MODULES (nksu_modules_emit_json) and the sepolicy sink the loader
 * writes to (selinux/rule_file.c), because only the kernel can edit the live
 * SELinux policy.
 */
#define NKSU_LOADER_DIR "/dev/nksu"
#define NKSU_LOADER_SCRIPT NKSU_LOADER_DIR "/modules.sh"

#define NKSU_MODULES_JSON_MAX (256 * 1024)

void nksu_modules_exit(void);

/*
 * Write a JSON array describing every module under /data/adb/modules into
 * @buf (NUL-terminated, always within @size).  Returns the number of bytes
 * written excluding the terminator.
 */
size_t nksu_modules_emit_json(char *buf, size_t size);

#endif /* NKSU_MODULE_H */
