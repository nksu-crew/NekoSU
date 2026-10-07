// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Kernel symbol resolution -- let the module find its own unexported symbols.
 *
 * Background
 * ----------
 * Android GKI/KMI kernels strip a large set of internal symbols (SELinux,
 * cred, namespace, ...) from the export table. A module that references them
 * directly fails to load with:
 *
 *     nksu: Unknown symbol selinux_state (err -2)
 *
 * The historical approach, kallsyms_lookup_name(), is no longer exported as
 * of 5.7, so "ask the kernel for the symbol" is itself unavailable.
 *
 * This file takes a fully self-contained route: scan the compressed kallsyms
 * tables in kernel memory, which contain *all* symbols (exported and not),
 * and cache the results for later lookups. Resolution order:
 *
 *   1. /proc/kallsyms (cheapest when kptr_restrict is relaxed)
 *   2. the in-memory kallsyms tables (reliable source for unexported symbols)
 *
 * kallsyms memory layout (kernel/kallsyms.c)
 * ------------------------------------------
 *
 *   kallsyms_offsets[]  or kallsyms_addresses[]   // num_syms entries
 *   kallsyms_relative_base                       // BASE_RELATIVE only
 *   kallsyms_names[]     variable length: [len][token ...],
 *                        where len uses 0x80 continuation encoding
 *   kallsyms_markers[]   one u32 offset per 256 symbols
 *   kallsyms_token_table[]  256 NUL-terminated tokens (1-2 bytes each)
 *   kallsyms_token_index[]  256 u16 offsets into token_table
 *
 * token_table has a very distinctive shape (exactly 256 short tokens), so we
 * locate it first, then walk backwards to markers/names, then decode all
 * symbol names along names.
 *
 * Safe reads
 * ----------
 * Dereferencing arbitrary kernel addresses can fault. Every access to
 * external memory goes through copy_from_kernel_nofault() into a bounce
 * buffer before parsing.
 *
 * Address decoding:
 *   - BASE_RELATIVE: offsets[i] >= 0 -> base + offsets[i]
 *                    offsets[i] <  0 -> base - 1 - offsets[i]
 *     (see kallsyms_sym_address)
 *   - absolute array: addresses[i]
 *
 * To avoid depending on kernel global symbols, both the address array and
 * relative_base are recovered from the self-referential symbols inside the
 * kallsyms table ("kallsyms_names" / "kallsyms_token_table"): their values
 * must equal the names / token_table addresses we already located.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/hashtable.h>
#include <linux/sizes.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/uaccess.h>

#include "klog.h"
#include "symbol.h"

#define KSYM_MAX_SYMS     (1u << 21)  /* ~2M, far above real needs */
#define KSYM_NAME_MAX     512         /* matches kernel KSYM_NAME_LEN cap */
#define KSYM_MARKERS_MAX  8192        /* at most 8192 markers */

/* bounce buffer: copy this much before and after the table */
#define KSYM_BOUNCE_BACK  (16u << 20)
#define KSYM_BOUNCE_FWD   (1u << 20)

/* cache buckets: 2^18, very low load for ~100k symbols */
static DEFINE_HASHTABLE(ksym_htable, 18);
static DEFINE_MUTEX(ksym_lock);
static atomic_t ksym_count = ATOMIC_INIT(0);

/* the heavy in-memory kallsyms scan runs at most once */
static bool ksym_scanned;

/* set once we know /proc/kallsyms hides addresses (kptr_restrict) */
static bool ksym_proc_hidden;
static bool ksym_proc_checked;

struct ksym_entry {
    struct hlist_node node;
    unsigned long addr;
    char name[];
};

static u32 ksym_hash(const char *name)
{
    u32 h = 5381;

    while (*name)
        h = h * 33 + (unsigned char)*name++;
    return h;
}

static struct ksym_entry *ksym_find_locked(const char *name)
{
    struct ksym_entry *e;
    u32 h = ksym_hash(name);

    hash_for_each_possible(ksym_htable, e, node, h) {
        if (strcmp(e->name, name) == 0)
            return e;
    }
    return NULL;
}

