// SPDX-License-Identifier: GPL-2.0
/*
 * SELinux hiding, ported from KernelSU's feature/selinux_hide.c.
 *
 * su checkers read /sys/fs/selinux/{context,access} and /sys/fs/selinux/status;
 * those would expose the domain and the rules NekoSU injects.  When enabled,
 * requests from app uids (uid >= 10000) are answered from the untouched policy
 * kept aside by sepolicy_dup_and_apply(), and /sys/fs/selinux/status serves a
 * snapshot taken before the policy was modified, so the injected state is
 * invisible to userspace.
 *
 * It is a normal kernel feature: flip it with IOC_FEATURE_SET through the
 * manager's JNI interface; the kernel tracks the enabled set in
 * /data/adb/nksu/feature and turns it back on at boot.
 *
 * Linux 6.6 dropped the struct selinux_state argument from the policy helpers;
 * the older kernels answer through a throwaway state pointing at the untouched
 * policy, and 6.6+ through the vendored policy engine at the bottom of the
 * file (see the #if).
 */
#include "klog.h"
#include <linux/cred.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/jump_label.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/printk.h>
#include <linux/ptrace.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/vmalloc.h>

#include <security.h>
#include "ss/policydb.h"
#include "ss/context.h"
#include "ss/conditional.h"
#include "ss/mls.h"
#include "ss/services.h"
#include "avc.h"
#include "objsec.h"

#include "hook/lsm_hook.h"
#include "hook/patch.h"
#include "manager/feature.h"
#include "selinux/engine.h"
#include "selinux/hide.h"
#include "selinux/policy.h"
#include "selinux/selinux.h"
#include "symbol/symbol.h"

/*
 * symbol_compat.h redirects `selinux_state` to a resolved pointer, and the
 * macro would also rewrite the `struct selinux_state` tag; capture the real
 * type before it is in scope.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
typedef struct selinux_state nksu_fake_state_t;
#endif

/*
 * On 4.9/5.x-ss the policy tables live in struct selinux_ss (reached through
 * selinux_state.ss) and the status page/lock live there too.  5.7 moved them
 * onto struct selinux_state itself.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 7, 0)
static struct selinux_ss fake_ss;
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 7, 0)
#define NKSU_STATUS_PAGE (selinux_state.status_page)
#define NKSU_STATUS_LOCK_OBJ (selinux_state.status_lock)
#else
#define NKSU_STATUS_PAGE (selinux_state.ss->status_page)
#define NKSU_STATUS_LOCK_OBJ (selinux_state.ss->status_lock)
#endif

/* Last: redirects unexported symbols (selinux_state, ...) through pointers. */
#include "symbol/symbol_compat.h"

/*
 * This file calls several resolved-but-unexported kernel functions through
 * pointers (policydb_write, avc_has_perm, security_*_to_sid, and the saved
 * f_op handlers), exactly like selinux.c / policy.c / rule.c / manager.c, so it
 * disables CFI for its own functions the same way they do.  Without it the
 * first indirect call panics with "CFI failure (target: policydb_write)" when
 * the module was built by a different clang than the running kernel.
 *
 * Note this only drops the checks at *our* call sites: the compiler still
 * emits the .cfi_jt stub for address-taken functions, so the handlers nksu
 * installs into selinuxfs f_ops remain valid targets for the kernel's own
 * indirect calls.
 */
#if defined(__clang__)
#pragma clang attribute push(__attribute__((no_sanitize("cfi"))), apply_to = function)
#endif

/* Feature name in the registry, and the key in the persistent enabled set. */
#define NKSU_HIDE_FEATURE_NAME "selinux_hide"

/* selinuxfs caps one transaction at a page. */
#define NKSU_SIMPLE_TRANSACTION_LIMIT 4096

/* Android's first application uid.  Requests from app uids are answered from
 * the clean policy; system/root requests pass straight through. */
