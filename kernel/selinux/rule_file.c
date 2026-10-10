// SPDX-License-Identifier: GPL-2.0
/*
 * Decoder for the sepolicy batch the daemon sends through IOC_SET_SEPOLICY.
 *
 * The wire format is the same as KernelSU's (uapi/selinux.h): a stream of
 * { cmd, subcmd } headers, each followed by N length-prefixed arguments where a
 * zero length encodes the wildcard.  Every decoded command is dispatched to
 * the NekoSU rule engine (kernel/selinux/rule.c).
 */

#include <linux/build_bug.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/uaccess.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/limits.h>

#include <fmac.h>
#include "selinux/nksu_uapi_selinux.h"
#include "ss/avtab.h"

struct nksu_sepol_cmd {
	u32 cmd;
	u32 subcmd;
};

/* The batch header must be a bare { u32, u32 } pair (C11 static_assert). */
static_assert(sizeof(struct nksu_sepol_cmd) == 2 * sizeof(u32),
	      "sepolicy command header must be two u32s");

struct nksu_sepol_cursor {
	const u8 *cur;
	const u8 *end;
};

static size_t cursor_remaining(const struct nksu_sepol_cursor *c)
{
	return (size_t)(c->end - c->cur);
}

static int read_cmd(struct nksu_sepol_cursor *c, struct nksu_sepol_cmd *out)
{
	if (cursor_remaining(c) < sizeof(*out))
		return -EINVAL;
	memcpy(out, c->cur, sizeof(*out));
	c->cur += sizeof(*out);
	return 0;
}

/* Returns the string (or NULL for the wildcard ALL). */
static int read_string(struct nksu_sepol_cursor *c, const char **out)
{
	u32 len;
	const char *str;

	if (cursor_remaining(c) < sizeof(len))
		return -EINVAL;
	memcpy(&len, c->cur, sizeof(len));
	c->cur += sizeof(len);

	if (len >= cursor_remaining(c))
		return -EINVAL;

	str = (const char *)c->cur;
	if (memchr(str, '\0', len) != NULL || str[len] != '\0')
		return -EINVAL;

	c->cur += len + 1;
	*out = (len == 0) ? NULL : str;
	return 0;
}

static int expected_argc(u32 cmd)
{
	switch (cmd) {
	case NKSU_SEPOLICY_CMD_NORMAL_PERM:
		return 4;
	case NKSU_SEPOLICY_CMD_XPERM:
		return 5;
	case NKSU_SEPOLICY_CMD_TYPE_STATE:
	case NKSU_SEPOLICY_CMD_ATTR:
		return 1;
	case NKSU_SEPOLICY_CMD_TYPE:
	case NKSU_SEPOLICY_CMD_TYPE_ATTR:
		return 2;
	case NKSU_SEPOLICY_CMD_TYPE_TRANSITION:
		return 5;
	case NKSU_SEPOLICY_CMD_TYPE_CHANGE:
		return 4;
	case NKSU_SEPOLICY_CMD_GENFSCON:
		return 3;
	default:
		return -EINVAL;
	}
}

static int require(const char *v)
{
	return v ? 0 : -EINVAL;
}

