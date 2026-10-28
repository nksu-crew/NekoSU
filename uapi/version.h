/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * NekoSU kernel/userspace version (UAPI).
 *
 * This header is the single source of truth for the LKM's manually maintained
 * version.  It is included by the kernel module (kernel/include/nksu.h) and
 * parsed by the manager build, so a developer bumps this value when the KO
 * changes.
 */
#ifndef NKSU_UAPI_VERSION_H
#define NKSU_UAPI_VERSION_H

#define NKSU_VERSION "1.0.7"

#endif /* NKSU_UAPI_VERSION_H */
