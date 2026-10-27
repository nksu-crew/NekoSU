// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Profile storage backed by a single RCU-protected snapshot.
 *
 * Entries are sorted by uid for binary search; domain strings are packed in a
 * separate byte arena.  Every mutation clones the snapshot, applies the
 * change, and swaps the global pointer atomically; the old snapshot (and its
 * arena) is freed only after an RCU grace period.
 *
 * Memory-safety notes:
 *  - The entry array is a flexible array member of the header, so it is
 *    reached with ordinary array indexing and no pointer casts.
 *  - The presence bitmap is a real DECLARE_BITMAP, so set_bit()/test_bit()
 *    take its address directly.
 *  - Domain offsets/lengths are 32-bit and validated against arena_used on
 *    every read, so a stale or corrupt offset can never read out of bounds.
 *  - Growth frees the superseded working snapshot, so no clone is leaked.
 */

#include "klog.h"
#include "guards.h"
#include <linux/bitmap.h>
#include <linux/bitops.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/version.h>

/*
 * container_of() lives in <linux/kernel.h> before 5.16 and in its own header
 * afterwards (and is no longer pulled in by kernel.h on some newer trees).
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 16, 0)
#include <linux/container_of.h>
#endif

#include "privilege/profile.h"
#include "privilege/ns.h"

static DEFINE_MUTEX(profile_lock);
static struct nksu_profile_blob __rcu *g_profile_blob;

/*
 * Set by nksu_profile_exit() under profile_lock: once teardown has started no
 * mutation may publish a new snapshot again (which would leak it past unload
 * and revive the table).
 */
static bool profile_shutdown;

/* ── capability transfer ── */

/*
 * kernel_cap_t's internal layout varies across kernel versions (struct with
 * cap[], plain u32, ...), so move the bits with memcpy rather than assigning
 * the field.
 */
static kernel_cap_t entry_caps(const struct nksu_profile_entry *e)
{
	kernel_cap_t caps;

	memcpy(&caps, &e->caps, sizeof(caps));
	return caps;
}

static void entry_set_caps(struct nksu_profile_entry *e, kernel_cap_t caps)
{
	memcpy(&e->caps, &caps, sizeof(caps));
}

/* ── presence bitmap ── */

static void blob_set_bitmap(struct nksu_profile_blob *b, uid_t uid)
{
	if (uid < NKSU_BITMAP_MAX_UID)
		set_bit(uid, b->bitmap);
}

static void blob_clear_bitmap(struct nksu_profile_blob *b, uid_t uid)
{
	if (uid < NKSU_BITMAP_MAX_UID)
		clear_bit(uid, b->bitmap);
}

/* ── allocation ── */

static struct nksu_profile_blob *blob_alloc(u32 capacity, u32 arena_cap)
{
	struct nksu_profile_blob *b;

	b = kzalloc(struct_size(b, entries, capacity), GFP_KERNEL);
	if (!b)
		return NULL;

	if (arena_cap) {
		b->arena = kvmalloc(arena_cap, GFP_KERNEL);
		if (!b->arena) {
			kfree(b);
			return NULL;
		}
	}

	b->magic = NKSU_PROFILE_MAGIC;
	b->version = 1;
	b->capacity = capacity;
	b->arena_cap = arena_cap;
	return b;
}

static void blob_free(struct nksu_profile_blob *b)
{
	if (!b)
		return;
	kvfree(b->arena);
	kfree(b);
}

/*
 * RCU callback: the superseded snapshot (and its arena) is released once no
 * reader can still hold it.  Runs in softirq context, so it may not sleep --
 * kvfree()/kfree() are fine there.
 */
static void blob_free_rcu(struct rcu_head *head)
{
	blob_free(container_of(head, struct nksu_profile_blob, rcu));
}

static bool blob_domain_ok(const struct nksu_profile_blob *b, const struct nksu_profile_entry *e);

/*
 * Allocate a fresh snapshot with the requested capacity, copying src into it.
 * src may be NULL for an empty snapshot.  The new capacity/arena must be at
 * least as large as src's contents, or -EINVAL is returned.
 *
 * The arena is rebuilt from only the strings the entries still reference, so
 * repeated domain updates cannot grow it without bound and no dead bytes are
 * carried across snapshots.
 */
