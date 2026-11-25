/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NKSU_SYMBOL_COMPAT_H
#define NKSU_SYMBOL_COMPAT_H

/*
 * Redirect the unexported symbols to the pointers resolved by
 * symbol_compat.c.  Function symbols go through their __nocfi wrapper so the
 * indirect call is not CFI-checked; data symbols dereference the pointer
 * directly.  The declarations live in symbol_compat_decls.h.
 *
 * Requirement: include this header *after* every kernel/SELinux header that
 * declares these symbols, otherwise the macros would rewrite the
 * declarations. Each .c file therefore puts it last in its include block.
 * symbol_compat.c instead includes symbol_compat_decls.h directly, so it can
 * keep the real names while defining and resolving the pointers.
 */
#include "symbol_compat_decls.h"

#define alloc_uid nksu_alloc_uid_fn
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 14, 0)
#define set_cred_ucounts nksu_set_cred_ucounts_fn
#endif
#define switch_task_namespaces nksu_switch_task_namespaces_fn
#define avc_ss_reset nksu_avc_ss_reset_fn
#define selnl_notify_policyload nksu_selnl_notify_policyload_fn
#define selinux_status_update_policyload nksu_selinux_status_update_policyload_fn
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
#define vfs_mkdir nksu_vfs_mkdir_fn
#define vfs_unlink nksu_vfs_unlink_fn
#define lookup_one_len nksu_lookup_one_len_fn
#define iterate_dir nksu_iterate_dir_fn
#define __vfs_setxattr_noperm nksu___vfs_setxattr_noperm_fn
#define task_work_add nksu_task_work_add_fn
#define change_pid nksu_change_pid_fn
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 9, 0)
#define path_umount nksu_path_umount_fn
#else
#define sys_umount nksu_sys_umount_fn
#endif
#define filp_open nksu_filp_open_fn
#define shmem_file_setup nksu_shmem_file_setup_fn
#define kernel_read nksu_kernel_read
#define kernel_write nksu_kernel_write
#define find_task_by_vpid nksu_find_task_by_vpid_fn
#define free_uid nksu_free_uid_fn
#define access_process_vm nksu_access_process_vm_fn
#define tasklist_lock (*nksu_tasklist_lock)
#define commit_creds nksu_commit_creds_fn
#define abort_creds nksu_abort_creds_fn
#define groups_alloc nksu_groups_alloc_fn
#define set_groups nksu_set_groups_fn
#define unshare_fs_struct nksu_unshare_fs_struct_fn
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 13, 0)
#define wait_for_initramfs nksu_wait_for_initramfs
#endif
#define fsnotify_alloc_group nksu_fsnotify_alloc_group_fn
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 9, 0)
#define fsnotify_init_mark nksu_fsnotify_init_mark_fn
#endif
#define fsnotify_put_mark nksu_fsnotify_put_mark_fn
#define fsnotify_put_group nksu_fsnotify_put_group_fn
#define fsnotify_destroy_mark nksu_fsnotify_destroy_mark_fn
#define fsnotify_add_mark nksu_fsnotify_add_mark_fn
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
#define seccomp_filter_release nksu_seccomp_filter_release_fn
#define crypto_alloc_akcipher nksu_crypto_alloc_akcipher_fn
#define btf_name_by_offset nksu_btf_name_by_offset_fn
#define btf_find_by_name_kind nksu_btf_find_by_name_kind_fn
#define btf_type_by_id nksu_btf_type_by_id_fn
#define btf_type_skip_modifiers nksu_btf_type_skip_modifiers_fn

#endif /* NKSU_SYMBOL_COMPAT_H */
