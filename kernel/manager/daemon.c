// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Root-visible control-fd grant for the boot daemon.
 *
 * ksud runs as root, outside the uid profile, and is exec'd as a fresh
 * process at every boot stage; it needs the control fd (prctl 204, the manager
 * gate does not apply to it) to reach the safe-mode / sepolicy ioctls.  Since
 * the temporary boot watcher (boot/hook_init.c) no longer hooks prctl and
 * nothing group-restores slots, this component is armed in the early group at
 * second_stage and keeps the prctl slot hooked for the whole boot.
 *
 * It is removed when ksud disarms safe mode (IOC_DISARM_SAFEMODE, its last
 * stage) or at module unload.
 */

#include <linux/cred.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/uidgid.h>
#include <asm/ptrace.h>
#include <asm/unistd.h>

#include "klog.h"
#include "hook/syscall.h"
#include "manager/ioctl.h"

static syscall_fn_t nksu_daemon_orig_prctl;
static bool nksu_daemon_hooked;

static long nksu_daemon_prctl(const struct pt_regs *regs)
{
	unsigned long option;
#if defined(__aarch64__)
	option = regs->regs[0];
#elif defined(__x86_64__)
	option = regs->di;
#else
	option = 0;
#endif

	if (option == NKSU_PRCTL_GET_DRIVER_FD &&
	    (uid_eq(current_euid(), GLOBAL_ROOT_UID) ||
	     uid_eq(current_uid(), GLOBAL_ROOT_UID))) {
		fmac_ctlfd_get();
		return 1;
	}

	return nksu_daemon_orig_prctl ? nksu_daemon_orig_prctl(regs) : -ENOSYS;
}

int nksu_daemon_fd_init(void)
{
	int ret;

	if (nksu_daemon_hooked)
		return 0;

	ret = syscall_slot_hook(__NR_prctl, nksu_daemon_prctl,
				&nksu_daemon_orig_prctl, "nksu_daemon_fd");
	if (ret) {
		pr_err("[daemon] prctl hook failed: %d\n", ret);
		return 0;
	}
	nksu_daemon_hooked = true;

	pr_info("[daemon] control fd available to root at every stage\n");
	return 0;
}

void nksu_daemon_fd_exit(void)
{
	if (nksu_daemon_hooked) {
		syscall_slot_unhook(__NR_prctl);
		nksu_daemon_hooked = false;
	}
	pr_info("[daemon] daemon fd exit!\n");
}
