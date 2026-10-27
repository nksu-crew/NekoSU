// SPDX-License-Identifier: GPL-3.0-or-later
#include "klog.h"
#include <linux/fs.h>
#include <linux/mount.h>
#include <linux/uaccess.h>
#include <linux/anon_inodes.h>
#include <linux/version.h>
#include <linux/capability.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/pid.h>
#include <linux/sched.h>

#include "fmac.h"
#include "manager/feature.h"
#include "manager/umount.h"
#include "manager/ioctl.h"
#include "nksu.h"
#include "nksu_task.h"
#include "boot/safe_mode.h"
#include "privilege/profile_store.h"
#include "hook/handle.h"
#include "symbol/symbol_compat.h"

/* change_pid is reached through the kallsyms pointer; call it without CFI. */
#if defined(__clang__)
#pragma clang attribute push(__attribute__((no_sanitize("cfi"))), apply_to = function)
#endif

static long ioc_get_shm(void)
{
	return fmac_anonfd_get();
}

static long ioc_bind_evt(const void __user *data, unsigned int size)
{
	int efd;

	if (size < sizeof(efd))
		return -EINVAL;
	if (copy_from_user(&efd, data, sizeof(efd)))
		return -EFAULT;

	return nksu_bind_eventfd(efd);
}

static long ioc_chk_write(void __user *data, unsigned int size)
{
	int changed = check_mmap_write() ? 1 : 0;

	if (size < sizeof(changed))
		return -EINVAL;
	return copy_to_user(data, &changed, sizeof(changed)) ? -EFAULT : 0;
}

static long ioc_add_uid(const void __user *data, unsigned int size)
{
	unsigned int id;
	int ret;

	if (size < sizeof(id))
		return -EINVAL;
	if (copy_from_user(&id, data, sizeof(id)))
		return -EFAULT;

	ret = nksu_profile_set_default((uid_t)id);
	if (!ret)
		nksu_path_sync_all();
	return ret;
}

static long ioc_del_uid(const void __user *data, unsigned int size)
{
	unsigned int id;

	if (size < sizeof(id))
		return -EINVAL;
	if (copy_from_user(&id, data, sizeof(id)))
		return -EFAULT;

	return nksu_profile_clear((uid_t)id);
}

static long ioc_has_uid(void __user *data, unsigned int size)
{
	unsigned int id;

	if (size < sizeof(id))
		return -EINVAL;
	if (copy_from_user(&id, data, sizeof(id)))
		return -EFAULT;

	id = nksu_profile_has_uid((uid_t)id) ? 1 : 0;
	return copy_to_user(data, &id, sizeof(id)) ? -EFAULT : 0;
}

static long ioc_set_cap(const void __user *data, unsigned int size)
{
	unsigned int uid;
	u64 caps;

	if (size < FMAC_DATA_CAP)
		return -EINVAL;
	if (copy_from_user(&uid, data + FMAC_OFF_UID, sizeof(uid)) ||
	    copy_from_user(&caps, data + FMAC_OFF_CAPS, sizeof(caps)))
		return -EFAULT;

	return nksu_profile_set_caps((uid_t)uid, u64_to_cap(caps));
}

static long ioc_get_cap(void __user *data, unsigned int size)
{
	kernel_cap_t caps;
	unsigned int uid;
	u64 caps_u64;

	if (size < FMAC_DATA_CAP)
		return -EINVAL;
	if (copy_from_user(&uid, data + FMAC_OFF_UID, sizeof(uid)))
		return -EFAULT;

	if (nksu_profile_get((uid_t)uid, NULL, &caps, NULL, NULL, 0))
		return -ENOENT;

	caps_u64 = cap_to_u64(caps);
	return copy_to_user(data + FMAC_OFF_CAPS, &caps_u64, sizeof(caps_u64)) ? -EFAULT : 0;
}

