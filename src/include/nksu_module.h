/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NKSU_MODULE_H
#define NKSU_MODULE_H

/*
 * Magisk/KernelSU-style module loading, driven from the kernel.
 *
 * Enumerates /data/adb/modules and runs each enabled module's boot hooks
 * through nksu_spawn(), so bringing modules up does not depend on a userspace
 * daemon (ksud/ncore) walking the directory and forking the scripts.
 */
int nksu_modules_init(void);
void nksu_modules_exit(void);

#endif /* NKSU_MODULE_H */
