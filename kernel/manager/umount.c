// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Module mount handling: syscall observation, per-process removal and mount-id
 * isolation all live here.  The sys_enter probe is registered by this module
 * itself; the main syscall hook is not involved.
 */
#include <linux/cred.h>
#include <linux/atomic.h>
#include <linux/dcache.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/idr.h>
#include <linux/kernel.h>
#include <linux/kconfig.h>
#include <linux/kprobes.h>
#include <linux/limits.h>
#include <linux/list.h>
#include <linux/mount.h>
#include <linux/mutex.h>
#include <linux/namei.h>
#include <linux/nsproxy.h>
#include <linux/module.h>
#include <linux/path.h>
#include <linux/ptrace.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/syscalls.h>
#include <linux/task_work.h>
#include <linux/tracepoint.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <asm/ptrace.h>

#include "hook/tools.h"
#include "klog.h"
#include "manager/feature.h"
#include "manager/umount.h"
#include "nksu_task.h"
#include "selinux/selinux.h"
#include "symbol/symbol.h"
#include "symbol/symbol_compat.h"

struct nksu_umount_entry {
	struct list_head list;
	char *path;
	int flags;
};

struct mnt_namespace;

static LIST_HEAD(nksu_umount_list);
static DEFINE_SPINLOCK(nksu_umount_lock);
static DEFINE_MUTEX(nksu_umount_state_lock);
static bool nksu_umount_running;
static struct mnt_namespace *nksu_done_mnt_ns;
static u64 nksu_done_mnt_gen;
static atomic64_t nksu_umount_gen = ATOMIC64_INIT(0);
static bool nksu_mnt_ns_hooks_ready;

int nksu_umount_add(const char *path, int flags)
{
	struct nksu_umount_entry *entry;
	unsigned long irqflags;

	if (!path || !path[0])
		return -EINVAL;

	spin_lock_irqsave(&nksu_umount_lock, irqflags);
	list_for_each_entry(entry, &nksu_umount_list, list) {
		if (!strcmp(entry->path, path)) {
			spin_unlock_irqrestore(&nksu_umount_lock, irqflags);
			/* A remount at the same path still invalidates the cache. */
			atomic64_inc(&nksu_umount_gen);
			return 0;
		}
	}
	spin_unlock_irqrestore(&nksu_umount_lock, irqflags);

	entry = kzalloc(sizeof(*entry), GFP_ATOMIC);
	if (!entry)
		return -ENOMEM;
	entry->path = kstrdup(path, GFP_ATOMIC);
	if (!entry->path) {
		kfree(entry);
		return -ENOMEM;
	}
	entry->flags = flags;

	spin_lock_irqsave(&nksu_umount_lock, irqflags);
	list_add_tail(&entry->list, &nksu_umount_list);
	spin_unlock_irqrestore(&nksu_umount_lock, irqflags);
	atomic64_inc(&nksu_umount_gen);
	return 0;
}

static void nksu_umount_free_list(struct list_head *list)
{
	struct nksu_umount_entry *entry, *tmp;

	list_for_each_entry_safe(entry, tmp, list, list) {
		list_del(&entry->list);
		kfree(entry->path);
		kfree(entry);
	}
}

void nksu_umount_wipe(void)
{
	LIST_HEAD(victim);
	unsigned long irqflags;

	spin_lock_irqsave(&nksu_umount_lock, irqflags);
	list_splice_init(&nksu_umount_list, &victim);
	spin_unlock_irqrestore(&nksu_umount_lock, irqflags);

	nksu_umount_free_list(&victim);
	WRITE_ONCE(nksu_done_mnt_ns, NULL);
}

static bool nksu_umount_empty(void)
{
	bool empty;
	unsigned long irqflags;

	spin_lock_irqsave(&nksu_umount_lock, irqflags);
	empty = list_empty(&nksu_umount_list);
	spin_unlock_irqrestore(&nksu_umount_lock, irqflags);
	return empty;
}

/*
 * A path registered at mount(2) entry may never have become a mount (the
 * syscall can fail).  Only remove the mount when its root dentry lives under
 * the module store, so a stale entry can never unmount an unrelated mount.
 *
 * Walking the dentry parents is intentional: d_path() would resolve the mount
 * root through the namespace and report the mountpoint ("/system/..."), not
 * the filesystem-relative root (".../adb/modules/...") we need here.
 */
