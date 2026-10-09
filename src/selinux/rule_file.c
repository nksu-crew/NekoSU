// SPDX-License-Identifier: GPL-2.0
/*
 * sepolicy.rule loader — parse a KernelSU/Magisk-style rule file and push
 * every statement into the live policy.
 *
 * Module rule files live at /data/adb/modules/<id>/sepolicy.rule and follow
 * the KernelSU syntax (a whitespace-separated subset of the SELinux policy
 * language):
 *
 *   allow      <src> <tgt> <cls> <perm>
 *   deny       <src> <tgt> <cls> <perm>
 *   auditallow <src> <tgt> <cls> <perm>
 *   dontaudit  <src> <tgt> <cls> <perm>
 *   allowxperm      <src> <tgt> <cls> ioctl <set>
 *   auditallowxperm <src> <tgt> <cls> ioctl <set>
 *   dontauditxperm  <src> <tgt> <cls> ioctl <set>
 *   typeattribute <type> <attr>          (alias: attradd)
 *   type <name> [<attrs>]
 *
 * where each object is a word, "*", or "{ word word ... }".  The classic
 * "tgt:cls" form (e.g. "allow foo bar:file read") is accepted as well.
 *
 * Statements are separated by newlines and ';'; a line whose first
 * non-blank character is '#' is a comment.
 *
 * Statements the kernel-side API cannot express yet (permissive/enforce,
 * type_transition/type_change/type_member, genfscon, attribute) are logged
 * and skipped, matching KernelSU's non-strict behaviour.  A module rule
 * that names a type/class/perm this device does not have is skipped by the
 * rule writer itself, so one bad line never aborts the whole file.
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/printk.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/limits.h>

#include <fmac.h>

#include "ss/avtab.h"

#define NKSU_RULE_LINE_MAX   2048
#define NKSU_RULE_MAX_TOKENS 96
#define NKSU_RULE_MAX_ITEMS  64

struct nksu_tok {
	/* 0 for a word, otherwise the literal '{' or '}' */
	char  kind;
	char *p;
};

struct nksu_seobj {
	char *v[NKSU_RULE_MAX_ITEMS];
	int   n;
};

struct nksu_rule_scratch {
	char             line[NKSU_RULE_LINE_MAX];
	struct nksu_tok  tok[NKSU_RULE_MAX_TOKENS];
	struct nksu_seobj se[4];
};

/*
 * Split @s in place into whitespace-separated words and brace tokens.
 * Words are NUL-terminated; "{"/"}" become their own one-character tokens
 * so that "{read write}" tokenizes the same as "{ read write }".
 */
static int nksu_tokenize(char *s, struct nksu_tok *tok, int max)
{
	int n = 0;

	while (*s) {
		char *start;

		if (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
			s++;
			continue;
		}

		if (n >= max)
			return -E2BIG;

		if (*s == '{' || *s == '}') {
			tok[n].kind = *s;
			tok[n].p = NULL;
			n++;
			s++;
			continue;
		}

		start = s;
		while (*s && *s != ' ' && *s != '\t' && *s != '\r' &&
		       *s != '\n' && *s != '{' && *s != '}')
			s++;

		if (*s == '{' || *s == '}') {
			char brace = *s;

			/* terminate the word in place, then the brace */
			*s = '\0';
			tok[n].kind = 0;
			tok[n].p = start;
			n++;
			if (n >= max)
				return -E2BIG;
			tok[n].kind = brace;
			tok[n].p = NULL;
			n++;
			s++;
		} else if (*s) {
			*s = '\0';
			tok[n].kind = 0;
			tok[n].p = start;
			n++;
			s++;
		} else {
			tok[n].kind = 0;
			tok[n].p = start;
			n++;
		}
	}

	return n;
}

/* Parse one object: a word, "*", or "{ ... }". */
static int nksu_parse_seobj(struct nksu_tok *tok, int ntok, int *idx,
			    struct nksu_seobj *out)
{
	int i = *idx;

	out->n = 0;
	if (i >= ntok)
		return -EINVAL;

	if (tok[i].kind == '{') {
		i++;
		while (i < ntok && tok[i].kind != '}') {
			if (tok[i].kind != 0)
				return -EINVAL;
			if (out->n >= NKSU_RULE_MAX_ITEMS)
				return -E2BIG;
			out->v[out->n++] = tok[i].p;
			i++;
		}
		if (i >= ntok)
			return -EINVAL; /* missing '}' */
		i++; /* consume '}' */
	} else if (tok[i].kind == 0) {
		out->v[out->n++] = tok[i].p;
		i++;
	} else {
		return -EINVAL; /* lone '}' */
	}

