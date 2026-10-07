/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NKSU_SYMBOL_H
#define NKSU_SYMBOL_H

#include <linux/types.h>

/*
 * Kernel symbol self-resolution.
 *
 * Android GKI/KMI kernels no longer export many internal symbols (SELinux,
 * cred, namespace, ...). A module that references one directly fails to load
 * with "Unknown symbol xxx (err -2)". This module scans the compressed
 * kallsyms table in kernel memory at runtime, resolves *all* symbols
 * (exported or not) and caches them for name-based lookups.
 *
 * Typical usage:
 *   during init:
 *       if (!nksu_ksym_lookup("selinux_state"))
 *           pr_err("cannot resolve selinux_state\n");
 *
 *   afterwards nksu_ksym_lookup() returns function addresses usable through
 *   function pointers, or data addresses usable as pointers to globals.
 */

/*
 * Resolve a kernel symbol and return its address, or 0 on failure.
 * Results are cached; repeated lookups do not rescan.
 */
unsigned long nksu_ksym_lookup(const char *name);

/* Number of symbols currently cached (debug). */
unsigned long nksu_ksym_count(void);

/* Clear the symbol cache (call on module unload). */
void nksu_ksym_cache_clear(void);

/* Dump all cached symbols to the kernel log (CONFIG_NKSU_DEBUG only). */
void nksu_ksym_dump(void);

/* Build identifier of symbol.c; used to detect a stale object file. */
const char *nksu_ksym_build_id(void);

#endif /* NKSU_SYMBOL_H */
