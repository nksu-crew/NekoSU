/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef IOCTL_H
#define IOCTL_H

#include <linux/capability.h>
#include <linux/version.h>
#include <linux/build_bug.h>

/*
 * The wire contract (flags, struct fmac_ioc, payload offsets and prctl
 * opcodes) lives in the shared UAPI header so the kernel, the manager app and
 * ncore never drift apart.
 */
#include <uapi/ioctl.h>

static inline kernel_cap_t u64_to_cap(u64 v)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
	kernel_cap_t res;
	res.val = v;
	return res;
#else
	kernel_cap_t cap;
	cap.cap[0] = (u32)v;
	cap.cap[1] = (u32)(v >> 32);
	return cap;
#endif
}

static inline u64 cap_to_u64(kernel_cap_t cap)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
	return cap.val;
#else
	return ((u64)cap.cap[1] << 32) | cap.cap[0];
#endif
}

int fmac_ctlfd_get(void);

/*
 * Steady-state grant of the control fd to root (see manager/daemon.c).  Lets
 * ncore obtain the fd at every stage, not just before zygote.
 */
int nksu_daemon_fd_init(void);
void nksu_daemon_fd_exit(void);

#endif /* IOCTL_H */