static bool nksu_umount_module_root(struct dentry *root)
{
	struct dentry *dentry;
	bool saw_modules = false;
	int depth;

	rcu_read_lock();
	dentry = root;
	for (depth = 0; dentry && depth < 16; depth++) {
		const char *name = (const char *)dentry->d_name.name;

		if (!strcmp(name, "modules") || !strcmp(name, "modules_update"))
			saw_modules = true;
		else if (saw_modules && !strcmp(name, "adb")) {
			rcu_read_unlock();
			return true;
		}

		if (dentry == dentry->d_parent)
			break;
		dentry = dentry->d_parent;
	}
	rcu_read_unlock();
	return false;
}

struct nksu_umount_snapshot {
	char path[256];
	int flags;
};

static struct nksu_umount_snapshot *
nksu_umount_take_snapshot(size_t *count, struct nksu_umount_snapshot *inline_buf, size_t inline_n)
{
	struct nksu_umount_snapshot *snapshot;
	struct nksu_umount_entry *entry;
	unsigned long irqflags;
	size_t n = 0, i = 0;

	*count = 0;

	spin_lock_irqsave(&nksu_umount_lock, irqflags);
	list_for_each_entry(entry, &nksu_umount_list, list)
		n++;
	spin_unlock_irqrestore(&nksu_umount_lock, irqflags);

	if (!n)
		return NULL;

	if (n <= inline_n) {
		snapshot = inline_buf;
	} else {
		snapshot = kmalloc_array(n, sizeof(*snapshot), GFP_KERNEL);
		if (!snapshot) {
			pr_warn_ratelimited("[umount] cannot allocate snapshot\n");
			return NULL;
		}
	}

	spin_lock_irqsave(&nksu_umount_lock, irqflags);
	list_for_each_entry(entry, &nksu_umount_list, list) {
		if (i == n)
			break;
		strscpy(snapshot[i].path, entry->path, sizeof(snapshot[i].path));
		snapshot[i].flags = entry->flags;
		i++;
	}
	spin_unlock_irqrestore(&nksu_umount_lock, irqflags);

	*count = i;
	return snapshot;
}

static bool nksu_mnt_ns_done(void)
{
	struct mnt_namespace *ns;

	if (!READ_ONCE(nksu_mnt_ns_hooks_ready) || !current->nsproxy)
		return false;

	ns = READ_ONCE(current->nsproxy->mnt_ns);
	if (!ns)
		return false;

	return READ_ONCE(nksu_done_mnt_ns) == ns &&
	       READ_ONCE(nksu_done_mnt_gen) == atomic64_read(&nksu_umount_gen);
}

static void nksu_umount_run(void)
{
	const struct cred *old;
	/*
	 * Keep the stack copy small: this function is inlined into the
	 * task_work callback, which is built with -Wframe-larger-than=2048.
	 * Larger lists fall back to the heap snapshot.
	 */
	struct nksu_umount_snapshot inline_buf[4];
	struct nksu_umount_snapshot *snapshot;
	struct mnt_namespace *ns;
	u64 gen;
	size_t count = 0, i;
	bool failed = false;

	if (!READ_ONCE(nksu_umount_running) || nksu_umount_empty())
		return;

	if (nksu_mnt_ns_done())
		return;

	ns = current->nsproxy ? READ_ONCE(current->nsproxy->mnt_ns) : NULL;
	gen = atomic64_read(&nksu_umount_gen);

	snapshot = nksu_umount_take_snapshot(&count, inline_buf, ARRAY_SIZE(inline_buf));
	if (!snapshot)
		return;

	old = nksu_borrow_creds_begin();
	if (!old) {
		pr_err_ratelimited("[umount] cannot borrow nksu creds\n");
		failed = true;
		goto out;
	}
	for (i = 0; i < count; i++) {
		const char *target = snapshot[i].path;
		struct path path;
		int ret;

		ret = kern_path(target, 0, &path);
		if (ret) {
			pr_debug("[umount] %s: kern_path failed: %d\n", target, ret);
			continue;
		}
		if (path.dentry != path.mnt->mnt_root) {
			pr_debug("[umount] %s is not a mount root, skip\n", target);
			path_put(&path);
			continue;
		}
		if (!nksu_umount_module_root(path.dentry)) {
			pr_debug("[umount] %s is not under the module store, skip\n", target);
			path_put(&path);
			continue;
		}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 9, 0)
		if (!nksu_path_umount) {
			pr_err("[umount] path_umount is unresolved\n");
			path_put(&path);
			failed = true;
			continue;
		}
		ret = path_umount(&path, snapshot[i].flags);
#else
		if (!nksu_sys_umount) {
			pr_err("[umount] sys_umount is unresolved\n");
			path_put(&path);
			failed = true;
			continue;
		}
		{
			mm_segment_t old_fs = get_fs();

			set_fs(KERNEL_DS);
			ret = sys_umount((char __user *)target, snapshot[i].flags);
			set_fs(old_fs);
		}
#endif
		if (ret) {
			pr_info("[umount] %s failed: %d\n", target, ret);
			failed = true;
		} else {
			pr_info_ratelimited("[umount] removed %s (pid %d)\n", target, current->pid);
		}
	}
