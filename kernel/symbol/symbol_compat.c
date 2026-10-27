// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Implementation of the unexported-symbol indirection layer.
 *
 * Defines all symbol pointers and fills them with real addresses at runtime
 * via nksu_ksym_lookup(). See symbol_compat.h.
 */

/* Use the real types to declare the pointers so macros cannot rewrite them */
#define NKSU_SYMBOL_COMPAT_NO_MACROS
#include "klog.h"
#include <linux/errno.h>
#include <fmac.h>
#include "symbol/symbol_compat.h"

/* definitions */
typeof(alloc_uid) *nksu_alloc_uid;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 14, 0)
typeof(set_cred_ucounts) *nksu_set_cred_ucounts;
#endif
typeof(switch_task_namespaces) *nksu_switch_task_namespaces;
typeof(avc_ss_reset) *nksu_avc_ss_reset;
typeof(selnl_notify_policyload) *nksu_selnl_notify_policyload;
typeof(selinux_status_update_policyload) *nksu_selinux_status_update_policyload;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 7, 0)
typeof(symtab_search) *nksu_symtab_search;
typeof(symtab_insert) *nksu_symtab_insert;
#else
typeof(hashtab_search) *nksu_hashtab_search;
typeof(hashtab_insert) *nksu_hashtab_insert;
#endif
typeof(avtab_search_node) *nksu_avtab_search_node;
typeof(avtab_search_node_next) *nksu_avtab_search_node_next;
typeof(avtab_insert_nonunique) *nksu_avtab_insert_nonunique;
typeof(avtab_alloc) *nksu_avtab_alloc;
typeof(avtab_destroy) *nksu_avtab_destroy;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 9, 0)
typeof(policydb_filenametr_search) *nksu_policydb_filenametr_search;
#endif
typeof(policydb_write) *nksu_policydb_write;
typeof(policydb_read) *nksu_policydb_read;
typeof(policydb_destroy) *nksu_policydb_destroy;
typeof(ebitmap_set_bit) *nksu_ebitmap_set_bit;
typeof(ebitmap_get_bit) *nksu_ebitmap_get_bit;
typeof(ebitmap_destroy) *nksu_ebitmap_destroy;
typeof(ebitmap_cpy) *nksu_ebitmap_cpy;
typeof(hashtab_destroy) *nksu_hashtab_destroy;
typeof(hashtab_map) *nksu_hashtab_map;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 9, 0)
typeof(__hashtab_insert) *nksu___hashtab_insert;
#endif
typeof(security_context_to_sid) *nksu_security_context_to_sid;
typeof(avc_has_perm) *nksu_avc_has_perm;
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
typeof(security_sid_to_context) *nksu_security_sid_to_context;
typeof(security_context_str_to_sid) *nksu_security_context_str_to_sid;
typeof(security_compute_av_user) *nksu_security_compute_av_user;
#endif
typeof(vfs_mkdir) *nksu_vfs_mkdir;
typeof(vfs_unlink) *nksu_vfs_unlink;
typeof(lookup_one_len) *nksu_lookup_one_len;
typeof(__vfs_setxattr_noperm) *nksu___vfs_setxattr_noperm;
typeof(task_work_add) *nksu_task_work_add;
typeof(change_pid) *nksu_change_pid;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 9, 0)
typeof(path_umount) *nksu_path_umount;
#else
typeof(sys_umount) *nksu_sys_umount;
#endif

typeof(filp_open) *nksu_filp_open;
typeof(kernel_read) *nksu_kernel_read;
typeof(kernel_write) *nksu_kernel_write;

typeof(find_task_by_vpid) *nksu_find_task_by_vpid;
typeof(free_uid) *nksu_free_uid;
typeof(access_process_vm) *nksu_access_process_vm;
typeof(tasklist_lock) *nksu_tasklist_lock;

typeof(commit_creds) *nksu_commit_creds;
typeof(abort_creds) *nksu_abort_creds;
typeof(groups_alloc) *nksu_groups_alloc;
typeof(set_groups) *nksu_set_groups;
typeof(unshare_fs_struct) *nksu_unshare_fs_struct;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 13, 0)
typeof(wait_for_initramfs) *nksu_wait_for_initramfs;
#endif

typeof(fsnotify_alloc_group) *nksu_fsnotify_alloc_group;
typeof(fsnotify_init_mark) *nksu_fsnotify_init_mark;
typeof(fsnotify_put_mark) *nksu_fsnotify_put_mark;
typeof(fsnotify_put_group) *nksu_fsnotify_put_group;
typeof(fsnotify_destroy_mark) *nksu_fsnotify_destroy_mark;
typeof(fsnotify_add_mark) *nksu_fsnotify_add_mark;

