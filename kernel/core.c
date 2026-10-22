// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * FMAC - File Monitoring and Access Control Kernel Module
 * Copyright (C) 2025 Aqnya
 */

#include "klog.h"
#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include <fmac.h>
#include "symbol/symbol_compat.h"
#include "nksu.h"
#include "boot/init.h"
#include "boot/safe_mode.h"
#include "manager/ioctl.h"
#include "manager/umount.h"
#include "privilege/profile_store.h"
#include "selinux/hide.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Aqnya");
MODULE_DESCRIPTION("nekosu");
MODULE_IMPORT_NS(VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver);

bool late_load = false;

const char *nksu_version(void)
{
	return NKSU_VERSION;
}

typedef struct {
	const char *name;
	int (*init)(void);
	void (*exit)(void);
} module_component_t;

/*
 * Component groups.  Late load runs all three back to back; a first-stage
 * (vendor_boot) load stages them: the early group plus SELinux once the policy
 * exists, and the feature group once /data and the zygote are available.
 */
static const module_component_t early_components[] = {
	{
		/*
         * Volume-key safe mode depends only on the input subsystem, which
         * needs neither the SELinux policy, /data nor the zygote, so it is
         * armed at second_stage -- before post-fs-data, where ncore first
         * queries the flag over the control ioctl.
         */
		.name = "safe mode",
		.init = nksu_safe_mode_init,
		.exit = nksu_safe_mode_exit,
	},
	{
		/*
         * Mark ncore at exec with the syscall tracepoint flag and attach the
         * control fd through task_work.  Installed at second_stage, before
         * init execs ncore at post-fs-data; the fork probe propagates the mark
         * to every ncore descendant.
         */
		.name = "ncore tracepoint",
		.init = nksu_daemon_fd_init,
		.exit = nksu_daemon_fd_exit,
	},
};

/*
 * Features that must be armed before ncore runs are initialized directly from
 * module init, not through the staged component groups.  Their implementation
 * stays in manager/.
 */

/*
 * SELinux needs the policy, which is loaded during the "selinux_setup"
 * stage; everything else needs /data, which is mounted by the time zygote
 * runs (Android init triggers `post-fs-data` before `zygote-start`, matching
 * AOSP).  The feature group is therefore gated on zygote detection.  Late
 * load runs the groups back to back, a first-stage (vendor_boot) load
 * stages them.
 */
static const module_component_t selinux_components[] = {
	{
		.name = "SELinux Hook",
		.init = init_selinux_hook,
		.exit = selinux_exit,
	},
};

static const module_component_t feature_components[] = {
	{
		.name = "Anonymous FD",
		.init = fmac_anonfd_init,
		.exit = fmac_anonfd_exit,
	},
	{
		.name = "uid profile",
		.init = nksu_profile_init,
		.exit = nksu_profile_exit,
	},
	{
		.name = "profile store",
		.init = nksu_profile_store_init,
		.exit = nksu_profile_store_exit,
	},
#ifndef CONFIG_NKSU_SYSCALL
	{
		.name = "tracepoint hook",
		.init = load_tracepoint_hook,
		.exit = unload_tracepoint_hook,
	},
#endif
	{
		.name = "manager scan",
		.init = appscan_init,
		.exit = appscan_exit,
	},
	{
		/*
         * After the manager scan: the feature flag is persisted under
         * /data/adb/nksu, which that component bootstraps.
         */
		.name = "SELinux hide",
		.init = nksu_selinux_hide_init,
		.exit = nksu_selinux_hide_exit,
	},
	{
		/*
         * The umount feature is already running from module init (it must
         * cover ncore's mount scripts); this reads its persisted off switch
         * now that /data and the SELinux domain exist.
         */
		.name = "umount config",
		.init = nksu_umount_feature_init,
		.exit = NULL,
	},
#ifdef CONFIG_NKSU_SYSCALL
	{
		.name = "syscall hook",
		.init = init_syscall_hook,
		.exit = exit_syscall_hook,
	},
#endif
};

