// SPDX-License-Identifier: GPL-3.0-or-later

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include "symbol/symbol.h"
#include "btf_parser.h"

static struct btf **nksu_btf_vmlinux_sym;

int nksu_btf_resolve_fields(const char *type_name, struct nksu_btf_field *fields, u32 count)
{
	struct btf **vmlinux;
	struct btf *btf;
	unsigned long sym;

	if (!type_name || !fields || !count)
		return -EINVAL;

	vmlinux = READ_ONCE(nksu_btf_vmlinux_sym);
	if (!vmlinux) {
		sym = NKSU_KSYM_LOOKUP("btf_vmlinux");
		if (!sym) {
			pr_err("btf_vmlinux symbol is unresolved\n");
			return -ENOENT;
		}

		vmlinux = (struct btf **)sym;
		WRITE_ONCE(nksu_btf_vmlinux_sym, vmlinux);
	}

	btf = READ_ONCE(*vmlinux);
	if (!btf) {
		pr_err("kernel BTF is unavailable\n");
		return -ENODEV;
	}

	return btf_get_fields(btf, type_name, fields, count);
}