#define NKSU_APP_UID_MIN 10000

/* Index into selinuxfs's `write_op` table, mirroring sel_inos in selinuxfs.c. */
enum sel_inos {
	SEL_ROOT_INO = 2,
	SEL_LOAD,
	SEL_ENFORCE,
	SEL_CONTEXT,
	SEL_ACCESS,
	SEL_CREATE,
	SEL_RELABEL,
	SEL_USER,
	SEL_POLICYVERS,
	SEL_COMMIT_BOOLS,
	SEL_MLS,
	SEL_DISABLE,
	SEL_MEMBER,
	SEL_CHECKREQPROT,
	SEL_COMPAT_NET,
	SEL_REJECT_UNKNOWN,
	SEL_DENY_UNKNOWN,
	SEL_STATUS,
	SEL_POLICY,
	SEL_VALIDATE_TRANS,
	SEL_INO_NEXT,
};

typedef ssize_t (*write_op_fn)(struct file *, char *, size_t);

static DEFINE_MUTEX(nksu_hide_mutex);
static bool nksu_hide_running __read_mostly;
static bool nksu_hide_hooked;

static write_op_fn *context_write_slot;
static write_op_fn *access_write_slot;
static write_op_fn orig_context_write;
static write_op_fn orig_access_write;

static int (**status_open_slot)(struct inode *, struct file *);
static int (*orig_status_open)(struct inode *, struct file *);

static struct page *fake_status;

/*
 * Gates the lazy retry of the fake-status snapshot: enabled while the page may
 * still be missing, disabled once it is taken or the single retry has run.  A
 * static key keeps the status-open path branch-free once it is off.
 */
static DEFINE_STATIC_KEY_FALSE(nksu_fake_status_init_key);

/* The clean policy, serialised once when the feature is enabled. */
static void *clean_policy_data;
static size_t clean_policy_len;

static ssize_t (**policy_read_slot)(struct file *, char __user *, size_t, loff_t *);
static ssize_t (*orig_policy_read)(struct file *, char __user *, size_t, loff_t *);

/* Bound the capture; a loaded policy blob is a few hundred KiB. */
#define NKSU_POLICY_BLOB_MAX (16 * 1024 * 1024)

/* core.c: false for a first-stage (vendor_boot) load, true for a late insmod. */
extern bool late_load;

static void nksu_fake_status_prepare(void)
{
	struct selinux_kernel_status *cur, *copy;
	struct page *page;

	mutex_lock(&NKSU_STATUS_LOCK_OBJ);
	if (fake_status)
		goto out;
	if (!NKSU_STATUS_PAGE) {
		pr_warn("[selinux_hide] status page not ready\n");
		goto out;
	}

	cur = page_address(NKSU_STATUS_PAGE);
	if (!cur->enforcing && !late_load) {
		pr_warn("[selinux_hide] permissive, fake status skipped\n");
		goto out;
	}

	page = alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!page)
		goto out;

	copy = page_address(page);
	memcpy(copy, cur, sizeof(*copy));

	if (late_load) {
		/*
         * A loader may have reloaded sepolicy before us, so the captured page
         * is not stock.  Serve what a stock boot ends with instead: the
         * creation sentinel below 6.10, one load plus one setenforce above.
         */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 10, 0)
		copy->sequence = 4;
		copy->policyload = 1;
#else
		copy->sequence = 0;
		copy->policyload = 0;
#endif
		if (!copy->enforcing)
			copy->enforcing = 1;
	}

	fake_status = page;
	pr_info("[selinux_hide] fake status ready: sequence=%u policyload=%u enforcing=%u\n",
		copy->sequence, copy->policyload, copy->enforcing);
out:
	mutex_unlock(&NKSU_STATUS_LOCK_OBJ);
}

