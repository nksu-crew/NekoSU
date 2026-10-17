// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * NekoSU kernel interface constants.
 *
 * Upstream ksud generates these bindings from KernelSU's uapi/ksu.h.  NekoSU
 * exposes a different (multiplexed ioctl) interface, so this module only keeps
 * the small set of names and layouts that the rest of ksud uses.  The actual
 * ioctl encoding lives in ksucalls.rs.
 */
#![allow(nonstandard_style, dead_code)]

pub const KERNEL_SU_UAPI_VERSION: u32 = 5;

pub const KSU_GET_INFO_FLAG_LKM: u32 = 1 << 0;
pub const KSU_GET_INFO_FLAG_LATE_LOAD: u32 = 1 << 1;
pub const KSU_GET_INFO_FLAG_PR_BUILD: u32 = 1 << 2;
pub const KSU_GET_INFO_FLAG_BUNDLED: u32 = 1 << 3;
pub const KSU_GET_INFO_FLAG_PREINIT: u32 = 1 << 4;

pub const KSU_FEATURE_FLAG_SUPPORTED: u32 = 1 << 0;
pub const KSU_FEATURE_FLAG_FORCED: u32 = 1 << 1;

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct ksu_get_info_cmd {
    pub version: u32,
    pub flags: u32,
    pub features: u32,
    pub uapi_version: u32,
}

pub const EVENT_POST_FS_DATA: u32 = 1;
pub const EVENT_BOOT_COMPLETED: u32 = 2;
pub const EVENT_MODULE_MOUNTED: u32 = 3;
pub const EVENT_SERVICES: u32 = 4;

// SELinux batch command ids.  NekoSU uses the same wire format as KernelSU.
pub const KSU_SEPOLICY_CMD_NORMAL_PERM: u32 = 1;
pub const KSU_SEPOLICY_CMD_XPERM: u32 = 2;
pub const KSU_SEPOLICY_CMD_TYPE_STATE: u32 = 3;
pub const KSU_SEPOLICY_CMD_TYPE: u32 = 4;
pub const KSU_SEPOLICY_CMD_TYPE_ATTR: u32 = 5;
pub const KSU_SEPOLICY_CMD_ATTR: u32 = 6;
pub const KSU_SEPOLICY_CMD_TYPE_TRANSITION: u32 = 7;
pub const KSU_SEPOLICY_CMD_TYPE_CHANGE: u32 = 8;
pub const KSU_SEPOLICY_CMD_GENFSCON: u32 = 9;

pub const KSU_SEPOLICY_SUBCMD_NORMAL_PERM_ALLOW: u32 = 1;
pub const KSU_SEPOLICY_SUBCMD_NORMAL_PERM_DENY: u32 = 2;
pub const KSU_SEPOLICY_SUBCMD_NORMAL_PERM_AUDITALLOW: u32 = 3;
pub const KSU_SEPOLICY_SUBCMD_NORMAL_PERM_DONTAUDIT: u32 = 4;

pub const KSU_SEPOLICY_SUBCMD_XPERM_ALLOW: u32 = 1;
pub const KSU_SEPOLICY_SUBCMD_XPERM_AUDITALLOW: u32 = 2;
pub const KSU_SEPOLICY_SUBCMD_XPERM_DONTAUDIT: u32 = 3;

pub const KSU_SEPOLICY_SUBCMD_TYPE_STATE_PERMISSIVE: u32 = 1;
pub const KSU_SEPOLICY_SUBCMD_TYPE_STATE_ENFORCE: u32 = 2;

pub const KSU_SEPOLICY_SUBCMD_TYPE_CHANGE_CHANGE: u32 = 1;
pub const KSU_SEPOLICY_SUBCMD_TYPE_CHANGE_MEMBER: u32 = 2;
