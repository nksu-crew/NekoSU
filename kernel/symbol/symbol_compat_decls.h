/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NKSU_SYMBOL_COMPAT_DECLS_H
#define NKSU_SYMBOL_COMPAT_DECLS_H

/*
 * Declarations for the unexported-symbol indirection layer.
 *
 * Problem: these symbols are no longer exported on GKI/KMI kernels. A direct
 * reference makes the module fail to load with "Unknown symbol ... (err -2)".
 *
 * Approach: for each function symbol declare a pointer of the same type, fill
 * it at load time via nksu_ksym_lookup(), and call it through a __nocfi
 * wrapper.  Data symbols get a pointer that is dereferenced directly.  This
 * removes the relocation so unexported symbols can still be called.
 *
 *   NKSU_SYMBOL_FN(...);   // declares nksu_<name> and nksu_<name>_fn
 *   extern typeof(selinux_state) *nksu_selinux_state;  // data pointer
 *
 * This header only declares the pointers and wrappers; symbol_compat.h adds
 * the name redirection for the callers.  symbol_compat.c includes this header
 * directly, because it must keep the real spellings when it defines the
 * pointers and resolves the symbols.
 */

#include <linux/types.h>
#include <linux/version.h>
#include <linux/fs.h>
#include <linux/shmem_fs.h>
#include <linux/namei.h>
#include <linux/dcache.h>
#include <linux/xattr.h>
#include <linux/sched.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 12, 0)
#include <linux/sched/task.h>
#include <linux/sched/signal.h>
#endif
#include <linux/binfmts.h>
#include <linux/cred.h>
#include <linux/nsproxy.h>
#include <linux/fs_struct.h>
#include <linux/initrd.h>
#include <linux/fsnotify_backend.h>
#include <linux/uidgid.h>
#include <linux/user_namespace.h>
#include <linux/syscalls.h>
#include <linux/task_work.h>
#include <linux/pid.h>
#include <asm/fixmap.h>
#include <linux/seccomp.h>
#include <crypto/akcipher.h>
#include <linux/btf.h>

#include "kernel_compat.h"

#include "security.h"
#include "ss/policydb.h"
#include "ss/services.h"
#include "ss/avtab.h"
#include "ss/symtab.h"
#include "ss/hashtab.h"
#include "ss/ebitmap.h"
#include "ss/constraint.h"
#include "avc.h"
#include "avc_ss.h"
#include "xfrm.h"
#include "objsec.h"

/*
 * A resolved function symbol must be called from a __nocfi wrapper: the
 * indirect call is CFI-checked in the caller, and the module's function type
 * hash need not match the running kernel's.  Keeping the indirect call in one
 * wrapper also means individual .c files no longer need a file-wide
 * no_sanitize("cfi") pragma.
 *
 * Each invocation declares nksu_<name> using the kernel's own prototype and
 * defines nksu_<name>_fn, which symbol_compat.h redirects the original name
 * to.  _params declares the wrapper's parameters and _args forwards them to
 * the resolved pointer.
 */
#define NKSU_SYMBOL_FN(_ret, _name, _params, _args)          \
	extern typeof(_name) *nksu_##_name;                  \
	static inline __nocfi _ret nksu_##_name##_fn _params \
	{                                                    \
		return nksu_##_name _args;                   \
	}

#define NKSU_SYMBOL_VOID_FN(_name, _params, _args)           \
	extern typeof(_name) *nksu_##_name;                  \
	static inline __nocfi void nksu_##_name##_fn _params \
	{                                                    \
		nksu_##_name _args;                          \
	}

/*
 * Function symbols that are still called through the raw pointer.  These are
 * either only reached from __nocfi code already (spawn, patching) or wrapped
 * by the __nocfi helpers below, so they need no NKSU_SYMBOL_FN() wrapper.
 * Every other function symbol is declared by its NKSU_SYMBOL_FN() invocation.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 7, 0)
/*
 * 4.9 keeps struct symtab as a pointer to its hashtab and only ships
 * symtab_init(); the search/insert helpers were added later.  Reach the
 * hashtab primitives instead and provide the same API locally.
 */
