// SPDX-License-Identifier: GPL-3.0-or-later
#include <linux/binfmts.h>
#include <linux/compiler.h>
#include <linux/cred.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/mman.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/pid.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/shmem_fs.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/task_work.h>
#include <linux/trace_events.h>
#include <linux/tracepoint.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <asm/barrier.h>
#include <asm/ptrace.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 20, 0)
#include <linux/xarray.h>
#else
#include <linux/radix-tree.h>
#endif

#include "hook/hook.h"
#include "manager/manager.h"
#include "fd/fd.h"
#include "privilege/privilege.h"
#include "selinux/selinux.h"
#include "manager/ioctl.h"
#include <fmac.h>
#include "klog.h"
#include "symbol/symbol.h"

/* Last: redirects task_work_add and other unexported symbols. */
#include "symbol/symbol_compat.h"

/*
 * task_work_add and kernel_write are reached through kallsyms-resolved
 * pointers.  The module's CFI type hash for those indirect calls need not
 * match the running kernel's, so opt this file out like daemon.c does.
 */
#if defined(__clang__)
#pragma clang attribute push(__attribute__((no_sanitize("cfi"))), apply_to = function)
#endif

long handle_prctl_hooks(struct pt_regs *regs)
{
#if defined(__aarch64__)
	unsigned long option = regs->regs[0];
#elif defined(__x86_64__)
	unsigned long option = regs->di;
#endif

	if (likely(!is_manager())) {
		return 0;
	}

	switch (option) {
	case NKSU_PRCTL_GET_SHM_FD:
		if (fmac_anonfd_get() < 0)
			pr_err_ratelimited("[manager] cannot attach shared-memory fd\n");
		return 1;

	case NKSU_PRCTL_GET_CTL_FD:
		if (fmac_ctlfd_get() < 0)
			pr_err_ratelimited("[manager] cannot attach control fd\n");
		return 1;

	default:
		return 0;
	}
}

/*
 * The sys_enter tracepoint runs in atomic context.  It must not write into the
 * caller's address space (the stack may contain live data and copy_to_user()
 * may fault), and it must not allocate or sleep.  The redirect strings are
 * therefore staged once in a module-created shmem page and mapped read-only
 * into each process that can reach the hook.  sys_enter only locates that
 * mapping.
 *
 * A private shmem inode is used instead of a module-owned VMA so existing user
 * mappings keep the inode alive without pinning the module across unload.  The
 * inode is created by shmem_kernel_file_setup(), which marks it S_PRIVATE so
 * SELinux does not perform inode-level FILE__MAP checks against the target
 * process's domain.
 *
 * The mapping is created from sched_process_exec/sched_process_fork task_work
 * and from profile updates, where sleeping and allocating are allowed.  Fork
 * and exec establish it before the task first runs in userspace; a profile
 * update maps already-running tasks on their next return to userspace.  No
 * vvar/vdso layout is assumed.
 */
#define NKSU_PATH_EXEC_OFF 0UL
#define NKSU_PATH_SH_OFF 64UL

static struct file *nksu_path_file;
static bool nksu_path_ready;
static typeof(shmem_kernel_file_setup) *nksu_shmem_kernel_file_setup;

static struct tracepoint *nksu_find_tracepoint(const char *name);

struct nksu_tp_find_ctx {
	const char *name;
	struct tracepoint **out;
};

static void nksu_tp_find_cb(struct tracepoint *tp, void *priv)
{
	struct nksu_tp_find_ctx *ctx = priv;

	if (*ctx->out)
		return;
	if (strcmp(tp->name, ctx->name) == 0)
		*ctx->out = tp;
}

static struct tracepoint *nksu_find_tracepoint(const char *name)
{
	struct tracepoint *result = NULL;
	struct nksu_tp_find_ctx ctx = { .name = name, .out = &result };

	for_each_kernel_tracepoint(nksu_tp_find_cb, &ctx);
	return result;
}

/*
 * The task_work stage already knows the address returned by vm_mmap().  Keep
 * it keyed by the thread-group leader so the syscall hook can resume directly
 * without walking the VMA list or taking mmap_lock.  Entries disappear with
 * the task group, so the cache does not pin the mm or outlive the process.
 *
 * 4.9 uses this cache too (see below) only on kernels with xarray; older trees
 * fall back to scanning the VMA list when the path actually matches su, which
 * is rare.  A custom cache was intentionally avoided there: its rcu_head /
 * kfree_rcu lifetime interacted badly with 4.9's NOCB callback queueing.
 */