static struct nksu_profile_blob *blob_clone(const struct nksu_profile_blob *src, u32 capacity,
					    u32 arena_cap)
{
	struct nksu_profile_blob *dst;
	u32 i;

	if (src) {
		if (capacity < src->count || arena_cap < src->arena_used)
			return ERR_PTR(-EINVAL);
	}

	dst = blob_alloc(capacity, arena_cap);
	if (!dst)
		return ERR_PTR(-ENOMEM);

	if (!src)
		return dst;

	dst->version = src->version + 1;
	dst->count = src->count;
	if (src->count)
		memcpy(dst->entries, src->entries, src->count * sizeof(dst->entries[0]));

	for (i = 0; i < dst->count; i++) {
		struct nksu_profile_entry *e = &dst->entries[i];

		if (!blob_domain_ok(src, e)) {
			e->domain_off = NKSU_DOMAIN_NONE;
			e->domain_len = 0;
			continue;
		}
		memcpy(dst->arena + dst->arena_used, src->arena + e->domain_off, e->domain_len);
		e->domain_off = dst->arena_used;
		dst->arena_used += e->domain_len;
	}

	bitmap_copy(dst->bitmap, src->bitmap, NKSU_BITMAP_MAX_UID);
	return dst;
}

/* ── growth ── */

/*
 * Make *bp large enough for at least min_capacity entries and min_arena arena
 * bytes.  The superseded snapshot is freed, since *bp is always a private
 * working copy at this point.
 */
static int blob_reserve(struct nksu_profile_blob **bp, u32 min_capacity, u32 min_arena)
{
	struct nksu_profile_blob *src = *bp;
	struct nksu_profile_blob *dst;
	u32 new_capacity, new_arena;

	if (src->capacity >= min_capacity && src->arena_cap >= min_arena)
		return 0;

	new_capacity = src->capacity ? src->capacity : 1;
	while (new_capacity < min_capacity) {
		if (new_capacity > U32_MAX / 2) {
			new_capacity = min_capacity;
			break;
		}
		new_capacity *= 2;
	}

	new_arena = src->arena_cap ? src->arena_cap : 1;
	while (new_arena < min_arena) {
		if (new_arena > U32_MAX / 2) {
			new_arena = min_arena;
			break;
		}
		new_arena *= 2;
	}

	dst = blob_clone(src, new_capacity, new_arena);
	if (IS_ERR(dst))
		return PTR_ERR(dst);

	blob_free(src);
	*bp = dst;
	return 0;
}

/* ── binary search ── */

/*
 * Binary search for uid.  If found, *idx is the index of the matching entry
 * and returns true.  If not found, *idx is the insertion point (where uid
 * belongs to keep the array sorted) and returns false.
 */
static bool blob_bsearch(const struct nksu_profile_blob *b, uid_t uid, u32 *idx)
{
	u32 lo = 0, hi = b->count;

	while (lo < hi) {
		u32 mid = lo + (hi - lo) / 2;

		if (b->entries[mid].uid < uid)
			lo = mid + 1;
		else
			hi = mid;
	}
	*idx = lo;
	return lo < b->count && b->entries[lo].uid == uid;
}

/* ── arena ── */

/*
 * Append `len` bytes to the arena of *bp, growing (and replacing) the working
 * snapshot if needed.  On success *off_out receives the byte offset.
 */
static int arena_put(struct nksu_profile_blob **bp, const char *str, u32 len, u32 *off_out)
{
	struct nksu_profile_blob *b = *bp;
	int ret;

	if (!str || !len)
		return -EINVAL;

	if (b->arena_used > b->arena_cap || len > b->arena_cap - b->arena_used) {
		if (len > U32_MAX - b->arena_used)
			return -EOVERFLOW;
		ret = blob_reserve(bp, b->capacity, b->arena_used + len);
		if (ret)
			return ret;
		b = *bp;
	}

	*off_out = b->arena_used;
	memcpy(b->arena + b->arena_used, str, len);
	b->arena_used += len;
	return 0;
}

/*
 * Validate an entry's domain view against the arena.  Returns true when the
 * (offset, length) pair is entirely inside the used part of the arena.
 */
static bool blob_domain_ok(const struct nksu_profile_blob *b, const struct nksu_profile_entry *e)
{
	if (e->domain_len == 0 || e->domain_off == NKSU_DOMAIN_NONE)
		return false;
	if (e->domain_off > b->arena_used)
		return false;
	return e->domain_len <= b->arena_used - e->domain_off;
}

/* ── update machinery ── */

