/* SPDX-License-Identifier: GPL-2.0 */
/*
 * On-disk persistence for the uid profile table.
 *
 * The manager used to push every profile into the kernel at each startup.
 * Instead the kernel now owns the table's lifetime: every successful mutation
 * is written to /data/adb/nksu/allow.profile (nksu_profile_persist) and the
 * file is read back once at feature-init time (nksu_profile_store_load), so
 * root keeps working across reboots without the manager running.
 */
#ifndef __NKSU_PROFILE_STORE_H
#define __NKSU_PROFILE_STORE_H

#include <linux/types.h>

/* Feature component: load allow.profile and wire up teardown.  Never fails. */
int nksu_profile_store_init(void);
void nksu_profile_store_exit(void);

/* Read allow.profile and restore the table.  Never fatal: a missing or
 * malformed file just leaves the table empty and is reported in the log. */
int nksu_profile_store_load(void);

/* Serialize the current table to allow.profile.  Safe to call from process
 * context; suppressed while a load is in progress. */
void nksu_profile_store_save(void);

/*
 * Serialize the table as text into `buf` (`<uid> <caps_hex> <ns> <domain>`
 * per line).  Returns the byte count excluding the terminating NUL, or -ENOSPC
 * when `buf` is too small.  Backs both the on-disk store and IOC_GET_PROFILES.
 */
int nksu_profile_to_text(char *buf, size_t cap);

#endif /* __NKSU_PROFILE_STORE_H */
