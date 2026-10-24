// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Compressed kallsyms table decoding.
 *
 * kallsyms memory layout (kernel/kallsyms.c):
 *
 *   kallsyms_offsets[]  or kallsyms_addresses[]   // num_syms entries
 *   kallsyms_relative_base                       // BASE_RELATIVE only
 *   kallsyms_names[]     [len][token ...]; a 0x80 length bit adds one byte:
 *                        len = (first & 0x7f) | (second << 7)
 *   kallsyms_markers[]   one offset per 256 symbols (u32, or 64-bit on
 *                        older arm64 kernels where ".align 8" means 256)
 *   kallsyms_token_table[]  256 NUL-terminated tokens (1-2 bytes each)
 *   kallsyms_token_index[]  256 u16 offsets into token_table
 *
 * token_table has a very distinctive shape (exactly 256 short tokens), so we
 * locate it first, then walk backwards to markers/names, then decode all
 * symbol names along names.
 *
 * Address decoding (see kallsyms_sym_address):
 *   - BASE_RELATIVE, !ABSOLUTE_PERCPU: addr = base + (u32)offsets[i]
 *   - BASE_RELATIVE,  ABSOLUTE_PERCPU: offsets[i] >= 0 -> offsets[i]
 *                                     offsets[i] <  0 -> base - 1 - offsets[i]
 *   - absolute array: addresses[i]
 *
 * To avoid depending on kernel global symbols, the address array and
 * relative_base are recovered from the fixed .rodata layout emitted by
 * scripts/kallsyms.c (offsets[] .. relative_base .. num_syms .. names[]):
 * relative_base is the u64 at names-16, num_syms the u32 at names-8. No
 * kernel global is referenced.
 */

#include <linux/kernel.h>
#include <linux/kconfig.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/errno.h>
#include <linux/sizes.h>

#include "symbol/symbol_internal.h"

/* Decoded-name buffer bound (KSYM_NAME_LEN is at most 512 on supported kernels). */
#define KSYM_NAME_MAX 512
#define KSYM_SYMBOL_MAX (KSYM_NAME_MAX + 2) /* name plus extended length */
#define KSYM_MARKER_SPAN_MAX (256u * KSYM_SYMBOL_MAX)
#define KSYM_MARKERS_MAX 8192 /* at most 8192 markers */

/*
 * Symbol name decoding (plain memory access on the bounce buffer).
 */

/*
 * Decode one symbol from names[..names_end), advancing *offset. An extended
 * (0x80) length uses a second byte; the leading type character is dropped.
 */
static int decode_symbol(const u8 *names, const u8 *names_end, const u16 *token_index,
			 const u8 *token_table, u32 *offset, char *out, int outsz)
{
	u32 pos = *offset;
	u32 names_len = (u32)(names_end - names);
	u32 len;
	char *dst = out;
	char *limit = out + outsz - 1;
	bool first = true;

	if (pos >= names_len)
		return -1;

	len = names[pos++];
	if (len & 0x80) {
		if (pos >= names_len)
			return -1;
		len = (len & 0x7f) | ((u32)names[pos++] << 7);
	}
	if (len >= KSYM_NAME_MAX || pos + len > names_len)
		return -1;

	while (len--) {
		u16 idx = token_index[names[pos++]];
		const char *tok = (const char *)token_table + idx;

		while (*tok) {
			if (first)
				first = false; /* drop the symbol-type character */
			else if (dst < limit)
				*dst++ = *tok;
			else
				return -1;
			tok++;
		}
	}

	*dst = '\0';
	*offset = pos;
	return 0;
}

/* Strip compiler suffixes: foo.cfi_jt / foo$xxx / foo.llvm.xxx -> foo */
static void strip_symbol_suffix(char *name)
{
	char *cut = NULL;
	char *p;

	if ((p = strchr(name, '$')))
		cut = p;
	if ((p = strstr(name, ".llvm.")))
		if (!cut || p < cut)
			cut = p;
	if ((p = strstr(name, ".cfi_jt")))
		if (!cut || p < cut)
			cut = p;
	if ((p = strstr(name, ".constprop")))
		if (!cut || p < cut)
			cut = p;

	if (cut)
		*cut = '\0';
}

