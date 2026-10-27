/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NKSU_H
#define NKSU_H

#include "cleanup.h"

/* The source commit the running kernel module was built from (Kbuild sets it). */
#ifndef NKSU_GIT_COMMIT
#define NKSU_GIT_COMMIT "unknown"
#endif

/*
 * The LKM's manually maintained version, defined by the shared UAPI header.
 * The manager carries the same value and compares it with the running
 * module's, so a developer bumps uapi/version.h when the KO changes.
 */
#include <uapi/version.h>

/*
 * Component registration and staged lifecycle live in boot/component.h.  A
 * late load runs every stage at once; a first-stage (vendor_boot) load arms
 * the early stage at second_stage, and the feature stage once /data and the
 * zygote are available.
 */

/*
 * The kernel module's version.  The manager compares it with its own copy so it
 * can tell that the flashed LKM is stale and only ships a matching userspace
 * ncore.
 */
const char *nksu_version(void);

#endif /* NKSU_H */
