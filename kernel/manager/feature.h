#ifndef NKSU_MANAGER_FEATURE_H
#define NKSU_MANAGER_FEATURE_H

#include <linux/types.h>

/*
 * Kernel feature toggles.
 *
 * A feature is a named, id-addressed switch with a get and a set handler (the
 * set handler may be NULL for read-only features).  The manager drives them
 * over the control fd (IOC_FEATURE_*) and ksud exposes the same through JNI,
 * mirroring KernelSU's ksu_register_feature_handler.
 *
 * Feature ids are a wire contract shared with the manager; keep them in sync
 * with manager/app/src/main/cpp/libjni/ioctl.h and ksud.kt.
 */
#define NKSU_FEATURE_SELINUX_HIDE 1

struct nksu_feature {
    u32 id;
    const char *name;
    int (*get)(u64 *value);
    int (*set)(u64 value);
};

int nksu_feature_register(const struct nksu_feature *feature);
int nksu_feature_get(u32 id, u64 *value);
int nksu_feature_set(u32 id, u64 value);

/* Render "<id> <name> <value>\n" for every feature into buf; returns the
 * number of bytes (NUL-terminated) or a negative errno. */
int nksu_feature_render(char *buf, size_t size);

/*
 * Persistent enabled-feature set at /data/adb/nksu/feature, one feature name
 * per line (blank lines and '#' comments are ignored).  The kernel owns it, so
 * toggling a feature only adds or removes its own name and leaves the other
 * features' entries alone.
 */

/* Whether `name` is in the enabled set. */
bool nksu_feature_persist_has(const char *name);

/*
 * Add (enabled) or remove (!enabled) `name` from the enabled set, preserving
 * the other entries.  Returns 0 or a negative errno.
 */
int nksu_feature_persist_set(const char *name, bool enabled);

#endif /* NKSU_MANAGER_FEATURE_H */