out:
	if (old)
		nksu_borrow_creds_end(old);

	if (snapshot != inline_buf)
		kfree(snapshot);

	if (ns && !failed) {
		WRITE_ONCE(nksu_done_mnt_ns, ns);
		WRITE_ONCE(nksu_done_mnt_gen, gen);
	}
}

struct nksu_umount_work {
	struct callback_head cb;
};

static void nksu_umount_work_fn(struct callback_head *cb)
{
	struct nksu_umount_work *work = container_of(cb, struct nksu_umount_work, cb);

	nksu_umount_run();
	kfree(work);
	module_put(THIS_MODULE);
}

void nksu_umount_schedule(void)
{
	struct nksu_umount_work *work;

	if (!READ_ONCE(nksu_umount_running))
		return;
	if (nksu_mnt_ns_done())
		return;
	if (nksu_umount_empty())
		return;

	if (!try_module_get(THIS_MODULE)) {
		pr_warn_ratelimited("[umount] cannot pin module for task_work\n");
		return;
	}

	work = kzalloc(sizeof(*work), GFP_ATOMIC);
	if (!work) {
		module_put(THIS_MODULE);
		pr_warn_ratelimited("[umount] cannot allocate task_work\n");
		return;
	}
	init_task_work(&work->cb, nksu_umount_work_fn);
	if (task_work_add(current, &work->cb, TWA_RESUME)) {
		module_put(THIS_MODULE);
		pr_warn_ratelimited("[umount] cannot queue task_work\n");
		kfree(work);
	}
}

/*
 * ---------------------------------------------------------------------------
 * Mount-id isolation
 * ---------------------------------------------------------------------------
 * Every mount created by ncore (and every clone of such a mount) gets an id
 * from the high range instead of the kernel's low, sequential allocator.  The
 * mount-info readers then omit high-id mounts unless the reader is ncore
 * itself.  The low id sequence an app observes therefore stays contiguous even
 * after the per-app umount removed module mounts from its namespace.
 *
 * The struct mount field offsets are supplied by ncore, which reads them from
 * the kernel's BTF.  Hard-coding offsets across GKI versions is not an option.
 */
#if IS_ENABLED(CONFIG_KPROBES)

static typeof(register_kprobe) *nksu_register_kprobe;
static typeof(unregister_kprobe) *nksu_unregister_kprobe;
static typeof(ida_alloc_range) *nksu_ida_alloc_range;
static struct ida *nksu_mnt_id_ida;

static u32 nksu_mnt_id_off;
static u32 nksu_mnt_off;

static bool nksu_mount_layout_ready;
static bool nksu_mount_layout_seen;
static bool nksu_ida_probe_ready;
static bool nksu_mount_probes_ready;
static bool nksu_clone_probe_ready;
static bool nksu_copy_ns_probe_ready;
static bool nksu_install_ns_probe_ready;
static bool nksu_show_probe_ready[3];
static DEFINE_MUTEX(nksu_mount_layout_lock);

#if defined(__aarch64__)
static unsigned long nksu_arg0(const struct pt_regs *regs)
{
	return regs->regs[0];
}

static unsigned long nksu_arg1(const struct pt_regs *regs)
{
	return regs->regs[1];
}
#elif defined(__x86_64__)
static unsigned long nksu_arg0(const struct pt_regs *regs)
{
	return regs->di;
}

