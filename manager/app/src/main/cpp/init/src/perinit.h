/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

/*
 * Module preinit actions.
 *
 * The init wrapper in the boot ramdisk runs as PID 1 at the selinux_setup
 * stage.  Before handing over to the original init it executes each enabled
 * module's preinit.conf: kernel modules, static programs and procfs/sysfs
 * writes that must happen before the SELinux policy exists and before init.rc
 * is parsed.
 *
 * The function is called from PID 1 and returns once every action finished or
 * the global deadline expired; the caller then execs the original init.
 */
void perinit();
