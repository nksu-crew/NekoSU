// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * FMAC - File Monitoring and Access Control Kernel Module
 * Copyright (C) 2025 Aqnya
 */

#include "klog.h"
#include <linux/init.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/string.h>
#include "nksu.h"
#include "boot/component.h"
#include "boot/init.h"
#include "symbol/symbol.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Aqnya");
MODULE_DESCRIPTION("nekosu");
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 4, 0)
MODULE_IMPORT_NS(VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver);
#endif

bool late_load = false;

const char *nksu_version(void)
{
	return NKSU_VERSION;
}

/*
 * Component registration and lifecycle live in boot/component.c.  This file
 * owns only the module boundary: CORE comes up first, then either the staged
 * boot watcher (first-stage load) or the whole stack (late load).
 */

static int __init nekosu_init(void)
{
	int ret;

	/*
	 * A built-in module runs from kernel_init (pid 1, still "swapper/0" at
	 * that point); init's first-stage insmod also runs as pid 1 "init".
	 * Both need the staged boot watcher.  Only a later userspace load is a
	 * "late load" that initializes everything at once.
	 */
	late_load = current->pid != 1 ||
		    (strcmp(current->comm, "init") && strcmp(current->comm, "swapper/0"));

	pr_info("Loading nekosu module...\n");

#ifdef CONFIG_NKSU_DEBUG
	pr_alert("The current build is in debug mode, and security may be compromised.\n");
	pr_info("nekosu version: %s (commit %s)\n", NKSU_VERSION, NKSU_GIT_COMMIT);
#endif

	/*
	 * Resolve the GKI/KMI-unexported symbols, stage the su redirect paths and
	 * arm the mount feature before anything can call into userspace or load
	 * modules.
	 */
	ret = nksu_components_init(NKSU_STAGE_CORE);
	if (ret) {
		pr_err("Failed to initialize core components: %d\n", ret);
		return ret;
	}

	if (!late_load)
		ret = hook_init();
	else
		ret = nksu_components_init_all();

	if (ret) {
		pr_err("Failed to initialize nekosu: %d\n", ret);
		nksu_components_exit_from(NKSU_STAGE_CORE);
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

	nksu_components_exit_from(NKSU_STAGE_CORE);
	nksu_ksym_cache_clear();

	pr_info("nekosu module unloaded\n");
}

module_init(nekosu_init);
module_exit(nekosu_exit);
