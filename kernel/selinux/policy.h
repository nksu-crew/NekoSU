/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef _NKSU_SELINUX_POLICY_H
#define _NKSU_SELINUX_POLICY_H

#include <linux/version.h>

int sepolicy_dup_and_apply(void);
void sepolicy_restore(void);
int sepolicy_init(void);
void sepolicy_exit(void);
int load_policy(void);

/*
 * The untouched pre-NekoSU policy, kept aside by sepolicy_dup_and_apply().
 * The SELinux hiding feature answers userspace requests from this copy so the
 * injected domain/rules stay invisible.  NULL before the swap or after
 * sepolicy_restore().
 *
 * 5.7 wrapped the policy tables in struct selinux_policy; older kernels (4.9)
 * keep them directly in struct selinux_ss, so the accessor returns the
 * policydb there.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 7, 0)
struct selinux_policy;
struct selinux_policy *nksu_orig_policy_get(void);
#else
struct policydb;
struct policydb *nksu_orig_policy_get(void);

/*
 * 4.9 has no mutex-protected policy pointer and its rule paths allocate
 * memory, which must not happen while the policy rwlock is held.  The rule
 * engine therefore mutates a private clone and installs it with a short
 * write-locked pointer swap.
 */
struct policydb *nksu_policydb_dup_live(void);
int nksu_policydb_install(struct policydb *new_pdb);
void nksu_policydb_free(struct policydb *pdb);
#endif

#endif /* _NKSU_SELINUX_POLICY_H */