extern typeof(hashtab_search) *nksu_hashtab_search;
extern typeof(hashtab_insert) *nksu_hashtab_insert;
#endif
extern typeof(hashtab_destroy) *nksu_hashtab_destroy;
extern typeof(hashtab_map) *nksu_hashtab_map;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 9, 0)
extern typeof(__hashtab_insert) *nksu___hashtab_insert;
#endif
/*
 * 6.6 dropped the struct selinux_state argument from the helpers and removed
 * some of them outright; hide.c only calls the raw ones below 6.6 (6.6+ goes
 * through its own *_with_policy variants), so only avc_has_perm and
 * security_context_to_sid survive there, both via __nocfi wrappers.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
extern typeof(security_context_str_to_sid) *nksu_security_context_str_to_sid;
#endif

/*
 * File primitives for the module's own state files (feature registry, profile
 * store, packages.xml, /sys/kernel/vmcoreinfo) and for the symbol resolver's
 * bootstrap. The target kernel exports these only into its GKI VFS namespace,
 * so a direct reference is invisible to us and the module fails to load.
 * kernel_read()/kernel_write() are only used from the __nocfi nksu_kread()/
 * nksu_kwrite() helpers below.
 */
extern typeof(kernel_read) *nksu_kernel_read;
extern typeof(kernel_write) *nksu_kernel_write;

/* Task-list lock for the manager's process snapshot (manager.c). */
extern typeof(tasklist_lock) *nksu_tasklist_lock;

/*
 * Positional file I/O with the modern (buf, count, loff_t *) signature: 4.9
 * passes the offset by value and returns it separately, so translate here and
 * keep every call site identical across versions.
 */
static inline ssize_t __nocfi nksu_kread(struct file *file, void *buf, size_t count, loff_t *pos)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 14, 0)
	int ret = nksu_kernel_read(file, *pos, (char *)buf, count);

	if (ret > 0)
		*pos += ret;
	return ret;
#else
	return nksu_kernel_read(file, buf, count, pos);
#endif
}

static inline ssize_t __nocfi nksu_kwrite(struct file *file, const void *buf, size_t count,
					  loff_t *pos)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 14, 0)
	ssize_t ret = nksu_kernel_write(file, (const char *)buf, count, *pos);

	if (ret > 0)
		*pos += ret;
	return ret;
#else
	return nksu_kernel_write(file, buf, count, pos);
#endif
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 7, 0)
static inline void __nocfi *nksu_symtab_search(struct symtab *s, const char *name)
{
	if (!s || !s->table || !nksu_hashtab_search)
		return NULL;
	return nksu_hashtab_search(s->table, name);
}

static inline int __nocfi nksu_symtab_insert(struct symtab *s, char *name, void *datum)
{
	int rc;

	if (!s || !s->table || !nksu_hashtab_insert)
		return -EINVAL;
	rc = nksu_hashtab_insert(s->table, name, datum);
	if (!rc)
		s->nprim++;
	return rc;
}
#endif

/* The spawn helper waits for the initramfs before exec (spawn/spawn.c). */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 13, 0)
extern typeof(wait_for_initramfs) *nksu_wait_for_initramfs;
#endif

/*
 * fsnotify_add_inode_mark() is a header inline that forwards to the unexported
 * fsnotify_add_mark(), so the relocation lands on the call site: attach inode
 * marks through this wrapper instead.  4.9 always goes through the raw
 * fsnotify_add_mark() signature instead (see manager/manager.c).
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 9, 0)
int __nocfi nksu_fsnotify_add_inode_mark(struct fsnotify_mark *mark, struct inode *inode,
					 int add_flags);
#endif

/*
 * spawn primitives (kernel/spawn/spawn.c). None of these is exported on GKI, and they
 * are exactly what a self-contained call_usermodehelper replacement needs:
 * spawn a task, exec a program in it, wait for it, reset its handlers.
 * 4.9 has neither kernel_execve() nor kernel_wait(); the equivalent
 * do_execve(getname_kernel())/sys_wait4() pair is used there instead.
 * They are all called from __nocfi code, so the raw pointers are fine.
 */
extern typeof(kernel_thread) *nksu_kernel_thread;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0)
extern typeof(kernel_execve) *nksu_kernel_execve;
extern typeof(kernel_wait) *nksu_kernel_wait;
#else
extern typeof(do_execve) *nksu_do_execve;
extern typeof(getname_kernel) *nksu_getname_kernel;
extern typeof(sys_wait4) *nksu_sys_wait4;
#endif
extern typeof(flush_signal_handlers) *nksu_flush_signal_handlers;

/* data symbol (pointer, dereferenced by the macro) */
extern typeof(selinux_state) *nksu_selinux_state;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 1, 0)
extern typeof(selinux_blob_sizes) *nksu_selinux_blob_sizes;
#endif
extern typeof(init_nsproxy) *nksu_init_nsproxy;

