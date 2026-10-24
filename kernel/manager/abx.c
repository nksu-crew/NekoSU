// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Android Binary XML (ABX) reader, split out of manager.c.
 *
 * Only what the manager scan needs: find the <package> whose signing
 * certificate the caller accepts, plus its name, user id and codePath /
 * nativeLibraryPath attributes.  See manager/abx.h for the wire format.
 */

#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>

#include "kernel_compat.h"
#include "manager/abx.h"

#define ABX_TOKEN_START_DOCUMENT 0
#define ABX_TOKEN_END_DOCUMENT 1
#define ABX_TOKEN_START_TAG 2
#define ABX_TOKEN_END_TAG 3
#define ABX_TOKEN_TEXT 4
#define ABX_TOKEN_ATTRIBUTE 15

#define ABX_TYPE_NULL 0x10
#define ABX_TYPE_STRING 0x20
#define ABX_TYPE_STRING_INTERNED 0x30
#define ABX_TYPE_BYTES_HEX 0x40
#define ABX_TYPE_BYTES_BASE64 0x50
#define ABX_TYPE_INT 0x60
#define ABX_TYPE_INT_HEX 0x70
#define ABX_TYPE_LONG 0x80
#define ABX_TYPE_LONG_HEX 0x90
#define ABX_TYPE_FLOAT 0xa0
#define ABX_TYPE_DOUBLE 0xb0
#define ABX_TYPE_BOOLEAN_TRUE 0xc0
#define ABX_TYPE_BOOLEAN_FALSE 0xd0

#define ABX_INTERNED_SENTINEL 0xffff
#define ABX_MAX_INTERNED_STRINGS 512

#define ABX_MAGIC "ABX\0"
#define ABX_MAGIC_LEN 4

struct abx_reader {
	const u8 *buf;
	size_t len;
	size_t pos;
	const u8 **interned;
	size_t *interned_len;
	int interned_count;
};

static inline int abx_read_byte(struct abx_reader *r, u8 *out)
{
	if (r->pos >= r->len)
		return -1;
	*out = r->buf[r->pos++];
	return 0;
}

static inline int abx_read_u16(struct abx_reader *r, u16 *out)
{
	if (r->pos + 2 > r->len)
		return -1;
	*out = (u16)((r->buf[r->pos] << 8) | r->buf[r->pos + 1]);
	r->pos += 2;
	return 0;
}

static inline int abx_read_u64(struct abx_reader *r, u64 *out)
{
	if (r->pos + 8 > r->len)
		return -1;
	*out = ((u64)r->buf[r->pos] << 56) | ((u64)r->buf[r->pos + 1] << 48) |
	       ((u64)r->buf[r->pos + 2] << 40) | ((u64)r->buf[r->pos + 3] << 32) |
	       ((u64)r->buf[r->pos + 4] << 24) | ((u64)r->buf[r->pos + 5] << 16) |
	       ((u64)r->buf[r->pos + 6] << 8) | (u64)r->buf[r->pos + 7];
	r->pos += 8;
	return 0;
}

/* Read a plain (non-interned) UTF-8 string; returns a view into the buffer. */
static int abx_read_utf(struct abx_reader *r, const u8 **str, size_t *len)
{
	u16 n;

	if (abx_read_u16(r, &n) < 0)
		return -1;
	if (r->pos + n > r->len)
		return -1;
	*str = r->buf + r->pos;
	*len = n;
	r->pos += n;
	return 0;
}

/*
 * Read a string written through writeInternedUTF(). The first occurrence
 * carries a 0xffff sentinel followed by the full string, which is appended to
 * the intern table; later occurrences are an index into the table.
 */
static int abx_read_interned(struct abx_reader *r, const u8 **str, size_t *len)
{
	u16 ref;

	if (abx_read_u16(r, &ref) < 0)
		return -1;
	if (ref == ABX_INTERNED_SENTINEL) {
		if (abx_read_utf(r, str, len) < 0)
			return -1;
		if (r->interned_count >= ABX_MAX_INTERNED_STRINGS)
			return -1;
		r->interned[r->interned_count] = *str;
		r->interned_len[r->interned_count] = *len;
		r->interned_count++;
	} else {
		if (ref >= r->interned_count)
			return -1;
		*str = r->interned[ref];
		*len = r->interned_len[ref];
	}
	return 0;
}

static inline bool abx_str_eq(const u8 *s, size_t len, const char *lit)
{
	size_t l = strlen(lit);

	return len == l && !memcmp(s, lit, l);
}

static void copy_path_attr(char *dst, size_t cap, const u8 *src, size_t len)
{
	size_t n = 0;

	if (src && len) {
		n = len < cap - 1 ? len : cap - 1;
		memcpy(dst, src, n);
	}
	dst[n] = '\0';
}

