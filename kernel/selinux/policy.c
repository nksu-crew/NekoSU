// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Policy lifecycle: duplicate the live policy, mutate it, restore it, and
 * apply NekoSU's static rule set.
 *
 * The duplication uses the same policydb_write()/policydb_read() round-trip as
 * KernelSU: it yields a fully independent policy, so the rule engine can
 * mutate types, avtab and filename_trans without touching the original.
 *
 * The static rule set mirrors KernelSU's apply_kernelsu_rules() (domain, an
 * unconstrained file type, allow-any-any, the ioctl xperms and the
 * Magisk/KernelSU-compatible allow list), with NekoSU's domain names.
 *
 * Locking: policy_mutex protects selinux_state.policy.
 */

#include "klog.h"
#include "guards.h"
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/rcupdate.h>
#include <linux/version.h>
#include <linux/printk.h>
#include <linux/gfp.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <fmac.h>

#include "ss/policydb.h"
#include "ss/services.h"
#include "ss/avtab.h"
#include "ss/ebitmap.h"
#include "ss/hashtab.h"

#include "symbol/symbol_compat.h"

/* the pre-hook policy, restored on unload */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 7, 0)
static struct selinux_policy *nksu_orig_policy __read_mostly;
static struct selinux_policy *nksu_work_policy __read_mostly;

struct selinux_policy *nksu_orig_policy_get(void)
{
	return nksu_orig_policy;
}
#else
/*
 * 4.9 keeps the policy tables directly in struct selinux_ss.  The original
 * policydb struct is saved aside and the working deep copy is swapped into the
 * live ss in place, so the ss pointer (and its status page / locks) never
 * changes.
 */
static struct policydb *nksu_orig_pdb __read_mostly;

struct policydb *nksu_orig_policy_get(void)
{
	return nksu_orig_pdb;
}
#endif

/*
 * Independent deep-copy of a selinux_policy via the policydb (de)serializer,
 * exactly like KernelSU's ksu_dup_sepolicy.  This is what makes mutation and
 * eventual destruction of the copy safe.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 7, 0)
static struct selinux_policy *nksu_policy_dup(struct selinux_policy *old_pol)
#else
static struct policydb *nksu_policy_dup(struct policydb *old_pdb)
#endif
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 7, 0)
	struct selinux_policy *new_pol;
#else
	struct policydb *new_pdb;
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 7, 0)
	struct policydb *old_pdb = &old_pol->policydb;
#endif
	struct policy_file fp;
	size_t len;
	void *data;
	int ret;

	// policydb_read() adds each type to its own attribute map; keep one
	// spare ebitmap entry per type so the round-trip never returns -EINVAL.
	len = old_pdb->len + (size_t)old_pdb->p_types.nprim * (sizeof(u32) + sizeof(u64));

	data = vmalloc(len);
	if (!data)
		return ERR_PTR(-ENOMEM);

	fp.data = data;
	fp.len = len;

	ret = policydb_write(old_pdb, &fp);
	if (ret) {
		pr_err("[selinux]: policydb_write: %d\n", ret);
		vfree(data);
		return ERR_PTR(ret);
	}
	len -= fp.len;

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 18, 0)
	/* re-add the Android netlink config bits the writer may have dropped */
	{
		static const size_t kConfigOff = 20; /* 4*2+8+4 */

		if (len >= kConfigOff + sizeof(u32)) {
			u32 *config_ptr = (u32 *)((unsigned long)data + kConfigOff);

			if (old_pdb->android_netlink_route)
				*config_ptr |= POLICYDB_CONFIG_ANDROID_NETLINK_ROUTE;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 0, 0)
			if (old_pdb->android_netlink_getneigh)
				*config_ptr |= POLICYDB_CONFIG_ANDROID_NETLINK_GETNEIGH;
#endif
		}
	}
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 7, 0)
	/*
	 * kmemdup() copies selinux_policy.sidtab too, so the working copy
	 * shares the original's sidtab. See the KNOWN RISK note in
	 * sepolicy_restore() for why that matters on unload.
	 */
	new_pol = kmemdup(old_pol, sizeof(*old_pol), GFP_KERNEL);
	if (!new_pol) {
		vfree(data);
		return ERR_PTR(-ENOMEM);
	}
	memset(&new_pol->policydb, 0, sizeof(new_pol->policydb));

	fp.data = data;
	fp.len = len;

	ret = policydb_read(&new_pol->policydb, &fp);
	if (ret) {
		pr_err("[selinux]: policydb_read: %d\n", ret);
		kfree(new_pol);
		vfree(data);
		return ERR_PTR(ret);
	}
	new_pol->policydb.len = len;
