/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Profile storage — a single RCU-protected snapshot containing all managed
 * UID profiles.  Entries are sorted by uid for binary search; SELinux domain
 * strings live in a separate, packed byte arena referenced by offset+length.
 * Updates clone the snapshot, apply the change, and atomically swap the
 * global pointer; the old snapshot is freed after an RCU grace period.
 */
#ifndef __NKSU_PROFILE_H
#define __NKSU_PROFILE_H

#include <linux/bitmap.h>
#include <linux/bitops.h>
#include <linux/build_bug.h>
#include <linux/capability.h>
#include <linux/rcupdate.h>
#include <linux/stddef.h>
#include <linux/string.h>
#include <linux/types.h>
 #include <linux/version.h>

#define NKSU_PROFILE_MAGIC 0x4b4e5355 /* "NKSU" */

#define NKSU_PROFILE_INIT_CAP 16 /* initial entry slots */
#define NKSU_PROFILE_INIT_ARENA 256 /* initial string arena size */
/*
 * The presence bitmap is a fast path for the common (low) uid range only.
 * UIDs at or above it are served by the sorted entry array (binary search), so
 * the full 32-bit uid space is addressable; Android's per-user uids
 * (user * 100000 + app) are far above this.  Must be a multiple of
 * BITS_PER_LONG (see the static_assert below).
 */
#define NKSU_BITMAP_MAX_UID 32768

/* Upper bound on a stored SELinux domain; callers pass much shorter ones. */
#define NKSU_PROFILE_DOMAIN_MAX 256

/* Sentinel stored in nksu_profile_entry.domain_off when there is no domain. */
#define NKSU_DOMAIN_NONE 0xffffffffU

/* Capability storage: a fixed 64-bit blob whose layout is version-neutral. */
struct nksu_profile_caps {
	u32 cap_lo;
	u32 cap_hi;
};

/* A single profile entry. */
struct nksu_profile_entry {
	u32 uid;
	u32 root_uid; /* uid the authorized process switches to (0 = root) */
	struct nksu_profile_caps caps;
	u32 domain_off; /* offset into arena, NKSU_DOMAIN_NONE if none */
	u32 domain_len; /* bytes of domain at domain_off, 0 if none */
	int ns; /* NKSU_NS_* */
};

/*
 * Snapshot header.  The entry array is a flexible array at the tail (so no
 * pointer arithmetic or casts are needed to reach it); the domain string
 * arena is a separate allocation referenced by `arena`.  Keeping the two
 * apart means each can grow independently and both can be freed with their
 * own allocator.
 *
 * The entry array is kept sorted by uid for O(log N) binary search.  The
 * arena packs domain strings back-to-back; entries reference them by
 * offset+length.
 */
struct nksu_profile_blob {
	struct rcu_head rcu;
	u32 magic;
	u32 version; /* monotonic generation counter */
	u32 count; /* active entries */
	u32 capacity; /* total entry slots */
	u32 arena_used; /* bytes consumed in the arena */
	u32 arena_cap; /* total arena capacity */
	DECLARE_BITMAP(bitmap, NKSU_BITMAP_MAX_UID); /* fast UID presence bitmap */
	char *arena; /* domain string storage (may be NULL when empty) */
	struct nksu_profile_entry entries[];
};