enum abx_result abx_find_package_by_cert(const u8 *buf, size_t len, abx_cert_match match, void *ctx,
					 struct abx_package *out)
{
	struct abx_reader r = { .buf = buf, .len = len, .pos = ABX_MAGIC_LEN };
	const u8 *name;
	size_t nlen;
	int depth = 0;
	int pkg_depth = -1;
	int sigs_depth = -1;
	enum abx_result result = ABX_PARSE_ERROR;
	bool document_complete = false;
	const u8 *cert = NULL;
	size_t cert_len = 0;
	/* Whether the current package's certificate has been offered to match().
	 * Only the first <cert> (the current signer) is checked. */
	bool cert_checked = false;

	memset(out, 0, sizeof(*out));

	if (!match || len < ABX_MAGIC_LEN || memcmp(buf, ABX_MAGIC, ABX_MAGIC_LEN) != 0)
		return ABX_PARSE_ERROR;

	r.interned = kvmalloc_array(ABX_MAX_INTERNED_STRINGS, sizeof(*r.interned), GFP_KERNEL);
	r.interned_len =
		kvmalloc_array(ABX_MAX_INTERNED_STRINGS, sizeof(*r.interned_len), GFP_KERNEL);
	if (!r.interned || !r.interned_len)
		goto out_free;