	*idx = i;
	return out->n ? 0 : -EINVAL;
}

/* "*" and unset both mean "wildcard" (NULL) to the rule writer. */
static const char *nksu_se_val(const char *s)
{
	return (s && strcmp(s, "*") != 0) ? s : NULL;
}

static const char *nksu_se_val_type(const char *s)
{
	/* types/attrs cannot be wildcards */
	return (s && strcmp(s, "*") != 0) ? s : NULL;
}

static int nksu_effect_normal(const char *op, int *effect, bool *invert)
{
	if (strcmp(op, "allow") == 0) {
		*effect = AVTAB_ALLOWED;
		*invert = false;
	} else if (strcmp(op, "deny") == 0) {
		*effect = AVTAB_ALLOWED;
		*invert = true;
	} else if (strcmp(op, "auditallow") == 0) {
		*effect = AVTAB_AUDITALLOW;
		*invert = false;
	} else if (strcmp(op, "dontaudit") == 0) {
		*effect = AVTAB_AUDITDENY;
		*invert = true;
	} else {
		return -EINVAL;
	}
	return 0;
}

static void nksu_apply_normal(const char *op, struct nksu_seobj *src,
			      struct nksu_seobj *tgt, struct nksu_seobj *cls,
			      struct nksu_seobj *perm)
{
	int effect = AVTAB_ALLOWED;
	bool invert = false;
	int i, j, k, l;

	if (nksu_effect_normal(op, &effect, &invert))
		return;

	for (i = 0; i < src->n; i++)
	for (j = 0; j < tgt->n; j++)
	for (k = 0; k < cls->n; k++)
	for (l = 0; l < perm->n; l++)
		sepolicy_add_rule(nksu_se_val(src->v[i]),
				  nksu_se_val(tgt->v[j]),
				  nksu_se_val(cls->v[k]),
				  nksu_se_val(perm->v[l]),
				  effect, invert);
}

/*
 * Normal-permission statement.  Handles both the KernelSU grammar
 * (source, target, class, permission as separate objects) and the classic
 * "source target:class permission" form.
 */
static int nksu_parse_normal(struct nksu_rule_scratch *sc, int ntok,
			     const char *op)
{
	struct nksu_seobj *src = &sc->se[0], *tgt = &sc->se[1];
	struct nksu_seobj *cls = &sc->se[2], *perm = &sc->se[3];
	int idx = 1, ret;
	char *colon;

	ret = nksu_parse_seobj(sc->tok, ntok, &idx, src);
	if (ret)
		return ret;

	if (idx < ntok && sc->tok[idx].kind == 0 &&
	    (colon = strchr(sc->tok[idx].p, ':')) && colon[1]) {
		*colon = '\0';
		tgt->n = 1;
		tgt->v[0] = sc->tok[idx].p;
		cls->n = 1;
		cls->v[0] = colon + 1;
		idx++;
		ret = nksu_parse_seobj(sc->tok, ntok, &idx, perm);
		if (ret)
			return ret;
	} else {
		ret = nksu_parse_seobj(sc->tok, ntok, &idx, tgt);
		if (ret)
			return ret;
		ret = nksu_parse_seobj(sc->tok, ntok, &idx, cls);
		if (ret)
			return ret;
		ret = nksu_parse_seobj(sc->tok, ntok, &idx, perm);
		if (ret)
			return ret;
	}

	nksu_apply_normal(op, src, tgt, cls, perm);
	pr_debug("[selinux]: sepolicy.rule: %s %s %s %s %s\n", op,
		 src->v[0] ? src->v[0] : "*", tgt->v[0] ? tgt->v[0] : "*",
		 cls->v[0] ? cls->v[0] : "*", perm->v[0] ? perm->v[0] : "*");
	return 0;
}