/*
 * The entry layout is rebuilt by the ioctl path and the store, and the
 * bitmap must cover every addressable uid, so pin both at compile time
 * (C11 static_assert).
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 1, 0)
static_assert(sizeof(struct nksu_profile_entry) == 28, "nksu_profile_entry layout changed");
static_assert(offsetof(struct nksu_profile_entry, caps) == 8, "nksu_profile_entry.caps moved");
static_assert(offsetof(struct nksu_profile_entry, domain_off) == 16,
	      "nksu_profile_entry.domain_off moved");
static_assert(offsetof(struct nksu_profile_entry, domain_len) == 20,
	      "nksu_profile_entry.domain_len moved");
static_assert(offsetof(struct nksu_profile_entry, ns) == 24, "nksu_profile_entry.ns moved");
static_assert(sizeof(struct nksu_profile_caps) == sizeof(u64),
	      "caps storage must hold a 64-bit capability set");
static_assert(sizeof(kernel_cap_t) == sizeof(struct nksu_profile_caps),
	      "kernel_cap_t size must match the caps storage slot");
static_assert(BITS_TO_LONGS(NKSU_BITMAP_MAX_UID) * BITS_PER_LONG == NKSU_BITMAP_MAX_UID,
	      "bitmap must cover NKSU_BITMAP_MAX_UID uids exactly");
static_assert(sizeof(struct nksu_profile_blob) % _Alignof(struct nksu_profile_entry) == 0,
	      "blob header must leave the entry array aligned");
#else
_Static_assert(sizeof(struct nksu_profile_entry) == 28, "nksu_profile_entry layout changed");
_Static_assert(offsetof(struct nksu_profile_entry, caps) == 8, "nksu_profile_entry.caps moved");
_Static_assert(offsetof(struct nksu_profile_entry, domain_off) == 16,
	      "nksu_profile_entry.domain_off moved");
_Static_assert(offsetof(struct nksu_profile_entry, domain_len) == 20,
	      "nksu_profile_entry.domain_len moved");
_Static_assert(offsetof(struct nksu_profile_entry, ns) == 24, "nksu_profile_entry.ns moved");
_Static_assert(sizeof(struct nksu_profile_caps) == sizeof(u64),
	      "caps storage must hold a 64-bit capability set");
_Static_assert(sizeof(kernel_cap_t) == sizeof(struct nksu_profile_caps),
	      "kernel_cap_t size must match the caps storage slot");
_Static_assert(BITS_TO_LONGS(NKSU_BITMAP_MAX_UID) * BITS_PER_LONG == NKSU_BITMAP_MAX_UID,
	      "bitmap must cover NKSU_BITMAP_MAX_UID uids exactly");
_Static_assert(sizeof(struct nksu_profile_blob) % _Alignof(struct nksu_profile_entry) == 0,
	      "blob header must leave the entry array aligned");
#endif
/* --- public API --- */

/*
 * Presence check: true when a profile exists for uid.  O(1) for uids covered
 * by the bitmap, O(log N) above it.  Safe to call from any context.
 */
bool nksu_profile_has_uid(uid_t uid);

/*
 * Look up a profile for uid.  On success, root_uid, caps and ns are written
 * back (each may be NULL) and the domain string is copied into domain_buf (at
 * most domain_len bytes, always null-terminated).  If no domain is stored,
 * domain_buf[0] is set to '\0'.
 *
 * Returns 0 on success, -ENOENT if no profile exists for uid.
 */
int nksu_profile_get(uid_t uid, uid_t *root_uid, kernel_cap_t *caps, int *ns, char *domain_buf,
		     size_t domain_len);

int nksu_profile_set(uid_t uid, uid_t root_uid, kernel_cap_t caps, const char *domain, int ns);
int nksu_profile_set_caps(uid_t uid, kernel_cap_t caps);
int nksu_profile_set_domain(uid_t uid, const char *domain);
int nksu_profile_set_ns(uid_t uid, int ns);
int nksu_profile_set_default(uid_t uid);

int nksu_profile_clear(uid_t uid);
void nksu_profile_clear_all(void);

int nksu_profile_init(void);

/*
 * Module-teardown hook: detach the snapshot, drop it after an RCU grace
 * period and flush the deferred free callbacks so no module text is invoked
 * after unload.  Distinct from nksu_profile_clear_all(), which empties the
 * table in place and persists.
 */
void nksu_profile_exit(void);

/* Number of active entries in the profile table. */
u32 nksu_profile_count(void);

/*
 * Visit every profile entry in ascending uid order.  The callback runs under
 * the RCU read lock, so it must not sleep; `domain` is a non-terminated view
 * into the arena, valid only for the duration of the call.
 */
typedef void (*nksu_profile_iter_fn)(uid_t uid, uid_t root_uid, kernel_cap_t caps, int ns,
				     const char *domain, size_t domain_len, void *ctx);
void nksu_profile_foreach(nksu_profile_iter_fn fn, void *ctx);

/*
 * Persistence hook, implemented by the profile store (privilege/profile_store.c).
 * Called after every successful mutation so the on-disk copy stays in sync.
 */
void nksu_profile_persist(void);

#endif /* __NKSU_PROFILE_H */
