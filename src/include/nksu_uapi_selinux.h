/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NKSU_UAPI_SELINUX_H
#define NKSU_UAPI_SELINUX_H

#include <linux/types.h>

/*
 * Wire format for the sepolicy batch the daemon sends through
 * IOC_SET_SEPOLICY (see ioctl.h).  Layout and command ids match KernelSU's
 * uapi/selinux.h, so the same encoder can be reused.
 *
 *   struct nksu_sepol_cmd { u32 cmd; u32 subcmd; };
 *   then for each argument: u32 len; char bytes[len]; '\0'
 *   (len == 0 encodes the wildcard / ALL)
 */
#define NKSU_SEPOLICY_CMD_NORMAL_PERM    1
#define NKSU_SEPOLICY_CMD_XPERM          2
#define NKSU_SEPOLICY_CMD_TYPE_STATE     3
#define NKSU_SEPOLICY_CMD_TYPE           4
#define NKSU_SEPOLICY_CMD_TYPE_ATTR      5
#define NKSU_SEPOLICY_CMD_ATTR           6
#define NKSU_SEPOLICY_CMD_TYPE_TRANSITION 7
#define NKSU_SEPOLICY_CMD_TYPE_CHANGE    8
#define NKSU_SEPOLICY_CMD_GENFSCON       9

#define NKSU_SEPOLICY_SUBCMD_NORMAL_PERM_ALLOW      1
#define NKSU_SEPOLICY_SUBCMD_NORMAL_PERM_DENY       2
#define NKSU_SEPOLICY_SUBCMD_NORMAL_PERM_AUDITALLOW 3
#define NKSU_SEPOLICY_SUBCMD_NORMAL_PERM_DONTAUDIT  4

#define NKSU_SEPOLICY_SUBCMD_XPERM_ALLOW      1
#define NKSU_SEPOLICY_SUBCMD_XPERM_AUDITALLOW 2
#define NKSU_SEPOLICY_SUBCMD_XPERM_DONTAUDIT  3

#define NKSU_SEPOLICY_SUBCMD_TYPE_STATE_PERMISSIVE 1
#define NKSU_SEPOLICY_SUBCMD_TYPE_STATE_ENFORCE    2

#define NKSU_SEPOLICY_SUBCMD_TYPE_CHANGE_CHANGE 1
#define NKSU_SEPOLICY_SUBCMD_TYPE_CHANGE_MEMBER 2

#define NKSU_SEPOLICY_MAX_BATCH_SIZE (8U * 1024U * 1024U)
#define NKSU_SEPOLICY_MAX_ARGS 5

#endif /* NKSU_UAPI_SELINUX_H */
