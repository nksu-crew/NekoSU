/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * NekoSU kernel/userspace control interface (UAPI).
 *
 * This header is the single source of truth for:
 *   - the prctl opcodes used to obtain the kernel's control file descriptors,
 *   - the single multiplexed ioctl (IOC_CMD) and its flag set,
 *   - the on-wire payload layout of every flag.
 *
 * It is included by the kernel module (kernel/manager/ioctl.h) and the ncore
 * userspace runtime (neko/ncore); the Compose manager reaches the same ABI
 * through the Rust JNI bridge (neko/jni).  Keep it portable between kernel and userspace:
 * only <linux/ioctl.h> and <linux/types.h> may be used.
 */
#ifndef NKSU_UAPI_IOCTL_H
#define NKSU_UAPI_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

/* static_assert that works in both kernel C11 and userspace C++. */
#ifndef NKSU_STATIC_ASSERT
#if defined(__cplusplus)
#define NKSU_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define NKSU_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#else
#define NKSU_STATIC_ASSERT(cond, msg)
#endif
#endif

/*
 * 统一的控制接口：所有操作通过同一个 IOC_CMD 下发，
 * 请求体为 { flag, size, data[] }，内核按 flag 从 data 中自由解析。
 * 不依赖任何业务结构体传递数据。
 */
#define IOC_MAGIC 'F'
#define IOC_CMD_NR 0
#define IOC_CMD _IOWR(IOC_MAGIC, IOC_CMD_NR, struct fmac_ioc)

/* 操作类型 */
enum fmac_flag {
	IOC_GET_SHM = 0, /* data 为空，返回 shm fd */
	IOC_BIND_EVT, /* data[4]   = int efd */
	IOC_CHK_WRITE, /* data[4]   = int changed (out) */
	IOC_ADD_UID, /* data[4]   = uint32_t uid */
	IOC_DEL_UID, /* data[4]   = uint32_t uid */
	IOC_HAS_UID, /* data[4]   = uint32_t uid (in/out: 0 或 1) */
	IOC_SET_CAP, /* data[12]  = uint32_t uid | uint64_t caps */
	IOC_GET_CAP, /* data[12]  = uint32_t uid (in), uint64_t caps (out) */
	IOC_DEL_CAP, /* data[12]  = uint32_t uid */
	IOC_SEL_ADD_RULE, /* data[264] = src[64] tgt[64] cls[64] perm[64] effect[4] invert[4] */
	IOC_SET_PROFILE, /* data[84]  = uid[4] caps[8] domain[64] namespace[4] root_uid[4] */
	/* 11 was IOC_LIST_MODULES; module enumeration now lives in ncore (userspace). */
	IOC_SET_SEPOLICY = 12, /* data = KernelSU-format sepolicy batch, size = 长度 */
	/*
     * data[<= NKSU_PROFILE_TEXT_MAX] (out) = the profile table as text, one
     * `<uid> <caps_hex> <ns> <domain>` line per entry.  Returns the number of
     * bytes written, or -ENOSPC when the buffer is too small.  Backs the
     * manager's JNI listProfiles() so it never has to persist the profiles.
     */
	IOC_GET_PROFILES = 13,
	/*
     * data[<= 64] (out) = the kernel module's version string (NKSU_VERSION),
     * NUL-terminated.  The manager compares it with its own copy so it never
     * runs a userspace ncore that does not match the flashed LKM.
     */
	IOC_GET_VERSION = 14,
	/*
     * Kernel feature toggles (see kernel/manager/feature.h).  LIST serialises
     * the registered features as "<id> <name> <value>\n" lines; GET/SET act on
     * a single feature id.  Backs the manager's JNI feature query/toggle.
     */
	IOC_FEATURE_LIST = 15, /* data[<= NKSU_FEATURE_TEXT_MAX] out = feature table */
	IOC_FEATURE_GET = 16, /* data[12] = uint32_t id (in) | uint64_t value (out) */
	IOC_FEATURE_SET = 17, /* data[12] = uint32_t id | uint64_t value */
	/*
     * Volume-key safe mode (see kernel/boot/safe_mode.c).  GET reports whether
     * the watcher latched a request (return 1/0, data empty); DISARM stops the
     * watcher (data empty, return 0).  Root callers only.
     */
	IOC_GET_SAFEMODE = 18, /* data 为空，返回 1=已请求安全模式 / 0=未请求 */
	IOC_DISARM_SAFEMODE = 19, /* data 为空，关闭安全模式观察器 */
	IOC_SET_INIT_PGRP = 20, /* data 为空，把调用者进程组切到 init（root） */
	IOC_ADD_TRY_UMOUNT = 21, /* data = 挂载点路径（含 NUL），flags 固定 MNT_DETACH */
	IOC_WIPE_UMOUNT = 22, /* data 为空，清空 umount 列表 */
	/*
     * data[8] = uint32_t mnt_id_off | uint32_t mnt_off.  ncore reads the
     * offsets from /sys/kernel/btf/vmlinux and installs them before running
     * the first module mount script.
     */
	IOC_SET_MNT_LAYOUT = 23,
};