/*
 * Kallsyms view location (all done on the bounce buffer).
 */

struct kallsyms_view {
	const u8 *token_table;
	u32 tlen;
	const u16 *token_index; /* 256 entries */
	const u8 *markers; /* one entry per 256 symbols */
	u32 marker_count;
	u8 marker_shift; /* 2 = u32 entry (>=5.4), 3 = u64 (older 64-bit) */
	const u8 *names; /* start of kallsyms_names */
	const u8 *names_end; /* == markers */
};

static u64 kallsyms_marker(const struct kallsyms_view *v, u32 i)
{
	if (v->marker_shift == 3) {
		u64 value;

		memcpy(&value, v->markers + (size_t)i * 8, sizeof(value));
		return value;
	} else {
		u32 value;

		memcpy(&value, v->markers + (size_t)i * 4, sizeof(value));
		return value;
	}
}

/*
 * Find the token index array that follows the token table.
 *
 * output_label() aligns the label with ALGN: ".align 8" means 8 bytes on
 * x86 but 2^8 = 256 bytes on arm64 (4.9), so both alignments are tried.
 */
static const u16 *token_index_candidate(const u8 *q, const u8 *end, const u32 *offsets)
{
	static const unsigned int aligns[] = { 8, 256 };
	unsigned int a;
	int i;

	for (a = 0; a < ARRAY_SIZE(aligns); a++) {
		const u16 *ti = (const u16 *)PTR_ALIGN(q, aligns[a]);

		if ((const u8 *)ti + 512 > end)
			continue;
		for (i = 0; i < 256; i++) {
			if (ti[i] != offsets[i])
				break;
		}
		if (i == 256)
			return ti;
	}
	return NULL;
}

static u32 token_table_scan(const u8 *p, const u8 *end, const u16 **ti_out)
{
	const u8 *q = p;
	u32 offsets[256];
	u32 tlen;
	const u16 *ti;
	int i;

	if (end - p < 16)
		return 0;

	/*
     * Quick reject: kallsyms_token_table is a dense run of NUL-terminated
     * tokens, so its leading bytes contain many NULs (~15 in the first 64 for
     * the 5.15 vermeer table).  The window must tolerate a long first token:
     * though the tokens are frequency-sorted, the most frequent one can still
     * be several bytes -- the vermeer kernel starts with "_global", which
     * leaves only 3 NULs in the first 16 bytes, so the previous "4 NULs in 16
     * bytes" test rejected the real table outright and every symbol lookup
     * then failed (cache=0).  Count over 64 bytes instead; ordinary
     * code/rodata rarely holds 8 NULs there.
     */
	{
		const u8 *s = p;
		const u8 *s_end = p + 64;
		int nuls = 0;

		if (s_end > end)
			s_end = end;
		while (s < s_end) {
			if (!*s && ++nuls >= 8)
				break;
			s++;
		}
		if (nuls < 8)
			return 0;
	}

	for (i = 0; i < 256; i++) {
		const u8 *start = q;

		offsets[i] = (u32)(start - p);
		while (q < end && *q)
			q++;
		if (q >= end || *q != '\0' || q == start)
			return 0;
		if ((size_t)(q - p) > 4096)
			return 0;
		q++;
	}

	tlen = (u32)(q - p);
	ti = token_index_candidate(q, end, offsets);
	if (!ti)
		return 0;
	if (ti_out)
		*ti_out = ti;
	return tlen;
}

/*
 * Return the token_table size if p is a valid token_table, else 0.
 *
 * A token_table is exactly 256 non-empty NUL-terminated tokens immediately
 * followed by a u16 index array whose i-th entry is the byte offset of the
 * i-th token. Requiring that exact match makes false positives in unrelated
 * rodata essentially impossible, and does not assume anything about
 * individual token length.
 */
u32 nksu_kallsyms_token_table_len(const u8 *p, const u8 *end)
{
	return token_table_scan(p, end, NULL);
}