struct nksu_path_cache {
	struct rcu_head rcu;
	unsigned long addr;
};

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 20, 0)
static DEFINE_XARRAY(nksu_path_addrs);
#endif

static void nksu_path_cache_store(unsigned long addr)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 20, 0)
	struct nksu_path_cache *cache;
	struct nksu_path_cache *old;

	if (!addr)
		return;
	cache = kzalloc(sizeof(*cache), GFP_KERNEL);
	if (!cache)
		return;
	cache->addr = addr;
	old = xa_store(&nksu_path_addrs, (unsigned long)current->group_leader, cache, GFP_KERNEL);
	if (xa_err(old)) {
		kfree(cache);
		return;
	}
	if (old)
		kfree_rcu(old, rcu);
#else
	(void)addr;
#endif
}

static unsigned long nksu_path_cached_addr(void)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 20, 0)
	struct nksu_path_cache *cache;
	unsigned long addr = 0;

	rcu_read_lock();
	cache = xa_load(&nksu_path_addrs, (unsigned long)current->group_leader);
	if (cache)
		addr = cache->addr;
	rcu_read_unlock();
	return addr;
#else
	return 0;
#endif
}

static void nksu_path_task_exit(void *data, struct task_struct *task)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 20, 0)
	struct nksu_path_cache *cache;

	if (!task->signal || atomic_read(&task->signal->live) > 1)
		return;
	cache = xa_erase(&nksu_path_addrs, (unsigned long)task->group_leader);
	if (cache)
		kfree_rcu(cache, rcu);
#else
	(void)data;
	(void)task;
#endif
}

struct nksu_path_work {
	struct callback_head cb;
};

/*
 * The user mapping is found by its shmem file pointer, not by a magic value or
 * a fixed address.  A process can move or unmap it, so search the current VMA
 * list; this only happens after the path has matched su.
 * The tracepoint path may only trylock; task_work may block.
 */
static bool nksu_path_vma_match(struct vm_area_struct *vma, struct file *file)
{
	if (vma->vm_file != file)
		return false;
	if (vma->vm_end - vma->vm_start != PAGE_SIZE || vma->vm_pgoff)
		return false;
	if (!(vma->vm_flags & VM_READ) || (vma->vm_flags & (VM_WRITE | VM_MAYWRITE)))
		return false;
	return true;
}

/* 6.1 replaced the VMA linked list with the maple tree. */
static unsigned long nksu_path_scan(struct mm_struct *mm, struct file *file)
{
	struct vm_area_struct *vma;
	unsigned long addr = 0;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
	{
		VMA_ITERATOR(vmi, mm, 0);

		for_each_vma(vmi, vma)
		{
			if (nksu_path_vma_match(vma, file)) {
				addr = vma->vm_start;
				break;
			}
		}
	}
#else
	for (vma = mm->mmap; vma; vma = vma->vm_next) {
		if (nksu_path_vma_match(vma, file)) {
			addr = vma->vm_start;
			break;
		}
	}
#endif

	return addr;
}

static unsigned long nksu_path_user_addr(struct mm_struct *mm, bool trylock)
{
	struct file *file;
	unsigned long addr;

	if (unlikely(!mm || !smp_load_acquire(&nksu_path_ready)))
		return 0;
	file = READ_ONCE(nksu_path_file);
	if (unlikely(!file))
		return 0;

	if (trylock) {
		if (!mmap_read_trylock(mm))
			return 0;
	} else {
		mmap_read_lock(mm);
	}

	addr = nksu_path_scan(mm, file);
	mmap_read_unlock(mm);
	return addr;
}