#else
	new_pdb = kzalloc(sizeof(*new_pdb), GFP_KERNEL);
	if (!new_pdb) {
		vfree(data);
		return ERR_PTR(-ENOMEM);
	}

	fp.data = data;
	fp.len = len;

	ret = policydb_read(new_pdb, &fp);
	if (ret) {
		pr_err("[selinux]: policydb_read: %d\n", ret);
		kfree(new_pdb);
		vfree(data);
		return ERR_PTR(ret);
	}
	new_pdb->len = len;
#endif
	vfree(data);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 7, 0)
	return new_pol;
#else
	return new_pdb;
#endif
}

/*
 * Clone the live policy while holding only the read lock for the actual
 * serializer copy (policydb_write() cannot sleep), then parse it back with
 * allocation permitted.  Used by the 4.9 rule engine, which must not allocate
 * under the policy write lock.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 7, 0)
struct policydb *nksu_policydb_dup_live(void)
{
	struct policydb *new_pdb;
	struct policy_file fp;
	size_t alloc, need;
	void *data;
	u32 netlink_route;
	int ret, attempt;

	if (!selinux_state.ss)
		return ERR_PTR(-ENOENT);

	for (attempt = 0; attempt < 3; attempt++) {
		alloc = selinux_state.ss->policydb.len +
			(size_t)selinux_state.ss->policydb.p_types.nprim *
				(sizeof(u32) + sizeof(u64)) + 4096;
		data = vmalloc(alloc);
		if (!data)
			return ERR_PTR(-ENOMEM);

		read_lock(&selinux_state.ss->policy_rwlock);
		need = selinux_state.ss->policydb.len +
		       (size_t)selinux_state.ss->policydb.p_types.nprim *
			       (sizeof(u32) + sizeof(u64));
		if (need > alloc) {
			read_unlock(&selinux_state.ss->policy_rwlock);
			vfree(data);
			continue;
		}
		netlink_route = selinux_state.ss->policydb.android_netlink_route;
		fp.data = data;
		fp.len = alloc;
		ret = policydb_write(&selinux_state.ss->policydb, &fp);
		read_unlock(&selinux_state.ss->policy_rwlock);
		if (ret) {
			vfree(data);
			return ERR_PTR(ret);
		}
		if (fp.len > alloc) {
			/* put_entry() underflowed: the buffer was too small. */
			vfree(data);
			return ERR_PTR(-EOVERFLOW);
		}
		alloc -= fp.len;

		/* policydb_write() drops the Android netlink config bit. */
		if (alloc >= 24 && netlink_route) {
			u32 *config_ptr = (u32 *)((unsigned long)data + 20);

			*config_ptr |= POLICYDB_CONFIG_ANDROID_NETLINK_ROUTE;
		}

		new_pdb = kzalloc(sizeof(*new_pdb), GFP_KERNEL);
		if (!new_pdb) {
			vfree(data);
			return ERR_PTR(-ENOMEM);
		}

		fp.data = data;
		fp.len = alloc;
		ret = policydb_read(new_pdb, &fp);
		vfree(data);
		if (ret) {
			kfree(new_pdb);
			return ERR_PTR(ret);
		}
		new_pdb->len = alloc;
		return new_pdb;
	}

	return ERR_PTR(-EAGAIN);
}

void nksu_policydb_free(struct policydb *pdb)
{
	if (!pdb)
		return;
	policydb_destroy(pdb);
	kfree(pdb);
}

int nksu_policydb_install(struct policydb *new_pdb)
{
	struct policydb *old;
	unsigned long flags;

	if (!new_pdb || !selinux_state.ss) {
		nksu_policydb_free(new_pdb);
		return -EINVAL;
	}

	/*
	 * Keep the outgoing tables on the heap so destroying them happens
	 * after the swap, outside the lock.
	 */
	old = kmalloc(sizeof(*old), GFP_KERNEL);
	if (!old) {
		nksu_policydb_free(new_pdb);
		return -ENOMEM;
	}

	write_lock_irqsave(&selinux_state.ss->policy_rwlock, flags);
	*old = selinux_state.ss->policydb;
	selinux_state.ss->policydb = *new_pdb;
	write_unlock_irqrestore(&selinux_state.ss->policy_rwlock, flags);

	/* The struct container is now redundant; its tables are live. */
	kfree(new_pdb);

	/*
	 * The clean policy kept for SELinux hiding must never be freed here;
	 * rules are only applied after the working copy was installed, so this
	 * is a safety net rather than an expected path.
	 */
	if (nksu_orig_pdb && old->p_types.table == nksu_orig_pdb->p_types.table) {
		pr_err("[selinux]: refusing to free the clean policy\n");
		kfree(old);
		return 0;
	}

	policydb_destroy(old);
	kfree(old);
	return 0;
}
#endif

