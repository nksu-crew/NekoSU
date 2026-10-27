/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef _NKSU_SELINUX_SELINUX_H
#define _NKSU_SELINUX_SELINUX_H

#define DOMAIN "nksu"
#define DOMAIN_FILE "nksu_file"
#define DOMAIN_CTX "u:r:" DOMAIN ":s0"
#define DOMAIN_FILE_CTX "u:object_r:" DOMAIN_FILE ":s0"

void setenforce(bool status);
bool getenforce(void);
int set_domain(const char *domain, struct cred *new_cred);

/*
 * Subjective SID of the current task.  The kernel's current_sid() inline pulls
 * in selinux_blob_sizes via selinux_cred(); this accessor avoids the
 * relocation (see selinux.c).  hide.c uses it for its avc_has_perm checks.
 */
u32 nksu_current_sid(void);

/*
 * SELinux blob of a cred (task_security_struct).  hide.c's setprocattr hook
 * reads and rewrites ->sid through this, avoiding the selinux_cred() inline's
 * relocation against the unexported selinux_blob_sizes.
 */
struct task_security_struct;
struct task_security_struct *nksu_cred_security(const struct cred *cred);

/*
 * Borrow a cred switched to the nksu domain (which the policy makes
 * unconfined) with the full capability set, for kernel code that must reach
 * /data/adb.  The cred is built once and cached; feed _begin()'s return value
 * back to _end(), and call _release() on module unload.
 */
const struct cred *nksu_borrow_creds_begin(void);
void nksu_borrow_creds_end(const struct cred *old);
void nksu_borrow_creds_release(void);

/*
 * Relabel an existing path's inode to DOMAIN_FILE.  Used by the manager
 * bootstrap to label /data/adb/nksu/ncore so the init.rc exec
 * (which runs them as DOMAIN) is allowed -- the policy only grants init access
 * to files labelled nksu_file.
 */
void nksu_relabel_path(const char *path);

int init_selinux_hook(void);
void selinux_exit(void);

#endif /* _NKSU_SELINUX_SELINUX_H */