extern typeof(__set_fixmap) *nksu___set_fixmap;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 8, 0)
extern typeof(copy_to_kernel_nofault) *nksu_copy_to_kernel_nofault;
#else
extern typeof(probe_kernel_write) *nksu_probe_kernel_write;
#define copy_to_kernel_nofault(dst, src, size) nksu_probe_kernel_write((dst), (src), (size))
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 9, 0)
/* Needed for typeof(path_umount) on kernels whose headers do not declare it. */
extern int path_umount(struct path *path, int flags);
#endif

NKSU_SYMBOL_FN(struct user_struct *, alloc_uid, (kuid_t kuid), (kuid))
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 14, 0)
NKSU_SYMBOL_FN(int, set_cred_ucounts, (struct cred * new), (new))
#endif
NKSU_SYMBOL_VOID_FN(switch_task_namespaces, (struct task_struct * p, struct nsproxy *new), (p, new))
NKSU_SYMBOL_FN(struct file *, filp_open, (const char *filename, int flags, umode_t mode),
	       (filename, flags, mode))
NKSU_SYMBOL_FN(struct file *, shmem_file_setup,
	       (const char *name, loff_t size, unsigned long flags), (name, size, flags))
NKSU_SYMBOL_FN(int, iterate_dir, (struct file * file, struct dir_context *ctx), (file, ctx))
/*
 * VFS helpers for the ncore bootstrap (manager/manager.c).  Like the rest,
 * these must not be called through the raw pointer: the module's CFI type
 * hash for a symbol resolved by kallsyms need not match the one vmlinux
 * emitted, and the checked indirect call then panics the boot:
 *
 *     CFI failure (target: lookup_one_len+0x0/0x1ac)
 *      kdir_mkdir_p+0x23c/0x2e0 [nksu]
 *
 * vfs_mkdir()/vfs_unlink() gained the mount-namespace argument in 5.12 and
 * the mnt_idmap argument in 6.3; the call sites in manager.c use the same
 * three variants.
 */
NKSU_SYMBOL_FN(struct dentry *, lookup_one_len, (const char *name, struct dentry *base, int len),
	       (name, base, len))
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
NKSU_SYMBOL_FN(int, vfs_mkdir,
	       (struct mnt_idmap * idmap, struct inode *dir, struct dentry *dentry, umode_t mode),
	       (idmap, dir, dentry, mode))
NKSU_SYMBOL_FN(int, vfs_unlink,
	       (struct mnt_idmap * idmap, struct inode *dir, struct dentry *dentry,
		struct inode **delegated_inode),
	       (idmap, dir, dentry, delegated_inode))
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 12, 0)
NKSU_SYMBOL_FN(int, vfs_mkdir,
	       (struct user_namespace * mnt_userns, struct inode *dir, struct dentry *dentry,
		umode_t mode),
	       (mnt_userns, dir, dentry, mode))
NKSU_SYMBOL_FN(int, vfs_unlink,
	       (struct user_namespace * mnt_userns, struct inode *dir, struct dentry *dentry,
		struct inode **delegated_inode),
	       (mnt_userns, dir, dentry, delegated_inode))
#else
NKSU_SYMBOL_FN(int, vfs_mkdir, (struct inode * dir, struct dentry *dentry, umode_t mode),
	       (dir, dentry, mode))
NKSU_SYMBOL_FN(int, vfs_unlink,
	       (struct inode * dir, struct dentry *dentry, struct inode **delegated_inode),
	       (dir, dentry, delegated_inode))
#endif
NKSU_SYMBOL_FN(struct task_struct *, find_task_by_vpid, (pid_t nr), (nr))
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 8, 0)
NKSU_SYMBOL_FN(int, task_work_add,
	       (struct task_struct * task, struct callback_head *work,
		enum task_work_notify_mode notify),
	       (task, work, notify))
#else
NKSU_SYMBOL_FN(int, task_work_add,
	       (struct task_struct * task, struct callback_head *work, bool notify),
	       (task, work, notify))
#endif
NKSU_SYMBOL_VOID_FN(change_pid, (struct task_struct * task, enum pid_type type, struct pid *pid),
		    (task, type, pid))
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 9, 0)
NKSU_SYMBOL_FN(int, path_umount, (struct path * path, int flags), (path, flags))
#else
NKSU_SYMBOL_FN(long, sys_umount, (char __user *name, int flags), (name, flags))
#endif
NKSU_SYMBOL_VOID_FN(free_uid, (struct user_struct * up), (up))
NKSU_SYMBOL_FN(int, commit_creds, (struct cred * new), (new))
NKSU_SYMBOL_VOID_FN(abort_creds, (struct cred * new), (new))
NKSU_SYMBOL_FN(struct group_info *, groups_alloc, (int gidsetsize), (gidsetsize))
NKSU_SYMBOL_VOID_FN(set_groups, (struct cred * new, struct group_info *group_info),
		    (new, group_info))