typedef int (*blob_modify_fn)(struct nksu_profile_blob **bp, void *ctx);

/*
 * Atomic update: clone the current snapshot, apply the modification, and swap
 * the global pointer.  The old snapshot is freed after an RCU grace period.
 */
static int blob_update(blob_modify_fn mod, void *ctx)
{
	struct nksu_profile_blob *old, *new_blob;
	int ret;

	scoped_guard(nksu_mutex, &profile_lock)
	{
		if (profile_shutdown)
			return -ENODEV;

		old = rcu_dereference_protected(g_profile_blob, lockdep_is_held(&profile_lock));

		if (old)
			new_blob = blob_clone(old, old->capacity, old->arena_cap);
		else
			new_blob = blob_clone(NULL, NKSU_PROFILE_INIT_CAP, NKSU_PROFILE_INIT_ARENA);

		if (IS_ERR(new_blob))
			return PTR_ERR(new_blob);

		ret = mod(&new_blob, ctx);
		if (ret) {
			blob_free(new_blob);
			return ret;
		}

		rcu_assign_pointer(g_profile_blob, new_blob);

		/*
		 * Hand the old snapshot to RCU instead of blocking on
		 * synchronize_rcu(); the callback frees both the header and the
		 * arena once the grace period elapses.
		 */
		if (old)
			call_rcu(&old->rcu, blob_free_rcu);
	}
	return 0;
}

/* ── modify: set all fields ── */

struct set_all_ctx {
	uid_t uid;
	uid_t root_uid;
	kernel_cap_t caps;
	const char *domain;
	int ns;
};

static int __set_all(struct nksu_profile_blob **bp, void *ctx)
{
	struct set_all_ctx *s = ctx;
	struct nksu_profile_blob *b = *bp;
	struct nksu_profile_entry *e;
	u32 idx, dlen = 0, domain_off = NKSU_DOMAIN_NONE;
	u32 need_cap, need_arena;
	bool found;
	int ret;

	if (s->domain && s->domain[0]) {
		dlen = strnlen(s->domain, NKSU_PROFILE_DOMAIN_MAX);
		if (dlen == 0 || dlen == NKSU_PROFILE_DOMAIN_MAX)
			return -EINVAL;
	}

	found = blob_bsearch(b, s->uid, &idx);

	/*
	 * Reserve the entry slot and the domain bytes in a single growth step
	 * so an insert-with-domain clones at most once.
	 */
	need_cap = found ? b->capacity : b->count + 1;
	need_arena = b->arena_used;
	if (dlen) {
		if (dlen > U32_MAX - need_arena)
			return -EOVERFLOW;
		need_arena += dlen;
	}
	ret = blob_reserve(bp, need_cap, need_arena);
	if (ret)
		return ret;
	b = *bp;

	if (dlen) {
		/* blob_reserve() guaranteed room, so this cannot grow again. */
		ret = arena_put(bp, s->domain, dlen, &domain_off);
		if (ret)
			return ret;
		b = *bp;
	}

	/* Growth preserves order, so re-find against the final blob. */
	found = blob_bsearch(b, s->uid, &idx);

	if (found) {
		e = &b->entries[idx];
		entry_set_caps(e, s->caps);
		e->root_uid = s->root_uid;
		e->ns = s->ns;
		if (domain_off != NKSU_DOMAIN_NONE) {
			e->domain_off = domain_off;
			e->domain_len = dlen;
		}
		return 0;
	}

	if (idx < b->count)
		memmove(&b->entries[idx + 1], &b->entries[idx],
			(b->count - idx) * sizeof(b->entries[0]));
	b->count++;

	e = &b->entries[idx];
	e->uid = s->uid;
	e->root_uid = s->root_uid;
	entry_set_caps(e, s->caps);
	e->ns = s->ns;
	e->domain_off = domain_off;
	e->domain_len = dlen;
	blob_set_bitmap(b, s->uid);
	return 0;
}

/* ── modify: set one field ── */

struct set_field_ctx {
	uid_t uid;
	kernel_cap_t caps;
	const char *domain;
	int ns;
};

static int __set_caps(struct nksu_profile_blob **bp, void *ctx)
{
	struct set_field_ctx *s = ctx;
	struct nksu_profile_blob *b = *bp;
	u32 idx;

	if (!blob_bsearch(b, s->uid, &idx))
		return -ENOENT;

	entry_set_caps(&b->entries[idx], s->caps);
	return 0;
}

