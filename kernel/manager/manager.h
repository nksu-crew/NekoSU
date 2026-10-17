#ifndef NKSU_MANAGER_H
#define NKSU_MANAGER_H

#include <linux/types.h>

int appscan_init(void);
void appscan_exit(void);
bool is_manager(void);

/*
 * True once the manager scan has confirmed (or installed) /data/adb/nksu/ksud.
 * The su hook reads this instead of touching the filesystem: it runs inside a
 * syscall tracepoint, where sleeping (kern_path) is not allowed.
 */
bool is_daemon_ready(void);

/* True when uid is the currently verified manager's UID.  Used by the profile
 * store to keep the manager's own entry out of allow.profile: it is re-granted
 * each boot after the signature check, so a stale entry must never survive a
 * failed verification (or a reinstall that changes the UID). */
bool is_manager_uid(uid_t uid);

/* mkdir -p for kernel-internal paths.  Runs with the caller's creds, so callers
 * that touch /data must borrow the nksu domain first. */
int nksu_kpath_mkdir_p(const char *dir);

#endif /* NKSU_MANAGER_H */