static int nksu_status_open(struct inode *inode, struct file *filp)
{
	if (likely(current_uid().val >= NKSU_APP_UID_MIN && nksu_hide_running)) {
		void *page;
		int ret;

		mutex_lock(&NKSU_STATUS_LOCK_OBJ);
		page = fake_status;
		mutex_unlock(&NKSU_STATUS_LOCK_OBJ);

		if (page) {
			filp->private_data = page;
			return 0;
		}

		/*
         * The status page is allocated lazily by the stock handler on the
         * first open, which can happen after the feature is enabled.  Run the
         * original first so the page exists, then snapshot it for the next
         * call (mirrors KernelSU's fake_status_initialize_key).
         */
		ret = orig_status_open(inode, filp);
		if (!ret && static_branch_unlikely(&nksu_fake_status_init_key)) {
			static_key_disable(&nksu_fake_status_init_key.key);
			nksu_fake_status_prepare();
		}
		return ret;
	}
	return orig_status_open(inode, filp);
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
static nksu_fake_state_t fake_state;
#endif

/*
 * Version-neutral policy queries against the clean policy.  On 6.6+ the
 * kernel's context/SID/AV helpers are gone, so the vendored engine answers;
 * older kernels reach their exported helpers through fake_state, a throwaway
 * selinux_state pointing at the same clean policy.
 */
static int nksu_clean_context_to_sid(const char *scontext, u32 scontext_len, u32 *sid)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
	return nksu_engine_context_to_sid(nksu_orig_policy_get(), scontext, scontext_len, sid,
					  SECSID_NULL, GFP_KERNEL);
#else
	return security_context_to_sid(&fake_state, scontext, scontext_len, sid, GFP_KERNEL);
#endif
}

static int nksu_clean_sid_to_context(u32 sid, char **scontext, u32 *scontext_len)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
	return nksu_engine_sid_to_context(nksu_orig_policy_get(), sid, scontext, scontext_len);
#else
	return security_sid_to_context(&fake_state, sid, scontext, scontext_len);
#endif
}

static void nksu_clean_compute_av(u32 ssid, u32 tsid, u16 tclass, struct av_decision *avd)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
	nksu_engine_compute_av(nksu_orig_policy_get(), ssid, tsid, tclass, avd);
#else
	security_compute_av_user(&fake_state, ssid, tsid, tclass, avd);
#endif
}

/*
 * Kernel-side setprocattr hook, ported from KernelSU's my_setprocattr.
 *
 * A su checker (or anything else) writes its SELinux context to
 * /proc/self/attr/current; the kernel routes that through the LSM setprocattr
 * hook to selinux_setprocattr().  We answer app uids from the clean policy the
 * same way the context/access writes above do, while keeping the live
 * permission checks (SETCURRENT, DYNTRANSITION, ptrace) intact.
 *
 * The working policy shares the original policy's sidtab (see
 * nksu_policy_dup), so the SID resolved here is already valid in the live
 * policy.  KernelSU needs a second resolve into a separate sidtab for this;
 * NekoSU does not.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
#define NKSU_AVC_HAS_PERM(...) avc_has_perm(__VA_ARGS__)
#define NKSU_BOUNDED_TRANSITION(old, new) nksu_bounded_transition_fn(old, new)
#else
#define NKSU_AVC_HAS_PERM(...) avc_has_perm(&selinux_state, __VA_ARGS__)
#define NKSU_BOUNDED_TRANSITION(old, new) nksu_bounded_transition_fn(&selinux_state, old, new)
#endif

/*
 * The LSM setprocattr hook lost its struct task_struct argument in 5.9; keep
 * both call signatures behind one parameter list.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 9, 0)
typedef int (*nksu_setprocattr_fn)(struct task_struct *p, char *name, void *value, size_t size);
#define NKSU_SETPROC_PARAMS struct task_struct *p, char *name, void *value, size_t size
#define NKSU_SETPROC_ARGS p, name, value, size
#else
typedef int (*nksu_setprocattr_fn)(const char *name, void *value, size_t size);
#define NKSU_SETPROC_PARAMS const char *name, void *value, size_t size
#define NKSU_SETPROC_ARGS name, value, size
#endif

/* security_bounded_transition(), resolved in nksu_hide_enable(); optional. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
static int (*nksu_bounded_transition_fn)(u32 old_sid, u32 new_sid);
#else
static int (*nksu_bounded_transition_fn)(nksu_fake_state_t *state, u32 old_sid, u32 new_sid);
#endif

/*
 * current_is_single_threaded() is not exported on GKI and is referenced by the
 * setprocattr hook, so reach it through kallsyms like every other unexported
 * symbol (a direct reference makes the module fail to load).
 */