static unsigned long nksu_arg1(const struct pt_regs *regs)
{
	return regs->si;
}
#else
#error "unsupported arch"
#endif

/* Skip the probed function and make it return @ret. */
static void nksu_skip_current(struct pt_regs *regs, long ret)
{
#if defined(__aarch64__)
	regs->regs[0] = ret;
	regs->pc = regs->regs[30];
#elif defined(__x86_64__)
	regs->ax = ret;
	regs->ip = *(unsigned long *)regs->sp;
#endif
}

/*
 * mnt_alloc_id() is inlined into alloc_vfsmnt() on newer kernels, so the
 * stable probe point is the ida_alloc_range() call it makes.  Match the IDA
 * object itself: only allocations from mnt_id_ida are redirected.
 */
static int __nocfi nksu_ida_alloc_pre(struct kprobe *p, struct pt_regs *regs)
{
	struct task_struct *task = current;
	struct ida *ida = (struct ida *)nksu_arg0(regs);
	bool clone = nksu_task_clone_mnt(task);
	int id;

	if (ida != nksu_mnt_id_ida || !READ_ONCE(nksu_mount_layout_ready))
		return 0;
	if (!nksu_ida_alloc_range)
		return 0;
	if (!nksu_task_is_ncore(task) && !clone)
		return 0;
	/* The recursive allocation below must reach the real function. */
	if (nksu_task_ida_alloc(task))
		return 0;

	/*
	 * GFP_ATOMIC: kprobe handlers run with preemption disabled.  The
	 * allocation can fail under memory pressure; fall through to the
	 * kernel allocator and make the failure visible instead of handing out
	 * a duplicate id.
	 */
	nksu_task_set_ida_alloc(task);
	id = nksu_ida_alloc_range(nksu_mnt_id_ida, NKSU_MNT_ID_MIN, INT_MAX - 1, GFP_ATOMIC);
	nksu_task_clear_ida_alloc(task);
	if (id < 0) {
		pr_warn_ratelimited("[mount-id] high id allocation failed: %d\n", id);
		if (clone)
			nksu_task_clear_clone_mnt(task);
		return 0;
	}

	if (clone)
		nksu_task_clear_clone_mnt(task);

	nksu_skip_current(regs, id);
	return 1;
}

/*
 * clone_mnt(old, root, flag) calls alloc_vfsmnt() -> mnt_alloc_id() for the
 * copy.  A namespace clone in zygote/app context has no ncore marker, so the
 * copy must inherit the "high id" property from the source mount.
 */
static int nksu_clone_mnt_pre(struct kprobe *p, struct pt_regs *regs)
{
	void *old = (void *)nksu_arg0(regs);
	int id;

	if (!old || !READ_ONCE(nksu_mount_layout_ready))
		return 0;

	id = READ_ONCE(*(int *)((char *)old + nksu_mnt_id_off));
	if (id >= (int)NKSU_MNT_ID_MIN)
		nksu_task_set_clone_mnt(current);

	return 0;
}

/*
 * show_mountinfo()/show_vfsmnt()/show_vfsstat() receive the embedded
 * struct vfsmount, not struct mount.  The offset comes from BTF, so recover
 * the container and read its id.
 */
static int nksu_show_pre(struct kprobe *p, struct pt_regs *regs)
{
	void *vmnt = (void *)nksu_arg1(regs);
	void *mnt;
	int id;

	if (!vmnt || !READ_ONCE(nksu_mount_layout_ready))
		return 0;
	/* ncore must still see its own mounts (module install/debug). */
	if (nksu_task_is_ncore(current))
		return 0;

	mnt = (char *)vmnt - nksu_mnt_off;
	id = READ_ONCE(*(int *)((char *)mnt + nksu_mnt_id_off));
	if (id < (int)NKSU_MNT_ID_MIN)
		return 0;

	nksu_skip_current(regs, 0);
	return 1;
}

static struct kprobe nksu_kp_ida_alloc = {
	.symbol_name = "ida_alloc_range",
	.pre_handler = nksu_ida_alloc_pre,
};

static struct kprobe nksu_kp_clone_mnt = {
	.symbol_name = "clone_mnt",
	.pre_handler = nksu_clone_mnt_pre,
};

