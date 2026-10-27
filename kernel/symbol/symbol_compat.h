/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NKSU_SYMBOL_COMPAT_H
#define NKSU_SYMBOL_COMPAT_H

/*
 * Indirection layer for unexported kernel symbols.
 *
 * Problem: these symbols are no longer exported on GKI/KMI kernels. A direct
 * reference makes the module fail to load with "Unknown symbol ... (err -2)".
 *
 * Approach: for each function symbol declare a pointer of the same type, fill
 * it at load time via nksu_ksym_lookup(), and redirect the original name to a
 * __nocfi wrapper. Data symbols redirect to the pointer directly. This removes
 * the relocation so unexported symbols can still be called.
 *
 *   extern typeof(alloc_uid) *nksu_alloc_uid;      // function pointer
 *   NKSU_SYMBOL_FN(...);                           // -> nksu_alloc_uid_fn
 *   #define alloc_uid nksu_alloc_uid_fn            // redirect call sites
 *
 *   extern typeof(selinux_state) *nksu_selinux_state; // data pointer
 *   #define selinux_state (*nksu_selinux_state)      // dereference
 *
 * Requirement: include this header *after* every kernel/SELinux header that
 * declares these symbols, otherwise the macros would rewrite the
 * declarations. Each .c file therefore puts it last in its include block.
 */

#include <linux/types.h>
#include <linux/version.h>
#include <linux/fs.h>
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
 * The wrapper is named nksu_<name>_fn; the macro-redirection block below maps
 * the original name to it.  _params declares the wrapper's parameters and
 * _args forwards them to the resolved pointer.
 */
#define NKSU_SYMBOL_FN(_ret, _name, _params, _args)                  \
	static inline __nocfi _ret nksu_##_name##_fn _params         \
	{                                                             \
		return nksu_##_name _args;                            \
	}

#define NKSU_SYMBOL_VOID_FN(_name, _params, _args)                   \
	static inline __nocfi void nksu_##_name##_fn _params         \
	{                                                             \
		nksu_##_name _args;                                   \
	}

/* function symbols (pointers) */
extern typeof(alloc_uid) *nksu_alloc_uid;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 14, 0)
extern typeof(set_cred_ucounts) *nksu_set_cred_ucounts;
#endif
extern typeof(switch_task_namespaces) *nksu_switch_task_namespaces;
extern typeof(avc_ss_reset) *nksu_avc_ss_reset;
extern typeof(selnl_notify_policyload) *nksu_selnl_notify_policyload;
extern typeof(selinux_status_update_policyload) *nksu_selinux_status_update_policyload;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 7, 0)
extern typeof(symtab_search) *nksu_symtab_search;
extern typeof(symtab_insert) *nksu_symtab_insert;
#else
/*
 * 4.9 keeps struct symtab as a pointer to its hashtab and only ships
 * symtab_init(); the search/insert helpers were added later.  Reach the
 * hashtab primitives instead and provide the same API locally.
 */
extern typeof(hashtab_search) *nksu_hashtab_search;
extern typeof(hashtab_insert) *nksu_hashtab_insert;
#endif
extern typeof(avtab_search_node) *nksu_avtab_search_node;
extern typeof(avtab_search_node_next) *nksu_avtab_search_node_next;
extern typeof(avtab_insert_nonunique) *nksu_avtab_insert_nonunique;
extern typeof(avtab_alloc) *nksu_avtab_alloc;
extern typeof(avtab_destroy) *nksu_avtab_destroy;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 9, 0)
extern typeof(policydb_filenametr_search) *nksu_policydb_filenametr_search;
#endif
extern typeof(policydb_write) *nksu_policydb_write;
extern typeof(policydb_read) *nksu_policydb_read;
extern typeof(policydb_destroy) *nksu_policydb_destroy;
extern typeof(ebitmap_set_bit) *nksu_ebitmap_set_bit;
extern typeof(ebitmap_get_bit) *nksu_ebitmap_get_bit;
extern typeof(ebitmap_destroy) *nksu_ebitmap_destroy;
extern typeof(ebitmap_cpy) *nksu_ebitmap_cpy;
extern typeof(hashtab_destroy) *nksu_hashtab_destroy;
extern typeof(hashtab_map) *nksu_hashtab_map;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 9, 0)
extern typeof(__hashtab_insert) *nksu___hashtab_insert;
#endif
extern typeof(security_context_to_sid) *nksu_security_context_to_sid;
/*
 * Policy queries the SELinux hiding answers out of the clean policy.  None of
 * them is exported.  6.6 dropped the struct selinux_state argument from the
 * helpers and removed some of them outright; hide.c only calls the raw ones
 * below 6.6 (6.6+ goes through its own *_with_policy variants), so only
 * avc_has_perm exists at every version.
 */