static int nksu_path_map_current(void)
{
	const struct cred *old;
	struct file *file;
	unsigned long addr;
	int ret = 0;

	if (unlikely(!smp_load_acquire(&nksu_path_ready)))
		return -ESHUTDOWN;
	if (unlikely(!current->mm || (current->flags & PF_EXITING)))
		return -ESRCH;
	addr = nksu_path_user_addr(current->mm, false);
	if (addr) {
		nksu_path_cache_store(addr);
		return 0;
	}

	file = READ_ONCE(nksu_path_file);
	if (unlikely(!file))
		return -ESHUTDOWN;

	/*
	 * MAP_SHARED plus a file without FMODE_WRITE makes mmap_region clear
	 * both VM_SHARED and VM_MAYWRITE.  The mapping stays read-only and
	 * cannot be turned writable with mprotect(); MAP_PRIVATE would still
	 * allow a private COW copy and let the process replace the path.
	 *
	 * The inode is S_PRIVATE, so SELinux skips the inode permission checks,
	 * but file_has_perm() still rejects an FD__USE transition from the target
	 * process's SID to the SID that created the file.  Map under the file's
	 * own cred for the duration of vm_mmap() and restore the target cred
	 * before returning to userspace.  This does not depend on resolving the
	 * nksu domain, so it is safe during early task_work.
	 */
	if (unlikely(!file->f_cred))
		return -EINVAL;
	old = override_creds(file->f_cred);
	addr = vm_mmap(file, 0, PAGE_SIZE, PROT_READ, MAP_SHARED, 0);
	revert_creds(old);
	if (IS_ERR_VALUE(addr))
		ret = (int)addr;
	else
		nksu_path_cache_store(addr);

	return ret;
}

static void nksu_path_work_fn(struct callback_head *cb)
{
	struct nksu_path_work *work = container_of(cb, struct nksu_path_work, cb);
	int ret;

	ret = nksu_path_map_current();
	if (ret && ret != -ESRCH)
		pr_err_ratelimited("[hook]: cannot map su redirect page for pid %d: %d\n",
				   current->pid, ret);

	kfree(work);
	module_put(THIS_MODULE);
}

static void nksu_path_queue(struct task_struct *task)
{
	struct nksu_path_work *work;
	int ret;

	if (!smp_load_acquire(&nksu_path_ready) || (task->flags & PF_KTHREAD) || !task->mm)
		return;

	if (!try_module_get(THIS_MODULE)) {
		pr_warn_ratelimited("[hook]: cannot pin module for path task_work\n");
		return;
	}

	work = kzalloc(sizeof(*work), GFP_ATOMIC);
	if (!work) {
		module_put(THIS_MODULE);
		pr_warn_ratelimited("[hook]: cannot allocate path task_work\n");
		return;
	}

	init_task_work(&work->cb, nksu_path_work_fn);
	ret = task_work_add(task, &work->cb, TWA_RESUME);
	if (ret) {
		pr_warn_ratelimited("[hook]: cannot queue path task_work for pid %d: %d\n",
				    task->pid, ret);
		kfree(work);
		module_put(THIS_MODULE);
	}
}

void nksu_path_install_task(struct task_struct *task)
{
	if (task)
		nksu_path_queue(task);
}

void nksu_path_sync_all(void)
{
	struct task_struct *task;

	rcu_read_lock();
	for_each_process(task) {
		if (task->flags & PF_KTHREAD)
			continue;
		if (nksu_profile_has_uid(__kuid_val(task_uid(task))) ||
		    test_tsk_thread_flag(task, TIF_SYSCALL_TRACEPOINT))
			nksu_path_queue(task);
	}
	rcu_read_unlock();
}

static void nksu_path_exec(void *data, struct task_struct *task, pid_t old_pid,
			   struct linux_binprm *bprm)
{
	if (nksu_profile_has_uid(__kuid_val(task_uid(task))) ||
	    test_tsk_thread_flag(task, TIF_SYSCALL_TRACEPOINT))
		nksu_path_queue(task);
}

static void nksu_path_fork(void *data, struct task_struct *parent, struct task_struct *child)
{
	if (nksu_profile_has_uid(__kuid_val(task_uid(child))) ||
	    test_tsk_thread_flag(parent, TIF_SYSCALL_TRACEPOINT) ||
	    test_tsk_thread_flag(child, TIF_SYSCALL_TRACEPOINT))
		nksu_path_queue(child);
}

static struct tracepoint *tp_path_exec;
static struct tracepoint *tp_path_fork;
static struct tracepoint *tp_path_exit;