static int nekosu_init_component(const module_component_t *comp, int index)
{
	int ret;

	if (!comp->init) {
		pr_debug("Skipping %s (no init function)\n", comp->name);
		return 0;
	}
#ifdef CONFIG_NKSU_DEBUG
	ktime_t t0 = ktime_get();
#endif

	pr_info("Initializing %s...\n", comp->name);
	ret = comp->init();

#ifdef CONFIG_NKSU_DEBUG
	{
		s64 us = ktime_to_us(ktime_sub(ktime_get(), t0));
		if (ret)
			pr_err("Failed to initialize %s: %d (took %lld us)\n", comp->name, ret, us);
		else
			pr_info("%s initialized in %lld us\n", comp->name, us);
	}
#else
	if (ret)
		pr_err("Failed to initialize %s: %d\n", comp->name, ret);
	else
		pr_debug("%s initialized successfully (index: %d)\n", comp->name, index);
#endif

	return ret;
}

static void nekosu_cleanup_components(const module_component_t *comps, int count)
{
	int i;
	for (i = count - 1; i >= 0; i--) {
		if (comps[i].exit) {
			pr_debug("Cleaning up %s...\n", comps[i].name);
			comps[i].exit();
		}
	}
}

static int init_component_list(const module_component_t *comps, int count)
{
	int ret, i;

	for (i = 0; i < count; i++) {
		ret = nekosu_init_component(&comps[i], i);
		if (ret) {
			nekosu_cleanup_components(comps, i);
			return ret;
		}
	}
	return 0;
}

int nksu_init_early_components(void)
{
	return init_component_list(early_components, ARRAY_SIZE(early_components));
}

void nksu_exit_early_components(void)
{
	nekosu_cleanup_components(early_components, ARRAY_SIZE(early_components));
}

int nksu_init_selinux_components(void)
{
	return init_component_list(selinux_components, ARRAY_SIZE(selinux_components));
}

void nksu_exit_selinux_components(void)
{
	nekosu_cleanup_components(selinux_components, ARRAY_SIZE(selinux_components));
}

int nksu_init_feature_components(void)
{
	return init_component_list(feature_components, ARRAY_SIZE(feature_components));
}

void nksu_exit_feature_components(void)
{
	nekosu_cleanup_components(feature_components, ARRAY_SIZE(feature_components));
}

static int nekosu_init_all_components(void)
{
	int ret = nksu_init_early_components();

	if (ret)
		return ret;

	ret = nksu_init_selinux_components();
	if (ret) {
		nksu_exit_early_components();
		return ret;
	}

	ret = nksu_init_feature_components();
	if (ret) {
		nksu_exit_selinux_components();
		nksu_exit_early_components();
	}

	return ret;
}

static void nekosu_cleanup_all_components(void)
{
	nksu_exit_feature_components();
	nksu_exit_selinux_components();
	nksu_exit_early_components();
}

static int __init nekosu_init(void)
{
	int ret;

	late_load = ((current->pid != 1) || strcmp(current->comm, "init"));

	pr_info("Loading nekosu module...\n");

#ifdef CONFIG_NKSU_DEBUG
	pr_alert("The current build is in debug mode, and security may be compromised.\n");
	pr_info("nekosu version: %s (commit %s)\n", NKSU_VERSION, NKSU_GIT_COMMIT);
#endif

	/*
     * Resolve the GKI/KMI-unexported kernel symbols into function/data
     * pointers first. Components then reach them through this indirection
     * layer, so the module no longer carries relocations for those symbols
     * and will not fail to load with "Unknown symbol".
     */
	ret = nksu_symbol_compat_init();
	if (ret) {
		pr_err("Failed to resolve unexported symbols: %d\n", ret);
		return ret;
	}

	/*
     * This feature must be armed before ncore can mount modules, so it is
     * initialized here rather than in a staged component group.
     */
	ret = nksu_umount_init();
	if (ret) {
		pr_err("Failed to initialize module mount feature: %d\n", ret);
		nksu_symbol_compat_exit();
		return ret;
	}

	if (!late_load) {
		ret = hook_init();
	} else {
		ret = nekosu_init_all_components();
	}

	if (ret) {
		pr_err("Failed to initialize nekosu: %d\n", ret);
		nksu_umount_exit();
		nksu_symbol_compat_exit();
		return ret;
	}

	pr_info("nekosu module loaded successfully\n");
	return 0;
}

static void __exit nekosu_exit(void)
{
	pr_info("Unloading nekosu module...\n");
	if (!late_load)
		hook_exit();
	else
		nekosu_cleanup_all_components();

	nksu_umount_exit();
	nksu_symbol_compat_exit();
	nksu_ksym_cache_clear();
	pr_info("nekosu module unloaded\n");
}

module_init(nekosu_init);
module_exit(nekosu_exit);
