// SPDX-License-Identifier: GPL-2.0
/*
 * SELinux rule manipulation — add allow/deny/xperm rules, type rules, type
 * state and type-attribute associations to the running policy.
 *
 * Everything operates on the working copy installed by
 * sepolicy_dup_and_apply(), so the original policy stays untouched.  The rule
 * semantics and coverage follow KernelSU's sepolicy engine (allow/deny,
 * auditallow/dontaudit, ioctl xperms, type/attribute, permissive/enforce,
 * type_transition/change/member and genfscon), reimplemented with NekoSU's
 * helpers and naming.
 *
 * Locking matches security/selinux/ss/services.c:
 *   - public functions grab selinux_state.policy_mutex
 *   - policy pointer via rcu_dereference_protected()
 *   - avc_reset() after every successful change
 */

#include <linux/string.h>
#include <linux/stringhash.h>
#include <linux/errno.h>
#include <linux/version.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>
#include <linux/sort.h>
#include <fmac.h>

#include "ss/policydb.h"
#include "ss/services.h"
#include "ss/avtab.h"
#include "ss/symtab.h"
#include "ss/hashtab.h"
#include "ss/constraint.h"
#include "ss/ebitmap.h"
#include "avc.h"
#include "avc_ss.h"
#include "xfrm.h"
#include "security.h"

#include "symbol_compat.h"
/*
 * This file calls resolved unexported kernel functions through pointers.
 * Disable CFI for its functions so those indirect calls are not type-hash
 * checked (the module's build headers may hash differently than the running
 * kernel). Functions the kernel calls back live in other files.
 */
#if defined(__clang__)
#pragma clang attribute push(__attribute__((no_sanitize("cfi"))), apply_to=function)
#endif