static bool (*nksu_current_is_single_threaded_fn)(void);

static u32 nksu_cred_sid(const struct cred *cred)
{
	return nksu_cred_security(cred)->sid;
}

static u32 nksu_task_sid_obj(const struct task_struct *task)
{
	u32 sid;

	rcu_read_lock();
	sid = nksu_cred_sid(__task_cred(task));
	rcu_read_unlock();
	return sid;
}

static u32 nksu_ptrace_parent_sid(void)
{
	u32 sid = 0;
	struct task_struct *tracer;

	rcu_read_lock();
	tracer = ptrace_parent(current);
	if (tracer)
		sid = nksu_task_sid_obj(tracer);
	rcu_read_unlock();
	return sid;
}

static int __nocfi nksu_setprocattr(NKSU_SETPROC_PARAMS);

static struct nksu_lsm_hook nksu_setprocattr_hook =
	NKSU_LSM_HOOK_INIT(setprocattr, "selinux_setprocattr", nksu_setprocattr);

static int __nocfi nksu_setprocattr(NKSU_SETPROC_PARAMS)
{
	struct task_security_struct *tsec;
	struct cred *new;
	u32 mysid, sid = 0, ptsid;
	int error;
	char *str = value;

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 9, 0)
	if (p != current)
		goto call_orig;
#endif

	if (likely(current_uid().val < NKSU_APP_UID_MIN))
		goto call_orig;

	if (strcmp(name, "current"))
		goto call_orig;

	mysid = nksu_current_sid();
	error = NKSU_AVC_HAS_PERM(mysid, mysid, SECCLASS_PROCESS, PROCESS__SETCURRENT, NULL);
	if (error)
		return error;

	/* Obtain a SID for the context, if one was specified. */
	if (size && str[0] && str[0] != '\n') {
		if (str[size - 1] == '\n') {
			str[size - 1] = 0;
			size--;
		}
		error = nksu_clean_context_to_sid(value, size, &sid);
		if (error)
			return error;
	}

	new = prepare_creds();
	if (!new)
		return -ENOMEM;

	/*
     * Permission checking based on the specified context is performed
     * during the actual operation (execve, open/mkdir/...), when the full
     * context is known; see selinux_bprm_creds_for_exec and may_create.
     */
	tsec = nksu_cred_security(new);
	error = -EINVAL;
	if (sid == 0)
		goto abort_change;

	if (!nksu_current_is_single_threaded_fn || !nksu_current_is_single_threaded_fn()) {
		/* Multi-threaded (or undetectable): enforce the bounded
         * transition, as the stock handler does. */
		if (nksu_bounded_transition_fn) {
			error = NKSU_BOUNDED_TRANSITION(tsec->sid, sid);
			if (error)
				goto abort_change;
		}
	}

	/* Check permissions for the transition. */
	error = NKSU_AVC_HAS_PERM(tsec->sid, sid, SECCLASS_PROCESS, PROCESS__DYNTRANSITION, NULL);
	if (error)
		goto abort_change;

	/* Check for ptracing, and update the task SID if ok.  Otherwise leave
     * the SID unchanged and fail. */
	ptsid = nksu_ptrace_parent_sid();
	if (ptsid != 0) {
		error = NKSU_AVC_HAS_PERM(ptsid, sid, SECCLASS_PROCESS, PROCESS__PTRACE, NULL);
		if (error)
			goto abort_change;
	}

	tsec->sid = sid;
	commit_creds(new);
	return size;

