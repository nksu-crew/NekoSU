/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NKSU_BOOT_SAFE_MODE_H
#define NKSU_BOOT_SAFE_MODE_H

#include <linux/types.h>

/*
 * Volume-key safe mode.
 *
 * The module watches KEY_VOLUMEUP from early boot (second_stage).  Three
 * key-down events set an in-kernel "safe mode requested" flag.  Unlike the
 * original design the kernel does NOT write a marker file or reboot; it only
 * reports the flag through the control ioctl (IOC_GET_SAFEMODE).  ksud reads
 * it at each stage and, when set, creates /data/adb/nksu/safe_module and skips
 * module loading; it then disarms the watcher with IOC_DISARM_SAFEMODE (see
 * userspace/ksud).
 *
 * The watcher also stops when the manager scan finds no ksud at all (without
 * it no module can run, so safe mode has nothing to protect).  There is no
 * timeout window.
 */

int  nksu_safe_mode_init(void);
void nksu_safe_mode_exit(void);

/* True once three volume-up presses have been seen this boot. */
bool nksu_safe_mode_requested(void);

/* Stop counting volume-up.  Idempotent; safe to call before init (the state
 * is then noticed by nksu_safe_mode_init).  Backs IOC_DISARM_SAFEMODE and the
 * manager scan when ksud is unavailable. */
void nksu_safe_mode_disable(void);

#endif /* NKSU_BOOT_SAFE_MODE_H */