/*
 * Clone the live policy and RCU-swap it in, keeping the original aside so
 * unload can restore it.
 */
int sepolicy_dup_and_apply(void)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 7, 0)
	struct selinux_policy *orig, *work;

	if (nksu_orig_policy) {
		pr_warn("[selinux]: sepolicy_dup_and_apply: already active\n");
		return -EBUSY;
	}

	scoped_guard(nksu_mutex, &selinux_state.policy_mutex)
	{
		orig = rcu_dereference_protected(selinux_state.policy,
						 lockdep_is_held(&selinux_state.policy_mutex));
		if (!orig) {
			pr_err("[selinux]: sepolicy_dup_and_apply: no live policy\n");
			return -ENOENT;
		}

		work = nksu_policy_dup(orig);
		if (IS_ERR(work)) {
			pr_err("[selinux]: sepolicy_dup_and_apply: dup failed\n");
			return PTR_ERR(work);
		}

		nksu_orig_policy = orig;
		nksu_work_policy = work;

		rcu_assign_pointer(selinux_state.policy, work);
	}
	synchronize_rcu();
#else
	struct policydb *live, *saved, *work;
	struct policy_file fp;
	size_t alloc;
	void *data;
	int ret;

	if (nksu_orig_pdb) {
		pr_warn("[selinux]: sepolicy_dup_and_apply: already active\n");
		return -EBUSY;
	}
	if (!selinux_state.ss) {
		pr_err("[selinux]: sepolicy_dup_and_apply: no live policy\n");
		return -ENOENT;
	}

	live = &selinux_state.ss->policydb;
	saved = kmalloc(sizeof(*saved), GFP_KERNEL);
	if (!saved)
		return -ENOMEM;

	/*
	 * Serialise under the policy write lock, but only with a preallocated
	 * buffer: policydb_write() just copies memory and cannot sleep.
	 */
	alloc = live->len +
		(size_t)live->p_types.nprim * (sizeof(u32) + sizeof(u64));
	data = vmalloc(alloc);
	if (!data) {
		kfree(saved);
		return -ENOMEM;
	}

	write_lock_irq(&selinux_state.ss->policy_rwlock);
	*saved = *live;
	fp.data = data;
	fp.len = alloc;
	ret = policydb_write(live, &fp);
	write_unlock_irq(&selinux_state.ss->policy_rwlock);
	if (ret) {
		pr_err("[selinux]: policydb_write: %d\n", ret);
		vfree(data);
		kfree(saved);
		return ret;
	}

	{
		size_t len = alloc - fp.len;

		if (len >= 24) {
			u32 *config_ptr = (u32 *)((unsigned long)data + 20);

			if (saved->android_netlink_route)
				*config_ptr |= POLICYDB_CONFIG_ANDROID_NETLINK_ROUTE;
		}

		work = kzalloc(sizeof(*work), GFP_KERNEL);
		if (!work) {
			vfree(data);
			kfree(saved);
			return -ENOMEM;
		}

		fp.data = data;
		fp.len = len;
		ret = policydb_read(work, &fp);
		if (ret) {
			pr_err("[selinux]: policydb_read: %d\n", ret);
			kfree(work);
			vfree(data);
			kfree(saved);
			return ret;
		}
		work->len = len;
	}
	vfree(data);

	write_lock_irq(&selinux_state.ss->policy_rwlock);
	/* A concurrent reload would have replaced the tables underneath us. */
	if (selinux_state.ss->policydb.p_types.table != saved->p_types.table) {
		write_unlock_irq(&selinux_state.ss->policy_rwlock);
		pr_err("[selinux]: policy reloaded during dup, retrying next boot\n");
		policydb_destroy(work);
		kfree(work);
		kfree(saved);
		return -EAGAIN;
	}
	selinux_state.ss->policydb = *work;
	kfree(work);
	write_unlock_irq(&selinux_state.ss->policy_rwlock);

	nksu_orig_pdb = saved;