abort_change:
	abort_creds(new);
	return error;

call_orig:
	return ((nksu_setprocattr_fn)nksu_setprocattr_hook.original)(NKSU_SETPROC_ARGS);
}

static ssize_t nksu_write_context(struct file *file, char *buf, size_t size)
{
	char *canon = NULL;
	u32 sid, len;
	ssize_t length;

	if (likely(current_uid().val < NKSU_APP_UID_MIN))
		return orig_context_write(file, buf, size);

	length = NKSU_AVC_HAS_PERM(nksu_current_sid(), SECINITSID_SECURITY, SECCLASS_SECURITY,
				   SECURITY__CHECK_CONTEXT, NULL);
	if (length)
		goto out;

	length = nksu_clean_context_to_sid(buf, size, &sid);
	if (length)
		goto out;

	length = nksu_clean_sid_to_context(sid, &canon, &len);
	if (length)
		goto out;

	length = -ERANGE;
	if (len > NKSU_SIMPLE_TRANSACTION_LIMIT)
		goto out;

	memcpy(buf, canon, len);
	length = len;
out:
	kfree(canon);
	return length;
}

static ssize_t nksu_write_access(struct file *file, char *buf, size_t size)
{
	char *scon = NULL, *tcon = NULL;
	u32 ssid, tsid;
	u16 tclass;
	struct av_decision avd;
	ssize_t length;

	if (likely(current_uid().val < NKSU_APP_UID_MIN))
		return orig_access_write(file, buf, size);

	length = NKSU_AVC_HAS_PERM(nksu_current_sid(), SECINITSID_SECURITY, SECCLASS_SECURITY,
				   SECURITY__COMPUTE_AV, NULL);
	if (length)
		goto out;

	length = -ENOMEM;
	scon = kzalloc(2 * (size + 1), GFP_KERNEL);
	if (!scon)
		goto out;
	tcon = scon + size + 1;

	length = -EINVAL;
	if (sscanf(buf, "%s %s %hu", scon, tcon, &tclass) != 3)
		goto out;

	length = nksu_clean_context_to_sid(scon, strlen(scon), &ssid);
	if (length)
		goto out;

	length = nksu_clean_context_to_sid(tcon, strlen(tcon), &tsid);
	if (length)
		goto out;

	nksu_clean_compute_av(ssid, tsid, tclass, &avd);

	/*
     * Stock reads 1 here (the policy's real load counter).  The status page
     * is deliberately left with the kernel's own values -- sequence and
     * policyload stay 0 below 6.10 (see avc_reset) -- so this is not a
     * cross-check that has to agree with it.
     */
	avd.seqno = 1;
	length = scnprintf(buf, NKSU_SIMPLE_TRANSACTION_LIMIT, "%x %x %x %x %u %x", avd.allowed,
			   0xffffffff, avd.auditallow, avd.auditdeny, avd.seqno, avd.flags);
out:
	kfree(scon);
	return length;
}

/*
 * Serialise the clean policy the way security_read_policy() does:
 * policydb_write() into a buffer sized for the loaded blob, with the per-type
 * slack policydb_read() needs for the attribute maps (see nksu_policy_dup()).
 */
static int nksu_clean_policy_prepare(void)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 7, 0)
	struct selinux_policy *pol = nksu_orig_policy_get();
#else
	struct policydb *pol = nksu_orig_policy_get();
#endif
	struct policy_file fp;
	size_t len;
	void *data;
	int ret;

	if (clean_policy_data)
		return 0;
	if (!pol)
		return -EAGAIN;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 7, 0)
	len = pol->policydb.len + (size_t)pol->policydb.p_types.nprim * (sizeof(u32) + sizeof(u64));
