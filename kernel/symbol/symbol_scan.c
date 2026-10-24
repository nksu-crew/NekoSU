// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * In-memory kernel image scanning for the compressed kallsyms tables.
 *
 * Dereferencing arbitrary kernel addresses can fault. Every access to
 * external memory goes through copy_from_kernel_nofault() into a bounce
 * buffer before parsing.
 *
 * /sys/kernel/vmcoreinfo bounds the preferred scan to the runtime kernel
 * image. If it is unavailable, scan around an exported text symbol and
 * finally fall back to the common arm64 KIMAGE bases.
 */

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/sizes.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/uaccess.h>

#include "guards.h"
#include "symbol/symbol_internal.h"

/* Bootstrap file: call resolved pointers directly, no redirect macros. */
#define NKSU_SYMBOL_COMPAT_NO_MACROS
#include "symbol/symbol_compat.h"

/*
 * filp_open()/nksu_kread() are reached through resolved pointers here; the
 * type-hash the module emits for those indirect calls may differ from the
 * running kernel's, so opt this file out of CFI like the other users do.
 */
#if defined(__clang__)
#pragma clang attribute push(__attribute__((no_sanitize("cfi"))), apply_to = function)
#endif

/* bounce buffer: copy this much before and after the table */
#define KSYM_BOUNCE_BACK (16u << 20)
#define KSYM_BOUNCE_FWD (1u << 20)

/*
 * Safe memory reads. All external memory access goes through
 * copy_from_kernel_nofault(); failure simply means "no data here".
 */

static int kread(const void *addr, void *dst, size_t len)
{
	return copy_from_kernel_nofault(dst, addr, len);
}

/*
 * Best-effort read: copy page by page, zero-filling any unreadable page.
 * This keeps the readable part usable even when the bounce buffer spans an
 * unmapped region.
 */
static void kread_best_effort(void *dst, unsigned long src, size_t len)
{
	size_t done = 0;

	while (done < len) {
		size_t n = min_t(size_t, PAGE_SIZE - (src & (PAGE_SIZE - 1)), len - done);

		if (kread((const void *)(src + done), (u8 *)dst + done, n) < 0)
			memset((u8 *)dst + done, 0, n);
		done += n;
	}
}

/*
 * Copy [start, end) through nofault into a bounce buffer and search it for
 * token_table. On a hit, copy a larger window centred on token_table
 * (including the preceding address array / names) and parse it.
 *
 * Returns the number of symbol tables found.
 */
static int scan_region(unsigned long start, unsigned long end)
{
	const size_t chunk = SZ_64K;
	/* overlap so a token_table spanning a chunk boundary is still seen */
	const size_t step = SZ_64K - 4096;
	u8 *probe;
	unsigned long first, pos;
	int found = 0;

	/* inverted or empty range: nothing to do */
	if (end <= start)
		return 0;

	first = ALIGN(start, 8);
	if (first < start) /* ALIGN wrapped past the top of the address space */
		first = start;

	probe = kvmalloc(chunk, GFP_KERNEL);
	if (!probe)
		return -ENOMEM;

	/*
     * Include the final partial chunk rather than requiring pos+chunk<=end.
     * pos += step cannot wrap: it only runs after the "len < chunk" break
     * test, i.e. only while end - pos >= chunk > step, so pos + step < end.
     */
	for (pos = first; pos < end; pos += step) {
		size_t len = min_t(unsigned long, chunk, end - pos);
		size_t off;
		bool hit = false;

		/*
         * Fast path: one bulk copy when the whole chunk is mapped. Only if
         * that fails fall back to the page-granular best-effort copy, which
         * always fully initialises `probe` (unreadable pages -> zero), so no
         * stale bytes from a previous iteration can be mistaken for data.
         */
		if (kread((const void *)pos, probe, len) < 0)
			kread_best_effort(probe, pos, len);

		/* token_table is 8-byte aligned via ALGN; step 8 */
		for (off = 0; off + 16 <= len; off += 8) {
			const u8 *tt = probe + off;
			unsigned long tt_kaddr = pos + off;
			u8 *buf;
			unsigned long buf_len, buf_kaddr;

			if (!nksu_kallsyms_token_table_len(tt, probe + len))
				continue;

			/* candidate hit: copy a window ending past token_table */
			buf_len = KSYM_BOUNCE_BACK + KSYM_BOUNCE_FWD;
			buf = kvmalloc(buf_len, GFP_KERNEL);
			if (!buf)
				break;

			buf_kaddr = tt_kaddr > KSYM_BOUNCE_BACK ? tt_kaddr - KSYM_BOUNCE_BACK : 0;

			kread_best_effort(buf, buf_kaddr, buf_len);

			if (nksu_kallsyms_consume(buf, buf_len, buf + (tt_kaddr - buf_kaddr)) ==
			    0) {
				found++;
				hit = true;
			}
			kvfree(buf);

			if (hit)
				break;
		}

		/* stop after the last (short) chunk or once something was found */
		if (hit || len < chunk)
			break;
	}

	kvfree(probe);
	return found;
}