/* some older kernels don't have this helper */
#ifndef hashtab_for_each
#define hashtab_for_each(h, node)                                            \
	for (int _i##__LINE__ = 0; _i##__LINE__ < (h).size; _i##__LINE__++) \
		for (node = (h).htable[_i##__LINE__]; node; node = node->next)
#endif

/* grab the working policydb under the lock */
static struct policydb *fmac_get_pdb(void)
{
	struct selinux_policy *pol;

	pol = rcu_dereference_protected(selinux_state.policy,
					lockdep_is_held(&selinux_state.policy_mutex));
	if (!pol)
		return NULL;
	return &pol->policydb;
}

/*
 * avc_reset — tell every SELinux subsystem the policy changed.
 * AVC, netlink, status page, xfrm — all of them.  Order follows
 * sel_write_load() in selinuxfs.c.
 */
void avc_reset(void)
{
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0))
	avc_ss_reset(0);
	selnl_notify_policyload(0);
	selinux_status_update_policyload(0);
#else
	avc_ss_reset(selinux_state.avc, 0);
	selnl_notify_policyload(0);
	selinux_status_update_policyload(&selinux_state, 0);
#endif
	selinux_xfrm_notify_policyload();
}

/* ---- low-level avtab helpers ---- */

static struct avtab_node *rule_get_avtab_node(struct policydb *db,
					      struct avtab_key *key,
					      struct avtab_extended_perms *xperms)
{
	struct avtab_node *node;

	/* AVTAB_XPERMS entries are not necessarily unique */
	if (key->specified & AVTAB_XPERMS) {
		bool match = false;

		node = avtab_search_node(&db->te_avtab, key);
		while (node) {
			if (node->datum.u.xperms->specified == xperms->specified &&
			    node->datum.u.xperms->driver == xperms->driver) {
				match = true;
				break;
			}
			node = avtab_search_node_next(node, key->specified);
		}
		if (!match)
			node = NULL;
	} else {
		node = avtab_search_node(&db->te_avtab, key);
	}

	if (!node) {
		struct avtab_datum datum = {};

		if (key->specified & AVTAB_XPERMS)
			datum.u.xperms = xperms;
		else
			datum.u.data = key->specified == AVTAB_AUDITDENY ? ~0U : 0U;

		node = avtab_insert_nonunique(&db->te_avtab, key, &datum);
		if (!node)
			return NULL;

		db->len += sizeof(struct avtab_key) + sizeof(struct avtab_datum);
		if (key->specified & AVTAB_XPERMS)
			db->len += sizeof(xperms->specified) + sizeof(xperms->driver) +
				   sizeof(xperms->perms.p);
	}

	return node;
}

static bool rule_node_redundant(struct avtab_node *node)
{
	if (node->key.specified & AVTAB_XPERMS)
		return node->datum.u.xperms == NULL;
	if (!(node->key.specified & AVTAB_AV))
		return false;
	if (node->key.specified & AVTAB_AUDITDENY)
		return node->datum.u.data == ~0U;
	return node->datum.u.data == 0U;
}

static bool rule_remove_avtab_node(struct policydb *db, struct avtab_node *node)
{
	int shrink = sizeof(node->key.source_type) + sizeof(node->key.target_type) +
		     sizeof(node->key.target_class) + sizeof(node->key.specified);
	struct avtab removed = {};
	struct avtab_node *n, *prev;
	int i;

	if (avtab_alloc(&removed, 1) < 0)
		return false;

	for (i = 0; i < db->te_avtab.nslot; i++) {
		prev = NULL;
		for (n = db->te_avtab.htable[i]; n; prev = n, n = n->next) {
			if (n != node)
				continue;

			if (prev)
				prev->next = n->next;
			else
				db->te_avtab.htable[i] = n->next;

			if (db->te_avtab.nel > 0)
				db->te_avtab.nel--;

			if (n->key.specified & AVTAB_XPERMS)
				shrink += sizeof(n->datum.u.xperms->specified) +
					  sizeof(n->datum.u.xperms->driver) +
					  sizeof(n->datum.u.xperms->perms.p);
			else
				shrink += sizeof(n->datum.u.data);

			n->next = NULL;
			removed.htable[0] = n;
			removed.nel = 1;
			avtab_destroy(&removed);
			if (db->len >= (unsigned)shrink)
				db->len -= shrink;
			return true;
		}
	}

	avtab_destroy(&removed);
	return false;
}

/* invert is adding rules for auditdeny; in other cases it removes rules */
#define rule_strip_av(effect, invert) ((effect == AVTAB_AUDITDENY) == !(invert))

#define for_each_htable(htab, cur)                       \
	for (int _i = 0; _i < (htab).size; _i++)         \
		for (cur = (htab).htable[_i]; cur; cur = cur->next)

static bool rule_add_raw(struct policydb *db, struct type_datum *src,
			 struct type_datum *tgt, struct class_datum *cls,
			 struct perm_datum *perm, int effect, bool invert)
{
	bool success = true;

	if (src == NULL) {
		struct hashtab_node *node;

		for_each_htable(db->p_types.table, node) {
			struct type_datum *type = node->datum;

			if (!rule_strip_av(effect, invert) && !type->attribute)
				continue;
			success &= rule_add_raw(db, type, tgt, cls, perm, effect, invert);
		}
	} else if (tgt == NULL) {
		struct hashtab_node *node;

		for_each_htable(db->p_types.table, node) {
			struct type_datum *type = node->datum;

			if (!rule_strip_av(effect, invert) && !type->attribute)
				continue;
			success &= rule_add_raw(db, src, type, cls, perm, effect, invert);
		}
	} else if (cls == NULL) {
		struct hashtab_node *node;

		for_each_htable(db->p_classes.table, node) {
			success &= rule_add_raw(db, src, tgt, node->datum, perm, effect, invert);
		}
	} else {
		struct avtab_key key;
		struct avtab_node *node;

		key.source_type = src->value;
		key.target_type = tgt->value;
		key.target_class = cls->value;
		key.specified = effect;

		if (invert && effect != AVTAB_AUDITDENY) {
			node = avtab_search_node(&db->te_avtab, &key);
			if (!node)
				return true;
		} else {
			node = rule_get_avtab_node(db, &key, NULL);
			if (!node)
				return false;
		}

		if (invert) {
			if (perm)
				node->datum.u.data &= ~(1U << (perm->value - 1));
			else
				node->datum.u.data = 0U;
		} else {
			if (perm)
				node->datum.u.data |= 1U << (perm->value - 1);
			else
				node->datum.u.data = ~0U;
		}

		if (rule_node_redundant(node))
			return rule_remove_avtab_node(db, node);
	}

	return success;
}

static bool rule_add(struct policydb *db, const char *s, const char *t,
		     const char *c, const char *p, int effect, bool invert)
{
	struct type_datum *src = NULL, *tgt = NULL;
	struct class_datum *cls = NULL;
	struct perm_datum *perm = NULL;

	if (s && !(src = symtab_search(&db->p_types, s))) {
		pr_info("[selinux]: source type '%s' not found\n", s);
		return false;
	}
	if (t && !(tgt = symtab_search(&db->p_types, t))) {
		pr_info("[selinux]: target type '%s' not found\n", t);
		return false;
	}
	if (c && !(cls = symtab_search(&db->p_classes, c))) {
		pr_info("[selinux]: class '%s' not found\n", c);
		return false;
	}
	if (p) {
		if (!cls)
			return false;
		perm = symtab_search(&cls->permissions, p);
		if (!perm && cls->comdatum)
			perm = symtab_search(&cls->comdatum->permissions, p);
		if (!perm) {
			pr_info("[selinux]: perm '%s' not found in class '%s'\n", p, c);
			return false;
		}
	}

	return rule_add_raw(db, src, tgt, cls, perm, effect, invert);
}

/* ---- xperms ---- */

#define ioctl_driver(x) ((x) >> 8 & 0xFF)
#define ioctl_func(x) ((x) & 0xFF)
#define xperm_set(x, p) ((p)[(x) >> 5] |= (1 << ((x) & 0x1f)))
#define xperm_clear(x, p) ((p)[(x) >> 5] &= ~(1 << ((x) & 0x1f)))

static void rule_add_xperm_raw(struct policydb *db, struct type_datum *src,
			       struct type_datum *tgt, struct class_datum *cls,
			       u16 low, u16 high, int effect, bool invert)
{
	if (src == NULL) {
		struct hashtab_node *node;

		for_each_htable(db->p_types.table, node) {
			struct type_datum *type = node->datum;

			if (type->attribute)
				rule_add_xperm_raw(db, type, tgt, cls, low, high, effect, invert);
		}
	} else if (tgt == NULL) {
		struct hashtab_node *node;

		for_each_htable(db->p_types.table, node) {
			struct type_datum *type = node->datum;

			if (type->attribute)
				rule_add_xperm_raw(db, src, type, cls, low, high, effect, invert);
		}
	} else if (cls == NULL) {
		struct hashtab_node *node;

		for_each_htable(db->p_classes.table, node)
			rule_add_xperm_raw(db, src, tgt, node->datum, low, high, effect, invert);
	} else {
		struct avtab_extended_perms xperms;
		struct avtab_key key;
		struct avtab_node *node;
		int i;

		key.source_type = src->value;
		key.target_type = tgt->value;
		key.target_class = cls->value;
		key.specified = effect;

		memset(&xperms, 0, sizeof(xperms));
		if (ioctl_driver(low) != ioctl_driver(high)) {
			xperms.specified = AVTAB_XPERMS_IOCTLDRIVER;
			xperms.driver = 0;
		} else {
			xperms.specified = AVTAB_XPERMS_IOCTLFUNCTION;
			xperms.driver = ioctl_driver(low);
		}

		if (xperms.specified == AVTAB_XPERMS_IOCTLDRIVER) {
			for (i = ioctl_driver(low); i <= ioctl_driver(high); ++i) {
				if (invert)
					xperm_clear(i, xperms.perms.p);
				else
					xperm_set(i, xperms.perms.p);
			}
		} else {
			for (i = ioctl_func(low); i <= ioctl_func(high); ++i) {
				if (invert)
					xperm_clear(i, xperms.perms.p);
				else
					xperm_set(i, xperms.perms.p);
			}
		}

		node = rule_get_avtab_node(db, &key, &xperms);
		if (!node) {
			pr_warn("[selinux]: xperm node not found\n");
			return;
		}

		for (i = 0; i < ARRAY_SIZE(xperms.perms.p); i++)
			node->datum.u.xperms->perms.p[i] |= xperms.perms.p[i];
	}
}

static bool rule_add_xperm(struct policydb *db, const char *s, const char *t,
			   const char *c, const char *range, int effect, bool invert)
{
	struct type_datum *src = NULL, *tgt = NULL;
	struct class_datum *cls = NULL;
	u16 low, high;

	if (s && !(src = symtab_search(&db->p_types, s))) {
		pr_info("[selinux]: source type '%s' not found\n", s);
		return false;
	}
	if (t && !(tgt = symtab_search(&db->p_types, t))) {
		pr_info("[selinux]: target type '%s' not found\n", t);
		return false;
	}
	if (c && !(cls = symtab_search(&db->p_classes, c))) {
		pr_info("[selinux]: class '%s' not found\n", c);
		return false;
	}

	if (range && *range) {
		if (strchr(range, '-')) {
			if (sscanf(range, "0x%hx-0x%hx", &low, &high) != 2 &&
			    sscanf(range, "%hx-%hx", &low, &high) != 2)
				return false;
		} else {
			if (sscanf(range, "0x%hx", &low) != 1 &&
			    sscanf(range, "%hx", &low) != 1)
				return false;
			high = low;
		}
	} else {
		low = 0;
		high = 0xFFFF;
	}

	rule_add_xperm_raw(db, src, tgt, cls, low, high, effect, invert);
	return true;
}

/* ---- type rules and filename transitions ---- */

static bool rule_add_type(struct policydb *db, const char *s, const char *t,
			  const char *c, const char *d, int effect)
{
	struct type_datum *src, *tgt, *def;
	struct class_datum *cls;
	struct avtab_key key;
	struct avtab_node *node;

	if (!(src = symtab_search(&db->p_types, s)))
		return false;
	if (!(tgt = symtab_search(&db->p_types, t)))
		return false;
	if (!(cls = symtab_search(&db->p_classes, c)))
		return false;
	if (!(def = symtab_search(&db->p_types, d)))
		return false;

	key.source_type = src->value;
	key.target_type = tgt->value;
	key.target_class = cls->value;
	key.specified = effect;

	node = rule_get_avtab_node(db, &key, NULL);
	if (!node)
		return false;
	node->datum.u.data = def->value;
	return true;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 9, 0)
static u32 filenametr_hash(const void *k)
{
	const struct filename_trans_key *ft = k;
	unsigned long hash = ft->ttype ^ ft->tclass;
	unsigned int i = 0;
	unsigned char focus;

	while ((focus = ft->name[i++]))
		hash = partial_name_hash(focus, hash);
	return hash;
}

static int filenametr_cmp(const void *k1, const void *k2)
{
	const struct filename_trans_key *a = k1, *b = k2;
	int v = a->ttype - b->ttype;

	if (!v)
		v = a->tclass - b->tclass;
	if (!v)
		v = strcmp(a->name, b->name);
	return v;
}

static const struct hashtab_key_params filenametr_key_params = {
	.hash = filenametr_hash,
	.cmp = filenametr_cmp,
};
#endif

static bool rule_add_filename_trans(struct policydb *db, const char *s,
				    const char *t, const char *c, const char *d,
				    const char *o)
{
	struct type_datum *src, *tgt, *def;
	struct class_datum *cls;
	struct filename_trans_key key;
	struct filename_trans_key *new_key = NULL;
	struct filename_trans_datum *last = NULL, *trans;
	int rc;

	if (!(src = symtab_search(&db->p_types, s)))
		return false;
	if (!(tgt = symtab_search(&db->p_types, t)))
		return false;
	if (!(cls = symtab_search(&db->p_classes, c)))
		return false;
	if (!(def = symtab_search(&db->p_types, d)))
		return false;

	key.ttype = tgt->value;
	key.tclass = cls->value;
	key.name = (char *)o;

	trans = policydb_filenametr_search(db, &key);
	while (trans) {
		if (ebitmap_get_bit(&trans->stypes, src->value - 1)) {
			trans->otype = def->value;
			return true;
		}
		if (trans->otype == def->value)
			break;
		last = trans;
		trans = trans->next;
	}

	if (trans == NULL) {
		trans = kcalloc(1, sizeof(*trans), GFP_KERNEL);
		if (!trans)
			return false;

		new_key = kzalloc(sizeof(*new_key), GFP_KERNEL);
		if (!new_key)
			goto free_trans;

		*new_key = key;
		new_key->name = kstrdup(key.name, GFP_KERNEL);
		if (!new_key->name)
			goto free_key;

		trans->next = last;
		trans->otype = def->value;
		rc = hashtab_insert(&db->filename_trans, new_key, trans, filenametr_key_params);
		if (rc)
			goto free_name;
	}

	db->compat_filename_trans_count++;
	return ebitmap_set_bit(&trans->stypes, src->value - 1, 1) == 0;

free_name:
	kfree(new_key->name);
free_key:
	kfree(new_key);
free_trans:
	kfree(trans);
	return false;
}

/* NekoSU does not implement genfscon yet; keep the call a no-op. */
static bool rule_add_genfscon(struct policydb *db, const char *fs_name,
			      const char *path, const char *ctx)
{
	return false;
}

/* ---- type / attribute / type state ---- */

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
#define nksu_kvrealloc(p, new_size, _old_size) kvrealloc(p, new_size, GFP_KERNEL)
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)
#define nksu_kvrealloc(p, new_size, old_size) kvrealloc(p, old_size, new_size, GFP_KERNEL)
#else
static void *nksu_kvrealloc_compat(const void *p, size_t oldsize, size_t newsize, gfp_t flags)
{
	void *newp;

	if (oldsize >= newsize)
		return (void *)p;
	newp = kvmalloc(newsize, flags);
	if (!newp)
		return NULL;
	memcpy(newp, p, oldsize);
	kvfree(p);
	return newp;
}
#define nksu_kvrealloc(p, new_size, old_size) nksu_kvrealloc_compat(p, old_size, new_size, GFP_KERNEL)
#endif