NKSU_SYMBOL_FN(int, unshare_fs_struct, (void), ())

/*
 * SELinux internals.  These symbols are called by the policy/rule engine in
 * tight loops; keep their indirect calls in the same __nocfi wrappers so the
 * engine files can drop their file-wide CFI opt-outs too.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 7, 0)
NKSU_SYMBOL_FN(void *, symtab_search, (struct symtab * s, const char *name), (s, name))
NKSU_SYMBOL_FN(int, symtab_insert, (struct symtab * s, char *name, void *datum), (s, name, datum))
#endif
NKSU_SYMBOL_FN(struct avtab_node *, avtab_search_node, (struct avtab * h, struct avtab_key *key),
	       (h, key))
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
NKSU_SYMBOL_FN(struct avtab_node *, avtab_search_node_next, (struct avtab_node * node, u16 tclass),
	       (node, tclass))
#else
NKSU_SYMBOL_FN(struct avtab_node *, avtab_search_node_next, (struct avtab_node * node, int tclass),
	       (node, tclass))
#endif
NKSU_SYMBOL_FN(struct avtab_node *, avtab_insert_nonunique,
	       (struct avtab * h, struct avtab_key *key, struct avtab_datum *datum),
	       (h, key, datum))
NKSU_SYMBOL_FN(int, avtab_alloc, (struct avtab * h, u32 nr), (h, nr))
NKSU_SYMBOL_VOID_FN(avtab_destroy, (struct avtab * h), (h))
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 9, 0)
NKSU_SYMBOL_FN(struct filename_trans_datum *, policydb_filenametr_search,
	       (struct policydb * p, struct filename_trans_key *key), (p, key))
#endif
NKSU_SYMBOL_FN(int, policydb_write, (struct policydb * p, void *fp), (p, fp))
NKSU_SYMBOL_FN(int, policydb_read, (struct policydb * p, void *fp), (p, fp))
NKSU_SYMBOL_VOID_FN(policydb_destroy, (struct policydb * p), (p))
NKSU_SYMBOL_FN(int, ebitmap_set_bit, (struct ebitmap * e, unsigned long bit, int value),
	       (e, bit, value))
NKSU_SYMBOL_FN(int, ebitmap_get_bit, (struct ebitmap * e, unsigned long bit), (e, bit))
NKSU_SYMBOL_VOID_FN(ebitmap_destroy, (struct ebitmap * e), (e))
NKSU_SYMBOL_FN(int, ebitmap_cpy, (struct ebitmap * dst, struct ebitmap *src), (dst, src))
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
NKSU_SYMBOL_VOID_FN(avc_ss_reset, (u32 seqno), (seqno))
#else
NKSU_SYMBOL_VOID_FN(avc_ss_reset, (void *avc, u32 seqno), (avc, seqno))
#endif
NKSU_SYMBOL_VOID_FN(selnl_notify_policyload, (u32 seqno), (seqno))
/*
 * selinux_status_update_policyload() kept a struct selinux_state argument
 * until 6.4 dropped it with the other helpers' state argument; the seqno
 * parameter became u32 in 6.6.  Match the running kernel's declaration so the
 * __nocfi wrapper is called with an identical type under CFI.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 4, 0)
NKSU_SYMBOL_VOID_FN(selinux_status_update_policyload, (struct selinux_state * state, int seqno),
		    (state, seqno))
#elif LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
NKSU_SYMBOL_VOID_FN(selinux_status_update_policyload, (int seqno), (seqno))
#else
NKSU_SYMBOL_VOID_FN(selinux_status_update_policyload, (u32 seqno), (seqno))
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
NKSU_SYMBOL_FN(int, security_context_to_sid,
	       (const char *scontext, u32 scontext_len, u32 *sid, gfp_t gfp_flags),
	       (scontext, scontext_len, sid, gfp_flags))
NKSU_SYMBOL_FN(int, avc_has_perm,
	       (u32 ssid, u32 tsid, u16 tclass, u32 requested, struct common_audit_data *auditdata),
	       (ssid, tsid, tclass, requested, auditdata))
#else
NKSU_SYMBOL_FN(int, security_context_to_sid,
	       (struct selinux_state * state, const char *scontext, u32 scontext_len, u32 *sid,
		gfp_t gfp_flags),
	       (state, scontext, scontext_len, sid, gfp_flags))
NKSU_SYMBOL_FN(int, security_sid_to_context,
	       (struct selinux_state * state, u32 sid, char **scontext, u32 *scontext_len),
	       (state, sid, scontext, scontext_len))
NKSU_SYMBOL_VOID_FN(security_compute_av_user,
		    (struct selinux_state * state, u32 ssid, u32 tsid, u16 tclass,
		     struct av_decision *avd),
		    (state, ssid, tsid, tclass, avd))
NKSU_SYMBOL_FN(int, avc_has_perm,
	       (struct selinux_state * state, u32 ssid, u32 tsid, u16 tclass, u32 requested,
		struct common_audit_data *auditdata),
	       (state, ssid, tsid, tclass, requested, auditdata))
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
NKSU_SYMBOL_FN(int, __vfs_setxattr_noperm,
	       (struct mnt_idmap * idmap, struct dentry *dentry, const char *name,
		const void *value, size_t size, int flags),
	       (idmap, dentry, name, value, size, flags))
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 12, 0)
NKSU_SYMBOL_FN(int, __vfs_setxattr_noperm,
	       (struct user_namespace * mnt_userns, struct dentry *dentry, const char *name,
		const void *value, size_t size, int flags),
	       (mnt_userns, dentry, name, value, size, flags))
#else
NKSU_SYMBOL_FN(int, __vfs_setxattr_noperm,
	       (struct dentry * dentry, const char *name, const void *value, size_t size,
		int flags),
	       (dentry, name, value, size, flags))
#endif
NKSU_SYMBOL_VOID_FN(seccomp_filter_release, (struct task_struct * tsk), (tsk))
NKSU_SYMBOL_FN(struct crypto_akcipher *, crypto_alloc_akcipher,
	       (const char *alg_name, u32 type, u32 mask), (alg_name, type, mask))
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 0, 0)
NKSU_SYMBOL_FN(struct fsnotify_group *, fsnotify_alloc_group,
	       (const struct fsnotify_ops *ops, int flags), (ops, flags))
#else
NKSU_SYMBOL_FN(struct fsnotify_group *, fsnotify_alloc_group, (const struct fsnotify_ops *ops),
	       (ops))
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 9, 0)
NKSU_SYMBOL_VOID_FN(fsnotify_init_mark, (struct fsnotify_mark * mark, struct fsnotify_group *group),
		    (mark, group))
#endif
NKSU_SYMBOL_VOID_FN(fsnotify_put_mark, (struct fsnotify_mark * mark), (mark))
NKSU_SYMBOL_VOID_FN(fsnotify_put_group, (struct fsnotify_group * group), (group))
NKSU_SYMBOL_VOID_FN(fsnotify_destroy_mark,
		    (struct fsnotify_mark * mark, struct fsnotify_group *group), (mark, group))
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 10, 0)
NKSU_SYMBOL_FN(int, fsnotify_add_mark,
	       (struct fsnotify_mark * mark, void *obj, unsigned int obj_type, int add_flags),
	       (mark, obj, obj_type, add_flags))
#else
NKSU_SYMBOL_FN(int, fsnotify_add_mark,
	       (struct fsnotify_mark * mark, struct fsnotify_mark_connector **conn,
		unsigned int obj_type, int add_flags, __kernel_fsid_t *fsid),
	       (mark, conn, obj_type, add_flags, fsid))
#endif
NKSU_SYMBOL_FN(int, access_process_vm,
	       (struct task_struct * tsk, unsigned long addr, void *buf, size_t len,
		unsigned int gup_flags),
	       (tsk, addr, buf, len, gup_flags))
NKSU_SYMBOL_FN(const char *, btf_name_by_offset, (const struct btf *btf, u32 offset), (btf, offset))
NKSU_SYMBOL_FN(s32, btf_find_by_name_kind, (const struct btf *btf, const char *name, u8 kind),
	       (btf, name, kind))
NKSU_SYMBOL_FN(const struct btf_type *, btf_type_by_id, (const struct btf *btf, u32 type_id),
	       (btf, type_id))
NKSU_SYMBOL_FN(const struct btf_type *, btf_type_skip_modifiers,
	       (const struct btf *btf, u32 id, u32 *res_id), (btf, id, res_id))

/* Resolve all symbols into the pointers; returns negative errno if any fail. */
int nksu_symbol_compat_init(void);

/* Clear the pointers (module unload). */
void nksu_symbol_compat_exit(void);

#endif /* NKSU_SYMBOL_COMPAT_DECLS_H */