/*
 * Locate markers.
 *
 * kallsyms_markers sits just before kallsyms_token_table, but both labels are
 * 8-byte aligned, so there can be up to 4 bytes of zero padding between the
 * last marker and token_table. The array is strictly increasing and starts at
 * 0. Walk backwards from the last marker while the sequence keeps increasing;
 * the result is markers[0].
 */
static bool locate_markers_width(const u8 *buf_start, const u8 *token_table, u8 shift,
				 const u8 **markers_out, u32 *count_out)
{
	const size_t width = (size_t)1 << shift;
	const u8 *limit = token_table - (size_t)KSYM_MARKERS_MAX * width;
	const u8 *p, *start;
	u32 count;

	if (limit < buf_start)
		limit = buf_start;

	/* token_table is 8-byte aligned; the marker array shares that align. */
	if (((unsigned long)token_table & (width - 1)) != 0)
		return false;

	p = token_table - width;
	if (p < limit)
		return false;

	/*
	 * Skip the alignment padding before token_table.  output_label aligns
	 * it to 8 bytes; a 4-byte marker array needs at most one pad word,
	 * while a 64-bit one can contribute up to 7 zero bytes (here the 4.9
	 * image has five zero quads).  Real marker entries are strictly
	 * increasing and end with a non-zero offset, so any run of trailing
	 * zeros is padding.
	 */
	for (;;) {
		u64 tail;

		if (p - width < limit)
			break;
		memcpy(&tail, p, width);
		if (tail != 0)
			break;
		p -= width;
	}

	start = p;
	count = 1;

	while (start - width >= limit) {
		u64 cur, nxt;

		memcpy(&cur, start - width, width);
		memcpy(&nxt, start, width);
		if (cur < nxt && nxt - cur <= KSYM_MARKER_SPAN_MAX) {
			start -= width;
			count++;
		} else {
			break;
		}
	}

	/*
     * The real markers array starts at 0. If we did not land on 0, the
     * walk went into preceding data; scan forward for the first 0.
     */
	{
		u64 head;

		memcpy(&head, start, width);
		if (count >= 2 && head != 0) {
			const u8 *q;

			for (q = start; q < token_table; q += width) {
				u64 v;

				memcpy(&v, q, width);
				if (v == 0) {
					start = q;
					count = (u32)((token_table - q) / width);
					break;
				}
			}
		}
	}

	{
		u64 head;

		memcpy(&head, start, width);
		if (count < 2 || head != 0)
			return false;
	}

	*markers_out = start;
	*count_out = count;
	return true;
}

/*
 * kallsyms_markers holds one offset per 256 symbols.  64-bit kernels before
 * 5.4 emit them as .quad (unsigned long); newer kernels use .long.  Both
 * widths are tried by view_from_token_table().
 */

/*
 * Validate a candidate names start:
 *   1. after every 256 decoded symbols the offset equals the matching
 *      markers[k];
 *   2. the whole names section decodes cleanly.
 */
static bool names_start_valid(const u8 *cand, const struct kallsyms_view *v)
{
	const u8 *names_end = v->names_end;
	u32 names_size = (u32)(names_end - cand);
	u32 off = 0;
	u32 k = 1;
	u32 sym_in_group = 0;

	for (;;) {
		char tmp[KSYM_NAME_MAX];
		u32 before = off;

		if (decode_symbol(cand, names_end, v->token_index, v->token_table, &off, tmp,
				  sizeof(tmp)) < 0)
			break;

		if (off <= before)
			return false;

		if (++sym_in_group == 256) {
			sym_in_group = 0;
			if (k < v->marker_count) {
				if (off != kallsyms_marker(v, k))
					return false;
				k++;
			}
		}
	}

	/*
	 * All markers matched and decoding ended near the end of names. There
	 * is alignment padding between names and the markers label: up to 7
	 * bytes on kernels that align labels to 8, up to 255 on arm64 4.9
	 * (where ".align 8" means 2^8 bytes).  Allow the full 256-byte range.
	 */
	if (k != v->marker_count)
		return false;
	return off + 256 >= names_size;
}

/*
 * Try every plausible names start until the callback accepts one.
 *
 * Candidates satisfy names_size in
 * [markers[last], markers[last] + 256 * KSYM_SYMBOL_MAX] and every 256-symbol
 * boundary matching markers.
 */