static int __set_domain(struct nksu_profile_blob **bp, void *ctx)
{
	struct set_field_ctx *s = ctx;
	struct nksu_profile_blob *b = *bp;
	struct nksu_profile_entry *e;
	u32 idx, dlen, off;
	int ret;

	if (!blob_bsearch(b, s->uid, &idx))
		return -ENOENT;

	if (!s->domain || !s->domain[0]) {
		b->entries[idx].domain_off = NKSU_DOMAIN_NONE;
		b->entries[idx].domain_len = 0;
		return 0;
	}

	dlen = strnlen(s->domain, NKSU_PROFILE_DOMAIN_MAX);
	if (dlen == 0 || dlen == NKSU_PROFILE_DOMAIN_MAX)
		return -EINVAL;

	ret = arena_put(bp, s->domain, dlen, &off);
	if (ret)
		return ret;

	/*
	 * arena_put may have replaced the snapshot, but the clone preserves
	 * the sorted entry order, so idx still addresses the same uid.
	 */
	b = *bp;
	e = &b->entries[idx];
	e->domain_off = off;
	e->domain_len = dlen;
	return 0;
}

static int __set_ns(struct nksu_profile_blob **bp, void *ctx)
{
	struct set_field_ctx *s = ctx;
	struct nksu_profile_blob *b = *bp;
	u32 idx;

	if (!blob_bsearch(b, s->uid, &idx))
		return -ENOENT;

	b->entries[idx].ns = s->ns;
	return 0;
}

/* ── modify: remove an entry ── */

struct clear_ctx {
	uid_t uid;
	bool clear_all;
};

static int __clear_entry(struct nksu_profile_blob **bp, void *ctx)
{
	struct clear_ctx *c = ctx;
	struct nksu_profile_blob *b = *bp;
	u32 idx;

	if (c->clear_all) {
		b->count = 0;
		b->arena_used = 0;
		bitmap_zero(b->bitmap, NKSU_BITMAP_MAX_UID);
		return 0;
	}

	if (!blob_bsearch(b, c->uid, &idx))
		return -ENOENT;

	blob_clear_bitmap(b, c->uid);

	if (idx + 1 < b->count)
		memmove(&b->entries[idx], &b->entries[idx + 1],
			(b->count - idx - 1) * sizeof(b->entries[0]));
	b->count--;
	return 0;
}

/* ── public API ── */

bool nksu_profile_has_uid(uid_t uid)
{
	struct nksu_profile_blob *b;
	bool result = false;

	rcu_read_lock();
	b = rcu_dereference(g_profile_blob);
	if (b) {
		/*
		 * The bitmap only covers the low uid range; anything above it
		 * falls back to the sorted entry array so larger uids (Android
		 * per-user offsets) are supported too.
		 */
		if (uid < NKSU_BITMAP_MAX_UID) {
			result = test_bit(uid, b->bitmap);
		} else {
			u32 idx;

			result = blob_bsearch(b, uid, &idx);
		}
	}
	rcu_read_unlock();
	return result;
}

int nksu_profile_get(uid_t uid, uid_t *root_uid, kernel_cap_t *caps, int *ns, char *domain_buf,
		     size_t domain_len)
{
	struct nksu_profile_blob *b;
	struct nksu_profile_entry *e;
	u32 idx;
	int ret = -ENOENT;

	rcu_read_lock();

	b = rcu_dereference(g_profile_blob);
	if (!b)
		goto out;

	if (!blob_bsearch(b, uid, &idx))
		goto out;

	e = &b->entries[idx];
	if (caps)
		*caps = entry_caps(e);
	if (root_uid)
		*root_uid = e->root_uid;
	if (ns)
		*ns = e->ns;

	if (domain_buf && domain_len > 0) {
		domain_buf[0] = '\0';
		if (blob_domain_ok(b, e)) {
			size_t n = e->domain_len;

			if (n > domain_len - 1)
				n = domain_len - 1;
			memcpy(domain_buf, b->arena + e->domain_off, n);
			domain_buf[n] = '\0';
		}
	}
	ret = 0;

out:
	rcu_read_unlock();
	return ret;
}

/*
 * Every successful mutation is mirrored to disk so the table survives a
 * reboot; the store is what makes the manager's per-startup sync unnecessary.
 */
static int blob_update_and_persist(blob_modify_fn mod, void *ctx)
{
	int ret = blob_update(mod, ctx);

	if (ret == 0)
		nksu_profile_persist();
	return ret;
}

