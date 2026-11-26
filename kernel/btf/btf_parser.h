// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef INFRA_BTF_PARSER_H
#define INFRA_BTF_PARSER_H

#include <linux/types.h>

struct btf;

/**
 * struct nksu_btf_field - one member lookup request/result
 * @name: member name
 * @kind: expected BTF_KIND_*, or 0 to skip the kind check
 * @size: expected member size in bytes, or 0 to skip the size check
 * @offset: resolved byte offset
 */
struct nksu_btf_field {
	const char *name;
	u32 kind;
	u32 size;
	u32 offset;
};

/*
 * Resolve a batch of members from the same structure/union.  The BTF type is
 * looked up once; each requested member is then validated independently.
 *
 * Returns 0 on success, a negative errno otherwise.
 */
int btf_get_fields(const struct btf *btf, const char *struct_name, struct nksu_btf_field *fields,
		   u32 count);

/*
 * Resolve fields against the running kernel's vmlinux BTF.
 *
 * Returns 0 on success, a negative errno otherwise.
 */
int nksu_btf_resolve_fields(const char *type_name, struct nksu_btf_field *fields, u32 count);

#endif
