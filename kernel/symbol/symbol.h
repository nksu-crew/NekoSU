/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NKSU_SYMBOL_H
#define NKSU_SYMBOL_H

#include <linux/types.h>

/*
 * Kernel symbol self-resolution.
 *
 * Android GKI/KMI kernels no longer export many internal symbols (SELinux,
 * cred, namespace, ...). A module that references one directly fails to load
 * with "Unknown symbol xxx (err -2)". This module recovers the address of
 * kallsyms_lookup_name() with a one-shot kprobe, resolves *all* symbols
 * (exported or not) through it and caches them for name-based lookups.
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
 * Record a symbol name in the module's .nksu_ksymtab section.
 *
 * Every symbol the module resolves through kallsyms at load time is listed in
 * that table. bootpatcher reads it from nksu.ko and verifies, against the
 * kernel in the target boot image, that every required entry exists before it
 * patches the ramdisk. A missing symbol would otherwise only surface as a
 * failed insmod after a reboot, when recovery is much harder.
 *
 * Entries are (flags byte, NUL-terminated name). Bit 0 marks a symbol the
 * module cannot run without; entries without it are best-effort lookups whose
 * absence the code already tolerates, so bootpatcher only warns about them.
 *
 * The macro emits the entry at the point of use, so NKSU_RESOLVE() and the
 * compile-time nksu_ksym_lookup("name") call sites record themselves without a
 * second, manually maintained list. _name must be a string literal.
 */
#define NKSU_SYMBOL_EXPORT_FLAGS(_flags, _name)                            \
	__asm__(".pushsection .nksu_ksymtab, \"a\"\n\t.byte " #_flags      \
		"\n\t.asciz \"" _name "\"\n\t.popsection\n")

#define NKSU_SYMBOL_EXPORT(_name) NKSU_SYMBOL_EXPORT_FLAGS(1, _name)
#define NKSU_SYMBOL_EXPORT_OPTIONAL(_name) NKSU_SYMBOL_EXPORT_FLAGS(0, _name)

/*
 * nksu_ksym_lookup() for a symbol name known at compile time, also recording
 * it in the export table. Use NKSU_KSYM_TRY_LOOKUP() when the caller tolerates
 * a missing symbol, and nksu_ksym_lookup() directly when the name is only
 * known at runtime: those names cannot be checked before flashing.
 */
#define NKSU_KSYM_LOOKUP(_name)                                            \
	({                                                               \
		NKSU_SYMBOL_EXPORT(_name);                              \
		nksu_ksym_lookup(_name);                                 \
	})

#define NKSU_KSYM_TRY_LOOKUP(_name)                                        \
	({                                                               \
		NKSU_SYMBOL_EXPORT_OPTIONAL(_name);                     \
		nksu_ksym_lookup(_name);                                 \
	})

/*
 * Resolve a kernel symbol and return its address, or 0 on failure.
 * Results are cached; repeated lookups do not re-resolve.
 */
unsigned long nksu_ksym_lookup(const char *name);

/* Number of symbols currently cached (debug). */
unsigned long nksu_ksym_count(void);

/* Clear the symbol cache (call on module unload). */
void nksu_ksym_cache_clear(void);

/* Dump all cached symbols to the kernel log (CONFIG_NKSU_DEBUG only). */
void nksu_ksym_dump(void);

#endif /* NKSU_SYMBOL_H */
