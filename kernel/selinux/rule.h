/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _NKSU_SELINUX_RULE_H
#define _NKSU_SELINUX_RULE_H

#include <linux/types.h>

/*
 * Rule manipulation on the working policy copy.  The semantics follow
 * KernelSU's sepolicy engine; the names are NekoSU's.
 *
 *   avc_reset                    invalidate cached access vectors
 *   sepolicy_add_rule            allow/deny/auditallow/dontaudit (effect+invert)
 *   sepolicy_add_xperm           ioctl extended permissions
 *   sepolicy_add_type            new concrete type
 *   sepolicy_add_attribute       new attribute
 *   sepolicy_add_typeattribute   attach an attribute to a type
 *   sepolicy_set_permissive      mark a type permissive
 *   sepolicy_set_enforce         mark a type enforcing
 *   sepolicy_type_exists         type lookup
 *   sepolicy_add_type_transition type_transition (obj = filename or NULL)
 *   sepolicy_add_type_change     type_change
 *   sepolicy_add_type_member     type_member
 *   sepolicy_add_genfscon        genfscon (currently a no-op)
 *   sepolicy_add_domain          new domain type (type + domain attribute)
 */

void avc_reset(void);

int sepolicy_add_rule(const char *sname, const char *tname, const char *cname,
		      const char *pname, int effect, bool invert);
int sepolicy_add_xperm(const char *s, const char *t, const char *c,
		       const char *range, int effect, bool invert);
int sepolicy_add_type(const char *name);
int sepolicy_add_attribute(const char *name);
int sepolicy_add_typeattribute(const char *type_name, const char *attr_name);
int sepolicy_set_permissive(const char *type_name);
int sepolicy_set_enforce(const char *type_name);
int sepolicy_type_exists(const char *type_name);
int sepolicy_add_type_transition(const char *s, const char *t, const char *c,
				 const char *d, const char *obj);
int sepolicy_add_type_change(const char *s, const char *t, const char *c, const char *d);
int sepolicy_add_type_member(const char *s, const char *t, const char *c, const char *d);
int sepolicy_add_genfscon(const char *fs_name, const char *path, const char *ctx);

int sepolicy_allow_any_any(const char *sname);
int sepolicy_allow_all_types(const char *sname, const char *cname);
int sepolicy_add_domain(const char *name);

/*
 * Decode and apply a KernelSU-format sepolicy batch (kernel/selinux/rule_file.c).
 * Returns the number of commands applied, or a negative errno.
 */
int sepolicy_apply_batch(const void __user *data, size_t len);

#ifdef CONFIG_NKSU_DEBUG
int sepolicy_make_audit(void);
#endif

#endif /* _NKSU_SELINUX_RULE_H */
