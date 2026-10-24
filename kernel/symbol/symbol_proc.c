// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * /proc/kallsyms lookup, the cheap first source for exported and visible
 * symbols. When kptr_restrict hides addresses the lookup is skipped and the
 * in-memory scanner takes over.
 */

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/sizes.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "guards.h"
#include "symbol/symbol_internal.h"

/* Bootstrap file: call resolved pointers directly, no redirect macros. */
#define NKSU_SYMBOL_COMPAT_NO_MACROS
#include "symbol/symbol_compat.h"

/* Set once we know /proc/kallsyms hides addresses (kptr_restrict). */
static bool ksym_proc_hidden;
static bool ksym_proc_checked;

/*
 * filp_open()/nksu_kread() are reached through resolved pointers here; the
 * type-hash the module emits for those indirect calls may differ from the
 * running kernel's, so opt this file out of CFI like the other users do.
 */
#if defined(__clang__)
#pragma clang attribute push(__attribute__((no_sanitize("cfi"))), apply_to = function)
#endif

/*
 * Parse one "addr type name" kallsyms line in place. Returns the address if
 * the name matches, otherwise 0. `line` is modified (delimiters -> NUL).
 */
static unsigned long match_kallsyms_line(char *line, const char *name, size_t namelen)
{
	char *sp1, *sp2, *sym;

	if (!*line)
		return 0;

	sp1 = strchr(line, ' ');
	if (!sp1)
		return 0;
	*sp1 = '\0';

	sym = sp1 + 1;
	sp2 = strchr(sym, ' ');
	if (!sp2)
		return 0;
	sym = sp2 + 1;

	{
		char *e = sym + strcspn(sym, " \t");

		*e = '\0';
	}

	if (sym[0] && strlen(sym) == namelen && memcmp(sym, name, namelen) == 0) {
		char *endp;
		unsigned long a = simple_strtoul(line, &endp, 16);

		/* reject lines whose address did not parse cleanly */
		if (endp != line)
			return a;
	}

	return 0;
}

/*
 * Returns true if the leading lines of a /proc/kallsyms dump all carry an
 * all-zero address field (kptr_restrict=2 style output). Used to skip the
 * file entirely once we know lookups can never succeed there.
 */
static bool kallsyms_addrs_hidden(const char *buf, size_t len)
{
	const char *p = buf, *end = buf + len;
	int lines = 0;

	while (p < end && lines < 32) {
		const char *nl = memchr(p, '\n', end - p);
		size_t ll = nl ? (size_t)(nl - p) : (size_t)(end - p);
		const char *sp = memchr(p, ' ', ll);

		if (sp && sp > p) {
			const char *q;

			for (q = p; q < sp; q++)
				if (*q != '0')
					return false;
		}
		lines++;
		if (!nl)
			break;
		p = nl + 1;
	}
	return lines > 0;
}

/*
 * Look up an address by name in /proc/kallsyms. When kptr_restrict hides
 * addresses the values are 0 and this returns 0.
 *
 * Uses carry-over: if a read ends mid-line, keep the trailing partial line
 * for the next read so a symbol name is never split.
 */
unsigned long nksu_ksym_lookup_proc(const char *name)
{
	loff_t pos = 0;
	ssize_t rd;
	unsigned long addr = 0;
	size_t namelen = strlen(name);
	size_t used = 0;
	struct file *f __free(nksu_filp) = NULL;
	char *buf __free(nksu_kvfree) = NULL;

	if (ksym_proc_hidden)
		return 0;

	/* File primitives are unavailable until a scan has resolved them. */
	if (!nksu_filp_open || !nksu_kernel_read)
		return 0;

	f = nksu_filp_open("/proc/kallsyms", O_RDONLY, 0);
	if (IS_ERR(f))
		return 0;

	/*
     * nksu_kread() drives ->read_iter only. This kernel's kallsyms proc
     * file registers .proc_read = seq_read without proc_read_iter, so
     * nksu_kread() fails with -EINVAL and prints "kernel read not
     * supported for file /kallsyms". Detect that up-front and let the
     * in-memory kallsyms scan handle the lookup instead.
     */
	if (!f->f_op->read_iter)
		return 0;

	buf = kvmalloc(SZ_64K, GFP_KERNEL);
	if (!buf)
		return 0;

	for (;;) {
		char *p, *line;

		if (used >= SZ_64K - 1) {
			/*
             * A single kallsyms line is at most a few hundred bytes, so a
             * pending partial line filling the whole buffer means there was
             * no newline in 64 KiB: malformed input. Stop instead of
             * silently reusing the buffer, which could misparse the line
             * tail as a fresh entry and produce a wrong match.
             */
			break;
		}

		rd = nksu_kread(f, buf + used, SZ_64K - 1 - used, &pos);
		if (rd < 0)
			break;
		if (rd == 0) {
			/* EOF: the remaining bytes (if any) are the final line */
			if (used) {
				buf[used] = '\0';
				addr = match_kallsyms_line(buf, name, namelen);
			}
			break;
		}

		buf[used + rd] = '\0';
		used += (size_t)rd;

		/*
         * One-time probe: if the kernel hides addresses, bail out
         * immediately and remember it, so we don't re-read the whole
         * file for every symbol we resolve.
         */
		if (!ksym_proc_checked) {
			ksym_proc_checked = true;
			if (kallsyms_addrs_hidden(buf, used)) {
				ksym_proc_hidden = true;
				break;
			}
		}

		p = buf;
		while ((line = strsep(&p, "\n")) != NULL) {
			if (!p) {
				/*
                 * Last piece. If the buffer ended on a newline this is an
                 * empty tail: reset the buffer. Otherwise keep the partial
                 * line for the next read.
                 */
				size_t linelen = strlen(line);

				if (linelen == 0) {
					used = 0;
					break;
				}
				if (line != buf) {
					/* move partial line to the front for the next round */
					memmove(buf, line, linelen + 1);
				}
				used = linelen;
				break;
			}

			addr = match_kallsyms_line(line, name, namelen);
			if (addr)
				return addr;
		}
	}

	return addr;
}

void nksu_ksym_proc_reset(void)
{
	ksym_proc_hidden = false;
	ksym_proc_checked = false;
}

#if defined(__clang__)
#pragma clang attribute pop
#endif