	while (r.pos < r.len) {
		u8 ev, tok, typ;

		if (abx_read_byte(&r, &ev) < 0)
			goto out_free;
		tok = ev & 0x0f;
		typ = ev & 0xf0;

		if (tok == ABX_TOKEN_START_DOCUMENT) {
			continue;
		} else if (tok == ABX_TOKEN_END_DOCUMENT) {
			document_complete = (depth == 0);
			break;
		} else if (tok == ABX_TOKEN_START_TAG) {
			bool is_sigs, is_cert, is_pkg;
			const u8 *name_attr = NULL;
			size_t name_attr_len = 0;
			const u8 *code_attr = NULL;
			size_t code_attr_len = 0;
			const u8 *nlib_attr = NULL;
			size_t nlib_attr_len = 0;
			u32 shared_uid = 0, own_uid = 0;
			bool shared_seen = false, own_seen = false;

			if (abx_read_interned(&r, &name, &nlen) < 0)
				goto out_free;
			depth++;

			is_sigs = (pkg_depth > 0 && sigs_depth < 0) && depth == pkg_depth + 1 &&
				  abx_str_eq(name, nlen, "sigs");
			is_cert = (sigs_depth > 0) && depth == sigs_depth + 1 &&
				  abx_str_eq(name, nlen, "cert");

			/* Consume attributes (if any) until the next non-attribute event. */
			while (r.pos < r.len) {
				size_t save = r.pos;
				u8 ev2, typ2;
				const u8 *aname;
				size_t alen;

				if (abx_read_byte(&r, &ev2) < 0)
					break;
				if ((ev2 & 0x0f) != ABX_TOKEN_ATTRIBUTE) {
					r.pos = save;
					break;
				}
				typ2 = ev2 & 0xf0;

				if (abx_read_interned(&r, &aname, &alen) < 0)
					goto out_free;

				if (is_cert && abx_str_eq(aname, alen, "key") &&
				    (typ2 == ABX_TYPE_BYTES_HEX || typ2 == ABX_TYPE_BYTES_BASE64)) {
					u16 blen;

					if (abx_read_u16(&r, &blen) < 0 || r.pos + blen > r.len)
						goto out_free;
					cert = r.buf + r.pos;
					cert_len = blen;
					r.pos += blen;
				} else if (typ2 == ABX_TYPE_STRING) {
					u16 slen;

					if (abx_read_u16(&r, &slen) < 0 || r.pos + slen > r.len)
						goto out_free;
					if (abx_str_eq(aname, alen, "name")) {
						name_attr = r.buf + r.pos;
						name_attr_len = slen;
					} else if (abx_str_eq(aname, alen, "codePath")) {
						code_attr = r.buf + r.pos;
						code_attr_len = slen;
					} else if (abx_str_eq(aname, alen, "nativeLibraryPath")) {
						nlib_attr = r.buf + r.pos;
						nlib_attr_len = slen;
					}
					r.pos += slen;
				} else if (typ2 == ABX_TYPE_STRING_INTERNED) {
					const u8 *sv;
					size_t svlen;

					if (abx_read_interned(&r, &sv, &svlen) < 0)
						goto out_free;
					if (abx_str_eq(aname, alen, "name")) {
						name_attr = sv;
						name_attr_len = svlen;
					} else if (abx_str_eq(aname, alen, "codePath")) {
						code_attr = sv;
						code_attr_len = svlen;
					} else if (abx_str_eq(aname, alen, "nativeLibraryPath")) {
						nlib_attr = sv;
						nlib_attr_len = svlen;
					}
				} else if (typ2 == ABX_TYPE_INT || typ2 == ABX_TYPE_INT_HEX) {
					u32 v;

					if (r.pos + 4 > r.len)
						goto out_free;
					v = ((u32)r.buf[r.pos] << 24) |
					    ((u32)r.buf[r.pos + 1] << 16) |
					    ((u32)r.buf[r.pos + 2] << 8) | (u32)r.buf[r.pos + 3];
					r.pos += 4;
					if (abx_str_eq(aname, alen, "sharedUserId")) {
						shared_uid = v;
						shared_seen = true;
					} else if (abx_str_eq(aname, alen, "userId")) {
						own_uid = v;
						own_seen = true;
					}
				} else if (typ2 == ABX_TYPE_BYTES_HEX ||
					   typ2 == ABX_TYPE_BYTES_BASE64) {
					u16 blen;

					if (abx_read_u16(&r, &blen) < 0 || r.pos + blen > r.len)
						goto out_free;
					r.pos += blen;
				} else if (typ2 == ABX_TYPE_FLOAT) {
					if (r.pos + 4 > r.len)
						goto out_free;
					r.pos += 4;
				} else if (typ2 == ABX_TYPE_LONG || typ2 == ABX_TYPE_LONG_HEX ||
					   typ2 == ABX_TYPE_DOUBLE) {
					u64 lv;

					if (abx_read_u64(&r, &lv) < 0)
						goto out_free;
				} else if (typ2 == ABX_TYPE_BOOLEAN_TRUE ||
					   typ2 == ABX_TYPE_BOOLEAN_FALSE ||
					   typ2 == ABX_TYPE_NULL) {
					/* No payload */
				} else {
					goto out_free;
				}
			}

			/* Every top-level <package> is a candidate; the caller's
			 * match() decides which one is the manager. */
			is_pkg = (pkg_depth < 0) && abx_str_eq(name, nlen, "package");

			if (is_pkg) {
				pkg_depth = depth;
				cert = NULL;
				cert_len = 0;
				cert_checked = false;
				copy_path_attr(out->name, sizeof(out->name), name_attr,
					       name_attr_len);
				copy_path_attr(out->code_path, sizeof(out->code_path), code_attr,
					       code_attr_len);
				copy_path_attr(out->native_lib_path, sizeof(out->native_lib_path),
					       nlib_attr, nlib_attr_len);
				out->uid = shared_seen ? shared_uid : own_uid;
				out->has_uid = shared_seen || own_seen;
			}
			if (is_sigs)
				sigs_depth = depth;

			if (cert && cert_len > 0) {
				if (!cert_checked) {
					cert_checked = true;
					if (match(cert, cert_len, ctx))
						goto check;
				}
				/* Not the package we want (or a past cert we do not
				 * check); forget it and keep scanning. */
				cert = NULL;
				cert_len = 0;
			}
		} else if (tok == ABX_TOKEN_END_TAG) {
			if (abx_read_interned(&r, &name, &nlen) < 0)
				goto out_free;
			if (depth == pkg_depth) {
				pkg_depth = -1;
				cert = NULL;
				cert_len = 0;
			}
			if (depth == sigs_depth)
				sigs_depth = -1;
			depth--;
			if (depth < 0)
				goto out_free;
		} else if (tok == ABX_TOKEN_TEXT || tok == 5 || tok == 6 || tok == 7 || tok == 8 ||
			   tok == 9 || tok == 10) {
			/* TEXT(4) CDSECT(5) ENTITY_REF(6) IGNORABLE_WHITESPACE(7)
			 * PROCESSING_INSTRUCTION(8) COMMENT(9) DOCDECL(10) */
			if (typ == ABX_TYPE_STRING) {
				u16 slen;

				if (abx_read_u16(&r, &slen) < 0 || r.pos + slen > r.len)
					goto out_free;
				r.pos += slen;
			} else if (typ == ABX_TYPE_STRING_INTERNED) {
				if (abx_read_interned(&r, &name, &nlen) < 0)
					goto out_free;
			} else if (typ == ABX_TYPE_NULL) {
				/* No payload */
			} else {
				goto out_free;
			}
		} else {
			goto out_free;
		}
	}
	goto out_free;

check:
	out->cert = cert;
	out->cert_len = cert_len;
	result = ABX_PACKAGE_FOUND;

out_free:
	kvfree(r.interned);
	kvfree(r.interned_len);
	if (result == ABX_PARSE_ERROR && document_complete)
		result = ABX_PACKAGE_NOT_FOUND;
	return result;
}