static bool rule_add_type_datum(struct policydb *db, const char *type_name, bool attr)
{
	struct type_datum *type = symtab_search(&db->p_types, type_name);
	u32 value;
	char *key;
	int i;

	if (type)
		return true;

	value = ++db->p_types.nprim;
	type = kzalloc(sizeof(struct type_datum), GFP_KERNEL);
	if (!type)
		return false;

	type->primary = 1;
	type->value = value;
	type->attribute = attr;

	key = kstrdup(type_name, GFP_KERNEL);
	if (!key) {
		kfree(type);
		return false;
	}

	if (symtab_insert(&db->p_types, key, type)) {
		kfree(key);
		kfree(type);
		return false;
	}

	db->type_attr_map_array = nksu_kvrealloc(db->type_attr_map_array,
						 value * sizeof(struct ebitmap),
						 (value - 1) * sizeof(struct ebitmap));
	db->type_val_to_struct = nksu_kvrealloc(db->type_val_to_struct,
						sizeof(*db->type_val_to_struct) * value,
						sizeof(*db->type_val_to_struct) * (value - 1));
	db->sym_val_to_name[SYM_TYPES] = nksu_kvrealloc(db->sym_val_to_name[SYM_TYPES],
							sizeof(char *) * value,
							sizeof(char *) * (value - 1));
	if (!db->type_attr_map_array || !db->type_val_to_struct ||
	    !db->sym_val_to_name[SYM_TYPES])
		return false;

	ebitmap_init(&db->type_attr_map_array[value - 1]);
	ebitmap_set_bit(&db->type_attr_map_array[value - 1], value - 1, 1);
	db->type_val_to_struct[value - 1] = type;
	db->sym_val_to_name[SYM_TYPES][value - 1] = key;

	for (i = 0; i < db->p_roles.nprim; ++i)
		ebitmap_set_bit(&db->role_val_to_struct[i]->types, value - 1, 1);

	return true;
}