static const u8 *find_names_start(const u8 *buf_start, const struct kallsyms_view *v,
				  bool (*try)(const u8 *names, const u8 *names_end, void *arg),
				  void *arg)
{
	const u8 *names_end = v->names_end;
	const u8 *cand;
	u32 last_marker;
	u32 max_names_size;

	if (v->marker_count < 2)
		return NULL;
	last_marker = (u32)kallsyms_marker(v, v->marker_count - 1);
	if (last_marker < 1 || (size_t)last_marker >= (size_t)(names_end - buf_start))
		return NULL;

	/*
     * names_size lies in [last_marker, last_marker + 256*KSYM_SYMBOL_MAX]:
     * the last marker points at the start of the final group, which has at
     * most 256 symbols of at most KSYM_SYMBOL_MAX bytes each.
     */
	max_names_size = last_marker + 256u * KSYM_SYMBOL_MAX;
	if (max_names_size < last_marker) /* overflow guard */
		return NULL;

	cand = names_end - last_marker;

	/*
     * Walk candidate starts downward, but never form a pointer before
     * buf_start (that would be undefined behaviour): terminate explicitly
     * when cand == buf_start instead of decrementing past it. As cand
     * decreases names_end - cand grows, so the size bound is tested first.
     */
	for (;;) {
		if ((size_t)(names_end - cand) > max_names_size)
			break;
		if (names_start_valid(cand, v) && try(cand, names_end, arg))
			return cand;
		if (cand == buf_start)
			break;
		cand--;
	}
	return NULL;
}

/*
 * Given token_table, fill token_index / markers. Returns 0 on success.
 * The names start is left to the caller (there may be several candidates).
 */
static bool view_accept_all(const u8 *names, const u8 *names_end, void *arg)
{
	(void)names;
	(void)names_end;
	(void)arg;
	return true;
}

static int view_from_token_table(const u8 *buf_start, const u8 *token_table, const u8 *buf_end,
				 struct kallsyms_view *v)
{
	u32 tlen;
	u8 shift;
	int i;

	memset(v, 0, sizeof(*v));

	tlen = token_table_scan(token_table, buf_end, &v->token_index);
	if (!tlen)
		return -ENOENT;
	/* token_index is emitted with output_label's ALGN. */
	if (token_table + tlen + 7 + 512 > buf_end)
		return -ENOENT;

	v->token_table = token_table;
	v->tlen = tlen;

	if (v->token_index[0] != 0)
		return -ENOENT;
	for (i = 1; i < 256; i++) {
		if (v->token_index[i] <= v->token_index[i - 1] || v->token_index[i] >= tlen)
			return -ENOENT;
	}

	/*
	 * Try the modern u32 marker array first, then the 64-bit one used by
	 * older kernels (4.9).  Only accept a width whose marker boundaries
	 * actually line up with a decodable kallsyms_names section.
	 */
	for (i = 0; i < 2; i++) {
		shift = i == 0 ? 2 : 3;

		if (!locate_markers_width(buf_start, token_table, shift, &v->markers,
					  &v->marker_count))
			continue;

		v->marker_shift = shift;
		v->names_end = v->markers;
		if (find_names_start(buf_start, v, view_accept_all, NULL))
			return 0;
	}

	return -ENOENT;
}

/*
 * Full name decoding is streamed on demand (see nksu_kallsyms_consume): names
 * are
 * never stored, only decoded into a small stack buffer.
 */

/*
 * Address array location.
 */

struct addr_info {
	bool base_relative;
	const u32 *offsets; /* BASE_RELATIVE mode */
	unsigned long relative_base;
	const unsigned long *addresses; /* absolute mode (kallsyms_addresses) */
};