typeof(kernel_thread) *nksu_kernel_thread;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0)
typeof(kernel_execve) *nksu_kernel_execve;
typeof(kernel_wait) *nksu_kernel_wait;
#else
typeof(do_execve) *nksu_do_execve;
typeof(getname_kernel) *nksu_getname_kernel;
typeof(sys_wait4) *nksu_sys_wait4;
#endif
typeof(flush_signal_handlers) *nksu_flush_signal_handlers;

typeof(selinux_state) *nksu_selinux_state;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 1, 0)
typeof(selinux_blob_sizes) *nksu_selinux_blob_sizes;
#endif
typeof(init_nsproxy) *nksu_init_nsproxy;
typeof(__set_fixmap) *nksu___set_fixmap;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 8, 0)
typeof(copy_to_kernel_nofault) *nksu_copy_to_kernel_nofault;
#else
typeof(probe_kernel_write) *nksu_probe_kernel_write;
#endif

#define NKSU_RESOLVE(ptr, sym)                                   \
	do {                                                     \
		(ptr) = (typeof(ptr))nksu_ksym_lookup(sym);      \
		if (!(ptr)) {                                    \
			pr_err("[ksym] unresolved %s\n", (sym)); \
			missing++;                               \
		}                                                \
	} while (0)

/*
 * fsnotify_add_inode_mark() is a header inline forwarding to fsnotify_add_mark(),
 * so the call is written out here to keep the relocation out of the module.
 * 6.10 replaced the mark-connector argument with the watched object and dropped
 * the trailing fsid out-argument:
 *
 *   <= 6.9  fsnotify_add_mark(mark, &inode->i_fsnotify_marks, type, flags, NULL)
 *   >= 6.10 fsnotify_add_mark(mark, inode, type, flags)
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 9, 0)
int __nocfi nksu_fsnotify_add_inode_mark(struct fsnotify_mark *mark, struct inode *inode, int add_flags)
{
	if (!nksu_fsnotify_add_mark)
		return -ENOSYS;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 10, 0)
	return nksu_fsnotify_add_mark(mark, inode, FSNOTIFY_OBJ_TYPE_INODE, add_flags);
#else
	return nksu_fsnotify_add_mark(mark, &inode->i_fsnotify_marks, FSNOTIFY_OBJ_TYPE_INODE,
				      add_flags, NULL);
#endif
}
#endif

int nksu_symbol_compat_init(void)
{
	int missing = 0;

	/*
	 * The file primitives are resolved alongside the rest; the module's
	 * state files and SELinux relabel paths reach them through this layer.
	 */
	NKSU_RESOLVE(nksu_filp_open, "filp_open");
	NKSU_RESOLVE(nksu_kernel_read, "kernel_read");
	NKSU_RESOLVE(nksu_kernel_write, "kernel_write");
	NKSU_RESOLVE(nksu_find_task_by_vpid, "find_task_by_vpid");
	NKSU_RESOLVE(nksu_free_uid, "free_uid");
	NKSU_RESOLVE(nksu_access_process_vm, "access_process_vm");
	NKSU_RESOLVE(nksu_tasklist_lock, "tasklist_lock");

	NKSU_RESOLVE(nksu_alloc_uid, "alloc_uid");
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 14, 0)
	NKSU_RESOLVE(nksu_set_cred_ucounts, "set_cred_ucounts");
#endif
	NKSU_RESOLVE(nksu_switch_task_namespaces, "switch_task_namespaces");

	NKSU_RESOLVE(nksu_avc_ss_reset, "avc_ss_reset");
	NKSU_RESOLVE(nksu_selnl_notify_policyload, "selnl_notify_policyload");
	NKSU_RESOLVE(nksu_selinux_status_update_policyload, "selinux_status_update_policyload");
	NKSU_RESOLVE(nksu_security_context_to_sid, "security_context_to_sid");
	NKSU_RESOLVE(nksu_avc_has_perm, "avc_has_perm");
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
	NKSU_RESOLVE(nksu_security_sid_to_context, "security_sid_to_context");
	NKSU_RESOLVE(nksu_security_context_str_to_sid, "security_context_str_to_sid");
	NKSU_RESOLVE(nksu_security_compute_av_user, "security_compute_av_user");