int nksu_path_init(void)
{
	char *page;
	struct file *file;
	loff_t pos = 0;
	ssize_t written;
	int ret;

	if (REDIRECT_TARGET_LEN > NKSU_PATH_SH_OFF || NKSU_PATH_SH_OFF + SH_PATH_LEN > PAGE_SIZE) {
		pr_err("[hook]: redirect paths do not fit the staging page\n");
		return -EINVAL;
	}

	nksu_shmem_kernel_file_setup =
		(typeof(nksu_shmem_kernel_file_setup))nksu_ksym_lookup("shmem_kernel_file_setup");
	if (unlikely(!nksu_shmem_kernel_file_setup)) {
		pr_err("[hook]: unresolved shmem_kernel_file_setup\n");
		return -ENOENT;
	}

	page = kzalloc(PAGE_SIZE, GFP_KERNEL);
	if (!page)
		return -ENOMEM;
	memcpy(page + NKSU_PATH_EXEC_OFF, REDIRECT_TARGET, REDIRECT_TARGET_LEN);
	memcpy(page + NKSU_PATH_SH_OFF, SH_PATH, SH_PATH_LEN);

	file = nksu_shmem_kernel_file_setup("[nksu_path]", PAGE_SIZE, 0);
	if (IS_ERR(file)) {
		ret = PTR_ERR(file);
		kfree(page);
		return ret;
	}

	written = nksu_kwrite(file, page, PAGE_SIZE, &pos);
	kfree(page);
	if (written != (ssize_t)PAGE_SIZE) {
		pr_err("[hook]: cannot stage redirect page: %zd\n", written);
		fput(file);
		return written < 0 ? (int)written : -EIO;
	}

	/*
	 * Drop write access before the first mapping.  mmap_region consults
	 * f_mode when it builds the VMA flags, so this is what keeps the
	 * page shared, read-only and immune to mprotect(PROT_WRITE).
	 */
	file->f_mode &= ~FMODE_WRITE;
	WRITE_ONCE(nksu_path_file, file);

	tp_path_exec = nksu_find_tracepoint("sched_process_exec");
	tp_path_fork = nksu_find_tracepoint("sched_process_fork");
	tp_path_exit = nksu_find_tracepoint("sched_process_exit");
	if (!tp_path_exec || !tp_path_fork || !tp_path_exit) {
		pr_err("[hook]: scheduler tracepoints not found\n");
		ret = -ENOENT;
		goto err_page;
	}

	ret = tracepoint_probe_register(tp_path_exec, nksu_path_exec, NULL);
	if (ret) {
		pr_err("[hook]: register path exec probe failed: %d\n", ret);
		goto err_page;
	}

	ret = tracepoint_probe_register(tp_path_fork, nksu_path_fork, NULL);
	if (ret) {
		pr_err("[hook]: register path fork probe failed: %d\n", ret);
		tracepoint_probe_unregister(tp_path_exec, nksu_path_exec, NULL);
		goto err_page;
	}

	ret = tracepoint_probe_register(tp_path_exit, nksu_path_task_exit, NULL);
	if (ret) {
		pr_err("[hook]: register path exit probe failed: %d\n", ret);
		tracepoint_probe_unregister(tp_path_fork, nksu_path_fork, NULL);
		tracepoint_probe_unregister(tp_path_exec, nksu_path_exec, NULL);
		goto err_page;
	}

	smp_store_release(&nksu_path_ready, true);

	pr_info("[hook]: staged su redirect paths in a private shmem page\n");
	return 0;

err_page:
	smp_store_release(&nksu_path_ready, false);
	tracepoint_synchronize_unregister();
	tp_path_exec = NULL;
	tp_path_fork = NULL;
	tp_path_exit = NULL;
	WRITE_ONCE(nksu_path_file, NULL);
	fput(file);
	return ret;
}

void nksu_path_exit(void)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 20, 0)
	struct nksu_path_cache *cache;