static void ksym_cache_add(const char *name, unsigned long addr)
{
    struct ksym_entry *e;

    if (!name || !name[0] || !addr || ksym_find_locked(name))
        return;

    e = kmalloc(sizeof(*e) + strlen(name) + 1, GFP_KERNEL);
    if (!e)
        return;

    strcpy(e->name, name);
    e->addr = addr;
    hash_add(ksym_htable, &e->node, ksym_hash(name));
    atomic_inc(&ksym_count);
}

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
        size_t n = min_t(size_t, PAGE_SIZE - (src & (PAGE_SIZE - 1)),
                         len - done);

        if (kread((const void *)(src + done), (u8 *)dst + done, n) < 0)
            memset((u8 *)dst + done, 0, n);
        done += n;
    }
}

/*
 * Symbol name decoding (plain memory access on the bounce buffer).
 */

/*
 * Decode one symbol from names[..names_end). Returns 0 on success and
 * advances *offset.
 *
 * Matches this kernel's kallsyms_expand_symbol(): the compressed length is a
 * single byte followed by that many token indices, and the first character of
 * the expanded string (the symbol type) is dropped.
 */
static int decode_symbol(const u8 *names, const u8 *names_end,
                         const u16 *token_index, const u8 *token_table,
                         u32 *offset, char *out, int outsz)
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
    if (len >= KSYM_NAME_MAX || pos + len > names_len)
        return -1;

    while (len--) {
        u16 idx = token_index[names[pos++]];
        const char *tok = (const char *)token_table + idx;

        while (*tok) {
            if (first)
                first = false;   /* drop the symbol-type character */
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
    const u8  *token_table;
    u32        tlen;
    const u16 *token_index;   /* 256 entries */
    const u32 *markers;       /* one offset per 256 symbols */
    u32        marker_count;
    const u8  *names;         /* start of kallsyms_names */
    const u8  *names_end;     /* == markers */
};

/*
 * Return the token_table size if p is a valid token_table, else 0.
 *
 * A token_table is exactly 256 non-empty NUL-terminated tokens immediately
 * followed (after 2-byte alignment) by a u16 index array whose i-th entry is
 * the byte offset of the i-th token. Requiring that exact match makes false
 * positives in unrelated rodata essentially impossible, and does not assume
 * anything about individual token length.
 *
 * A cheap prefilter (a NUL within the first 64 bytes) keeps the scan fast in
 * code/rodata regions with long NUL-free runs.
 */
static u32 token_table_len(const u8 *p, const u8 *end)
{
    const u8 *q = p;
    u32 offsets[256];
    u32 tlen;
    const u16 *ti;
    int i;

    if (end - p < 16)
        return 0;

    /*
     * Quick reject: kallsyms_token_table is a dense run of very short
     * NUL-terminated tokens, so the leading bytes contain many NULs.
     * Ordinary code/rodata rarely has 4 NULs within 16 bytes.
     */
    {
        const u8 *s = p;
        const u8 *s_end = p + 16;
        int nuls = 0;

        while (s < s_end) {
            if (!*s && ++nuls >= 4)
                break;
            s++;
        }
        if (nuls < 4)
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
    /* kallsyms_token_index is emitted with the label's 8-byte alignment */
    ti = (const u16 *)PTR_ALIGN(q, 8);
    if ((const u8 *)ti + 512 > end)
        return 0;

    for (i = 0; i < 256; i++) {
        if (ti[i] != offsets[i])
            return 0;
    }

    return tlen;
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
static bool locate_markers(const u8 *buf_start, const u8 *token_table,
                           const u32 **markers_out, u32 *count_out)
{
    const u8 *limit = token_table - (size_t)KSYM_MARKERS_MAX * 4;
    const u8 *p, *start;
    u32 count;

    if (limit < buf_start)
        limit = buf_start;

    /* token_table must be 4-byte aligned for the backwards u32 walk */
    if (((unsigned long)token_table & 3) != 0)
        return false;

    p = token_table - 4;
    if (p < limit)
        return false;

    /* skip the 0..4 bytes of alignment padding before token_table */
    if (*(const u32 *)p == 0 && p - 4 >= limit)
        p -= 4;

    start = p;
    count = 1;

    while (start - 4 >= limit) {
        u32 cur = *(const u32 *)(start - 4);
        u32 nxt = *(const u32 *)start;

        if (cur < nxt && nxt - cur < 0x20000) {
            start -= 4;
            count++;
        } else {
            break;
        }
    }

    /*
     * The real markers array starts at 0. If we did not land on 0, the
     * walk went into preceding data; scan forward for the first 0.
     */
    if (count >= 2 && *(const u32 *)start != 0) {
        const u8 *q;

        for (q = start; q < token_table; q += 4) {
            if (*(const u32 *)q == 0) {
                start = q;
                count = (u32)(token_table - q) / 4;
                break;
            }
        }
    }

    if (count < 2 || *(const u32 *)start != 0)
        return false;

    *markers_out = (const u32 *)start;
    *count_out = count;
    return true;
}

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

        if (decode_symbol(cand, names_end, v->token_index, v->token_table,
                          &off, tmp, sizeof(tmp)) < 0)
            break;

        if (off <= before)
            return false;

        if (++sym_in_group == 256) {
            sym_in_group = 0;
            if (k < v->marker_count) {
                if (off != v->markers[k])
                    return false;
                k++;
            }
        }
    }

    /*
     * All markers matched and decoding ended near the end of names. There
     * may be a little alignment padding between names and markers (16 bytes
     * of slack).
     */
    if (k != v->marker_count)
        return false;
    return off + 16 >= names_size;
}

/*
 * Try every plausible names start until the callback accepts one.
 *
 * Candidates satisfy names_size in
 * [markers[last], markers[last] + 256 * KSYM_NAME_MAX] and every 256-symbol
 * boundary matching markers.
 */
static const u8 *find_names_start(const u8 *buf_start,
                                  const struct kallsyms_view *v,
                                  bool (*try)(const u8 *names,
                                              const u8 *names_end, void *arg),
                                  void *arg)
{
    const u8 *names_end = v->names_end;
    const u8 *cand;
    u32 last_marker;
    u32 max_names_size;

    if (v->marker_count < 2)
        return NULL;
    last_marker = v->markers[v->marker_count - 1];
    if (last_marker < 1 ||
        (size_t)last_marker >= (size_t)(names_end - buf_start))
        return NULL;

    /*
     * names_size lies in [last_marker, last_marker + 256*KSYM_NAME_MAX]:
     * the last marker points at the start of the final group, which has at
     * most 256 symbols of at most KSYM_NAME_MAX bytes each.
     */
    max_names_size = last_marker + 256u * KSYM_NAME_MAX;
    if (max_names_size < last_marker) /* overflow guard */
        return NULL;

    cand = names_end - last_marker;

    for (; cand >= buf_start &&
           (size_t)(names_end - cand) <= max_names_size; cand--) {
        if (!names_start_valid(cand, v))
            continue;
        if (try(cand, names_end, arg))
            return cand;
        if (cand == buf_start)
            break;
    }
    return NULL;
}

/*
 * Given token_table, fill token_index / markers. Returns 0 on success.
 * The names start is left to the caller (there may be several candidates).
 */
static int view_from_token_table(const u8 *buf_start, const u8 *token_table,
                                 const u8 *buf_end, struct kallsyms_view *v)
{
    u32 tlen;
    int i;

    memset(v, 0, sizeof(*v));

    tlen = token_table_len(token_table, buf_end);
    if (!tlen)
        return -ENOENT;
    /* token_index is 8-byte aligned by output_label's ALGN */
    if (token_table + tlen + 7 + 512 > buf_end)
        return -ENOENT;

    v->token_table = token_table;
    v->tlen = tlen;
    v->token_index = (const u16 *)PTR_ALIGN(token_table + tlen, 8);

    if (v->token_index[0] != 0)
        return -ENOENT;
    for (i = 1; i < 256; i++) {
        if (v->token_index[i] <= v->token_index[i - 1] ||
            v->token_index[i] >= tlen)
            return -ENOENT;
    }

    if (!locate_markers(buf_start, token_table, &v->markers,
                        &v->marker_count))
        return -ENOENT;

    v->names_end = (const u8 *)v->markers;
    return 0;
}

/*
 * Full name decoding is streamed on demand (see consume_buffer): names are
 * never stored, only decoded into a small stack buffer.
 */

/*
 * Address array location.
 */

struct addr_info {
    bool                base_relative;
    const u32          *offsets;      /* BASE_RELATIVE mode */
    unsigned long       relative_base;
    const unsigned long *addresses;   /* absolute mode (kallsyms_addresses) */
};

static unsigned long symbol_addr(const struct addr_info *ai, u32 i)
{
    s32 off;

    if (ai->base_relative) {
        off = (s32)ai->offsets[i];
        if (off >= 0)
            return ai->relative_base + (u32)off;
        return ai->relative_base - 1 - (u32)off;
    }
    if (ai->addresses)
        return ai->addresses[i];
    return 0;
}

/*
 * Recover the address array and relative_base from the fixed on-disk layout.
 *
 * scripts/kallsyms.c emits, in this order:
 *     kallsyms_offsets[]        (u32 / .long per symbol)
 *     kallsyms_relative_base    (.quad, 8-byte aligned)
 *     kallsyms_num_syms         (.long, 8-byte aligned)
 *     kallsyms_names            (8-byte aligned)
 *
 * So relative_base is the u64 at names-16, num_syms the u32 at names-8, and
 * the offsets array starts at names-16-num_syms*4. num_syms must be
 * consistent with the number of markers, which validates the layout.
 */
static int resolve_addresses(const struct kallsyms_view *v,
                             const u8 *buf_start, const u8 *buf_end,
                             struct addr_info *ai, u32 *num_out)
{
    const u8 *names = v->names;
    unsigned long names_addr = (unsigned long)names;
    unsigned long rel_base;
    u32 num_syms;

    memset(ai, 0, sizeof(*ai));

    /* marker_count = ceil(num_syms / 256) */
    if (v->marker_count < 2)
        return -ENOENT;

    /* the two scalars sit just before names */
    if (names_addr < (unsigned long)buf_start + 24)
        return -ENOENT;

    memcpy(&num_syms, names - 8, sizeof(num_syms));
    if (num_syms == 0 ||
        num_syms <= (v->marker_count - 1) * 256u ||
        num_syms > v->marker_count * 256u)
        return -ENOENT;

    /* the offsets array plus the two scalars must be inside the buffer */
    if (names_addr < (unsigned long)buf_start + 16 +
                     (size_t)num_syms * sizeof(u32))
        return -ENOENT;
    if (names_addr + 16 > (unsigned long)buf_end)
        return -ENOENT;

    memcpy(&rel_base, names - 16, sizeof(rel_base));
    if (rel_base < 0xffff000000000000UL)
        return -ENOENT;

    ai->base_relative = true;
    ai->offsets = (const u32 *)(names_addr - 16 -
                                (size_t)num_syms * sizeof(u32));
    ai->relative_base = rel_base;
    *num_out = num_syms;
    return 0;
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
static bool try_names_candidate(const u8 *names, const u8 *names_end,
                                void *arg)
{
    struct try_ctx *ctx = arg;
    struct kallsyms_view v = *ctx->v;

    v.names = names;
    v.names_end = names_end;

    if (resolve_addresses(&v, ctx->buf, ctx->buf_end,
                          &ctx->ai, &ctx->num_syms) != 0)
        return false;

    ctx->ok = true;
    return true;
}

static int consume_buffer(const u8 *buf, unsigned long len,
                          const u8 *token_table)
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
        unsigned long a;

        if (decode_symbol(names, v.names_end, v.token_index,
                          v.token_table, &offset, nm, sizeof(nm)) < 0)
            break;

        a = symbol_addr(&ctx.ai, i);
        if (!a)
            continue;
        strip_symbol_suffix(nm);
        if (nm[0])
            ksym_cache_add(nm, a);
    }

    return 0;
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
    unsigned long pos;
    int found = 0;

    probe = kvmalloc(chunk, GFP_KERNEL);
    if (!probe)
        return -ENOMEM;

    /* include the final partial chunk rather than requiring pos+chunk<=end */
    for (pos = ALIGN(start, 8); pos < end; pos += step) {
        size_t len = min_t(unsigned long, chunk, end - pos);
        size_t off;
        bool hit = false;

        if (kread((const void *)pos, probe, len) == 0) {
            /* token_table is 8-byte aligned via ALGN; step 8 */
            for (off = 0; off + 16 <= len; off += 8) {
                const u8 *tt = probe + off;
                unsigned long tt_kaddr = pos + off;
                u8 *buf;
                unsigned long buf_len, buf_kaddr;

                if (!token_table_len(tt, probe + len))
                    continue;

                /* candidate hit: copy a window ending past token_table */
                buf_len = KSYM_BOUNCE_BACK + KSYM_BOUNCE_FWD;
                buf = kvmalloc(buf_len, GFP_KERNEL);
                if (!buf)
                    break;

                buf_kaddr = tt_kaddr > KSYM_BOUNCE_BACK ?
                            tt_kaddr - KSYM_BOUNCE_BACK : 0;

                kread_best_effort(buf, buf_kaddr, buf_len);

                if (consume_buffer(buf, buf_len,
                                   buf + (tt_kaddr - buf_kaddr)) == 0) {
                    found++;
                    hit = true;
                }
                kvfree(buf);

                if (hit)
                    break;
            }
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

static unsigned long read_vmcoreinfo_symbol(const char *sym)
{
    struct file *f;
    char *buf;
    loff_t pos = 0;
    ssize_t rd;
    size_t total = 0;
    unsigned long addr = 0;
    char key[64];
    char *p;

    snprintf(key, sizeof(key), "SYMBOL(%s)=", sym);

    f = filp_open(VMCOREINFO_PATH, O_RDONLY, 0);
    if (IS_ERR(f))
        return 0;

    buf = kvmalloc(SZ_64K, GFP_KERNEL);
    if (!buf) {
        filp_close(f, NULL);
        return 0;
    }

    /* the file can be much larger than a page; read it all */
    while (total < SZ_64K - 1) {
        rd = kernel_read(f, buf + total, SZ_64K - 1 - total, &pos);
        if (rd <= 0)
            break;
        total += (size_t)rd;
    }
    filp_close(f, NULL);
    buf[total] = '\0';

    p = strstr(buf, key);
    if (p) {
        p += strlen(key);
        addr = simple_strtoul(p, NULL, 16);
    }

    kvfree(buf);
    return addr;
}

/*
 * Scan the kernel image for kallsyms. Caller must hold ksym_lock.
 */
static void scan_kernel_memory_locked(void)
{
    unsigned long stext, img_size, anchor;
    int found;

    /*
     * Anchor the scan to the runtime address of an exported vmlinux symbol
     * that this module already links against. The kernel resolves it after
     * KASLR, so it points inside the real kernel image without us having to
     * know the KASLR base. The kallsyms tables live in the image's rodata,
     * well within +/-64MB of the text.
     */
    anchor = (unsigned long)&copy_from_kernel_nofault;
    if (anchor) {
        unsigned long lo = anchor > SZ_256M ? anchor - SZ_256M : 0;
        unsigned long hi = anchor + SZ_256M;

        found = scan_region(lo, hi);
        if (found)
            return;
    }

    /* Fallback 1: vmcoreinfo gives the image base directly when readable. */
    stext = read_vmcoreinfo_symbol("_stext");
    if (!stext)
        stext = read_vmcoreinfo_symbol("_text");
    img_size = read_vmcoreinfo_symbol("KERNEL_IMAGE_SIZE");

    if (stext) {
        if (!img_size || img_size > SZ_1G)
            img_size = SZ_512M;
        found = scan_region(stext, stext + img_size);
        if (found)
            return;
    }

    /* Fallback 2: common arm64 KIMAGE bases. */
    {
        static const unsigned long bases[] = {
            0xffffff8000000000UL,  /* 39-bit VA */
            0xffff800000000000UL,  /* 48-bit VA */
        };
        int i;

        for (i = 0; i < ARRAY_SIZE(bases); i++) {
            found = scan_region(bases[i], bases[i] + SZ_1G);
            if (found)
                break;
        }
    }
}

/*
 * /proc/kallsyms
 */

/*
 * Parse one "addr type name" kallsyms line in place. Returns the address if
 * the name matches, otherwise 0. `line` is modified (delimiters -> NUL).
 */
static unsigned long match_kallsyms_line(char *line, const char *name,
                                         size_t namelen)
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

    if (sym[0] && strlen(sym) == namelen &&
        memcmp(sym, name, namelen) == 0) {
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
static unsigned long lookup_proc_kallsyms(const char *name)
{
    struct file *f;
    char *buf;
    loff_t pos = 0;
    ssize_t rd;
    unsigned long addr = 0;
    size_t namelen = strlen(name);
    size_t used = 0;

    if (ksym_proc_hidden)
        return 0;

    f = filp_open("/proc/kallsyms", O_RDONLY, 0);
    if (IS_ERR(f))
        return 0;

    buf = kvmalloc(SZ_64K, GFP_KERNEL);
    if (!buf) {
        filp_close(f, NULL);
        return 0;
    }

    for (;;) {
        char *p, *line;

        if (used >= SZ_64K - 1) {
            /* defensive: never let the buffer overrun */
            used = 0;
        }

        rd = kernel_read(f, buf + used, SZ_64K - 1 - used, &pos);
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
                goto out;
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
                if (linelen != (size_t)(line - buf)) {
                    /* move partial line to the front for the next round */
                    memmove(buf, line, linelen + 1);
                }
                used = linelen;
                break;
            }

            addr = match_kallsyms_line(line, name, namelen);
            if (addr)
                goto out;
        }
    }

out:
    kvfree(buf);
    filp_close(f, NULL);
    return addr;
}

/*
 * Public interface
 */

unsigned long nksu_ksym_lookup(const char *name)
{
    unsigned long addr;
    struct ksym_entry *e;

    if (!name || !name[0])
        return 0;

    mutex_lock(&ksym_lock);

    e = ksym_find_locked(name);
    if (e) {
        addr = e->addr;
        mutex_unlock(&ksym_lock);
        return addr;
    }

    /* 1) /proc/kallsyms */
    addr = lookup_proc_kallsyms(name);
    if (addr) {
        ksym_cache_add(name, addr);
        mutex_unlock(&ksym_lock);
        return addr;
    }

    /*
     * 2) Scan kernel memory once, then re-check the cache. The scan is
     *    expensive, so it must never run twice even if it finds nothing.
     */
    if (!ksym_scanned) {
        ksym_scanned = true;
        scan_kernel_memory_locked();

        e = ksym_find_locked(name);
        addr = e ? e->addr : 0;
    } else {
        addr = 0;
    }

    mutex_unlock(&ksym_lock);
    return addr;
}

unsigned long nksu_ksym_count(void)
{
    return (unsigned long)atomic_read(&ksym_count);
}

void nksu_ksym_cache_clear(void)
{
    struct ksym_entry *e;
    struct hlist_node *tmp;
    int bkt;

    mutex_lock(&ksym_lock);
    hash_for_each_safe(ksym_htable, bkt, tmp, e, node) {
        hash_del(&e->node);
        kfree(e);
    }
    atomic_set(&ksym_count, 0);
    ksym_scanned = false;
    ksym_proc_hidden = false;
    ksym_proc_checked = false;
    mutex_unlock(&ksym_lock);
}

#ifdef CONFIG_NKSU_DEBUG
void nksu_ksym_dump(void)
{
    struct ksym_entry *e;
    int bkt;

    mutex_lock(&ksym_lock);
    hash_for_each(ksym_htable, bkt, e, node)
        pr_info("[ksym] %s = 0x%lx\n", e->name, e->addr);
    mutex_unlock(&ksym_lock);
}
#else
void nksu_ksym_dump(void)
{
}
#endif
