#ifndef NKSU_H
#define NKSU_H

int init_nksu(void);
void exit_nksu(void);

/*
 * Component groups.  Late load runs both at once; a first-stage (vendor_boot)
 * load stages them: SELinux once the policy exists, the rest once /data and
 * the zygote are available.
 */
int nksu_init_selinux_components(void);
void nksu_exit_selinux_components(void);
int nksu_init_feature_components(void);
void nksu_exit_feature_components(void);

#endif /* NKSU_H */
