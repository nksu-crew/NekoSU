/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _NKSU_SELINUX_DOMAIN_H
#define _NKSU_SELINUX_DOMAIN_H

int sepolicy_add_domain(const char *name);
int sepolicy_add_type(const char *name);

#endif /* _NKSU_SELINUX_DOMAIN_H */