#else
	len = pol->len + (size_t)pol->p_types.nprim * (sizeof(u32) + sizeof(u64));
#endif
	if (!len || len > NKSU_POLICY_BLOB_MAX)
		return -EINVAL;

	data = vmalloc(len);
	if (!data)
		return -ENOMEM;

	fp.data = data;
	fp.len = len;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 7, 0)
	ret = policydb_write(&pol->policydb, &fp);
#else
	ret = policydb_write(pol, &fp);
#endif
	if (ret) {
		vfree(data);
		return ret;
	}

	clean_policy_len = len - fp.len;
	clean_policy_data = data;
	pr_info("[selinux_hide] captured %zu-byte clean policy\n", clean_policy_len);
	return 0;
}

static void nksu_clean_policy_release(void)
{
	if (clean_policy_data) {
		vfree(clean_policy_data);
		clean_policy_data = NULL;
		clean_policy_len = 0;
	}
}

/* /sys/fs/selinux/policy: hand apps the clean policy instead of the live one. */
static ssize_t nksu_read_policy(struct file *filp, char __user *buf, size_t count, loff_t *ppos)
{
	if (likely(current_uid().val < NKSU_APP_UID_MIN) || !nksu_hide_running ||
	    !clean_policy_data)
		return orig_policy_read(filp, buf, count, ppos);

	return simple_read_from_buffer(buf, count, ppos, clean_policy_data, clean_policy_len);
}

static int nksu_hide_unhook(void)
{
	int ret = 0, err;

	if (context_write_slot) {
		err = nksu_patch_text(context_write_slot, &orig_context_write,
				      sizeof(orig_context_write));
		if (err) {
			pr_err("[selinux_hide] restore context write: %d\n", err);
			ret = err;
		} else {
			context_write_slot = NULL;
			orig_context_write = NULL;
		}
	}
	if (access_write_slot) {
		err = nksu_patch_text(access_write_slot, &orig_access_write,
				      sizeof(orig_access_write));
		if (err) {
			pr_err("[selinux_hide] restore access write: %d\n", err);
			ret = ret ? ret : err;
		} else {
			access_write_slot = NULL;
			orig_access_write = NULL;
		}
	}
	if (status_open_slot && orig_status_open) {
		err = nksu_patch_text(status_open_slot, &orig_status_open,
				      sizeof(orig_status_open));
		if (err) {
			pr_err("[selinux_hide] restore status open: %d\n", err);
			ret = ret ? ret : err;
		} else {
			status_open_slot = NULL;
			orig_status_open = NULL;
		}
	}
	if (policy_read_slot && orig_policy_read) {
		err = nksu_patch_text(policy_read_slot, &orig_policy_read,
				      sizeof(orig_policy_read));
		if (err) {
			pr_err("[selinux_hide] restore policy read: %d\n", err);
			ret = ret ? ret : err;
		} else {
			policy_read_slot = NULL;
			orig_policy_read = NULL;
		}
	}

	nksu_lsm_unhook(&nksu_setprocattr_hook);
	if (nksu_setprocattr_hook.entry) {
		pr_err("[selinux_hide] restore setprocattr hook failed\n");
		ret = ret ? ret : -EIO;
	}

	return ret;
}

static int nksu_hide_enable(void)
{
	struct file_operations *policy_ops, *status_ops;
	write_op_fn *write_op, new_write;
	int ret;

	if (!nksu_orig_policy_get()) {
		pr_err("[selinux_hide] no original policy available\n");
		return -EAGAIN;
	}

	write_op = (write_op_fn *)nksu_ksym_lookup("write_op");
	if (!write_op) {
		pr_err("[selinux_hide] symbol 'write_op' not found\n");
		return -ENOSYS;
	}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
	nksu_engine_init();
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 7, 0)
	fake_state.initialized = true;
	fake_state.policy = nksu_orig_policy_get();