#define NKSU_PROFILE_TEXT_MAX (64 * 1024)
#define NKSU_FEATURE_TEXT_MAX 1024

struct fmac_ioc {
	__u32 flag;
	__u32 size;
	__u8 data[];
};

/* 各 flag 的 data 布局 */
#define FMAC_DATA_UID 4
#define FMAC_DATA_EFD 4
#define FMAC_DATA_CHKWRITE 4
#define FMAC_DATA_CAP 12
#define FMAC_DATA_SELRULE 264
#define FMAC_DATA_PROFILE 84
#define FMAC_DATA_FEATURE 12

/*
 * prctl opcodes.  201 and 203 are gated to the manager (is_manager-gated).
 * ncore does not use a prctl: the sched_process_exec tracepoint marks it and
 * attaches the control fd through task_work (see kernel/manager/daemon.c).
 */
#define NKSU_PRCTL_GET_SHM_FD 201
#define NKSU_PRCTL_GET_CTL_FD 203

#define FMAC_OFF_UID 0
#define FMAC_OFF_CAPS 4
#define FMAC_OFF_DOMAIN 12
#define FMAC_OFF_NS 76
#define FMAC_OFF_ROOT_UID 80
#define FMAC_OFF_TGT 64
#define FMAC_OFF_CLS 128
#define FMAC_OFF_PERM 192
#define FMAC_OFF_EFFECT 256
#define FMAC_OFF_INVERT 260
#define FMAC_OFF_FEATURE_VALUE 4

/*
 * The flag payloads are a wire contract shared between kernel and userspace,
 * and the offsets are hand-written, so pin them against the element sizes at
 * compile time.  Any drift here changes the on-wire ABI.
 */
NKSU_STATIC_ASSERT(sizeof(struct fmac_ioc) == 2 * sizeof(__u32),
		   "fmac_ioc header must be two u32s");
NKSU_STATIC_ASSERT(FMAC_DATA_UID == sizeof(__u32), "IOC uid is a u32");
NKSU_STATIC_ASSERT(FMAC_DATA_EFD == sizeof(int), "IOC eventfd is an int");
NKSU_STATIC_ASSERT(FMAC_DATA_CHKWRITE == sizeof(int), "IOC chkwrite is an int");
NKSU_STATIC_ASSERT(FMAC_OFF_CAPS == FMAC_OFF_UID + FMAC_DATA_UID, "uid/caps layout");
NKSU_STATIC_ASSERT(FMAC_DATA_CAP == FMAC_OFF_CAPS + sizeof(__u64), "cap payload size");
NKSU_STATIC_ASSERT(FMAC_OFF_DOMAIN == FMAC_OFF_CAPS + sizeof(__u64), "caps/domain layout");
NKSU_STATIC_ASSERT(FMAC_OFF_NS == FMAC_OFF_DOMAIN + 64, "domain/ns layout");
NKSU_STATIC_ASSERT(FMAC_OFF_ROOT_UID == FMAC_OFF_NS + sizeof(int), "ns/root_uid layout");
NKSU_STATIC_ASSERT(FMAC_DATA_PROFILE == FMAC_OFF_ROOT_UID + sizeof(__u32), "profile payload size");
NKSU_STATIC_ASSERT(FMAC_OFF_TGT == FMAC_OFF_UID + 64, "rule src/tgt layout");
NKSU_STATIC_ASSERT(FMAC_OFF_CLS == FMAC_OFF_TGT + 64, "rule tgt/cls layout");
NKSU_STATIC_ASSERT(FMAC_OFF_PERM == FMAC_OFF_CLS + 64, "rule cls/perm layout");
NKSU_STATIC_ASSERT(FMAC_OFF_EFFECT == FMAC_OFF_PERM + 64, "rule perm/effect layout");
NKSU_STATIC_ASSERT(FMAC_OFF_INVERT == FMAC_OFF_EFFECT + sizeof(int), "rule effect/invert layout");
NKSU_STATIC_ASSERT(FMAC_DATA_SELRULE == FMAC_OFF_INVERT + sizeof(int), "selrule payload size");
NKSU_STATIC_ASSERT(FMAC_OFF_FEATURE_VALUE == FMAC_DATA_UID, "feature id/value layout");
NKSU_STATIC_ASSERT(FMAC_DATA_FEATURE == FMAC_OFF_FEATURE_VALUE + sizeof(__u64),
		   "feature payload size");

#endif /* NKSU_UAPI_IOCTL_H */