#endif
	struct file *file;
	smp_store_release(&nksu_path_ready, false);

	if (tp_path_exec)
		tracepoint_probe_unregister(tp_path_exec, nksu_path_exec, NULL);
	if (tp_path_fork)
		tracepoint_probe_unregister(tp_path_fork, nksu_path_fork, NULL);
	if (tp_path_exit)
		tracepoint_probe_unregister(tp_path_exit, nksu_path_task_exit, NULL);
	tp_path_exec = NULL;
	tp_path_fork = NULL;
	tp_path_exit = NULL;
	tracepoint_synchronize_unregister();
	synchronize_rcu();

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 20, 0)
	{
		unsigned long index = 0;

		while ((cache = xa_find(&nksu_path_addrs, &index, ~0UL, XA_PRESENT))) {
			xa_erase(&nksu_path_addrs, index);
			kfree(cache);
			index++;
		}
		xa_destroy(&nksu_path_addrs);
	}
#endif

	file = READ_ONCE(nksu_path_file);
	WRITE_ONCE(nksu_path_file, NULL);
	if (file)
		fput(file);
}

static unsigned long try_redirect_path(struct pt_regs *regs, unsigned int arg_index,
				       size_t target_off, bool require_target, bool *map_missing)
{
	char buf[SU_PATH_LEN];
	const char __user *upath;
	unsigned long base;
	ssize_t ulen;

	*map_missing = false;
	if (unlikely(!current->mm))
		return 0;

#if defined(__aarch64__)
	if (unlikely(arg_index > 5))
		return 0;
#else
	if (unlikely(arg_index > 7))
		return 0;
#endif

	if (unlikely(target_off >= PAGE_SIZE))
		return 0;

	upath = (const char __user *)regs->regs[arg_index];
	if (unlikely(!upath))
		return 0;

	/*
     * Only an exact match matters, so copy at most SU_PATH_LEN bytes: the
     * overwhelmingly common case is a completely different path (every
     * stat/access/exec a managed UID performs), and it must not pay for a
     * full-sized copy.  A longer path is truncated to SU_PATH_LEN and fails
     * the length check below; -EFAULT also fails it.
     * strncpy_from_user() returns the length without the trailing NUL, while
     * SU_PATH_LEN is sizeof(SU_PATH) (NUL included).
     */
	ulen = strncpy_from_user(buf, upath, sizeof(buf));
	if (unlikely(ulen != SU_PATH_LEN - 1) || !path_is_su(buf))
		return 0;

	/*
     * KernelSU gates the su→daemon redirect on the daemon existing; without a target
     * there is nothing to exec, so run the real su instead of escalating into
     * a failed exec.  This must not sleep (the tracepoint path runs with
     * preemption disabled), so use the flag the manager scan publishes.
     */
	if (require_target && !is_daemon_ready())
		return 0;

	if (unlikely(!smp_load_acquire(&nksu_path_ready))) {
		*map_missing = true;
		pr_err_ratelimited("[hook]: su redirect paths are not staged\n");
		return 0;
	}

	base = nksu_path_cached_addr();
	if (unlikely(!base))
		base = nksu_path_user_addr(current->mm, true);
	if (unlikely(!base)) {
		*map_missing = true;
		pr_warn_ratelimited("[hook]: pid %d has no su redirect mapping\n", current->pid);
		return 0;
	}

	return base + target_off;
}

/*
 * The hook runs from a tracepoint, so there is no way to return an errno from
 * the handler itself.  Point the syscall at an unmapped address instead: the
 * real syscall then fails with -EFAULT.  This is deliberate -- if an exec of
 * su matched but its redirect page is missing, running the original su would
 * silently bypass NekoSU's policy and hide the internal failure.
 */
static void fail_redirect(struct pt_regs *regs, unsigned int arg_index)
{
	pr_err_ratelimited("[hook]: refusing su exec without redirect mapping (pid %d)\n",
			   current->pid);
	regs->regs[arg_index] = ~0UL;
}

long hook_path_at(struct pt_regs *regs)
{
	bool map_missing;
	unsigned long new_uaddr = try_redirect_path(regs, 1, NKSU_PATH_SH_OFF, false, &map_missing);

	if (new_uaddr > 0) {
		regs->regs[1] = new_uaddr;
	}
	return 0;
}

long hook__NR_execve(struct pt_regs *regs)
{
	bool map_missing;
	unsigned long new_uaddr =
		try_redirect_path(regs, 0, NKSU_PATH_EXEC_OFF, true, &map_missing);

	if (unlikely(map_missing)) {
		fail_redirect(regs, 0);
		return 0;
	}
	if (new_uaddr > 0) {
		regs->regs[0] = new_uaddr;
		privilege_escalate_from_profile(NULL);
	}
	return 0;
}