#else
	/*
	 * Build a throwaway state whose ss keeps the untouched policydb but
	 * shares the live sidtab (and class map), so contexts resolved here
	 * stay valid for the live policy.
	 */
	memset(&fake_ss, 0, sizeof(fake_ss));
	fake_ss.policydb = *nksu_orig_policy_get();
	fake_ss.sidtab = selinux_state.ss->sidtab;
	fake_ss.map = selinux_state.ss->map;
	fake_ss.latest_granting = selinux_state.ss->latest_granting;
	rwlock_init(&fake_ss.policy_rwlock);
	mutex_init(&fake_ss.status_lock);
	fake_state.initialized = true;
	fake_state.ss = &fake_ss;
#endif

	nksu_bounded_transition_fn =
		(typeof(nksu_bounded_transition_fn))nksu_ksym_lookup("security_bounded_transition");
	if (!nksu_bounded_transition_fn)
		pr_warn("[selinux_hide] security_bounded_transition not found\n");

	nksu_current_is_single_threaded_fn =
		(typeof(nksu_current_is_single_threaded_fn))nksu_ksym_lookup(
			"current_is_single_threaded");
	if (!nksu_current_is_single_threaded_fn)
		pr_warn("[selinux_hide] current_is_single_threaded not found\n");

	ret = nksu_clean_policy_prepare();
	if (ret) {
		pr_err("[selinux_hide] cannot prepare the clean policy: %d\n", ret);
		return ret;
	}
	nksu_hide_hooked = true;

	policy_ops = (struct file_operations *)nksu_ksym_lookup("sel_policy_ops");
	if (policy_ops) {
		ssize_t (*new_read)(struct file *, char __user *, size_t, loff_t *) =
			nksu_read_policy;

		policy_read_slot = &policy_ops->read;
		orig_policy_read = *policy_read_slot;
		ret = nksu_patch_text(policy_read_slot, &new_read, sizeof(new_read));
		if (ret) {
			pr_err("[selinux_hide] patch policy read: %d\n", ret);
			policy_read_slot = NULL;
			orig_policy_read = NULL;
		}
	} else {
		pr_warn("[selinux_hide] sel_policy_ops not found\n");
	}

	context_write_slot = &write_op[SEL_CONTEXT];
	orig_context_write = *context_write_slot;
	new_write = nksu_write_context;
	ret = nksu_patch_text(context_write_slot, &new_write, sizeof(new_write));
	if (ret) {
		pr_err("[selinux_hide] patch context write: %d\n", ret);
		context_write_slot = NULL;
		goto fail;
	}

	access_write_slot = &write_op[SEL_ACCESS];
	orig_access_write = *access_write_slot;
	new_write = nksu_write_access;
	ret = nksu_patch_text(access_write_slot, &new_write, sizeof(new_write));
	if (ret) {
		pr_err("[selinux_hide] patch access write: %d\n", ret);
		access_write_slot = NULL;
		goto fail;
	}

	status_ops = (struct file_operations *)nksu_ksym_lookup("sel_handle_status_ops");
	if (status_ops) {
		int (*new_open)(struct inode *, struct file *) = nksu_status_open;

		status_open_slot = &status_ops->open;
		orig_status_open = *status_open_slot;
		ret = nksu_patch_text(status_open_slot, &new_open, sizeof(new_open));
		if (ret) {
			pr_err("[selinux_hide] patch status open: %d\n", ret);
			status_open_slot = NULL;
			orig_status_open = NULL;
		}
	} else {
		pr_warn("[selinux_hide] sel_handle_status_ops not found\n");
	}

	static_key_enable(&nksu_fake_status_init_key.key);
	nksu_fake_status_prepare();
	if (fake_status)
		static_key_disable(&nksu_fake_status_init_key.key);

	/*
     * The setprocattr hook is best-effort: the context/access/status/policy
     * hiding above is what makes the feature work, so do not fail the whole
     * enable if the LSM hook cannot be located.
     */
	ret = nksu_lsm_hook(&nksu_setprocattr_hook);
	if (ret)
		pr_warn("[selinux_hide] setprocattr hook not installed: %d\n", ret);

	pr_info("[selinux_hide] enabled\n");
	return 0;