static int apply_one(const struct nksu_sepol_cmd *h, const char *const *a)
{
	switch (h->cmd) {
	case NKSU_SEPOLICY_CMD_NORMAL_PERM:
		switch (h->subcmd) {
		case NKSU_SEPOLICY_SUBCMD_NORMAL_PERM_ALLOW:
			return sepolicy_add_rule(a[0], a[1], a[2], a[3], AVTAB_ALLOWED, false);
		case NKSU_SEPOLICY_SUBCMD_NORMAL_PERM_DENY:
			return sepolicy_add_rule(a[0], a[1], a[2], a[3], AVTAB_ALLOWED, true);
		case NKSU_SEPOLICY_SUBCMD_NORMAL_PERM_AUDITALLOW:
			return sepolicy_add_rule(a[0], a[1], a[2], a[3], AVTAB_AUDITALLOW, false);
		case NKSU_SEPOLICY_SUBCMD_NORMAL_PERM_DONTAUDIT:
			return sepolicy_add_rule(a[0], a[1], a[2], a[3], AVTAB_AUDITDENY, true);
		}
		return -EINVAL;

	case NKSU_SEPOLICY_CMD_XPERM:
		if (require(a[3]) || require(a[4]))
			return -EINVAL;
		switch (h->subcmd) {
		case NKSU_SEPOLICY_SUBCMD_XPERM_ALLOW:
			return sepolicy_add_xperm(a[0], a[1], a[2], a[4], AVTAB_XPERMS_ALLOWED, false);
		case NKSU_SEPOLICY_SUBCMD_XPERM_AUDITALLOW:
			return sepolicy_add_xperm(a[0], a[1], a[2], a[4], AVTAB_XPERMS_AUDITALLOW, false);
		case NKSU_SEPOLICY_SUBCMD_XPERM_DONTAUDIT:
			return sepolicy_add_xperm(a[0], a[1], a[2], a[4], AVTAB_XPERMS_DONTAUDIT, false);
		}
		return -EINVAL;

	case NKSU_SEPOLICY_CMD_TYPE_STATE:
		if (require(a[0]))
			return -EINVAL;
		if (h->subcmd == NKSU_SEPOLICY_SUBCMD_TYPE_STATE_PERMISSIVE)
			return sepolicy_set_permissive(a[0]);
		if (h->subcmd == NKSU_SEPOLICY_SUBCMD_TYPE_STATE_ENFORCE)
			return sepolicy_set_enforce(a[0]);
		return -EINVAL;

	case NKSU_SEPOLICY_CMD_TYPE:
		if (require(a[0]) || require(a[1]))
			return -EINVAL;
		if (sepolicy_add_type(a[0]))
			return -EINVAL;
		return sepolicy_add_typeattribute(a[0], a[1]);

	case NKSU_SEPOLICY_CMD_TYPE_ATTR:
		if (require(a[0]) || require(a[1]))
			return -EINVAL;
		return sepolicy_add_typeattribute(a[0], a[1]);

	case NKSU_SEPOLICY_CMD_ATTR:
		if (require(a[0]))
			return -EINVAL;
		return sepolicy_add_attribute(a[0]);

	case NKSU_SEPOLICY_CMD_TYPE_TRANSITION:
		if (require(a[0]) || require(a[1]) || require(a[2]) || require(a[3]))
			return -EINVAL;
		return sepolicy_add_type_transition(a[0], a[1], a[2], a[3], a[4]);

	case NKSU_SEPOLICY_CMD_TYPE_CHANGE:
		if (require(a[0]) || require(a[1]) || require(a[2]) || require(a[3]))
			return -EINVAL;
		if (h->subcmd == NKSU_SEPOLICY_SUBCMD_TYPE_CHANGE_CHANGE)
			return sepolicy_add_type_change(a[0], a[1], a[2], a[3]);
		if (h->subcmd == NKSU_SEPOLICY_SUBCMD_TYPE_CHANGE_MEMBER)
			return sepolicy_add_type_member(a[0], a[1], a[2], a[3]);
		return -EINVAL;

	case NKSU_SEPOLICY_CMD_GENFSCON:
		if (require(a[0]) || require(a[1]) || require(a[2]))
			return -EINVAL;
		return sepolicy_add_genfscon(a[0], a[1], a[2]);

	default:
		return -EINVAL;
	}
}

int sepolicy_apply_batch(const void __user *user_data, size_t data_len)
{
	struct nksu_sepol_cursor cursor;
	u8 *payload;
	int applied = 0;

	if (!user_data || !data_len)
		return -EINVAL;
	if (data_len > NKSU_SEPOLICY_MAX_BATCH_SIZE)
		return -E2BIG;

	payload = kvmalloc(data_len, GFP_KERNEL);
	if (!payload)
		return -ENOMEM;
	if (copy_from_user(payload, user_data, data_len)) {
		kvfree(payload);
		return -EFAULT;
	}

	cursor.cur = payload;
	cursor.end = payload + data_len;

	while (cursor.cur < cursor.end) {
		struct nksu_sepol_cmd h;
		const char *args[NKSU_SEPOLICY_MAX_ARGS] = { 0 };
		int argc, i, ret;

		ret = read_cmd(&cursor, &h);
		if (ret)
			break;

		argc = expected_argc(h.cmd);
		if (argc < 0 || argc > NKSU_SEPOLICY_MAX_ARGS)
			break;

		for (i = 0; i < argc; i++) {
			ret = read_string(&cursor, &args[i]);
			if (ret)
				goto out;
		}

		if (apply_one(&h, args) == 0)
			applied++;
		else
			pr_info("[selinux]: batch cmd %u/%u skipped\n", h.cmd, h.subcmd);
	}

out:
	kvfree(payload);
	return applied;
}