static long ioc_del_cap(const void __user *data, unsigned int size)
{
	kernel_cap_t empty = CAP_EMPTY_SET;
	unsigned int uid;

	if (size < FMAC_DATA_CAP)
		return -EINVAL;
	if (copy_from_user(&uid, data + FMAC_OFF_UID, sizeof(uid)))
		return -EFAULT;

	return nksu_profile_set_caps((uid_t)uid, empty);
}

static long ioc_sel_add_rule(const void __user *data, unsigned int size)
{
	char src[64], tgt[64], cls[64], perm[64];
	int effect, invert;

	if (size < FMAC_DATA_SELRULE)
		return -EINVAL;

	if (copy_from_user(src, data + FMAC_OFF_UID, sizeof(src)) ||
	    copy_from_user(tgt, data + FMAC_OFF_TGT, sizeof(tgt)) ||
	    copy_from_user(cls, data + FMAC_OFF_CLS, sizeof(cls)) ||
	    copy_from_user(perm, data + FMAC_OFF_PERM, sizeof(perm)) ||
	    copy_from_user(&effect, data + FMAC_OFF_EFFECT, sizeof(effect)) ||
	    copy_from_user(&invert, data + FMAC_OFF_INVERT, sizeof(invert)))
		return -EFAULT;

	src[63] = '\0';
	tgt[63] = '\0';
	cls[63] = '\0';
	perm[63] = '\0';

	return sepolicy_add_rule(src[0] ? src : NULL, tgt[0] ? tgt : NULL, cls[0] ? cls : NULL,
				 perm[0] ? perm : NULL, effect, (bool)invert);
}

static long ioc_set_profile(const void __user *data, unsigned int size)
{
	unsigned int uid;
	unsigned int root_uid;
	u64 caps;
	char domain[64];
	int namespace;
	int ret;

	if (size < FMAC_DATA_PROFILE)
		return -EINVAL;

	if (copy_from_user(&uid, data + FMAC_OFF_UID, sizeof(uid)) ||
	    copy_from_user(&caps, data + FMAC_OFF_CAPS, sizeof(caps)) ||
	    copy_from_user(domain, data + FMAC_OFF_DOMAIN, sizeof(domain)) ||
	    copy_from_user(&namespace, data + FMAC_OFF_NS, sizeof(namespace)) ||
	    copy_from_user(&root_uid, data + FMAC_OFF_ROOT_UID, sizeof(root_uid)))
		return -EFAULT;

	domain[63] = '\0';

	ret = nksu_profile_set((uid_t)uid, (uid_t)root_uid, u64_to_cap(caps), domain, namespace);
	if (!ret)
		nksu_path_sync_all();
	return ret;
}

/*
 * Serialize the profile table into the caller's buffer.  The manager reads it
 * with JNI instead of keeping its own copy, so the kernel stays the single
 * source of truth.  Returns the byte count, or a negative errno.
 */