#endif
	NKSU_RESOLVE(nksu_vfs_mkdir, "vfs_mkdir");
	NKSU_RESOLVE(nksu_vfs_unlink, "vfs_unlink");
	NKSU_RESOLVE(nksu_lookup_one_len, "lookup_one_len");
	NKSU_RESOLVE(nksu___vfs_setxattr_noperm, "__vfs_setxattr_noperm");
	NKSU_RESOLVE(nksu_task_work_add, "task_work_add");
	NKSU_RESOLVE(nksu_change_pid, "change_pid");
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 9, 0)
	NKSU_RESOLVE(nksu_path_umount, "path_umount");
#else
	NKSU_RESOLVE(nksu_sys_umount, "sys_umount");
#endif
	NKSU_RESOLVE(nksu_commit_creds, "commit_creds");
	NKSU_RESOLVE(nksu_abort_creds, "abort_creds");
	NKSU_RESOLVE(nksu_groups_alloc, "groups_alloc");
	NKSU_RESOLVE(nksu_set_groups, "set_groups");
	NKSU_RESOLVE(nksu_unshare_fs_struct, "unshare_fs_struct");
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 13, 0)
	NKSU_RESOLVE(nksu_wait_for_initramfs, "wait_for_initramfs");
#endif
	NKSU_RESOLVE(nksu_fsnotify_alloc_group, "fsnotify_alloc_group");
	NKSU_RESOLVE(nksu_fsnotify_init_mark, "fsnotify_init_mark");
	NKSU_RESOLVE(nksu_fsnotify_put_mark, "fsnotify_put_mark");
	NKSU_RESOLVE(nksu_fsnotify_put_group, "fsnotify_put_group");
	NKSU_RESOLVE(nksu_fsnotify_destroy_mark, "fsnotify_destroy_mark");
	NKSU_RESOLVE(nksu_fsnotify_add_mark, "fsnotify_add_mark");
	NKSU_RESOLVE(nksu_selinux_state, "selinux_state");
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 1, 0)
	NKSU_RESOLVE(nksu_selinux_blob_sizes, "selinux_blob_sizes");
#endif
	NKSU_RESOLVE(nksu_init_nsproxy, "init_nsproxy");

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 7, 0)
	NKSU_RESOLVE(nksu_symtab_search, "symtab_search");
	NKSU_RESOLVE(nksu_symtab_insert, "symtab_insert");
#else
	NKSU_RESOLVE(nksu_hashtab_search, "hashtab_search");
	NKSU_RESOLVE(nksu_hashtab_insert, "hashtab_insert");
#endif
	NKSU_RESOLVE(nksu_avtab_search_node, "avtab_search_node");
	NKSU_RESOLVE(nksu_avtab_search_node_next, "avtab_search_node_next");
	NKSU_RESOLVE(nksu_avtab_insert_nonunique, "avtab_insert_nonunique");
	NKSU_RESOLVE(nksu_avtab_alloc, "avtab_alloc");
	NKSU_RESOLVE(nksu_avtab_destroy, "avtab_destroy");
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 9, 0)
	NKSU_RESOLVE(nksu_policydb_filenametr_search, "policydb_filenametr_search");
#endif
	NKSU_RESOLVE(nksu_policydb_write, "policydb_write");
	NKSU_RESOLVE(nksu_policydb_read, "policydb_read");
	NKSU_RESOLVE(nksu_policydb_destroy, "policydb_destroy");
	NKSU_RESOLVE(nksu_ebitmap_set_bit, "ebitmap_set_bit");
	NKSU_RESOLVE(nksu_ebitmap_get_bit, "ebitmap_get_bit");
	NKSU_RESOLVE(nksu_ebitmap_destroy, "ebitmap_destroy");
	NKSU_RESOLVE(nksu_ebitmap_cpy, "ebitmap_cpy");
	NKSU_RESOLVE(nksu_hashtab_destroy, "hashtab_destroy");
	NKSU_RESOLVE(nksu_hashtab_map, "hashtab_map");
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 9, 0)
	NKSU_RESOLVE(nksu___hashtab_insert, "__hashtab_insert");
#endif

	/* kernel/spawn/spawn.c primitives -- no direct relocation against these */
	NKSU_RESOLVE(nksu_kernel_thread, "kernel_thread");
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0)
	NKSU_RESOLVE(nksu_kernel_execve, "kernel_execve");
	NKSU_RESOLVE(nksu_kernel_wait, "kernel_wait");
#else
	NKSU_RESOLVE(nksu_do_execve, "do_execve");
	NKSU_RESOLVE(nksu_getname_kernel, "getname_kernel");
	NKSU_RESOLVE(nksu_sys_wait4, "sys_wait4");