static bool rule_set_type_state(struct policydb *db, const char *type_name, bool permissive)
{
	struct type_datum *type;

	if (type_name == NULL || !*type_name) {
		struct hashtab_node *node;

		for_each_htable(db->p_types.table, node) {
			type = node->datum;
			ebitmap_set_bit(&db->permissive_map, type->value, permissive);
		}
		return true;
	}

	type = symtab_search(&db->p_types, type_name);
	if (!type) {
		pr_info("[selinux]: type '%s' not found\n", type_name);
		return false;
	}
	return ebitmap_set_bit(&db->permissive_map, type->value, permissive) == 0;
}

static void rule_add_typeattribute_raw(struct policydb *db, struct type_datum *type,
				       struct type_datum *attr)
{
	struct ebitmap *sattr = &db->type_attr_map_array[type->value - 1];
	struct hashtab_node *node;
	struct constraint_node *n;
	struct constraint_expr *e;

	ebitmap_set_bit(sattr, attr->value - 1, 1);

	for_each_htable(db->p_classes.table, node) {
		struct class_datum *cls = node->datum;

		for (n = cls->constraints; n; n = n->next) {
			for (e = n->expr; e; e = e->next) {
				if (e->expr_type == CEXPR_NAMES &&
				    ebitmap_get_bit(&e->type_names->types, attr->value - 1))
					ebitmap_set_bit(&e->names, type->value - 1, 1);
			}
		}
	}
}

