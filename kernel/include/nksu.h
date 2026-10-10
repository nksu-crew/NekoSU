#ifndef NKSU_H
#define NKSU_H

/* The source commit the running kernel module was built from (Kbuild sets it). */
#ifndef NKSU_GIT_COMMIT
#define NKSU_GIT_COMMIT "unknown"
#endif

/*
 * Component groups.  Late load runs both at once; a first-stage (vendor_boot)
 * load stages them: SELinux once the policy exists, the rest once /data and
 * the zygote are available.
 */
int nksu_init_selinux_components(void);
void nksu_exit_selinux_components(void);
int nksu_init_feature_components(void);
void nksu_exit_feature_components(void);

/*
 * The kernel module's build version.  The manager compares it with its own
 * (identically bound) version so it can tell that the flashed LKM is stale and
 * only ships the matching userspace ncore.
 */
const char *nksu_version(void);

#endif /* NKSU_H */