fail:
	if (nksu_hide_unhook())
		pr_err("[selinux_hide] rollback incomplete\n");
	else
		nksu_hide_hooked = false;
	return ret;
}

static int nksu_hide_disable(void)
{
	int ret;

	if (!nksu_hide_running && !nksu_hide_hooked)
		return 0;

	ret = nksu_hide_unhook();
	if (ret) {
		pr_err("[selinux_hide] disable failed, hooks still installed: %d\n", ret);
		return ret;
	}
	nksu_hide_hooked = false;
	if (nksu_hide_running) {
		static_key_disable(&nksu_fake_status_init_key.key);
		pr_info("[selinux_hide] disabled\n");
	}
	return 0;
}

static int nksu_hide_feature_get(u64 *value)
{
	*value = nksu_hide_running ? 1 : 0;
	return 0;
}

/*
 * Switch the feature.  `persist` is false for the boot-time enable: the
 * feature set already lists it, so rewriting it there would be pointless.
 */
static int nksu_hide_apply(u64 value, bool persist)
{
	int ret = 0;

	mutex_lock(&nksu_hide_mutex);
	if (value) {
		if (!nksu_hide_running) {
			if (nksu_hide_hooked) {
				ret = nksu_hide_disable();
				if (ret)
					goto out;
			}
			ret = nksu_hide_enable();
			if (!ret)
				nksu_hide_running = true;
		}
	} else if (nksu_hide_running || nksu_hide_hooked) {
		ret = nksu_hide_disable();
		if (!ret)
			nksu_hide_running = false;
	}
out:
	mutex_unlock(&nksu_hide_mutex);

	if (!ret && persist) {
		int persist_ret = nksu_feature_persist_set(NKSU_HIDE_FEATURE_NAME, value != 0);

		if (persist_ret)
			pr_err("[selinux_hide] cannot persist feature state: %d\n", persist_ret);
	}

	return ret;
}

static int nksu_hide_feature_set(u64 value)
{
	return nksu_hide_apply(value, true);
}

static const struct nksu_feature nksu_hide_feature = {
	.id = NKSU_FEATURE_SELINUX_HIDE,
	.name = "selinux_hide",
	.get = nksu_hide_feature_get,
	.set = nksu_hide_feature_set,
};

int nksu_selinux_hide_init(void)
{
	int ret;

	ret = nksu_feature_register(&nksu_hide_feature);
	if (ret)
		pr_warn("[selinux_hide] feature register failed: %d\n", ret);

	if (!nksu_feature_persist_has(NKSU_HIDE_FEATURE_NAME)) {
		pr_info("[selinux_hide] not in the enabled feature set, off\n");
		return 0;
	}

	ret = nksu_hide_apply(1, false);
	if (ret)
		pr_warn("[selinux_hide] enable from the feature set failed: %d\n", ret);

	return 0;
}

void nksu_selinux_hide_exit(void)
{
	int ret;

	mutex_lock(&nksu_hide_mutex);
	ret = nksu_hide_disable();
	if (!ret)
		nksu_hide_running = false;
	else
		pr_emerg("[selinux_hide] cannot restore hooks, keeping state: %d\n", ret);
	mutex_unlock(&nksu_hide_mutex);

	if (ret)
		return;

	nksu_clean_policy_release();

	mutex_lock(&NKSU_STATUS_LOCK_OBJ);
	if (fake_status) {
		__free_page(fake_status);
		fake_status = NULL;
	}
	mutex_unlock(&NKSU_STATUS_LOCK_OBJ);
}

#if defined(__clang__)
#pragma clang attribute pop
#endif