static bool rule_add_typeattribute(struct policydb *db, const char *type,
				   const char *attr)
{
	struct type_datum *type_d, *attr_d;

	if (!type || !attr)
		return false;

	type_d = symtab_search(&db->p_types, type);
	if (!type_d) {
		pr_info("[selinux]: type '%s' not found\n", type);
		return false;
	}
	if (type_d->attribute) {
		pr_info("[selinux]: '%s' is an attribute, not a type\n", type);
		return false;
	}

	attr_d = symtab_search(&db->p_types, attr);
	if (!attr_d) {
		pr_info("[selinux]: attribute '%s' not found\n", attr);
		return false;
	}
	if (!attr_d->attribute) {
		pr_info("[selinux]: '%s' is not an attribute\n", attr);
		return false;
	}

	rule_add_typeattribute_raw(db, type_d, attr_d);
	return true;
}

/* ---- public API (locks internally) ---- */

#define RULE_LOCK_GET(db)                                    \
	struct policydb *db;                                 \
	mutex_lock(&selinux_state.policy_mutex);             \
	db = fmac_get_pdb();                                 \
	if (!db) {                                           \
		mutex_unlock(&selinux_state.policy_mutex);   \
		return -ENOENT;                              \
	}

#define RULE_UNLOCK(ret)                                     \
	mutex_unlock(&selinux_state.policy_mutex);           \
	if ((ret) == 0)                                      \
		avc_reset();