static int nksu_parse_xperm(struct nksu_rule_scratch *sc, int ntok,
			    const char *op)
{
	struct nksu_seobj *src = &sc->se[0], *tgt = &sc->se[1];
	struct nksu_seobj *cls = &sc->se[2], *set = &sc->se[3];
	int effect;
	int idx = 1, ret, i, j, k, l;
	char *colon;

	if (strcmp(op, "allowxperm") == 0)
		effect = AVTAB_XPERMS_ALLOWED;
	else if (strcmp(op, "auditallowxperm") == 0)
		effect = AVTAB_XPERMS_AUDITALLOW;
	else if (strcmp(op, "dontauditxperm") == 0)
		effect = AVTAB_XPERMS_DONTAUDIT;
	else
		return -EINVAL;

	ret = nksu_parse_seobj(sc->tok, ntok, &idx, src);
	if (ret)
		return ret;

	if (idx < ntok && sc->tok[idx].kind == 0 &&
	    (colon = strchr(sc->tok[idx].p, ':')) && colon[1]) {
		*colon = '\0';
		tgt->n = 1;
		tgt->v[0] = sc->tok[idx].p;
		cls->n = 1;
		cls->v[0] = colon + 1;
		idx++;
	} else {
		ret = nksu_parse_seobj(sc->tok, ntok, &idx, tgt);
		if (ret)
			return ret;
		ret = nksu_parse_seobj(sc->tok, ntok, &idx, cls);
		if (ret)
			return ret;
	}

	/* operation (ioctl/nlmsg); only ioctl ranges are supported here */
	if (idx >= ntok || sc->tok[idx].kind != 0) {
		pr_warn("[selinux]: sepolicy.rule: %s missing operation, skipped\n", op);
		return -EINVAL;
	}
	if (strcmp(sc->tok[idx].p, "ioctl") != 0) {
		pr_warn("[selinux]: sepolicy.rule: %s operation '%s' not supported, skipped\n",
			op, sc->tok[idx].p);
		return -EOPNOTSUPP;
	}
	idx++;

	ret = nksu_parse_seobj(sc->tok, ntok, &idx, set);
	if (ret)
		return ret;

	for (i = 0; i < src->n; i++)
	for (j = 0; j < tgt->n; j++)
	for (k = 0; k < cls->n; k++)
	for (l = 0; l < set->n; l++)
		sepolicy_add_xperm(nksu_se_val(src->v[i]),
				   nksu_se_val(tgt->v[j]),
				   nksu_se_val(cls->v[k]),
				   nksu_se_val(set->v[l]), effect, false);

	return 0;
}

static int nksu_parse_typeattr(struct nksu_rule_scratch *sc, int ntok)
{
	struct nksu_seobj *type = &sc->se[0], *attr = &sc->se[1];
	int idx = 1, ret, i, j;

	ret = nksu_parse_seobj(sc->tok, ntok, &idx, type);
	if (ret)
		return ret;
	ret = nksu_parse_seobj(sc->tok, ntok, &idx, attr);
	if (ret)
		return ret;

	for (i = 0; i < type->n; i++)
		for (j = 0; j < attr->n; j++)
			sepolicy_add_typeattribute(nksu_se_val_type(type->v[i]),
						   nksu_se_val_type(attr->v[j]));
	return 0;
}

static int nksu_parse_type(struct nksu_rule_scratch *sc, int ntok)
{
	struct nksu_seobj *attr = &sc->se[0];
	const char *name;
	int idx = 1, ret, i;

	if (idx >= ntok || sc->tok[idx].kind != 0)
		return -EINVAL;
	name = sc->tok[idx].p;
	idx++;

	ret = sepolicy_add_type(name);
	if (ret)
		return ret;

	/* "type foo" defaults to the domain attribute, like KernelSU. */
	if (idx >= ntok) {
		(void)sepolicy_add_typeattribute(name, "domain");
		return 0;
	}

	ret = nksu_parse_seobj(sc->tok, ntok, &idx, attr);
	if (ret)
		return ret;

	for (i = 0; i < attr->n; i++) {
		const char *a = nksu_se_val_type(attr->v[i]);

		if (a)
			(void)sepolicy_add_typeattribute(name, a);
	}
	return 0;
}

static int nksu_parse_statement(struct nksu_rule_scratch *sc, int ntok)
{
	const char *op;

	if (ntok < 1 || sc->tok[0].kind != 0)
		return -EINVAL;

	op = sc->tok[0].p;

	if (strcmp(op, "allow") == 0 || strcmp(op, "deny") == 0 ||
	    strcmp(op, "auditallow") == 0 || strcmp(op, "dontaudit") == 0)
		return nksu_parse_normal(sc, ntok, op);

	if (strcmp(op, "allowxperm") == 0 ||
	    strcmp(op, "auditallowxperm") == 0 ||
	    strcmp(op, "dontauditxperm") == 0)
		return nksu_parse_xperm(sc, ntok, op);

	if (strcmp(op, "typeattribute") == 0 || strcmp(op, "attradd") == 0)
		return nksu_parse_typeattr(sc, ntok);

	if (strcmp(op, "type") == 0)
		return nksu_parse_type(sc, ntok);

	pr_warn("[selinux]: sepolicy.rule: unsupported statement '%s', skipped\n", op);
	return -EOPNOTSUPP;
}