/*
 * Match the kernel's kallsyms_sym_address().
 *
 * With CONFIG_KALLSYMS_BASE_RELATIVE (mandatory on arm64):
 *   - without CONFIG_KALLSYMS_ABSOLUTE_PERCPU every entry is an unsigned
 *     32-bit offset from kallsyms_relative_base;
 *   - with it, non-negative entries are absolute values and negative entries
 *     encode relative values as (-offset - 1).
 *
 * CONFIG_KALLSYMS_ABSOLUTE_PERCPU is x86_64-only by default (init/Kconfig:
 * "default X86_64 && SMP") and is not set on arm64, but both branches are
 * kept so the decoder matches whichever kernel config the module was built
 * against. Note the negative branch must sign-extend the offset; a plain
 * (u32) cast would zero-extend and be off by 2^32.
 */
static unsigned long symbol_addr(const struct addr_info *ai, u32 i)
{
	if (ai->base_relative) {
#if IS_ENABLED(CONFIG_KALLSYMS_ABSOLUTE_PERCPU)
		s32 off = (s32)ai->offsets[i];

		if (off >= 0)
			return (unsigned long)off; /* absolute */
		return ai->relative_base - 1 - (unsigned long)(long)off;
#else
		return ai->relative_base + (u32)ai->offsets[i];
#endif
	}
	if (ai->addresses)
		return ai->addresses[i];
	return 0;
}

/*
 * Recover the address array and relative_base from the on-disk layout.
 *
 * scripts/kallsyms.c emits, in this order:
 *     kallsyms_offsets[]        (u32 / .long per symbol)   [BASE_RELATIVE]
 *   or kallsyms_addresses[]     (u64 / PTR per symbol)      [!BASE_RELATIVE]
 *     kallsyms_relative_base    (.quad, BASE_RELATIVE only)
 *     kallsyms_num_syms         (PTR)
 *     kallsyms_names
 *
 * Each label goes through output_label(), which uses ALGN.  On little-endian
 * 64-bit x86 ALGN is 8 bytes, but on arm64 the assembler reads ".align 8" as
 * 2^8 = 256 bytes, so older kernels (4.9) align every table to 256 bytes.
 * Rather than hard-coding the layout, find num_syms just before names by
 * matching it against the marker count, derive the alignment from the gap to
 * names, and then locate relative_base/offsets from there.
 */
static unsigned long align_down_ul(unsigned long value, unsigned long align)
{
	return value & ~(align - 1);
}

static int resolve_addresses(const struct kallsyms_view *v, const u8 *buf_start, const u8 *buf_end,
			     struct addr_info *ai, u32 *num_out)
{
	const u8 *names = v->names;
	unsigned long names_addr = (unsigned long)names;
	unsigned long limit, num_addr, align;
	u32 num_syms;

	(void)buf_end;
	memset(ai, 0, sizeof(*ai));

	/* marker_count = ceil(num_syms / 256) */
	if (v->marker_count < 2)
		return -ENOENT;

	if (names_addr < (unsigned long)buf_start + 24)
		return -ENOENT;

	limit = names_addr > (unsigned long)buf_start + 0x800 ?
			names_addr - 0x800 :
			(unsigned long)buf_start;