/*
 * A namespace gets a fresh mount view; drop the "already processed" marker so
 * the first setresuid in it still walks the module mount list.
 */
static int nksu_mnt_ns_reset_pre(struct kprobe *p, struct pt_regs *regs)
{
	WRITE_ONCE(nksu_done_mnt_ns, NULL);
	return 0;
}

static struct kprobe nksu_kp_copy_mnt_ns = {
	.symbol_name = "copy_mnt_ns",
	.pre_handler = nksu_mnt_ns_reset_pre,
};

static struct kprobe nksu_kp_mntns_install = {
	.symbol_name = "mntns_install",
	.pre_handler = nksu_mnt_ns_reset_pre,
};

static struct kprobe nksu_kp_show_mountinfo = {
	.symbol_name = "show_mountinfo",
	.pre_handler = nksu_show_pre,
};

static struct kprobe nksu_kp_show_vfsmnt = {
	.symbol_name = "show_vfsmnt",
	.pre_handler = nksu_show_pre,
};

static struct kprobe nksu_kp_show_vfsstat = {
	.symbol_name = "show_vfsstat",
	.pre_handler = nksu_show_pre,
};

static struct kprobe *const nksu_show_probes[] = {
	&nksu_kp_show_mountinfo,
	&nksu_kp_show_vfsmnt,
	&nksu_kp_show_vfsstat,
};

static int nksu_resolve_probe_symbols(void)
{
	if (!nksu_register_kprobe) {
		nksu_register_kprobe =
			(typeof(register_kprobe) *)nksu_ksym_lookup("register_kprobe");
		nksu_unregister_kprobe =
			(typeof(unregister_kprobe) *)nksu_ksym_lookup("unregister_kprobe");
		nksu_ida_alloc_range =
			(typeof(ida_alloc_range) *)nksu_ksym_lookup("ida_alloc_range");
		nksu_mnt_id_ida = (struct ida *)nksu_ksym_lookup("mnt_id_ida");
	}

	if (!nksu_register_kprobe || !nksu_unregister_kprobe || !nksu_ida_alloc_range ||
	    !nksu_mnt_id_ida) {
		pr_err("[mount-id] unresolved kprobe/ida symbols\n");
		return -ENOENT;
	}
	return 0;
}

static int __nocfi nksu_register_one(struct kprobe *kp, const char *name, bool required)
{
	int ret;

	/* A probe that once failed registration keeps a stale .addr. */
	kp->addr = NULL;
	ret = nksu_register_kprobe(kp);
	if (ret) {
		if (required)
			pr_err("[mount-id] register_kprobe(%s) failed: %d\n", name, ret);
		else
			pr_warn("[mount-id] register_kprobe(%s) failed: %d\n", name, ret);
		return ret;
	}
	pr_info("[mount-id] hooked %s\n", name);
	return 0;
}

static int nksu_mount_probes_register_locked(void)
{
	int ret, i, show_ok = 0;

	if (nksu_mount_probes_ready)
		return 0;

	ret = nksu_resolve_probe_symbols();
	if (ret)
		return ret;

	if (!nksu_ida_probe_ready) {
		ret = nksu_register_one(&nksu_kp_ida_alloc, "ida_alloc_range", true);
		if (ret)
			return ret;
		nksu_ida_probe_ready = true;
	}

	if (!nksu_clone_probe_ready) {
		ret = nksu_register_one(&nksu_kp_clone_mnt, "clone_mnt", false);
		if (!ret)
			nksu_clone_probe_ready = true;
	}

	if (!nksu_copy_ns_probe_ready) {
		ret = nksu_register_one(&nksu_kp_copy_mnt_ns, "copy_mnt_ns", false);
		if (!ret)
			nksu_copy_ns_probe_ready = true;
	}
	if (!nksu_install_ns_probe_ready) {
		ret = nksu_register_one(&nksu_kp_mntns_install, "mntns_install", false);
		if (!ret)
			nksu_install_ns_probe_ready = true;
	}
	nksu_mnt_ns_hooks_ready = nksu_copy_ns_probe_ready && nksu_install_ns_probe_ready;
	WRITE_ONCE(nksu_done_mnt_ns, NULL);

	for (i = 0; i < ARRAY_SIZE(nksu_show_probes); i++) {
		if (nksu_show_probe_ready[i]) {
			show_ok++;
			continue;
		}
		ret = nksu_register_one(nksu_show_probes[i], nksu_show_probes[i]->symbol_name,
					false);
		if (!ret) {
			nksu_show_probe_ready[i] = true;
			show_ok++;
		}
	}

	if (!show_ok) {
		pr_err("[mount-id] no mountinfo reader could be hooked\n");
		return -ENOENT;
	}

	nksu_mount_probes_ready = true;
	return 0;
}

