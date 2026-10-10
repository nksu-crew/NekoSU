// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * FMAC - File Monitoring and Access Control Kernel Module
 * Copyright (C) 2025 Aqnya
 */

#ifndef _LINUX_FMAC_H
#define _LINUX_FMAC_H

#include <linux/hashtable.h>
#include <linux/jhash.h>
#include <linux/rcupdate.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/version.h>
#include <asm/syscall.h>

#include "selinux/selinux.h"
#include "selinux/rule.h"
#include "selinux/policy.h"

#include "klog.h"
#include "privilege/privilege.h"
#include "hook/handle.h"
#include "manager/ioctl.h"
#include "manager/manager.h"
#include "hook/hook.h"
#include "privilege/ns.h"
#include "symbol/symbol.h"
#include "spawn/spawn.h"

#include "privilege/profile.h"
#include "fd/fd.h"

#ifdef CONFIG_NKSU_SYSCALL
#include "hook/dispatch.h"
#include "hook/syscall.h"
#endif

#define MAX_PATH_LEN 1024

#endif /* _LINUX_FMAC_H */