int sepolicy_add_rule(const char *sname, const char *tname, const char *cname,
		      const char *pname, int effect, bool invert)
{
	int ret;
	RULE_LOCK_GET(db);

	ret = rule_add(db, sname, tname, cname, pname, effect, invert) ? 0 : -ENOENT;
	RULE_UNLOCK(ret);
	return ret;
}

int sepolicy_add_xperm(const char *s, const char *t, const char *c,
		       const char *range, int effect, bool invert)
{
	int ret;
	RULE_LOCK_GET(db);

	ret = rule_add_xperm(db, s, t, c, range, effect, invert) ? 0 : -EINVAL;
	RULE_UNLOCK(ret);
	return ret;
}

int sepolicy_add_type(const char *name)
{
	int ret;
	RULE_LOCK_GET(db);

	ret = rule_add_type_datum(db, name, false) ? 0 : -EINVAL;
	RULE_UNLOCK(ret);
	return ret;
}

int sepolicy_add_attribute(const char *name)
{
	int ret;
	RULE_LOCK_GET(db);

	ret = rule_add_type_datum(db, name, true) ? 0 : -EINVAL;
	RULE_UNLOCK(ret);
	return ret;
}

int sepolicy_add_typeattribute(const char *type_name, const char *attr_name)
{
	int ret;
	RULE_LOCK_GET(db);

	ret = rule_add_typeattribute(db, type_name, attr_name) ? 0 : -EINVAL;
	RULE_UNLOCK(ret);
	return ret;
}

int sepolicy_set_permissive(const char *type_name)
{
	int ret;
	RULE_LOCK_GET(db);

	ret = rule_set_type_state(db, type_name, true) ? 0 : -EINVAL;
	RULE_UNLOCK(ret);
	return ret;
}

int sepolicy_set_enforce(const char *type_name)
{
	int ret;
	RULE_LOCK_GET(db);

	ret = rule_set_type_state(db, type_name, false) ? 0 : -EINVAL;
	RULE_UNLOCK(ret);
	return ret;
}

int sepolicy_type_exists(const char *type_name)
{
	int ret;
	RULE_LOCK_GET(db);

	ret = symtab_search(&db->p_types, type_name) ? 1 : 0;
	mutex_unlock(&selinux_state.policy_mutex);
	return ret;
}

int sepolicy_add_type_transition(const char *s, const char *t, const char *c,
				 const char *d, const char *obj)
{
	int ret;
	RULE_LOCK_GET(db);

	if (obj)
		ret = rule_add_filename_trans(db, s, t, c, d, obj) ? 0 : -EINVAL;
	else
		ret = rule_add_type(db, s, t, c, d, AVTAB_TRANSITION) ? 0 : -EINVAL;
	RULE_UNLOCK(ret);
	return ret;
}