int nksu_mount_id_set_layout(u32 mnt_id_off, u32 mnt_off)
{
	int ret;

	if (mnt_id_off > 4096 - sizeof(int) || mnt_off > 4096 || (mnt_id_off & (sizeof(int) - 1)))
		return -EINVAL;

	mutex_lock(&nksu_mount_layout_lock);

	WRITE_ONCE(nksu_mnt_id_off, mnt_id_off);
	WRITE_ONCE(nksu_mnt_off, mnt_off);
	nksu_mount_layout_seen = true;

	/* The feature may be disabled from the manager; keep only the layout. */
	if (!READ_ONCE(nksu_umount_running)) {
		mutex_unlock(&nksu_mount_layout_lock);
		return 0;
	}

	ret = nksu_mount_probes_register_locked();
	if (ret) {
		mutex_unlock(&nksu_mount_layout_lock);
		return ret;
	}

	WRITE_ONCE(nksu_mount_layout_ready, true);
	mutex_unlock(&nksu_mount_layout_lock);
	pr_info("[mount-id] active (mnt_id@%u, mnt@%u)\n", mnt_id_off, mnt_off);
	return 0;
}

static void __nocfi nksu_mount_probes_exit(void)
{
	int i;

	WRITE_ONCE(nksu_mount_layout_ready, false);

	if (!nksu_unregister_kprobe)
		return;

	if (nksu_ida_probe_ready) {
		nksu_unregister_kprobe(&nksu_kp_ida_alloc);
		nksu_ida_probe_ready = false;
	}
	if (nksu_clone_probe_ready) {
		nksu_unregister_kprobe(&nksu_kp_clone_mnt);
		nksu_clone_probe_ready = false;
	}
	if (nksu_copy_ns_probe_ready) {
		nksu_unregister_kprobe(&nksu_kp_copy_mnt_ns);
		nksu_copy_ns_probe_ready = false;
	}
	if (nksu_install_ns_probe_ready) {
		nksu_unregister_kprobe(&nksu_kp_mntns_install);
		nksu_install_ns_probe_ready = false;
	}
	nksu_mnt_ns_hooks_ready = false;
	WRITE_ONCE(nksu_done_mnt_ns, NULL);
	for (i = 0; i < ARRAY_SIZE(nksu_show_probes); i++) {
		if (!nksu_show_probe_ready[i])
			continue;
		nksu_unregister_kprobe(nksu_show_probes[i]);
		nksu_show_probe_ready[i] = false;
	}
	nksu_mount_probes_ready = false;
}

#else /* !CONFIG_KPROBES */

/* Keep the layout bookkeeping (used by the umount start path) compiled in. */
static bool nksu_mount_layout_ready;
static bool nksu_mount_layout_seen;
static DEFINE_MUTEX(nksu_mount_layout_lock);

int nksu_mount_id_set_layout(u32 mnt_id_off, u32 mnt_off)
{
	pr_err("[mount-id] CONFIG_KPROBES is disabled\n");
	return -EOPNOTSUPP;
}

static int nksu_mount_probes_register_locked(void)
{
	return -EOPNOTSUPP;
}

static void __nocfi nksu_mount_probes_exit(void)
{
}

#endif /* CONFIG_KPROBES */

/*
 * ---------------------------------------------------------------------------
 * sys_enter observation
 * ---------------------------------------------------------------------------
 * This is a probe owned by the umount module, not an entry in the main syscall
 * hook.  It records module mount points and schedules the per-app umount.
 */
static bool nksu_umount_uid(uid_t uid)
{
	return (uid >= 10000 && uid < 20000) || (uid >= 90000 && uid < 100000);
}

static uid_t nksu_setresuid_target(const struct pt_regs *regs)
{
#if defined(__aarch64__)
	return (uid_t)regs->regs[1];
#elif defined(__x86_64__)
	return (uid_t)regs->si;
#else
	return 0;
#endif
}