#endif

	pr_info("[selinux]: policy duplicated, working copy installed\n");
	return 0;
}

void sepolicy_restore(void)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 7, 0)
	struct selinux_policy *live;

	if (!nksu_orig_policy) {
		pr_warn("[selinux]: sepolicy_restore: nothing to restore\n");
		return;
	}

	scoped_guard(nksu_mutex, &selinux_state.policy_mutex)
	{
		live = rcu_dereference_protected(selinux_state.policy,
						 lockdep_is_held(&selinux_state.policy_mutex));
		if (live == nksu_work_policy)
			rcu_assign_pointer(selinux_state.policy, nksu_orig_policy);
		else
			pr_warn("[selinux]: live policy already replaced, leaving it\n");
	}
	synchronize_rcu();

	/*
	 * Deliberately leak the working copy: freeing it raced live SELinux
	 * permission checks on other CPUs.  One leaked policy per load is a
	 * small price for a safe unload, and it mirrors KernelSU, which never
	 * tears its policy down either.
	 *
	 * KNOWN RISK (unload path): nksu_policy_dup() shares the original
	 * sidtab with the working copy, and the rules injected while the copy
	 * was live caused SIDs for types that exist only in the working
	 * policydb (nksu / nksu_file) to be inserted into that shared sidtab.
	 * Pointing selinux_state.policy back at the original policydb leaves
	 * those SIDs referencing type values past the original's type arrays,
	 * so a later access check or sid_to_context on them can index
	 * type_attr_map_array / sym_val_to_name out of bounds.  Not fixed
	 * here: making it safe needs the working copy to own a separate
	 * sidtab (or the original to be re-cloned into it), which is a larger
	 * change than this unload path warrants.
	 */
	nksu_work_policy = NULL;
	nksu_orig_policy = NULL;
#else
	if (!nksu_orig_pdb) {
		pr_warn("[selinux]: sepolicy_restore: nothing to restore\n");
		return;
	}

	write_lock_irq(&selinux_state.ss->policy_rwlock);
	selinux_state.ss->policydb = *nksu_orig_pdb;
	write_unlock_irq(&selinux_state.ss->policy_rwlock);

	kfree(nksu_orig_pdb);
	nksu_orig_pdb = NULL;
#endif

	avc_reset();

	pr_info("[selinux]: original policy restored (working copy kept)\n");
}

/* ---- static rule set (aligned with KernelSU's apply_kernelsu_rules) ---- */

#define ALL NULL

static void apply(const char *src, const char *tgt, const char *cls, const char *perm)
{
	sepolicy_add_rule(src, tgt, cls, perm, AVTAB_ALLOWED, false);
}

static bool xperm_supported(void)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 7, 0)
	struct selinux_policy *pol;
	bool ok;

	guard(nksu_mutex)(&selinux_state.policy_mutex);
	pol = rcu_dereference_protected(selinux_state.policy,
					lockdep_is_held(&selinux_state.policy_mutex));
	ok = pol && pol->policydb.policyvers >= POLICYDB_VERSION_XPERMS_IOCTL;
	return ok;
#else
	bool ok;

	read_lock(&selinux_state.ss->policy_rwlock);
	ok = selinux_state.ss->policydb.policyvers >= POLICYDB_VERSION_XPERMS_IOCTL;
	read_unlock(&selinux_state.ss->policy_rwlock);
	return ok;
#endif
}