int nksu_profile_set(uid_t uid, uid_t root_uid, kernel_cap_t caps, const char *domain, int ns)
{
	struct set_all_ctx ctx = {
		.uid = uid,
		.root_uid = root_uid,
		.caps = caps,
		.domain = domain,
		.ns = ns,
	};

	return blob_update_and_persist(__set_all, &ctx);
}

int nksu_profile_set_caps(uid_t uid, kernel_cap_t caps)
{
	struct set_field_ctx ctx = {
		.uid = uid,
		.caps = caps,
	};

	return blob_update_and_persist(__set_caps, &ctx);
}

int nksu_profile_set_domain(uid_t uid, const char *domain)
{
	struct set_field_ctx ctx = {
		.uid = uid,
		.domain = domain,
	};

	return blob_update_and_persist(__set_domain, &ctx);
}

int nksu_profile_set_ns(uid_t uid, int ns)
{
	struct set_field_ctx ctx = {
		.uid = uid,
		.ns = ns,
	};

	return blob_update_and_persist(__set_ns, &ctx);
}

/*
 * The manager accesses root through this profile.  A bare uid 0 does not
 * bypass DAC on its own: without capabilities the root shell cannot write
 * files it does not own — including its own app's 0700 data dir — which made
 * direct install fail with EACCES on the first dd.  Grant the full set, as a
 * root manager is expected to have (KernelSU does the same).
 */
int nksu_profile_set_default(uid_t uid)
{
	kernel_cap_t caps = CAP_FULL_SET;

	return nksu_profile_set(uid, 0, caps, "u:r:nksu:s0", NKSU_NS_INHERITED);
}

int nksu_profile_clear(uid_t uid)
{
	struct clear_ctx ctx = { .uid = uid };

	return blob_update_and_persist(__clear_entry, &ctx);
}

void nksu_profile_clear_all(void)
{
	struct clear_ctx ctx = { .clear_all = true };

	blob_update_and_persist(__clear_entry, &ctx);
}

void nksu_profile_exit(void)
{
	struct nksu_profile_blob *b;

	scoped_guard(nksu_mutex, &profile_lock)
	{
		profile_shutdown = true;
		b = rcu_dereference_protected(g_profile_blob, lockdep_is_held(&profile_lock));
		rcu_assign_pointer(g_profile_blob, NULL);
	}

	if (b) {
		/*
		 * Wait out any reader still holding the detached snapshot,
		 * then free it directly.
		 */
		synchronize_rcu();
		blob_free(b);
	}

	/*
	 * blob_update() hands superseded snapshots to blob_free_rcu() (module
	 * text) via call_rcu(); flush every pending callback so none can run
	 * after the module is unloaded.
	 */
	rcu_barrier();
}

int nksu_profile_init(void)
{
	struct nksu_profile_blob *b;

	b = blob_clone(NULL, NKSU_PROFILE_INIT_CAP, NKSU_PROFILE_INIT_ARENA);
	if (IS_ERR(b))
		return PTR_ERR(b);

	/* Idempotent: never publish (or leak) a second table. */
	scoped_guard(nksu_mutex, &profile_lock)
	{
		if (rcu_access_pointer(g_profile_blob)) {
			blob_free(b);
			return 0;
		}
		profile_shutdown = false;
		rcu_assign_pointer(g_profile_blob, b);
	}
	return 0;
}

u32 nksu_profile_count(void)
{
	struct nksu_profile_blob *b;
	u32 count = 0;

	rcu_read_lock();
	b = rcu_dereference(g_profile_blob);
	if (b)
		count = b->count;
	rcu_read_unlock();
	return count;
}

void nksu_profile_foreach(nksu_profile_iter_fn fn, void *ctx)
{
	struct nksu_profile_blob *b;
	u32 i;

	if (!fn)
		return;

	rcu_read_lock();
	b = rcu_dereference(g_profile_blob);
	if (b) {
		for (i = 0; i < b->count; i++) {
			struct nksu_profile_entry *e = &b->entries[i];
			const char *domain = "";
			size_t domain_len = 0;

			if (blob_domain_ok(b, e)) {
				domain = b->arena + e->domain_off;
				domain_len = e->domain_len;
			}
			fn(e->uid, e->root_uid, entry_caps(e), e->ns, domain, domain_len, ctx);
		}
	}
	rcu_read_unlock();
}