extern typeof(avc_has_perm) *nksu_avc_has_perm;
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
extern typeof(security_sid_to_context) *nksu_security_sid_to_context;
extern typeof(security_context_str_to_sid) *nksu_security_context_str_to_sid;
extern typeof(security_compute_av_user) *nksu_security_compute_av_user;
#endif

/* VFS helpers used to bootstrap /data/adb/nksu in the manager scan.  vfs_mkdir
 * and lookup_one_len are not part of every GKI export set, so reach them
 * through kallsyms like the rest. */
extern typeof(vfs_mkdir) *nksu_vfs_mkdir;
extern typeof(vfs_unlink) *nksu_vfs_unlink;
extern typeof(lookup_one_len) *nksu_lookup_one_len;
/* Persist the nksu_file label on the bootstrapped daemon (see selinux.c). */
extern typeof(__vfs_setxattr_noperm) *nksu___vfs_setxattr_noperm;

/*
 * Flush dirty filesystems before rebooting (safe mode) and defer work onto the
 * current task's resume queue (manager event scan). Neither is in every GKI
 * export set, so reach them through kallsyms like the rest.
 */
extern typeof(task_work_add) *nksu_task_work_add;
extern typeof(change_pid) *nksu_change_pid;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 9, 0)
extern int path_umount(struct path *path, int flags);
extern typeof(path_umount) *nksu_path_umount;
#else
/* 4.9 has no path_umount(); manager/umount.c calls sys_umount() instead. */
extern typeof(sys_umount) *nksu_sys_umount;
#endif

/*
 * File primitives for the module's own state files (feature registry, profile
 * store, packages.xml, /sys/kernel/vmcoreinfo) and for the symbol resolver's
 * bootstrap. The target kernel exports these only into its GKI VFS namespace,
 * so a direct reference is invisible to us and the module fails to load.
 */
extern typeof(filp_open) *nksu_filp_open;
extern typeof(kernel_read) *nksu_kernel_read;
extern typeof(kernel_write) *nksu_kernel_write;

/*
 * Process/cred helpers that 4.9 does not export either (used by the manager
 * scan, the daemon marker and credential switching).
 */
extern typeof(find_task_by_vpid) *nksu_find_task_by_vpid;
extern typeof(free_uid) *nksu_free_uid;
extern typeof(access_process_vm) *nksu_access_process_vm;
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

static inline ssize_t __nocfi nksu_kwrite(struct file *file, const void *buf, size_t count, loff_t *pos)
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

/* Credential and namespace primitives: escalation, spawn and SELinux relabel. */
extern typeof(commit_creds) *nksu_commit_creds;
extern typeof(abort_creds) *nksu_abort_creds;
extern typeof(groups_alloc) *nksu_groups_alloc;
extern typeof(set_groups) *nksu_set_groups;
extern typeof(unshare_fs_struct) *nksu_unshare_fs_struct;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 13, 0)
extern typeof(wait_for_initramfs) *nksu_wait_for_initramfs;
#endif

/* fsnotify: the package observer that watches packages.xml (manager.c). */
extern typeof(fsnotify_alloc_group) *nksu_fsnotify_alloc_group;
extern typeof(fsnotify_init_mark) *nksu_fsnotify_init_mark;
extern typeof(fsnotify_put_mark) *nksu_fsnotify_put_mark;
extern typeof(fsnotify_put_group) *nksu_fsnotify_put_group;
extern typeof(fsnotify_destroy_mark) *nksu_fsnotify_destroy_mark;
extern typeof(fsnotify_add_mark) *nksu_fsnotify_add_mark;