/*
 * ncore owns every module mount: module scripts are forked from ncore and
 * inherit NKSU_TIF_NCORE.  Register the target before the real mount runs.
 * A failed mount leaves a stale path behind; nksu_umount_run() verifies the
 * root is actually a module path before touching it.
 */
static bool nksu_path_is_module(const char *path)
{
	return !strncmp(path, "/data/adb/modules/", 18) ||
	       !strncmp(path, "/data/adb/modules_update/", 25) ||
	       !strncmp(path, "/adb/modules/", 13) || !strncmp(path, "/adb/modules_update/", 20);
}

static bool nksu_read_abs_path(struct pt_regs *regs, unsigned int argno, char *buf, size_t len)
{
	int n = get_path_arg(regs, argno, buf, len);

	return n > 0 && n < (int)len - 1 && buf[0] == '/';
}

static void nksu_register_mount_target(struct pt_regs *regs, unsigned int source_arg,
				       unsigned int target_arg, const char *what, bool force)
{
	char source[256];
	char target[256];
	bool source_ok;

	source_ok = nksu_read_abs_path(regs, source_arg, source, sizeof(source));
	if (!nksu_read_abs_path(regs, target_arg, target, sizeof(target))) {
		if (force || (source_ok && nksu_path_is_module(source)))
			pr_warn("[mount] cannot read %s target\n", what);
		else
			pr_debug("[mount] cannot read %s target\n", what);
		return;
	}

	/*
	 * Keep the list limited to mounts that can possibly be module mounts.
	 * Target-only checks cover mounts into the module store; the source
	 * check covers the usual bind mount from /data/adb/modules to /system.
	 */
	if (!force && !nksu_path_is_module(target) && !(source_ok && nksu_path_is_module(source)))
		return;

	if (nksu_umount_add(target, MNT_DETACH))
		pr_warn("[mount] cannot register %s: %s\n", what, target);
	else
		pr_debug("[mount] registered %s: %s\n", what, target);

	/*
	 * A mount made after the app already switched to its final UID (for
	 * example a zygisk daemon mounting in the app namespace) will not get a
	 * later setresuid trigger.  Queue the removal from this syscall's
	 * task_work instead.
	 */
	if (nksu_umount_uid(__kuid_val(current_euid())))
		nksu_umount_schedule();
}

static void nksu_umount_sys_enter(void *data, struct pt_regs *regs, long id)
{
	if (id == __NR_setresuid && nksu_umount_uid(nksu_setresuid_target(regs)))
		nksu_umount_schedule();

	/*
	 * Record mount targets, but only when the source or target can be a
	 * module path.  nksu_umount_run() still verifies the mount root before
	 * removing anything.
	 */
	switch (id) {
	case __NR_mount:
		nksu_register_mount_target(regs, 0, 1, "mount", false);
		break;
#ifdef __NR_move_mount
	case __NR_move_mount:
		/* Source may be an O_PATH fd, so the path can be empty. */
		nksu_register_mount_target(regs, 1, 3, "move_mount", true);
		break;
#endif
	}
}

static struct tracepoint *nksu_umount_tp_sys_enter;
static bool nksu_umount_hooked;
static DEFINE_MUTEX(nksu_umount_init_lock);

struct nksu_umount_tp_ctx {
	const char *name;
	struct tracepoint **out;
};

static void nksu_umount_tp_cb(struct tracepoint *tp, void *priv)
{
	struct nksu_umount_tp_ctx *ctx = priv;

	if (*ctx->out)
		return;
	if (!strcmp(tp->name, ctx->name))
		*ctx->out = tp;
}

static struct tracepoint *nksu_umount_find_tracepoint(const char *name)
{
	struct tracepoint *result = NULL;
	struct nksu_umount_tp_ctx ctx = { .name = name, .out = &result };

	for_each_kernel_tracepoint(nksu_umount_tp_cb, &ctx);
	return result;
}

