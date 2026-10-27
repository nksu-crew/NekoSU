// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Ordered component lifecycle for the NKSU module.
 *
 * The table order is the init order inside a stage; teardown walks it
 * backwards.  The bitmask records which components actually came up, so a
 * failed or interrupted boot stage can be retried without initializing a
 * component twice.
 */

#include <linux/bits.h>
#include <linux/compiler.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/printk.h>

#include "component.h"
#include "safe_mode.h"

#include "fd/fd.h"
#include "hook/handle.h"
#include "hook/hook.h"
#include "manager/ioctl.h"
#include "manager/manager.h"
#include "manager/umount.h"
#include "privilege/profile.h"
#include "privilege/profile_store.h"
#include "selinux/hide.h"
#include "selinux/selinux.h"
#include "symbol/symbol_compat.h"

static const struct nksu_component nksu_components[] = {
	/* CORE: needed by the boot watcher and every later stage. */
	NKSU_CORE_COMPONENT("symbol compatibility", nksu_symbol_compat_init,
			    nksu_symbol_compat_exit),
	NKSU_CORE_COMPONENT("su redirect paths", nksu_path_init, nksu_path_exit),
	NKSU_CORE_COMPONENT("mount feature", nksu_umount_init, nksu_umount_exit),

	/* EARLY: input and tracepoint support, before SELinux policy exists. */
	NKSU_EARLY_COMPONENT("safe mode", nksu_safe_mode_init, nksu_safe_mode_exit),
	NKSU_EARLY_COMPONENT("ncore tracepoint", nksu_daemon_fd_init,
			     nksu_daemon_fd_exit),

	/* SELINUX: the policy is loaded during init's selinux_setup stage. */
	NKSU_SELINUX_COMPONENT("SELinux hook", init_selinux_hook, selinux_exit),

	/* FEATURE: everything that needs the policy, /data and the zygote. */
	NKSU_FEATURE_COMPONENT("anonymous fd", fmac_anonfd_init, fmac_anonfd_exit),
	NKSU_FEATURE_COMPONENT("uid profile", nksu_profile_init, nksu_profile_exit),
	NKSU_FEATURE_COMPONENT("profile store", nksu_profile_store_init,
			       nksu_profile_store_exit),
#ifndef CONFIG_NKSU_SYSCALL
	NKSU_FEATURE_COMPONENT("tracepoint hook", load_tracepoint_hook,
			       unload_tracepoint_hook),
#endif
	NKSU_FEATURE_COMPONENT("manager scan", appscan_init, appscan_exit),
	NKSU_FEATURE_COMPONENT("SELinux hide", nksu_selinux_hide_init,
			       nksu_selinux_hide_exit),
	NKSU_FEATURE_COMPONENT("umount config", nksu_umount_feature_init, NULL),
#ifdef CONFIG_NKSU_SYSCALL
	NKSU_FEATURE_COMPONENT("syscall hook", init_syscall_hook, exit_syscall_hook),
#endif
};

static unsigned long nksu_ready;

/*
 * A stage is atomic: on failure every component already started in that stage
 * is torn down, so a retry starts from a clean stage.  The table is grouped by
 * stage, which lets the loop stop at the next stage boundary.
 */
int __nocfi nksu_components_init(enum nksu_init_stage stage)
{
	unsigned int i, start;
	int ret;

	if ((unsigned int)stage >= NKSU_STAGE_COUNT)
		return -EINVAL;
	if (nksu_ready & BIT(stage))
		return 0;

	for (start = 0; start < ARRAY_SIZE(nksu_components); start++)
		if (nksu_components[start].stage == stage)
			break;

	for (i = start; i < ARRAY_SIZE(nksu_components) &&
		    nksu_components[i].stage == stage; i++) {
		const struct nksu_component *comp = &nksu_components[i];

		ret = comp->init ? comp->init() : 0;
		if (ret) {
			pr_err("nksu: %s failed: %d\n", comp->name, ret);
			while (i > start) {
				i--;
				if (nksu_components[i].exit)
					nksu_components[i].exit();
			}
			return ret;
		}
	}

	nksu_ready |= BIT(stage);
	return 0;
}

int nksu_components_init_all(void)
{
	enum nksu_init_stage stage;
	int ret;

	for (stage = NKSU_STAGE_CORE; stage < NKSU_STAGE_COUNT; stage++)
		if ((ret = nksu_components_init(stage)))
			return ret;

	return 0;
}

void __nocfi nksu_components_exit_from(enum nksu_init_stage stage)
{
	enum nksu_init_stage current;
	unsigned int i;

	if ((unsigned int)stage >= NKSU_STAGE_COUNT)
		return;

	for (i = ARRAY_SIZE(nksu_components); i-- > 0;) {
		const struct nksu_component *comp = &nksu_components[i];

		if (comp->stage >= stage && (nksu_ready & BIT(comp->stage)) &&
		    comp->exit)
			comp->exit();
	}

	for (current = stage; current < NKSU_STAGE_COUNT; current++)
		nksu_ready &= ~BIT(current);
}