static long ioc_get_profiles(void __user *data, unsigned int size)
{
	char *buf;
	int len;

	if (size == 0 || size > NKSU_PROFILE_TEXT_MAX)
		return -EINVAL;

	buf = kvmalloc(size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	len = nksu_profile_to_text(buf, size);
	if (len >= 0 && copy_to_user(data, buf, (size_t)len))
		len = -EFAULT;

	kvfree(buf);
	return len;
}

/* Expose the module's build version so the manager can detect a stale LKM. */
static long ioc_get_version(void __user *data, unsigned int size)
{
	const char *version = nksu_version();
	size_t len = strlen(version) + 1;

	if (!data || size < len)
		return -ENOSPC;

	return copy_to_user(data, version, len) ? -EFAULT : 0;
}

/* Serialize the registered features for the manager's JNI query. */
static long ioc_feature_list(void __user *data, unsigned int size)
{
	char *buf;
	int len;

	if (!data || size == 0 || size > NKSU_FEATURE_TEXT_MAX)
		return -EINVAL;

	buf = kzalloc(size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	len = nksu_feature_render(buf, size - 1);
	if (len < 0) {
		kfree(buf);
		return len;
	}
	buf[size - 1] = '\0';
	if (copy_to_user(data, buf, (size_t)len + 1))
		len = -EFAULT;

	kfree(buf);
	return len;
}

static long ioc_feature_get(void __user *data, unsigned int size)
{
	u32 id;
	u64 value = 0;
	int ret;

	if (!data || size < FMAC_DATA_FEATURE)
		return -EINVAL;
	if (copy_from_user(&id, data, sizeof(id)))
		return -EFAULT;

	ret = nksu_feature_get(id, &value);
	if (ret)
		return ret;

	if (copy_to_user((char __user *)data + FMAC_OFF_FEATURE_VALUE, &value, sizeof(value)))
		return -EFAULT;
	return 0;
}

static long ioc_feature_set(void __user *data, unsigned int size)
{
	u32 id;
	u64 value;

	if (!data || size < FMAC_DATA_FEATURE)
		return -EINVAL;
	if (copy_from_user(&id, data, sizeof(id)) ||
	    copy_from_user(&value, (const char __user *)data + FMAC_OFF_FEATURE_VALUE,
			   sizeof(value)))
		return -EFAULT;

	return nksu_feature_set(id, value);
}

/*
 * Volume-key safe mode (kernel/boot/safe_mode.c).  The flag is reported as the
 * ioctl return value (1/0); a disarm is a side effect of any call.
 */
static long ioc_get_safemode(void)
{
	return nksu_safe_mode_requested() ? 1 : 0;
}

static long ioc_set_init_pgrp(void)
{
	struct task_struct *p;
	struct pid *init_group;
	int err = 0;

	if (!uid_eq(current_euid(), GLOBAL_ROOT_UID))
		return -EPERM;

	write_lock_irq(&tasklist_lock);
	p = current->group_leader;
	init_group = task_pgrp(&init_task);

	if (task_session(p) != task_session(&init_task))
		err = -EPERM;
	else if (task_pgrp(p) != init_group) {
		if (!nksu_change_pid)
			err = -ENOSYS;
		else
			change_pid(p, PIDTYPE_PGID, init_group);
	}

	write_unlock_irq(&tasklist_lock);
	return err;
}

static long ioc_add_try_umount(const void __user *data, unsigned int size)
{
	char path[256];

	if (!data || size == 0 || size > sizeof(path))
		return -EINVAL;
	memset(path, 0, sizeof(path));
	if (copy_from_user(path, data, size))
		return -EFAULT;
	if (!memchr(path, '\0', sizeof(path)))
		return -EINVAL;
	return nksu_umount_add(path, MNT_DETACH);
}

struct nksu_mnt_layout {
	u32 mnt_id_off;
	u32 mnt_off;
};

static void nksu_mark_current_ncore(void)
{
	struct task_struct *t;

	/*
     * The exec probe marks ncore at boot, but a late-loaded module sees an
     * ncore that is already running.  This ioctl is only reachable through
     * ncore's control fd, so it is a trustworthy place to mark the whole
     * thread group.
     */
	rcu_read_lock();
	for_each_thread(current, t) {
		set_tsk_thread_flag(t, TIF_SYSCALL_TRACEPOINT);
		set_tsk_thread_flag(t, NKSU_TIF_NCORE);
	}
	rcu_read_unlock();

	nksu_path_install_task(current);
}

static long ioc_set_mnt_layout(const void __user *data, unsigned int size)
{
	struct nksu_mnt_layout layout;
	int ret;

	if (!uid_eq(current_euid(), GLOBAL_ROOT_UID))
		return -EPERM;
	if (!data || size != sizeof(layout))
		return -EINVAL;
	if (copy_from_user(&layout, data, sizeof(layout)))
		return -EFAULT;

	/*
     * /data is available by the time ncore sends this.  Apply a persisted
     * feature-off before installing the layout so a disabled feature does
     * not leave its mount-id kprobes armed for the whole boot.
     */
	ret = nksu_umount_feature_init();
	if (ret)
		return ret;

	nksu_mark_current_ncore();

	/* Mark-only request: ncore could not read BTF, but mounting still works. */
	if (layout.mnt_id_off == 0 && layout.mnt_off == 0)
		return 0;

	return nksu_mount_id_set_layout(layout.mnt_id_off, layout.mnt_off);
}

static long ioc_disarm_safemode(void)
{
	nksu_safe_mode_disable();
	/*
     * Keep ncore's tracepoint marking alive: ncore still needs the control fd
     * for module installs and runtime sepolicy changes after boot.
     */
	return 0;
}

static long fmac_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct fmac_ioc ioc;
	const void __user *data;

	if (_IOC_TYPE(cmd) != IOC_MAGIC || _IOC_NR(cmd) != IOC_CMD_NR)
		return -ENOTTY;

	if (copy_from_user(&ioc, (void __user *)arg, sizeof(ioc)))
		return -EFAULT;

	data = (const char __user *)arg + sizeof(ioc);

	switch (ioc.flag) {
	case IOC_GET_SHM:
		return ioc_get_shm();
	case IOC_BIND_EVT:
		return ioc_bind_evt(data, ioc.size);
	case IOC_CHK_WRITE:
		return ioc_chk_write((void __user *)data, ioc.size);
	case IOC_ADD_UID:
		return ioc_add_uid(data, ioc.size);
	case IOC_DEL_UID:
		return ioc_del_uid(data, ioc.size);
	case IOC_HAS_UID:
		return ioc_has_uid((void __user *)data, ioc.size);
	case IOC_SET_CAP:
		return ioc_set_cap(data, ioc.size);
	case IOC_GET_CAP:
		return ioc_get_cap((void __user *)data, ioc.size);
	case IOC_DEL_CAP:
		return ioc_del_cap(data, ioc.size);
	case IOC_SEL_ADD_RULE:
		return ioc_sel_add_rule(data, ioc.size);
	case IOC_SET_PROFILE:
		return ioc_set_profile(data, ioc.size);
	case IOC_SET_SEPOLICY:
		return sepolicy_apply_batch(data, ioc.size);
	case IOC_GET_PROFILES:
		return ioc_get_profiles((void __user *)data, ioc.size);
	case IOC_GET_VERSION:
		return ioc_get_version((void __user *)data, ioc.size);
	case IOC_FEATURE_LIST:
		return ioc_feature_list((void __user *)data, ioc.size);
	case IOC_FEATURE_GET:
		return ioc_feature_get((void __user *)data, ioc.size);
	case IOC_FEATURE_SET:
		return ioc_feature_set((void __user *)data, ioc.size);
	case IOC_GET_SAFEMODE:
		return ioc_get_safemode();
	case IOC_DISARM_SAFEMODE:
		return ioc_disarm_safemode();
	case IOC_SET_INIT_PGRP:
		return ioc_set_init_pgrp();
	case IOC_ADD_TRY_UMOUNT:
		return ioc_add_try_umount(data, ioc.size);
	case IOC_WIPE_UMOUNT:
		nksu_umount_wipe();
		return 0;
	case IOC_SET_MNT_LAYOUT:
		return ioc_set_mnt_layout(data, ioc.size);
	default:
		return -ENOTTY;
	}
}

static const struct file_operations fmac_ctl_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = fmac_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = fmac_ioctl,
#endif
};

int fmac_ctlfd_get(void)
{
	return anon_inode_getfd("[fmac_ctl]", &fmac_ctl_fops, NULL, O_RDWR | O_CLOEXEC);
}

#if defined(__clang__)
#pragma clang attribute pop
#endif
