// SPDX-License-Identifier: GPL-3.0-or-later

#include "btf_parser.h"

#include <linux/types.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/limits.h>
#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/string.h>
#include <linux/bpf.h>
#include <linux/btf.h>
#include "symbol/symbol_compat.h"

#if LINUX_VERSION_CODE <= KERNEL_VERSION(6, 0, 0)
static inline const struct btf_member *btf_members(const struct btf_type *t)
{
	return (const struct btf_member *)(t + 1);
}
#endif

static const struct btf_type *btf_resolve_type(const struct btf *btf, u32 id)
{
	return btf_type_skip_modifiers(btf, id, NULL);
}

static const struct btf_type *btf_find_composite(const struct btf *btf, const char *name)
{
	const struct btf_type *type;
	s32 id;

	id = btf_find_by_name_kind(btf, name, BTF_KIND_STRUCT);
	if (id < 0) {
		id = btf_find_by_name_kind(btf, name, BTF_KIND_UNION);
		if (id < 0)
			return ERR_PTR(id);
	}

	type = btf_type_by_id(btf, id);
	return type ?: ERR_PTR(-EINVAL);
}

static int btf_get_type_size(const struct btf *btf, u32 type_id, u32 *size)
{
	const struct btf_type *type;

	type = btf_resolve_type(btf, type_id);
	if (!type)
		return -EINVAL;

	switch (BTF_INFO_KIND(type->info)) {
	case BTF_KIND_INT:
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 13, 0)
	case BTF_KIND_FLOAT:
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 0, 0)
	case BTF_KIND_ENUM64:
#endif
	case BTF_KIND_ENUM:
	case BTF_KIND_STRUCT:
	case BTF_KIND_UNION:
	case BTF_KIND_DATASEC:
		*size = type->size;
		return 0;

	case BTF_KIND_PTR:
		*size = sizeof(void *);
		return 0;

	case BTF_KIND_ARRAY: {
		const struct btf_array *array = (const void *)(type + 1);
		u32 element_size;
		int ret;

		ret = btf_get_type_size(btf, array->type, &element_size);
		if (ret)
			return ret;

		if (array->nelems && element_size > U32_MAX / array->nelems)
			return -EOVERFLOW;

		*size = element_size * array->nelems;
		return 0;
	}

	default:
		return -EOPNOTSUPP;
	}
}

static int btf_validate_member(const struct btf *btf, u32 type_id, u32 expected_kind,
			       u32 expected_size, u32 *size)
{
	const struct btf_type *type;
	u32 actual_size;
	int ret;

	type = btf_resolve_type(btf, type_id);
	if (!type)
		return -EINVAL;

	if (expected_kind && BTF_INFO_KIND(type->info) != expected_kind)
		return -EINVAL;

	ret = btf_get_type_size(btf, type_id, &actual_size);
	if (ret)
		return ret;

	if (expected_size && actual_size != expected_size)
		return -EINVAL;

	*size = actual_size;
	return 0;
}

static int btf_get_field_from_type(const struct btf *btf, const struct btf_type *type,
				   const char *member_name, u32 expected_kind, u32 expected_size,
				   u32 *offset_out)
{
	const struct btf_member *members;
	const struct btf_member *member;
	const char *name;
	u32 vlen, i, bits, offset, member_size;
	int ret;

	vlen = BTF_INFO_VLEN(type->info);
	members = btf_members(type);

	for (i = 0; i < vlen; i++) {
		member = &members[i];

		name = btf_name_by_offset(btf, member->name_off);
		if (!name)
			return -EINVAL;

		if (strcmp(name, member_name))
			continue;

		bits = member->offset;
		if (BTF_INFO_KFLAG(type->info)) {
			if (BTF_MEMBER_BITFIELD_SIZE(bits))
				return -EOPNOTSUPP;
			bits = BTF_MEMBER_BIT_OFFSET(bits);
		}

		if (bits & 7)
			return -EINVAL;

		offset = bits / 8;

		ret = btf_validate_member(btf, member->type, expected_kind, expected_size,
					  &member_size);
		if (ret)
			return ret;

		if (offset > type->size || member_size > type->size - offset)
			return -EINVAL;

		*offset_out = offset;
		return 0;
	}

	return -ENOENT;
}

int btf_get_fields(const struct btf *btf, const char *struct_name, struct nksu_btf_field *fields,
		   u32 count)
{
	const struct btf_type *type;
	u32 i;
	int ret;

	if (!btf || !struct_name || !fields || !count)
		return -EINVAL;

	type = btf_find_composite(btf, struct_name);
	if (IS_ERR(type)) {
		ret = PTR_ERR(type);
		pr_err("cannot find BTF type %s: %d\n", struct_name, ret);
		return ret;
	}
	if (!type)
		return -EINVAL;

	for (i = 0; i < count; i++) {
		if (!fields[i].name)
			return -EINVAL;

		ret = btf_get_field_from_type(btf, type, fields[i].name, fields[i].kind,
					      fields[i].size, &fields[i].offset);
		if (ret) {
			pr_err("cannot resolve BTF field %s.%s: %d\n", struct_name, fields[i].name,
			       ret);
			return ret;
		}
	}

	return 0;
}
