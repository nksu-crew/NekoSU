/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef _NKSU_SELINUX_ENGINE_H
#define _NKSU_SELINUX_ENGINE_H

#include <linux/gfp.h>
#include <linux/types.h>

/*
 * Vendored SELinux policy engine, used on 6.6+ where the kernel no longer
 * exports the context/SID/AV helpers (and dropped the struct selinux_state
 * argument).  hide.c's version-neutral wrappers call in here; the definitions
 * exist only for >= 6.6, where they are needed.
 */
struct selinux_policy;
struct av_decision;

/* Resolve the optional context_struct_compute_av()/security_dump_masked_av()
 * fallbacks.  Idempotent. */
int nksu_engine_init(void);

int nksu_engine_context_to_sid(struct selinux_policy *policy, const char *scontext,
			       u32 scontext_len, u32 *sid, u32 def_sid, gfp_t gfp_flags);
int nksu_engine_sid_to_context(struct selinux_policy *policy, u32 sid, char **scontext,
			       u32 *scontext_len);
void nksu_engine_compute_av(struct selinux_policy *policy, u32 ssid, u32 tsid, u16 tclass,
			    struct av_decision *avd);

#endif /* _NKSU_SELINUX_ENGINE_H */