long hook__NR_execveat(struct pt_regs *regs)
{
	bool map_missing;
	unsigned long new_uaddr =
		try_redirect_path(regs, 1, NKSU_PATH_EXEC_OFF, true, &map_missing);

	if (unlikely(map_missing)) {
		fail_redirect(regs, 1);
		return 0;
	}
	if (new_uaddr > 0) {
		regs->regs[1] = new_uaddr;
		privilege_escalate_from_profile(NULL);
	}
	return 0;
}

/*
 * Root from native code, without spawning `su`.
 *
 * An authorized app may ask the kernel for root directly from native
 * code.  Only setresuid() is usable for that: Android's app seccomp policy
 * allows setresuid(2) but kills the process with SIGSYS on setuid(2) /
 * setreuid(2), and all callers go through setresuid (see the JNI readFile
 * bridge in neko/jni).
 *
 * When the call targets root, escalate from the profile just before the real
 * syscall runs; the caller is root by then, so the original call succeeds
 * instead of failing with EPERM.
 *
 * Unauthorized processes never reach here: both dispatchers gate on a
 * managed UID (see probe_sys_enter and nksu_dispatch_fast), and
 * privilege_escalate_from_profile() re-checks the profile before touching
 * credentials.  This mirrors exactly what the su path already grants.
 */
long hook__NR_setresuid(struct pt_regs *regs)
{
	uid_t root_uid;

	/* setresuid(ruid, euid, suid): only euid == 0 actually grants privilege. */
	if (regs->regs[1] != 0)
		return 0;

	if (privilege_escalate_from_profile(&root_uid) == 0) {
		/*
         * The profile decides the resulting uid.  Point the real syscall at
         * that uid as well; otherwise a profile whose root uid is non-zero
         * would be undone the moment the original setresuid(..., 0, ...) runs
         * (the raised capabilities would let it succeed).
         */
		regs->regs[0] = root_uid;
		regs->regs[1] = root_uid;
		regs->regs[2] = root_uid;
	}
	return 0;
}

/*
 * Single source of truth for the syscalls we intercept.  The tracepoint
 * dispatcher (probe_sys_enter) expands it into a switch; the syscall-table
 * build (init_syscall_hook) expands it into the table below.
 */
#define NKSU_SYSCALL_HANDLERS(X)              \
	X(__NR_prctl, handle_prctl_hooks)     \
	X(__NR_execve, hook__NR_execve)       \
	X(__NR_execveat, hook__NR_execveat)   \
	X(__NR_setresuid, hook__NR_setresuid) \
	X(__NR_faccessat, hook_path_at)       \
	X(__NR_newfstatat, hook_path_at)

#define NKSU_HOOK_TABLE(nr, fn) { nr, fn },

static const struct syscall_hook syscall_hooks[] = { NKSU_SYSCALL_HANDLERS(NKSU_HOOK_TABLE) };

#ifdef CONFIG_NKSU_SYSCALL

int init_syscall_hook(void)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 17, 0)
	/*
	 * 4.9's sys_call_table holds native sys_* functions, not the pt_regs
	 * dispatcher this path installs.  Refuse instead of corrupting every
	 * syscall; the tracepoint dispatcher (CONFIG_NKSU_SYSCALL=n) works.
	 */
	pr_err("[hook]: syscall-table hooking needs Linux >= 4.17; use the tracepoint hook\n");
	return -EOPNOTSUPP;
#else
	int ret, i;

	/* Steady state is profile-gated; the temporary watcher may have left
	 * this on. */
	nksu_dispatch_set_unconditional(false);

	for (i = 0; i < ARRAY_SIZE(syscall_hooks); i++) {
		ret = nksu_redirect_syscall(syscall_hooks[i].nr);
		if (ret) {
			pr_err("[hook]: can't redirect syscall %d: %d\n", syscall_hooks[i].nr, ret);
			goto err;
		}
	}

	for (i = 0; i < ARRAY_SIZE(syscall_hooks); i++) {
		ret = nksu_register_handler(syscall_hooks[i].nr, syscall_hooks[i].handler);
		if (ret) {
			pr_err("[hook]: can't register handler for syscall %d: %d\n",
			       syscall_hooks[i].nr, ret);
			goto err;
		}
	}

	pr_info("[hook]: loaded syscall hook\n");
	return 0;

