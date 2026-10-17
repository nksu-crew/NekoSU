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
 * Component groups.  Late load runs all of them at once; a first-stage
 * (vendor_boot) load stages them: the early group at second_stage, and the
 * feature group once /data and the zygote are available.
 */
int nksu_init_early_components(void);
void nksu_exit_early_components(void);
int nksu_init_selinux_components(void);
void nksu_exit_selinux_components(void);
int nksu_init_feature_components(void);
void nksu_exit_feature_components(void);

/*
 * The kernel module's version.  The manager compares it with its own copy so it
 * can tell that the flashed LKM is stale and only ships a matching userspace
 * ksud.
 */
const char *nksu_version(void);

#endif /* NKSU_H */
