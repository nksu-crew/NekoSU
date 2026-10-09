/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NKSU_MODULE_H
#define NKSU_MODULE_H

/*
 * Magisk/KernelSU-style module loading, driven from the kernel.
 *
 * Enumerates /data/adb/modules and runs each enabled module's boot hooks
 * through nksu_spawn(), so bringing modules up does not depend on a userspace
 * daemon (ksud/ncore) walking the directory and forking the scripts.
 *
 * The boot stages are kept apart:
 *   nksu_modules_post_fs_data()  post-fs-data.sh  (init second_stage)
 *                                + metamodule metamount.sh
 *   nksu_modules_service()       service.sh       (late_start / zygote)
 * The late-load path serves post-fs-data before the feature stage.
 *
 * Mounting is delegated to the metamodule (module.prop "metamodule=1"): its
 * lifecycle scripts run first and its metamount.sh performs the mount.
 */
void nksu_modules_post_fs_data(void);
int nksu_modules_service(void);
void nksu_modules_exit(void);

#endif /* NKSU_MODULE_H */