err:
	/* Undo a partial install. */
	for (i = 0; i < ARRAY_SIZE(syscall_hooks); i++)
		nksu_unhook_syscall(syscall_hooks[i].nr);
	return ret;
#endif
}

void exit_syscall_hook(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(syscall_hooks); i++)
		nksu_unhook_syscall(syscall_hooks[i].nr);

	pr_info("[hook]: unloaded syscall hook\n");
}
#endif /* CONFIG_NKSU_SYSCALL */

#ifndef CONFIG_NKSU_SYSCALL

static struct tracepoint *tp_sys_enter;
static struct tracepoint *tp_sched_fork;

/* sys_enter callback: filter by managed UID, then dispatch.  The cases come
 * from NKSU_SYSCALL_HANDLERS, so the compiler still emits a decision tree /
 * jump table with no loop or array load on this per-syscall path. */
static void probe_sys_enter(void *data, struct pt_regs *regs, long id)
{
#define NKSU_HOOK_CASE(nr, fn) \
	case nr:               \
		fn(regs);      \
		return;

	if (!nksu_profile_has_uid(__kuid_val(task_uid(current))))
		return;

	switch (id) {
		NKSU_SYSCALL_HANDLERS(NKSU_HOOK_CASE)
	}

#undef NKSU_HOOK_CASE
}

/*
 * Android may start an app from a pre-forked USAP worker, and the tracepoint
 * flag can be cleared by another tracing user.  Do not rely solely on the
 * parent's current flag: zygote/USAP children must stay traceable even if the
 * pool worker lost it before specializing into the manager UID.
 */
static bool nksu_app_process(const struct task_struct *task)
{
	const char *comm = task->comm;

	return !strncmp(comm, "zygote", 6) || !strncmp(comm, "usap", 4) ||
	       strstr(comm, "app_process");
}

/* fork callback: propagate tracing to managed children and to every child of
 * zygote/USAP.  This is O(1) instead of scanning every thread on every fork. */
static void probe_sched_fork(void *data, struct task_struct *parent, struct task_struct *child)
{
	if (!test_tsk_thread_flag(parent, TIF_SYSCALL_TRACEPOINT) && !nksu_app_process(parent) &&
	    !nksu_profile_has_uid(__kuid_val(task_uid(child))))
		return;
	set_tsk_thread_flag(child, TIF_SYSCALL_TRACEPOINT);
}

int load_tracepoint_hook(void)
{
	int ret;

	tp_sys_enter = nksu_find_tracepoint("sys_enter");
	if (!tp_sys_enter) {
		pr_err("cannot find sys_enter tracepoint\n");
		return -ENOENT;
	}

	tp_sched_fork = nksu_find_tracepoint("sched_process_fork");
	if (!tp_sched_fork) {
		pr_err("cannot find sched_process_fork tracepoint\n");
		return -ENOENT;
	}

	ret = tracepoint_probe_register(tp_sys_enter, probe_sys_enter, NULL);
	if (ret) {
		pr_err("register sys_enter probe failed: %d\n", ret);
		return ret;
	}

	ret = tracepoint_probe_register(tp_sched_fork, probe_sched_fork, NULL);
	if (ret) {
		pr_err("register sched_process_fork probe failed: %d\n", ret);
		tracepoint_probe_unregister(tp_sys_enter, probe_sys_enter, NULL);
		return ret;
	}

	pr_info("tracepoint hooks loaded\n");
	return 0;
}

void unload_tracepoint_hook(void)
{
	if (tp_sys_enter)
		tracepoint_probe_unregister(tp_sys_enter, probe_sys_enter, NULL);
	if (tp_sched_fork)
		tracepoint_probe_unregister(tp_sched_fork, probe_sched_fork, NULL);

	tracepoint_synchronize_unregister();

	pr_info("tracepoint hooks unloaded\n");
}

#endif /* !CONFIG_NKSU_SYSCALL */

#if defined(__clang__)
#pragma clang attribute pop
#endif