static int nksu_umount_start(void)
{
	int ret;

	mutex_lock(&nksu_umount_init_lock);
	if (nksu_umount_hooked) {
		mutex_unlock(&nksu_umount_init_lock);
		return 0;
	}

	nksu_umount_tp_sys_enter = nksu_umount_find_tracepoint("sys_enter");
	if (!nksu_umount_tp_sys_enter) {
		pr_err("[umount] cannot find sys_enter tracepoint\n");
		mutex_unlock(&nksu_umount_init_lock);
		return -ENOENT;
	}

	ret = tracepoint_probe_register(nksu_umount_tp_sys_enter, nksu_umount_sys_enter, NULL);
	if (ret) {
		pr_err("[umount] register sys_enter probe failed: %d\n", ret);
		mutex_unlock(&nksu_umount_init_lock);
		return ret;
	}

	nksu_umount_hooked = true;
	mutex_unlock(&nksu_umount_init_lock);

	/*
	 * Mount-id probes are registered when the BTF layout arrives, i.e.
	 * immediately before ncore's first mount, or here if the feature is
	 * re-enabled after a boot-time disable.  Keeping them out of the
	 * pre-layout window avoids a kprobe on every ida_alloc_range() call.
	 */
	if (nksu_mount_layout_seen) {
		mutex_lock(&nksu_mount_layout_lock);
		ret = nksu_mount_probes_register_locked();
		if (!ret)
			WRITE_ONCE(nksu_mount_layout_ready, true);
		mutex_unlock(&nksu_mount_layout_lock);
		if (ret)
			pr_warn("[mount-id] probes not ready: %d\n", ret);
	}

	pr_info("[umount] sys_enter probe active\n");
	return 0;
}

static void nksu_umount_stop(void)
{
	nksu_mount_probes_exit();

	if (nksu_umount_hooked) {
		tracepoint_probe_unregister(nksu_umount_tp_sys_enter, nksu_umount_sys_enter, NULL);
		tracepoint_synchronize_unregister();
		nksu_umount_hooked = false;
	}
}

#define NKSU_UMOUNT_FEATURE_NAME "umount"
#define NKSU_UMOUNT_DISABLED_NAME "umount_disabled"

static int nksu_umount_feature_get(u64 *value)
{
	*value = READ_ONCE(nksu_umount_running) ? 1 : 0;
	return 0;
}

static int nksu_umount_apply(u64 value, bool persist)
{
	int ret = 0;

	mutex_lock(&nksu_umount_state_lock);
	if (value) {
		if (!nksu_umount_running) {
			ret = nksu_umount_start();
			if (!ret)
				nksu_umount_running = true;
		}
	} else if (nksu_umount_running) {
		nksu_umount_stop();
		nksu_umount_running = false;
	}
	mutex_unlock(&nksu_umount_state_lock);

	if (!ret && persist && nksu_feature_persist_set(NKSU_UMOUNT_DISABLED_NAME, !value))
		pr_warn("[umount] cannot persist feature state\n");

	return ret;
}

static int nksu_umount_feature_set(u64 value)
{
	return nksu_umount_apply(value, true);
}

static const struct nksu_feature nksu_umount_feature = {
	.id = NKSU_FEATURE_UMOUNT,
	.name = NKSU_UMOUNT_FEATURE_NAME,
	.get = nksu_umount_feature_get,
	.set = nksu_umount_feature_set,
};

int nksu_umount_init(void)
{
	int ret;

	ret = nksu_feature_register(&nksu_umount_feature);
	if (ret && ret != -EEXIST) {
		pr_err("[umount] feature register failed: %d\n", ret);
		return ret;
	}

	mutex_lock(&nksu_umount_state_lock);
	ret = nksu_umount_start();
	if (!ret)
		nksu_umount_running = true;
	mutex_unlock(&nksu_umount_state_lock);

	return ret;
}

/*
 * Runs in the feature group, after /data and the SELinux domain are available.
 * The feature itself must already be running (it has to cover ncore's mount
 * scripts), so this only applies a persisted "disabled" marker.
 */
int nksu_umount_feature_init(void)
{
	if (!nksu_feature_persist_has(NKSU_UMOUNT_DISABLED_NAME))
		return 0;

	pr_info("[umount] disabled by persisted feature state\n");
	return nksu_umount_apply(0, false);
}

void nksu_umount_exit(void)
{
	mutex_lock(&nksu_umount_state_lock);
	nksu_umount_stop();
	nksu_umount_running = false;
	nksu_umount_wipe();
	mutex_unlock(&nksu_umount_state_lock);
}