/*
 * Kernel image bounds.
 *
 * /sys/kernel/vmcoreinfo exposes the kernel's VMCOREINFO text, which
 * contains:
 *
 *     SYMBOL(_stext)=0xffff800010000000
 *     NUMBER(KERNEL_IMAGE_SIZE)=0x2000000
 *
 * Use it to bound the scan instead of blindly walking a huge address space.
 */

#define VMCOREINFO_PATH "/sys/kernel/vmcoreinfo"

/*
 * Read a value from VMCOREINFO. Entries look like:
 *
 *     SYMBOL(_stext)=0xffff800010000000
 *     NUMBER(KERNEL_IMAGE_SIZE)=0x2000000
 *
 * so the caller supplies both the entry kind ("SYMBOL" / "NUMBER") and the
 * name. Reading KERNEL_IMAGE_SIZE through "SYMBOL(...)" would never match.
 */
static unsigned long read_vmcoreinfo_entry(const char *kind, const char *name)
{
	loff_t pos = 0;
	ssize_t rd;
	size_t total = 0;
	unsigned long addr = 0;
	char key[64];
	char *p;
	struct file *f __free(nksu_filp) = NULL;

	snprintf(key, sizeof(key), "%s(%s)=", kind, name);

	/* Unavailable until the first scan has resolved the file primitives. */
	if (!nksu_filp_open || !nksu_kernel_read)
		return 0;

	f = nksu_filp_open(VMCOREINFO_PATH, O_RDONLY, 0);
	if (IS_ERR(f))
		return 0;

	char *buf __free(nksu_kvfree) = kvmalloc(SZ_64K, GFP_KERNEL);
	if (!buf)
		return 0;

	/* the file can be much larger than a page; read it all */
	while (total < SZ_64K - 1) {
		rd = nksu_kread(f, buf + total, SZ_64K - 1 - total, &pos);
		if (rd <= 0)
			break;
		total += (size_t)rd;
	}
	buf[total] = '\0';

	p = strstr(buf, key);
	if (p) {
		p += strlen(key);
		addr = simple_strtoul(p, NULL, 16);
	}

	return addr;
}

/*
 * Scan the kernel image for kallsyms. Caller must hold ksym_lock.
 */
void nksu_ksym_scan_memory(void)
{
	unsigned long stext, img_size, anchor;
	int found;

	/*
     * Preferred: /sys/kernel/vmcoreinfo exposes the runtime kernel image
     * base and size, so the scan can be confined to the real image
     * (typically tens of MB) instead of a broad guess window.
     */
	stext = read_vmcoreinfo_entry("SYMBOL", "_stext");
	if (!stext)
		stext = read_vmcoreinfo_entry("SYMBOL", "_text");
	img_size = read_vmcoreinfo_entry("NUMBER", "KERNEL_IMAGE_SIZE");

	if (stext) {
		if (!img_size || img_size > SZ_1G)
			img_size = SZ_512M;

		/* guard against stext + img_size wrapping the address space */
		found = stext <= ULONG_MAX - img_size ? scan_region(stext, stext + img_size) : 0;
		if (found)
			return;
	}

	/*
     * Fallback 1: anchor the scan to the runtime address of an exported
     * vmlinux symbol that this module already links against. The kernel
     * resolves it after KASLR, so it points inside the real kernel image
     * without us having to know the KASLR base.
     *
     * The kallsyms tables live in the image's rodata, which follows .text,
     * so the anchor (a text symbol) and the tables we are looking for both
     * sit at higher addresses: scan upward first and only fall back to the
     * region below the anchor if that misses.
     *
     * Do not use a compat pointer here: the first lookup (filp_open, which
     * bootstraps the whole layer) runs before any pointer has been filled
     * in, so it would be NULL.  printk is a direct module relocation and is
     * always valid.
     */
	anchor = (unsigned long)&printk;
	if (anchor) {
		pr_info("[ksym] scanning kernel image from anchor 0x%lx\n", anchor);

		if (anchor <= ULONG_MAX - SZ_256M) {
			found = scan_region(anchor, anchor + SZ_256M);
			if (found)
				return;
		}

		if (anchor > SZ_256M) {
			found = scan_region(anchor - SZ_256M, anchor);
			if (found)
				return;
		}
	}

	pr_warn("[ksym] in-memory kallsyms scan found no table (anchor=0x%lx)\n", anchor);
}

#if defined(__clang__)
#pragma clang attribute pop
#endif
