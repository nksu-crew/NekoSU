// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Kernel symbol resolution -- cache and lookup policy.
 *
 * Android GKI/KMI kernels strip a large set of internal symbols (SELinux,
 * cred, namespace, ...) from the export table. A module that references them
 * directly fails to load with:
 *
 *     nksu: Unknown symbol selinux_state (err -2)
 *
 * The historical approach, kallsyms_lookup_name(), is no longer exported as
 * of 5.7, so "ask the kernel for the symbol" is itself unavailable. This
 * subsystem takes a fully self-contained route and caches every resolved
 * symbol for later lookups:
 *
 *   1. /proc/kallsyms (cheapest when kptr_restrict is relaxed)
 *   2. the in-memory kallsyms tables (reliable source for unexported symbols)
 *
 * The /proc parser lives in symbol_proc.c, the memory scanner in
 * symbol_scan.c and the compressed-table decoder in symbol_kallsyms.c.
 */

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/hashtable.h>
#include <linux/atomic.h>

#include "klog.h"
#include "guards.h"
#include "symbol/symbol.h"
#include "symbol/symbol_internal.h"

/*
 * 4.9 still exports kallsyms_lookup_name() (the export was removed in 5.7).
 * Use it as the primary source there: it is cheap and independent of the
 * in-memory table layout.  The reference is weak so a vendor kernel that
 * dropped the export still loads (the pointer is then NULL and the /proc /
 * scanner fallbacks take over).
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 7, 0)
extern unsigned long kallsyms_lookup_name(const char *name) __attribute__((weak));
#endif

/* cache buckets: 2^18, very low load for ~100k symbols */
static DEFINE_HASHTABLE(ksym_htable, 18);
static DEFINE_MUTEX(ksym_lock);
static atomic_t ksym_count = ATOMIC_INIT(0);

/* the heavy in-memory kallsyms scan runs at most once */
static bool ksym_scanned;

struct ksym_entry {
	struct hlist_node node;
	unsigned long addr;
	char name[];
};

static u32 ksym_hash(const char *name)
{
	u32 h = 5381;

	while (*name)
		h = h * 33 + (unsigned char)*name++;
	return h;
}

static struct ksym_entry *ksym_find_locked(const char *name)
{
	struct ksym_entry *e;
	u32 h = ksym_hash(name);

	hash_for_each_possible(ksym_htable, e, node, h) {
		if (strcmp(e->name, name) == 0)
			return e;
	}
	return NULL;
}

void nksu_ksym_cache_add_locked(const char *name, unsigned long addr)
{
	struct ksym_entry *e;

	if (!name || !name[0] || !addr || ksym_find_locked(name))
		return;

	e = kmalloc(sizeof(*e) + strlen(name) + 1, GFP_KERNEL);
	if (!e)
		return;

	strcpy(e->name, name);
	e->addr = addr;
	hash_add(ksym_htable, &e->node, ksym_hash(name));
	atomic_inc(&ksym_count);
}

unsigned long nksu_ksym_lookup(const char *name)
{
	unsigned long addr;
	struct ksym_entry *e;

	if (!name || !name[0])
		return 0;

	guard(nksu_mutex)(&ksym_lock);

	e = ksym_find_locked(name);
	if (e)
		return e->addr;

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 7, 0)
	/* 0) 4.9 still exports kallsyms_lookup_name() (weak: may be absent). */
	if (kallsyms_lookup_name) {
		addr = kallsyms_lookup_name(name);
		if (addr) {
			nksu_ksym_cache_add_locked(name, addr);
			return addr;
		}
	}
#endif

	/* 1) /proc/kallsyms */
	addr = nksu_ksym_lookup_proc(name);
	if (addr) {
		nksu_ksym_cache_add_locked(name, addr);
		return addr;
	}

	/*
	 * 2) Scan kernel memory once, then re-check the cache. The scan is
	 *    expensive, so it must never run twice even if it finds nothing.
	 */
	if (!ksym_scanned) {
		ksym_scanned = true;
		nksu_ksym_scan_memory();

		e = ksym_find_locked(name);
		addr = e ? e->addr : 0;
	} else {
		addr = 0;
	}

	return addr;
}

unsigned long nksu_ksym_count(void)
{
	return (unsigned long)atomic_read(&ksym_count);
}

void nksu_ksym_cache_clear(void)
{
	struct ksym_entry *e;
	struct hlist_node *tmp;
	int bkt;

	guard(nksu_mutex)(&ksym_lock);
	hash_for_each_safe(ksym_htable, bkt, tmp, e, node) {
		hash_del(&e->node);
		kfree(e);
	}
	atomic_set(&ksym_count, 0);
	ksym_scanned = false;
	nksu_ksym_proc_reset();
}

#ifdef CONFIG_NKSU_DEBUG
void nksu_ksym_dump(void)
{
	struct ksym_entry *e;
	int bkt;

	guard(nksu_mutex)(&ksym_lock);
	hash_for_each(ksym_htable, bkt, e, node)
		pr_info("[ksym] %s = 0x%lx\n", e->name, e->addr);
}
#else
void nksu_ksym_dump(void)
{
}
#endif