	for (num_addr = (names_addr - sizeof(u64)) & ~(sizeof(u64) - 1);
	     num_addr >= limit; num_addr -= sizeof(u64)) {
		u64 value;
		unsigned long rel_addr, offsets_size, offsets_start;
		u64 rel_base;

		memcpy(&value, (const void *)num_addr, sizeof(value));
		if (value == 0 || value > KSYM_MARKERS_MAX * 256u)
			continue;
		if (value <= (u64)(v->marker_count - 1) * 256u ||
		    value > (u64)v->marker_count * 256u)
			continue;

		num_syms = (u32)value;

		/*
		 * output_label() aligns every label, so the gap between
		 * num_syms and names is exactly the alignment unit.
		 */
		align = names_addr - num_addr;
		if (align < sizeof(u64) || align > 0x800 || (align & (align - 1)))
			continue;

		rel_addr = num_addr - align;
		if (rel_addr < (unsigned long)buf_start)
			continue;

		memcpy(&rel_base, (const void *)rel_addr, sizeof(rel_base));
		if (rel_base >= 0xff00000000000000UL) {
			/* BASE_RELATIVE: offsets[] then relative_base */
			offsets_size = (unsigned long)num_syms * sizeof(u32);
			offsets_start = align_down_ul(rel_addr - offsets_size, align);
			if (offsets_start < (unsigned long)buf_start)
				continue;

			{
				const u32 *offs = (const u32 *)offsets_start;
				u32 first, last;

				memcpy(&first, offs, sizeof(first));
				memcpy(&last, (const u8 *)offs + offsets_size - sizeof(u32),
				       sizeof(last));
				/* relative_base is the lowest symbol address. */
				if (first != 0 || last > (u32)SZ_1G)
					continue;
			}

			ai->base_relative = true;
			ai->offsets = (const u32 *)offsets_start;
			ai->relative_base = rel_base;
			*num_out = num_syms;
			return 0;
		}

		/* Absolute address table: no relative_base scalar. */
		offsets_size = (unsigned long)num_syms * sizeof(unsigned long);
		offsets_start = align_down_ul(num_addr - offsets_size, align);
		if (offsets_start < (unsigned long)buf_start)
			continue;
		{
			const unsigned long *addr = (const unsigned long *)offsets_start;
			unsigned long first, last;

			memcpy(&first, addr, sizeof(first));
			memcpy(&last, (const u8 *)addr + offsets_size - sizeof(unsigned long),
			       sizeof(last));
			if (first < 0xff00000000000000UL || last < first ||
			    last - first > SZ_1G)
				continue;
		}

		ai->addresses = (const unsigned long *)offsets_start;
		*num_out = num_syms;
		return 0;
	}

	return -ENOENT;
}

/*
 * Single-table consumption.
 */

struct try_ctx {
	const struct kallsyms_view *v;
	const u8 *buf;
	const u8 *buf_end;
	/* results */
	u32 num_syms;
	struct addr_info ai;
	bool ok;
};

/*
 * Candidate names callback: locate the address array from the fixed layout
 * that follows the names stream.
 */
static bool try_names_candidate(const u8 *names, const u8 *names_end, void *arg)
{
	struct try_ctx *ctx = arg;
	struct kallsyms_view v = *ctx->v;

	v.names = names;
	v.names_end = names_end;

	if (resolve_addresses(&v, ctx->buf, ctx->buf_end, &ctx->ai, &ctx->num_syms) != 0)
		return false;

	ctx->ok = true;
	return true;
}

int nksu_kallsyms_consume(const u8 *buf, unsigned long len, const u8 *token_table)
{
	struct kallsyms_view v;
	struct try_ctx ctx;
	const u8 *names;
	int rc;
	u32 i, offset;

	rc = view_from_token_table(buf, token_table, buf + len, &v);
	if (rc)
		return rc;

	memset(&ctx, 0, sizeof(ctx));
	ctx.v = &v;
	ctx.buf = buf;
	ctx.buf_end = buf + len;

	names = find_names_start(buf, &v, try_names_candidate, &ctx);
	if (!names || !ctx.ok)
		return -ENOENT;
	v.names = names;

	/* decode each name and cache name -> address */
	offset = 0;
	for (i = 0; i < ctx.num_syms; i++) {
		char nm[KSYM_NAME_MAX];
		char raw[KSYM_NAME_MAX];
		unsigned long a;

		if (decode_symbol(names, v.names_end, v.token_index, v.token_table, &offset, nm,
				  sizeof(nm)) < 0)
			break;

		a = symbol_addr(&ctx.ai, i);
		if (!a)
			continue;

		/*
         * Keep the original name as well as the suffix-stripped alias.
         *
         * With CONFIG_CFI_CLANG the linker emits a jump-table stub per
         * address-taken function (foo.cfi_jt); sys_call_table holds those
         * stub addresses, not the plain function addresses. The stubs live
         * at the end of .text, so the plain symbol is always seen first and
         * would otherwise win the stripped name. A caller that resolves a
         * sys_call_table entry must get the stub, so the exact name has to
         * stay resolvable too.
         */
		strscpy(raw, nm, sizeof(raw));
		strip_symbol_suffix(nm);
		if (nm[0])
			nksu_ksym_cache_add_locked(nm, a);
		if (strstr(raw, ".cfi_jt"))
			nksu_ksym_cache_add_locked(raw, a);
	}

	return 0;
}