int sepolicy_add_type_change(const char *s, const char *t, const char *c, const char *d)
{
	int ret;
	RULE_LOCK_GET(db);

	ret = rule_add_type(db, s, t, c, d, AVTAB_CHANGE) ? 0 : -EINVAL;
	RULE_UNLOCK(ret);
	return ret;
}

int sepolicy_add_type_member(const char *s, const char *t, const char *c, const char *d)
{
	int ret;
	RULE_LOCK_GET(db);

	ret = rule_add_type(db, s, t, c, d, AVTAB_MEMBER) ? 0 : -EINVAL;
	RULE_UNLOCK(ret);
	return ret;
}

int sepolicy_add_genfscon(const char *fs_name, const char *path, const char *ctx)
{
	int ret;
	RULE_LOCK_GET(db);

	ret = rule_add_genfscon(db, fs_name, path, ctx) ? 0 : -EOPNOTSUPP;
	RULE_UNLOCK(ret);
	return ret;
}

/* give sname all perms to all types, scoped to a single class */
int sepolicy_allow_all_types(const char *sname, const char *cname)
{
	struct policydb *db;
	struct type_datum *src = NULL;
	struct class_datum *cls = NULL;
	int ret = 0;

	mutex_lock(&selinux_state.policy_mutex);

	db = fmac_get_pdb();
	if (!db) {
		ret = -ENOENT;
		goto out;
	}

	if (sname && *sname && !(src = symtab_search(&db->p_types, sname))) {
		ret = -ENOENT;
		goto out;
	}
	if (cname && *cname && !(cls = symtab_search(&db->p_classes, cname))) {
		ret = -ENOENT;
		goto out;
	}

	rule_add_raw(db, src, NULL, cls, NULL, AVTAB_ALLOWED, false);
	rule_add_raw(db, src, NULL, cls, NULL, AVTAB_AUDITDENY, true);
out:
	mutex_unlock(&selinux_state.policy_mutex);
	if (ret == 0)
		avc_reset();
	return ret;
}

/* give sname every perm on every class to every type — basically unconfined */
int sepolicy_allow_any_any(const char *sname)
{
	struct policydb *db;
	struct type_datum *src = NULL;
	int ret = 0;

	mutex_lock(&selinux_state.policy_mutex);

	db = fmac_get_pdb();
	if (!db) {
		ret = -ENOENT;
		goto out;
	}

	if (sname && *sname && !(src = symtab_search(&db->p_types, sname))) {
		ret = -ENOENT;
		goto out;
	}

	rule_add_raw(db, src, NULL, NULL, NULL, AVTAB_ALLOWED, false);
	rule_add_raw(db, src, NULL, NULL, NULL, AVTAB_AUDITDENY, true);
out:
	mutex_unlock(&selinux_state.policy_mutex);
	if (ret == 0)
		avc_reset();
	return ret;
}

int sepolicy_add_domain(const char *name)
{
	int ret;

	ret = sepolicy_add_type(name);
	if (ret)
		return ret;
	return sepolicy_add_typeattribute(name, "domain");
}

#ifdef CONFIG_NKSU_DEBUG
/*
 * Debug helper: flip all AUDITDENY rule data to ~0 so every denial gets
 * audited, even the ones the policy marked dontaudit.
 */
int sepolicy_make_audit(void)
{
	struct policydb *db;
	struct avtab_node *node;
	int i, ret = 0;

	mutex_lock(&selinux_state.policy_mutex);

	db = fmac_get_pdb();
	if (!db) {
		ret = -ENOENT;
		goto out;
	}

	for (i = 0; i < db->te_avtab.nslot; i++) {
		for (node = db->te_avtab.htable[i]; node; node = node->next) {
			if (node->key.specified & AVTAB_AUDITDENY)
				node->datum.u.data = ~0U;
		}
	}
out:
	mutex_unlock(&selinux_state.policy_mutex);
	if (ret == 0)
		avc_reset();
	return ret;
}
#endif /* CONFIG_NKSU_DEBUG */

#if defined(__clang__)
#pragma clang attribute pop
#endif
