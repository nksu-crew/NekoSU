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
 * The historical approach, kallsyms_lookup_name(), is still exported up to
 * 5.6, so it is called directly there.  From 5.7 on the export is gone, but
 * the function is still present in the kernel's kallsyms table, so we
 * recover its address with the classic kprobe trick:
 *
 *     struct kprobe kp = { .symbol_name = "kallsyms_lookup_name" };
 *     register_kprobe(&kp);        // the kprobe core resolves the name
 *     addr = (kallsyms_lookup_name_t)kp.addr;
 *     unregister_kprobe(&kp);
 *
 * From then on every symbol (exported or not) is resolved through that
 * function and cached for later lookups.
 */

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/hashtable.h>
#include <linux/atomic.h>
#include <linux/kprobes.h>

#include "klog.h"
#include "guards.h"
#include "symbol/symbol.h"

typedef unsigned long (*kallsyms_lookup_name_t)(const char *name);

/*
 * Up to 5.6 kallsyms_lookup_name() is exported, so reference it directly and
 * skip the kprobe entirely.  The weak reference keeps vendor kernels that
 * dropped the export loadable: the kprobe then recovers the address from the
 * kallsyms table.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 7, 0)
extern unsigned long kallsyms_lookup_name(const char *name) __attribute__((weak));
#endif

/* cache buckets: 2^18, very low load for ~100k symbols */
static DEFINE_HASHTABLE(ksym_htable, 18);
static DEFINE_MUTEX(ksym_lock);
static atomic_t ksym_count = ATOMIC_INIT(0);

/* resolved kallsyms_lookup_name() and its one-shot probe state */
static kallsyms_lookup_name_t ksym_lookup_name;
static bool ksym_lookup_name_tried;

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

static void ksym_cache_add_locked(const char *name, unsigned long addr)
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

/*
 * Resolve kallsyms_lookup_name() without scanning the kernel image.
 *
 * On <= 5.6 it is exported and used directly.  On newer kernels register a
 * throwaway kprobe on the symbol name, let the kprobe core do the kallsyms
 * lookup, then read the resolved address back from kp.addr.
 *
 * Called with ksym_lock held.  The probe is attempted only once: a failure is
 * permanent, so remember it instead of retrying (and logging) on every miss.
 */
static int ksym_resolve_lookup_name(void)
{
	struct kprobe kln_kp;
	int ret;

	if (ksym_lookup_name)
		return 0;

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 7, 0)
	/* Exported: use the real symbol, no kprobe needed. */
	if (kallsyms_lookup_name) {
		ksym_lookup_name = kallsyms_lookup_name;
		return 0;
	}
#endif

	if (ksym_lookup_name_tried)
		return -ENOENT;

	ksym_lookup_name_tried = true;

	kln_kp = (struct kprobe){ .symbol_name = "kallsyms_lookup_name" };
	ret = register_kprobe(&kln_kp);
	if (ret < 0) {
		pr_err("[ksym] register_kprobe(kallsyms_lookup_name) failed: %d\n", ret);
		return ret;
	}

	ksym_lookup_name = (kallsyms_lookup_name_t)kln_kp.addr;
	unregister_kprobe(&kln_kp);

	if (!ksym_lookup_name) {
		pr_err("[ksym] kallsyms_lookup_name not found\n");
		return -EINVAL;
	}

	return 0;
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

	/* kallsyms_lookup_name(), recovered through a kprobe. */
	if (ksym_resolve_lookup_name() == 0) {
		addr = ksym_lookup_name(name);
		if (addr) {
			ksym_cache_add_locked(name, addr);
			return addr;
		}
	}

	return 0;
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
	ksym_lookup_name = NULL;
	ksym_lookup_name_tried = false;
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