/*
 * fsnotify_add_inode_mark() is a header inline that forwards to the unexported
 * fsnotify_add_mark(), so the relocation lands on the call site: attach inode
 * marks through this wrapper instead.  4.9 always goes through the raw
 * fsnotify_add_mark() signature instead (see manager/manager.c).
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 9, 0)
int __nocfi nksu_fsnotify_add_inode_mark(struct fsnotify_mark *mark, struct inode *inode, int add_flags);
#endif

/*
 * spawn primitives (kernel/spawn/spawn.c). None of these is exported on GKI, and they
 * are exactly what a self-contained call_usermodehelper replacement needs:
 * spawn a task, exec a program in it, wait for it, reset its handlers.
 * 4.9 has neither kernel_execve() nor kernel_wait(); the equivalent
 * do_execve(getname_kernel())/sys_wait4() pair is used there instead.
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
/*
 * __nocfi wrappers for the function symbols that used to force a file-wide
 * CFI opt-out at their call sites.  The wrappers are only needed by the
 * modules that call the redirected names, so skip them when the definition
 * file includes this header with NKSU_SYMBOL_COMPAT_NO_MACROS.
 */
#ifndef NKSU_SYMBOL_COMPAT_NO_MACROS
NKSU_SYMBOL_FN(struct user_struct *, alloc_uid, (kuid_t kuid), (kuid))
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 14, 0)
NKSU_SYMBOL_FN(int, set_cred_ucounts, (struct cred *new), (new))
#endif
NKSU_SYMBOL_VOID_FN(switch_task_namespaces, (struct task_struct *p, struct nsproxy *new), (p, new))
NKSU_SYMBOL_FN(struct file *, filp_open, (const char *filename, int flags, umode_t mode),
	       (filename, flags, mode))
NKSU_SYMBOL_FN(struct task_struct *, find_task_by_vpid, (pid_t nr), (nr))
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 8, 0)
NKSU_SYMBOL_FN(int, task_work_add,
	       (struct task_struct *task, struct callback_head *work,
		enum task_work_notify_mode notify),
	       (task, work, notify))
#else
NKSU_SYMBOL_FN(int, task_work_add,
	       (struct task_struct *task, struct callback_head *work, bool notify),
	       (task, work, notify))
#endif
NKSU_SYMBOL_VOID_FN(change_pid, (struct task_struct *task, enum pid_type type, struct pid *pid),
		    (task, type, pid))
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 9, 0)
NKSU_SYMBOL_FN(int, path_umount, (struct path *path, int flags), (path, flags))
#else
NKSU_SYMBOL_FN(long, sys_umount, (char __user *name, int flags), (name, flags))
#endif
NKSU_SYMBOL_VOID_FN(free_uid, (struct user_struct *up), (up))
NKSU_SYMBOL_FN(int, commit_creds, (struct cred *new), (new))
NKSU_SYMBOL_VOID_FN(abort_creds, (struct cred *new), (new))
NKSU_SYMBOL_FN(struct group_info *, groups_alloc, (int gidsetsize), (gidsetsize))
NKSU_SYMBOL_VOID_FN(set_groups, (struct cred *new, struct group_info *group_info),
		    (new, group_info))
NKSU_SYMBOL_FN(int, unshare_fs_struct, (void), ())