int sepolicy_apply_rule_text(const char *text)
{
	struct nksu_rule_scratch *sc;
	const char *p;
	int applied = 0, skipped = 0;

	if (!text)
		return -EINVAL;

	sc = kvmalloc(sizeof(*sc), GFP_KERNEL);
	if (!sc)
		return -ENOMEM;

	for (p = text; *p; ) {
		size_t len = 0;
		char *q;
		int ntok, ret;

		while (*p && *p != '\n' && *p != ';' &&
		       len < NKSU_RULE_LINE_MAX - 1)
			sc->line[len++] = *p++;
		sc->line[len] = '\0';
		if (*p == '\n' || *p == ';')
			p++;

		/* blank line or comment */
		q = sc->line;
		while (*q == ' ' || *q == '\t' || *q == '\r')
			q++;
		if (!*q || *q == '#')
			continue;

		ntok = nksu_tokenize(q, sc->tok, NKSU_RULE_MAX_TOKENS);
		if (ntok <= 0)
			continue;

		ret = nksu_parse_statement(sc, ntok);
		if (ret == 0)
			applied++;
		else
			skipped++;
	}

	kvfree(sc);

	if (skipped)
		pr_info("[selinux]: sepolicy.rule: %d statement(s) applied, %d skipped\n",
			applied, skipped);

	return applied;
}

/*
 * Write-only sink that lets the userspace module loader apply a module's
 * sepolicy.rule.  The loader cannot call the manager-gated ioctl, so it
 * echoes the rule *path* here and the kernel reads and applies the file in
 * the loader's (unconfined nksu) context:
 *
 *     echo /data/adb/modules/<id>/sepolicy.rule > /proc/nksu/sepolicy
 */
#define NKSU_SEPOLICY_TEXT_MAX (128 * 1024)

struct proc_dir_entry *fmac_proc_dir;
static struct proc_dir_entry *nksu_sepolicy_pde;

static ssize_t nksu_sepolicy_write(struct file *file, const char __user *ubuf,
				   size_t count, loff_t *ppos)
{
	char *path, *p, *text;
	struct file *f;
	loff_t pos = 0;
	ssize_t n;
	int applied;

	(void)file;
	(void)ppos;

	if (!count || count > PATH_MAX)
		return -EINVAL;

	path = kvmalloc(count + 1, GFP_KERNEL);
	if (!path)
		return -ENOMEM;

	if (copy_from_user(path, ubuf, count)) {
		kvfree(path);
		return -EFAULT;
	}
	path[count] = '\0';
	p = strim(path); /* an `echo` leaves a trailing newline */

	if (!p[0]) {
		kvfree(path);
		return count;
	}

	text = kvmalloc(NKSU_SEPOLICY_TEXT_MAX, GFP_KERNEL);
	if (!text) {
		kvfree(path);
		return -ENOMEM;
	}

	f = filp_open(p, O_RDONLY, 0);
	if (IS_ERR(f)) {
		pr_warn("[selinux]: sepolicy sink: cannot open %s: %ld\n",
			p, PTR_ERR(f));
		kvfree(text);
		kvfree(path);
		return PTR_ERR(f);
	}

	n = kernel_read(f, text, NKSU_SEPOLICY_TEXT_MAX - 1, &pos);
	filp_close(f, NULL);
	if (n < 0) {
		pr_warn("[selinux]: sepolicy sink: cannot read %s: %zd\n",
			p, n);
		kvfree(text);
		kvfree(path);
		return n;
	}
	text[n] = '\0';

	applied = sepolicy_apply_rule_text(text);
	if (applied < 0)
		pr_warn("[selinux]: sepolicy sink: %s failed: %d\n",
			p, applied);
	else
		pr_info("[selinux]: sepolicy sink: %s: %d statement(s)\n",
			p, applied);

	kvfree(text);
	kvfree(path);
	return (ssize_t)count;
}

static const struct proc_ops nksu_sepolicy_proc_ops = {
	.proc_write = nksu_sepolicy_write,
};

int nksu_sepolicy_sink_init(void)
{
	if (nksu_sepolicy_pde)
		return 0;

	if (!fmac_proc_dir)
		fmac_proc_dir = proc_mkdir("nksu", NULL);
	if (!fmac_proc_dir)
		return -ENOMEM;

	nksu_sepolicy_pde = proc_create("sepolicy", 0200, fmac_proc_dir,
					&nksu_sepolicy_proc_ops);
	if (!nksu_sepolicy_pde)
		return -ENOMEM;

	pr_info("[selinux]: sepolicy sink at /proc/nksu/sepolicy\n");
	return 0;
}

void nksu_sepolicy_sink_exit(void)
{
	if (!nksu_sepolicy_pde)
		return;

	remove_proc_entry("sepolicy", fmac_proc_dir);
	nksu_sepolicy_pde = NULL;

	if (fmac_proc_dir) {
		remove_proc_entry("nksu", NULL);
		fmac_proc_dir = NULL;
	}
}