int load_policy(void)
{
	if (!getenforce())
		pr_info("[selinux]: enforcing is false, applying rules anyway\n");

	sepolicy_batch_begin();

	/* our domain: unconfined, permissive in debug builds */
	sepolicy_add_domain(DOMAIN);
	sepolicy_set_permissive(DOMAIN);
	sepolicy_add_typeattribute(DOMAIN, "mlstrustedsubject");
	sepolicy_add_typeattribute(DOMAIN, "netdomain");
	sepolicy_add_typeattribute(DOMAIN, "bluetoothdomain");

	/* an unconstrained file type for the daemon's own files */
	sepolicy_add_type(DOMAIN_FILE);
	sepolicy_add_typeattribute(DOMAIN_FILE, "file_type");
	sepolicy_add_typeattribute(DOMAIN_FILE, "mlstrustedobject");
	apply("domain", DOMAIN_FILE, ALL, ALL);

	/*
	 * `cmd`/`pm` run from a root shell hand the caller's tty to
	 * system_server through the binder ShellCallback, and system_server
	 * writes the command output back to that app-owned devpts device.
	 * Grant it access to the common app devpts types (and lift their MLS
	 * constraint) so the tty keeps its own label: a relabeled pty -- to
	 * nksu_file before, to generic devpts now -- is a root-solution
	 * fingerprint that detectors look for on /dev/pts.  Types that do not
	 * exist on this device are skipped with a log.
	 */
	{
		static const char *const app_devpts_types[] = {
			"untrusted_app_all_devpts",
			"untrusted_app_devpts",
			"platform_app_devpts",
			"system_app_devpts",
			"priv_app_devpts",
			"isolated_app_devpts",
			"shell_devpts",
			"devpts",
		};
		size_t i;
		size_t n = sizeof(app_devpts_types) / sizeof(app_devpts_types[0]);

		for (i = 0; i < n; i++) {
			apply("system_server", app_devpts_types[i], "chr_file", ALL);
			sepolicy_add_typeattribute(app_devpts_types[i], "mlstrustedobject");
		}
	}

	/* allow all */
	apply(DOMAIN, ALL, ALL, ALL);

	/* allow any ioctl */
	if (xperm_supported()) {
		sepolicy_add_xperm(DOMAIN, ALL, "blk_file", ALL, AVTAB_XPERMS_ALLOWED, false);
		sepolicy_add_xperm(DOMAIN, ALL, "fifo_file", ALL, AVTAB_XPERMS_ALLOWED, false);
		sepolicy_add_xperm(DOMAIN, ALL, "chr_file", ALL, AVTAB_XPERMS_ALLOWED, false);
		sepolicy_add_xperm(DOMAIN, ALL, "file", ALL, AVTAB_XPERMS_ALLOWED, false);
	}

	/* our daemon triggered by init */
	apply("init", DOMAIN, ALL, ALL);

	/* copied from Magisk/KernelSU rules */
	apply("servicemanager", DOMAIN, "dir", "search");
	apply("servicemanager", DOMAIN, "dir", "read");
	apply("servicemanager", DOMAIN, "file", "open");
	apply("servicemanager", DOMAIN, "file", "read");
	apply("servicemanager", DOMAIN, "process", "getattr");
	apply("domain", DOMAIN, "process", "sigchld");

	apply("logd", DOMAIN, "dir", "search");
	apply("logd", DOMAIN, "file", "read");
	apply("logd", DOMAIN, "file", "open");
	apply("logd", DOMAIN, "file", "getattr");

	apply("domain", DOMAIN, "fd", "use");
	apply("domain", DOMAIN, "fifo_file", "write");
	apply("domain", DOMAIN, "fifo_file", "read");
	apply("domain", DOMAIN, "fifo_file", "open");
	apply("domain", DOMAIN, "fifo_file", "getattr");
	apply("domain", DOMAIN, "unix_stream_socket", "read");
	apply("domain", DOMAIN, "unix_stream_socket", "write");
	apply("domain", DOMAIN, "unix_stream_socket", "connectto");
	apply("domain", DOMAIN, "unix_stream_socket", "getopt");
	apply("domain", DOMAIN, "unix_stream_socket", "getattr");

	apply("domain", DOMAIN, "memfd_file", "execute");
	apply("domain", DOMAIN, "memfd_file", "getattr");
	apply("domain", DOMAIN, "memfd_file", "map");
	apply("domain", DOMAIN, "memfd_file", "read");
	apply("domain", DOMAIN, "memfd_file", "write");

	apply("hwservicemanager", DOMAIN, "dir", "search");
	apply("hwservicemanager", DOMAIN, "file", "read");
	apply("hwservicemanager", DOMAIN, "file", "open");
	apply("hwservicemanager", DOMAIN, "process", "getattr");

	apply("domain", DOMAIN, "binder", ALL);

	apply("system_server", DOMAIN, "process", "getpgid");
	apply("system_server", DOMAIN, "process", "sigkill");

	sepolicy_batch_end();

	pr_info("[selinux]: policy for domain '%s' loaded\n", DOMAIN);
	return 0;
}

int sepolicy_init(void)
{
	return load_policy();
}

void sepolicy_exit(void)
{
	pr_info("[selinux]: sepolicy exit\n");
}