/*
 * SELinux internals.  These symbols are called by the policy/rule engine in
 * tight loops; keep their indirect calls in the same __nocfi wrappers so the
 * engine files can drop their file-wide CFI opt-outs too.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 7, 0)
NKSU_SYMBOL_FN(void *, symtab_search, (struct symtab *s, const char *name), (s, name))
NKSU_SYMBOL_FN(int, symtab_insert, (struct symtab *s, char *name, void *datum),
	       (s, name, datum))
#endif
NKSU_SYMBOL_FN(struct avtab_node *, avtab_search_node,
	       (struct avtab *h, struct avtab_key *key), (h, key))
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
NKSU_SYMBOL_FN(struct avtab_node *, avtab_search_node_next, (struct avtab_node *node, u16 tclass),
	       (node, tclass))
#else
NKSU_SYMBOL_FN(struct avtab_node *, avtab_search_node_next, (struct avtab_node *node, int tclass),
	       (node, tclass))
#endif
NKSU_SYMBOL_FN(struct avtab_node *, avtab_insert_nonunique,
	       (struct avtab *h, struct avtab_key *key, struct avtab_datum *datum),
	       (h, key, datum))
NKSU_SYMBOL_FN(int, avtab_alloc, (struct avtab *h, u32 nr), (h, nr))
NKSU_SYMBOL_VOID_FN(avtab_destroy, (struct avtab *h), (h))
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 9, 0)
NKSU_SYMBOL_FN(struct filename_trans_datum *, policydb_filenametr_search,
	       (struct policydb *p, struct filename_trans_key *key), (p, key))
#endif
NKSU_SYMBOL_FN(int, policydb_write, (struct policydb *p, void *fp), (p, fp))
NKSU_SYMBOL_FN(int, policydb_read, (struct policydb *p, void *fp), (p, fp))
NKSU_SYMBOL_VOID_FN(policydb_destroy, (struct policydb *p), (p))
NKSU_SYMBOL_FN(int, ebitmap_set_bit, (struct ebitmap *e, unsigned long bit, int value),
	       (e, bit, value))
NKSU_SYMBOL_FN(int, ebitmap_get_bit, (struct ebitmap *e, unsigned long bit), (e, bit))
NKSU_SYMBOL_VOID_FN(ebitmap_destroy, (struct ebitmap *e), (e))
NKSU_SYMBOL_FN(int, ebitmap_cpy, (struct ebitmap *dst, struct ebitmap *src), (dst, src))
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
NKSU_SYMBOL_VOID_FN(avc_ss_reset, (u32 seqno), (seqno))
#else
NKSU_SYMBOL_VOID_FN(avc_ss_reset, (void *avc, u32 seqno), (avc, seqno))
#endif
NKSU_SYMBOL_VOID_FN(selnl_notify_policyload, (u32 seqno), (seqno))
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
NKSU_SYMBOL_FN(int, security_context_to_sid,
	       (const char *scontext, u32 scontext_len, u32 *sid, gfp_t gfp_flags),
	       (scontext, scontext_len, sid, gfp_flags))
NKSU_SYMBOL_FN(int, avc_has_perm,
	       (u32 ssid, u32 tsid, u16 tclass, u32 requested,
		struct common_audit_data *auditdata),
	       (ssid, tsid, tclass, requested, auditdata))
#else
NKSU_SYMBOL_FN(int, security_context_to_sid,
	       (struct selinux_state *state, const char *scontext, u32 scontext_len, u32 *sid,
		gfp_t gfp_flags),
	       (state, scontext, scontext_len, sid, gfp_flags))
NKSU_SYMBOL_FN(int, security_sid_to_context,
	       (struct selinux_state *state, u32 sid, char **scontext, u32 *scontext_len),
	       (state, sid, scontext, scontext_len))
NKSU_SYMBOL_VOID_FN(security_compute_av_user,
		      (struct selinux_state *state, u32 ssid, u32 tsid, u16 tclass,
		       struct av_decision *avd),
		      (state, ssid, tsid, tclass, avd))
NKSU_SYMBOL_FN(int, avc_has_perm,
	       (struct selinux_state *state, u32 ssid, u32 tsid, u16 tclass, u32 requested,
		struct common_audit_data *auditdata),
	       (state, ssid, tsid, tclass, requested, auditdata))
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
NKSU_SYMBOL_FN(int, __vfs_setxattr_noperm,
	       (struct mnt_idmap *idmap, struct dentry *dentry, const char *name,
		const void *value, size_t size, int flags),
	       (idmap, dentry, name, value, size, flags))
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 12, 0)
NKSU_SYMBOL_FN(int, __vfs_setxattr_noperm,
	       (struct user_namespace *mnt_userns, struct dentry *dentry, const char *name,
		const void *value, size_t size, int flags),
	       (mnt_userns, dentry, name, value, size, flags))
#else
NKSU_SYMBOL_FN(int, __vfs_setxattr_noperm,
	       (struct dentry *dentry, const char *name, const void *value, size_t size,
		int flags),
	       (dentry, name, value, size, flags))
#endif
#endif

/*
 * Macro redirection. symbol_compat.c needs the real types to define the
 * variables and cast, so it defines NKSU_SYMBOL_COMPAT_NO_MACROS before
 * including this header, skipping this block.
 */