#endif
	NKSU_RESOLVE(nksu_flush_signal_handlers, "flush_signal_handlers");
	NKSU_RESOLVE(nksu___set_fixmap, "__set_fixmap");
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 8, 0)
	NKSU_RESOLVE(nksu_copy_to_kernel_nofault, "copy_to_kernel_nofault");
#else
	NKSU_RESOLVE(nksu_probe_kernel_write, "probe_kernel_write");
#endif

	if (missing) {
		pr_err("[ksym] %d unexported symbol(s) unresolved, cache=%lu\n", missing,
		       nksu_ksym_count());
		return -ENOENT;
	}

	pr_info("[ksym] all unexported symbols resolved, cache=%lu\n", nksu_ksym_count());
	return 0;
}

void nksu_symbol_compat_exit(void)
{
	nksu_alloc_uid = NULL;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 14, 0)
	nksu_set_cred_ucounts = NULL;
#endif
	nksu_switch_task_namespaces = NULL;
	nksu_avc_ss_reset = NULL;
	nksu_selnl_notify_policyload = NULL;
	nksu_selinux_status_update_policyload = NULL;
	nksu_security_context_to_sid = NULL;
	nksu_avc_has_perm = NULL;
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
	nksu_security_sid_to_context = NULL;
	nksu_security_context_str_to_sid = NULL;
	nksu_security_compute_av_user = NULL;
#endif
	nksu_vfs_mkdir = NULL;
	nksu_vfs_unlink = NULL;
	nksu_lookup_one_len = NULL;
	nksu___vfs_setxattr_noperm = NULL;
	nksu_task_work_add = NULL;
	nksu_change_pid = NULL;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 9, 0)
	nksu_path_umount = NULL;
#else
	nksu_sys_umount = NULL;
#endif
	nksu_filp_open = NULL;
	nksu_kernel_read = NULL;
	nksu_kernel_write = NULL;
	nksu_find_task_by_vpid = NULL;
	nksu_free_uid = NULL;
	nksu_access_process_vm = NULL;
	nksu_tasklist_lock = NULL;
	nksu_commit_creds = NULL;
	nksu_abort_creds = NULL;
	nksu_groups_alloc = NULL;
	nksu_set_groups = NULL;
	nksu_unshare_fs_struct = NULL;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 13, 0)
	nksu_wait_for_initramfs = NULL;
#endif
	nksu_fsnotify_alloc_group = NULL;
	nksu_fsnotify_init_mark = NULL;
	nksu_fsnotify_put_mark = NULL;
	nksu_fsnotify_put_group = NULL;
	nksu_fsnotify_destroy_mark = NULL;
	nksu_fsnotify_add_mark = NULL;
	nksu_selinux_state = NULL;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 1, 0)
	nksu_selinux_blob_sizes = NULL;
#endif
	nksu_init_nsproxy = NULL;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 7, 0)
	nksu_symtab_search = NULL;
	nksu_symtab_insert = NULL;
#else
	nksu_hashtab_search = NULL;
	nksu_hashtab_insert = NULL;
#endif
	nksu_avtab_search_node = NULL;
	nksu_avtab_search_node_next = NULL;
	nksu_avtab_insert_nonunique = NULL;
	nksu_avtab_alloc = NULL;
	nksu_avtab_destroy = NULL;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 9, 0)
	nksu_policydb_filenametr_search = NULL;
#endif
	nksu_policydb_write = NULL;
	nksu_policydb_read = NULL;
	nksu_policydb_destroy = NULL;
	nksu_ebitmap_set_bit = NULL;
	nksu_ebitmap_get_bit = NULL;
	nksu_ebitmap_destroy = NULL;
	nksu_ebitmap_cpy = NULL;
	nksu_hashtab_destroy = NULL;
	nksu_hashtab_map = NULL;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 9, 0)
	nksu___hashtab_insert = NULL;
#endif
	nksu___set_fixmap = NULL;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 8, 0)
	nksu_copy_to_kernel_nofault = NULL;
#else
	nksu_probe_kernel_write = NULL;
#endif
	nksu_kernel_thread = NULL;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0)
	nksu_kernel_execve = NULL;
	nksu_kernel_wait = NULL;
#else
	nksu_do_execve = NULL;
	nksu_getname_kernel = NULL;
	nksu_sys_wait4 = NULL;
#endif
	nksu_flush_signal_handlers = NULL;
}
