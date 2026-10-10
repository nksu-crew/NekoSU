/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NKSU_MODULE_H
#define NKSU_MODULE_H

#include <linux/types.h>

/*
 * Magisk/KernelSU-style module loading.
 *
 * Module loading lives in userspace: the rc nksu injects into init.rc (see
 * kernel/boot/init_rc.c) execs ncore (shipped by the manager as /data/adb/nksu/ncore)
 * at the well-defined boot stages, and ncore runs the KernelSU-compatible
 * module runtime -- enumerate /data/adb/modules, run the boot hooks, drive the
 * metamodule mount, and push each sepolicy.rule through /proc/nksu/sepolicy.
 *
 * This is a boot-time feature: nksu must be loaded by init from modules.load
 * (the vendor_boot path) so that init.rc injection happens.  A late load (an
 * insmod after init already parsed its rc files) has no rc left to fire and
 * therefore does not bring modules up.
 *
 * The kernel only keeps the module list the manager reads through
 * IOC_LIST_MODULES (nksu_modules_emit_json) and the sepolicy sink ncore writes
 * to (selinux/rule_file.c), because only the kernel can edit the live SELinux
 * policy.
 */
#define NKSU_MODULES_JSON_MAX (256 * 1024)

void nksu_modules_exit(void);

/*
 * Write a JSON array describing every module under /data/adb/modules into
 * @buf (NUL-terminated, always within @size).  Returns the number of bytes
 * written excluding the terminator.
 */
size_t nksu_modules_emit_json(char *buf, size_t size);

#endif /* NKSU_MODULE_H */