#ifndef NKSU_SYMBOL_COMPAT_NO_MACROS
#define alloc_uid nksu_alloc_uid_fn
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 14, 0)
#define set_cred_ucounts nksu_set_cred_ucounts_fn
#endif
#define switch_task_namespaces nksu_switch_task_namespaces_fn
#define avc_ss_reset nksu_avc_ss_reset_fn
#define selnl_notify_policyload nksu_selnl_notify_policyload_fn
#define selinux_status_update_policyload nksu_selinux_status_update_policyload
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 7, 0)
#define symtab_search nksu_symtab_search_fn
#define symtab_insert nksu_symtab_insert_fn
#else
#define symtab_search nksu_symtab_search
#define symtab_insert nksu_symtab_insert
#endif
#define avtab_search_node nksu_avtab_search_node_fn
#define avtab_search_node_next nksu_avtab_search_node_next_fn
#define avtab_insert_nonunique nksu_avtab_insert_nonunique_fn
#define avtab_alloc nksu_avtab_alloc_fn
#define avtab_destroy nksu_avtab_destroy_fn
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 9, 0)
#define policydb_filenametr_search nksu_policydb_filenametr_search_fn
#endif
#define policydb_write nksu_policydb_write_fn
#define policydb_read nksu_policydb_read_fn
#define policydb_destroy nksu_policydb_destroy_fn
#define ebitmap_set_bit nksu_ebitmap_set_bit_fn
#define ebitmap_get_bit nksu_ebitmap_get_bit_fn
#define ebitmap_destroy nksu_ebitmap_destroy_fn
#define ebitmap_cpy nksu_ebitmap_cpy_fn
#define hashtab_destroy nksu_hashtab_destroy
#define hashtab_map nksu_hashtab_map
#define security_context_to_sid nksu_security_context_to_sid_fn
#define avc_has_perm nksu_avc_has_perm_fn
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
#define security_sid_to_context nksu_security_sid_to_context_fn
#define security_context_str_to_sid nksu_security_context_str_to_sid
#define security_compute_av_user nksu_security_compute_av_user_fn
#endif
#define vfs_mkdir nksu_vfs_mkdir
#define vfs_unlink nksu_vfs_unlink
#define lookup_one_len nksu_lookup_one_len
#define __vfs_setxattr_noperm nksu___vfs_setxattr_noperm_fn
#define task_work_add nksu_task_work_add_fn
#define change_pid nksu_change_pid_fn
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 9, 0)
#define path_umount nksu_path_umount_fn
#else
#define sys_umount nksu_sys_umount_fn
#endif
#define filp_open nksu_filp_open_fn
#define kernel_read nksu_kernel_read
#define kernel_write nksu_kernel_write
#define find_task_by_vpid nksu_find_task_by_vpid_fn
#define free_uid nksu_free_uid_fn
#define access_process_vm nksu_access_process_vm
#define tasklist_lock (*nksu_tasklist_lock)
#define commit_creds nksu_commit_creds_fn
#define abort_creds nksu_abort_creds_fn
#define groups_alloc nksu_groups_alloc_fn
#define set_groups nksu_set_groups_fn
#define unshare_fs_struct nksu_unshare_fs_struct_fn
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 13, 0)
#define wait_for_initramfs nksu_wait_for_initramfs
#endif
#define fsnotify_alloc_group nksu_fsnotify_alloc_group
#define fsnotify_init_mark nksu_fsnotify_init_mark
#define fsnotify_put_mark nksu_fsnotify_put_mark
#define fsnotify_put_group nksu_fsnotify_put_group
#define fsnotify_destroy_mark nksu_fsnotify_destroy_mark
#define kernel_thread nksu_kernel_thread
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0)
#define kernel_execve nksu_kernel_execve
#define kernel_wait nksu_kernel_wait
#endif
#define flush_signal_handlers nksu_flush_signal_handlers
#define selinux_state (*nksu_selinux_state)
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 1, 0)
#define selinux_blob_sizes (*nksu_selinux_blob_sizes)
#endif
#define init_nsproxy (*nksu_init_nsproxy)
#define __set_fixmap nksu___set_fixmap
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 8, 0)
#define copy_to_kernel_nofault nksu_copy_to_kernel_nofault
#endif
#endif /* NKSU_SYMBOL_COMPAT_NO_MACROS */

/* Resolve all symbols into the pointers; returns negative errno if any fail. */
int nksu_symbol_compat_init(void);

/* Clear the pointers (module unload). */
void nksu_symbol_compat_exit(void);

#endif /* NKSU_SYMBOL_COMPAT_H */
