// SPDX-License-Identifier: GPL-3.0-or-later
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
 *   - one avc_reset() per batch of successful changes (sepolicy_batch_*)
 */

#include "klog.h"
#include <linux/atomic.h>
#include <linux/string.h>
#include <linux/stringhash.h>
#include <linux/errno.h>
#include <linux/version.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>
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

#include "symbol/symbol_compat.h"

/* some older kernels don't have this helper */
#ifndef hashtab_for_each
#define hashtab_for_each(h, node)                                           \
	for (int _i##__LINE__ = 0; _i##__LINE__ < (h).size; _i##__LINE__++) \
		for (node = (h).htable[_i##__LINE__]; node; node = node->next)
#endif

/* grab the working policydb under the lock */
static struct policydb *fmac_get_pdb(void)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 7, 0)
	struct selinux_policy *pol;

	pol = rcu_dereference_protected(selinux_state.policy,
					lockdep_is_held(&selinux_state.policy_mutex));
	if (!pol)
		return NULL;
	return &pol->policydb;
#else
	if (!selinux_state.ss)
		return NULL;
	return &selinux_state.ss->policydb;
#endif
}

/*
 * avc_reset — tell every SELinux subsystem the policy changed.
 * AVC, netlink, xfrm.  Order follows sel_write_load() in selinuxfs.c.
 *
 * Deliberately does NOT call selinux_status_update_policyload(): that would
 * overwrite the live /sys/fs/selinux/status page's policyload and bump its
 * sequence on every rule change, which stock only does on a real policy load.
 */
void avc_reset(void)
{
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0))
	avc_ss_reset(0);
#else
	avc_ss_reset(selinux_state.avc, 0);
#endif
	selnl_notify_policyload(0);
	selinux_xfrm_notify_policyload();
}

/* ---- low-level avtab helpers ---- */

/*
 * 4.9 stores the avtab buckets in a flex_array instead of a plain pointer
 * array; this accessor keeps the iteration code identical.
 */
static struct avtab_node *nksu_avtab_bucket(struct avtab *tab, u32 i)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 7, 0)
	return tab->htable[i];
#else
	return flex_array_get_ptr(tab->htable, i);
#endif
}

static struct avtab_node *rule_get_avtab_node(struct policydb *db, struct avtab_key *key,
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
		for (n = nksu_avtab_bucket(&db->te_avtab, i); n; prev = n, n = n->next) {
			if (n != node)
				continue;

			if (prev)
				prev->next = n->next;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 7, 0)
			else
				db->te_avtab.htable[i] = n->next;
#else
			else
				flex_array_put_ptr(db->te_avtab.htable, i, n->next, GFP_ATOMIC);
#endif

			if (db->te_avtab.nel > 0)
				db->te_avtab.nel--;

			if (n->key.specified & AVTAB_XPERMS)
				shrink += sizeof(n->datum.u.xperms->specified) +
					  sizeof(n->datum.u.xperms->driver) +
					  sizeof(n->datum.u.xperms->perms.p);
			else
				shrink += sizeof(n->datum.u.data);

			n->next = NULL;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 7, 0)
			removed.htable[0] = n;
#else
			flex_array_put_ptr(removed.htable, 0, n, GFP_ATOMIC);
#endif
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
static inline bool rule_strip_av(int effect, bool invert)
{
	return (effect == AVTAB_AUDITDENY) == !invert;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 7, 0)
#define for_each_htable(htab, cur)               \
	for (int _i = 0; _i < (htab).size; _i++) \
		for (cur = (htab).htable[_i]; cur; cur = cur->next)
#else
/* 4.9 struct symtab holds a pointer to its hashtab. */
#define for_each_htable(htab, cur)                \
	for (int _i = 0; _i < (htab)->size; _i++) \
		for (cur = (htab)->htable[_i]; cur; cur = cur->next)
#endif

static bool rule_add_raw(struct policydb *db, struct type_datum *src, struct type_datum *tgt,
			 struct class_datum *cls, struct perm_datum *perm, int effect, bool invert)
{
	bool success = true;

	if (src == NULL) {
		struct hashtab_node *node;

		for_each_htable(db->p_types.table, node)
		{
			struct type_datum *type = node->datum;

			if (!rule_strip_av(effect, invert) && !type->attribute)
				continue;
			success &= rule_add_raw(db, type, tgt, cls, perm, effect, invert);
		}
	} else if (tgt == NULL) {
		struct hashtab_node *node;

		for_each_htable(db->p_types.table, node)
		{
			struct type_datum *type = node->datum;

			if (!rule_strip_av(effect, invert) && !type->attribute)
				continue;
			success &= rule_add_raw(db, src, type, cls, perm, effect, invert);
		}
	} else if (cls == NULL) {
		struct hashtab_node *node;

		for_each_htable(db->p_classes.table, node)
		{
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

static bool rule_add(struct policydb *db, const char *s, const char *t, const char *c,
		     const char *p, int effect, bool invert)
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

static inline u32 ioctl_driver(u32 x)
{
	return (x >> 8) & 0xFF;
}

static inline u32 ioctl_func(u32 x)
{
	return x & 0xFF;
}

static inline void xperm_set(u32 x, u32 *p)
{
	p[x >> 5] |= 1U << (x & 0x1f);
}

static inline void xperm_clear(u32 x, u32 *p)
{
	p[x >> 5] &= ~(1U << (x & 0x1f));
}

static void rule_add_xperm_raw(struct policydb *db, struct type_datum *src, struct type_datum *tgt,
			       struct class_datum *cls, u16 low, u16 high, int effect, bool invert)
{
	if (src == NULL) {
		struct hashtab_node *node;

		for_each_htable(db->p_types.table, node)
		{
			struct type_datum *type = node->datum;

			if (type->attribute)
				rule_add_xperm_raw(db, type, tgt, cls, low, high, effect, invert);
		}
	} else if (tgt == NULL) {
		struct hashtab_node *node;

		for_each_htable(db->p_types.table, node)
		{
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

static bool rule_add_xperm(struct policydb *db, const char *s, const char *t, const char *c,
			   const char *range, int effect, bool invert)
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
			if (sscanf(range, "0x%hx", &low) != 1 && sscanf(range, "%hx", &low) != 1)
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

static bool rule_add_type(struct policydb *db, const char *s, const char *t, const char *c,
			  const char *d, int effect)
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

/*
 * hashtab_insert() is a static inline that dereferences the unexported
 * __hashtab_insert; reimplement it here so the module only depends on the
 * pointer resolved at load time.
 */
static int nksu_hashtab_insert(struct hashtab *h, void *key, void *datum,
			       struct hashtab_key_params key_params)
{
	u32 hvalue;
	struct hashtab_node *prev, *cur;

	cond_resched();

	if (!h->size || h->nel == HASHTAB_MAX_NODES)
		return -EINVAL;

	hvalue = key_params.hash(key) & (h->size - 1);
	prev = NULL;
	cur = h->htable[hvalue];
	while (cur) {
		int cmp = key_params.cmp(key, cur->key);

		if (cmp == 0)
			return -EEXIST;
		if (cmp < 0)
			break;
		prev = cur;
		cur = cur->next;
	}

	return nksu___hashtab_insert(h, prev ? &prev->next : &h->htable[hvalue], key, datum);
}

static bool rule_add_filename_trans(struct policydb *db, const char *s, const char *t,
				    const char *c, const char *d, const char *o)
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
		rc = nksu_hashtab_insert(&db->filename_trans, new_key, trans,
					 filenametr_key_params);
		if (rc)
			goto free_name;
	}

	/*
	 * filename_compute_type() bails out immediately unless the parent type
	 * is set in filename_trans_ttypes (ss/services.c), and policydb_read()
	 * only sets that bit for keys present when the policy was loaded. Mark
	 * it here, or a filename transition added for a parent type that had no
	 * rules yet is silently never applied.
	 */
	if (ebitmap_set_bit(&db->filename_trans_ttypes, key.ttype, 1))
		return false;

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
#else
/*
 * 4.9 keeps filename transitions in a plain hashtab keyed by the full
 * (stype, ttype, tclass, name) tuple with a single otype datum.
 */
static bool rule_add_filename_trans(struct policydb *db, const char *s, const char *t,
				    const char *c, const char *d, const char *o)
{
	struct type_datum *src, *tgt, *def;
	struct class_datum *cls;
	struct filename_trans *key;
	struct filename_trans_datum *datum;
	int rc;

	if (!(src = symtab_search(&db->p_types, s)))
		return false;
	if (!(tgt = symtab_search(&db->p_types, t)))
		return false;
	if (!(cls = symtab_search(&db->p_classes, c)))
		return false;
	if (!(def = symtab_search(&db->p_types, d)))
		return false;

	key = kzalloc(sizeof(*key), GFP_KERNEL);
	if (!key)
		return false;
	key->stype = src->value;
	key->ttype = tgt->value;
	key->tclass = cls->value;
	key->name = kstrdup(o, GFP_KERNEL);
	if (!key->name) {
		kfree(key);
		return false;
	}

	datum = nksu_hashtab_search(db->filename_trans, key);
	if (datum) {
		datum->otype = def->value;
		kfree(key->name);
		kfree(key);
		return true;
	}

	datum = kzalloc(sizeof(*datum), GFP_KERNEL);
	if (!datum)
		goto free_key;
	datum->otype = def->value;

	rc = nksu_hashtab_insert(db->filename_trans, key, datum);
	if (rc)
		goto free_datum;

	if (ebitmap_set_bit(&db->filename_trans_ttypes, key->ttype, 1))
		return false;
	return true;

free_datum:
	kfree(datum);
free_key:
	kfree(key->name);
	kfree(key);
	return false;
}
#endif

/* NekoSU does not implement genfscon yet; keep the call a no-op. */
static bool rule_add_genfscon(struct policydb *db, const char *fs_name, const char *path,
			      const char *ctx)
{
	return false;
}

/* ---- type / attribute / type state ---- */

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 7, 0)
static bool rule_add_type_datum(struct policydb *db, const char *type_name, bool attr)
{
	struct type_datum *type;
	struct ebitmap *attr_map;
	struct type_datum **val_to_struct;
	char **val_to_name;
	u32 value;
	char *key;
	int i;

	if (symtab_search(&db->p_types, type_name))
		return true;

	value = db->p_types.nprim + 1;

	/*
	 * Allocate every grown structure up front, before touching the policy.
	 * kvrealloc() frees the old buffer before returning, so a later failure
	 * would leave a db pointer dangling; allocating first keeps a failed
	 * add a no-op and never leaves db->p_types partly updated.
	 */
	type = kzalloc(sizeof(*type), GFP_KERNEL);
	key = kstrdup(type_name, GFP_KERNEL);
	attr_map = kvmalloc_array(value, sizeof(*attr_map), GFP_KERNEL);
	val_to_struct = kvmalloc_array(value, sizeof(*val_to_struct), GFP_KERNEL);
	val_to_name = kvmalloc_array(value, sizeof(*val_to_name), GFP_KERNEL);
	if (!type || !key || !attr_map || !val_to_struct || !val_to_name)
		goto err_free;

	if (symtab_insert(&db->p_types, key, type))
		goto err_free;

	/* Commit: from here on nothing can fail. */
	memcpy(attr_map, db->type_attr_map_array, (value - 1) * sizeof(*attr_map));
	memcpy(val_to_struct, db->type_val_to_struct, (value - 1) * sizeof(*val_to_struct));
	memcpy(val_to_name, db->sym_val_to_name[SYM_TYPES], (value - 1) * sizeof(*val_to_name));

	kvfree(db->type_attr_map_array);
	kvfree(db->type_val_to_struct);
	kvfree(db->sym_val_to_name[SYM_TYPES]);

	db->type_attr_map_array = attr_map;
	db->type_val_to_struct = val_to_struct;
	db->sym_val_to_name[SYM_TYPES] = val_to_name;
	db->p_types.nprim = value;

	type->primary = 1;
	type->value = value;
	type->attribute = attr;

	ebitmap_init(&db->type_attr_map_array[value - 1]);
	ebitmap_set_bit(&db->type_attr_map_array[value - 1], value - 1, 1);
	db->type_val_to_struct[value - 1] = type;
	db->sym_val_to_name[SYM_TYPES][value - 1] = key;

	for (i = 0; i < db->p_roles.nprim; ++i)
		ebitmap_set_bit(&db->role_val_to_struct[i]->types, value - 1, 1);

	return true;

err_free:
	kvfree(attr_map);
	kvfree(val_to_struct);
	kvfree(val_to_name);
	kfree(key);
	kfree(type);
	return false;
}
#else
/*
 * 4.9 stores the per-type arrays in flex_arrays whose element count is fixed at
 * allocation time, so growing the type table means building a larger array and
 * moving the entries over.
 */
static struct flex_array *nksu_flex_grow_ptrs(struct flex_array *old, u32 old_n, u32 new_n)
{
	struct flex_array *fa;
	u32 i;

	fa = flex_array_alloc(sizeof(void *), new_n, GFP_KERNEL | __GFP_ZERO);
	if (!fa)
		return NULL;
	if (flex_array_prealloc(fa, 0, new_n, GFP_KERNEL | __GFP_ZERO)) {
		flex_array_free(fa);
		return NULL;
	}
	if (old) {
		for (i = 0; i < old_n; i++) {
			void *p = flex_array_get_ptr(old, i);

			if (p && flex_array_put_ptr(fa, i, p, GFP_KERNEL)) {
				flex_array_free(fa);
				return NULL;
			}
		}
	}
	return fa;
}

static struct flex_array *nksu_flex_grow_ebitmaps(struct flex_array *old, u32 old_n, u32 new_n)
{
	struct flex_array *fa;
	u32 i;

	fa = flex_array_alloc(sizeof(struct ebitmap), new_n, GFP_KERNEL | __GFP_ZERO);
	if (!fa)
		return NULL;
	if (flex_array_prealloc(fa, 0, new_n, GFP_KERNEL | __GFP_ZERO)) {
		flex_array_free(fa);
		return NULL;
	}
	if (old) {
		for (i = 0; i < old_n; i++) {
			struct ebitmap *e = flex_array_get(old, i);
			struct ebitmap tmp;

			if (!e)
				continue;
			/*
			 * Deep copy: the private clone may be installed while the
			 * previous working policy is destroyed, so the two must
			 * not share bitmaps.
			 */
			if (ebitmap_cpy(&tmp, e)) {
				u32 j;

				for (j = 0; j < i; j++) {
					struct ebitmap *c = flex_array_get(fa, j);

					if (c)
						ebitmap_destroy(c);
				}
				flex_array_free(fa);
				return NULL;
			}
			if (flex_array_put(fa, i, &tmp, GFP_KERNEL)) {
				u32 j;

				ebitmap_destroy(&tmp);
				for (j = 0; j < i; j++) {
					struct ebitmap *c = flex_array_get(fa, j);

					if (c)
						ebitmap_destroy(c);
				}
				flex_array_free(fa);
				return NULL;
			}
		}
	}
	return fa;
}

static void nksu_flex_free_ebitmaps(struct flex_array *fa, u32 n)
{
	u32 i;

	if (!fa)
		return;
	for (i = 0; i < n; i++) {
		struct ebitmap *e = flex_array_get(fa, i);

		if (e)
			ebitmap_destroy(e);
	}
	flex_array_free(fa);
}

static bool rule_add_type_datum(struct policydb *db, const char *type_name, bool attr)
{
	struct flex_array *attr_map, *val_to_struct, *val_to_name;
	struct ebitmap *new_map;
	struct type_datum *type;
	u32 value;
	char *key;
	int i;

	if (symtab_search(&db->p_types, type_name))
		return true;

	value = db->p_types.nprim + 1;

	type = kzalloc(sizeof(*type), GFP_KERNEL);
	key = kstrdup(type_name, GFP_KERNEL);
	attr_map = nksu_flex_grow_ebitmaps(db->type_attr_map_array, value - 1, value);
	val_to_struct = nksu_flex_grow_ptrs(db->type_val_to_struct_array, value - 1, value);
	val_to_name = nksu_flex_grow_ptrs(db->sym_val_to_name[SYM_TYPES], value - 1, value);
	if (!type || !key || !attr_map || !val_to_struct || !val_to_name)
		goto err_free;

	if (symtab_insert(&db->p_types, key, type))
		goto err_free;

	/* Commit: from here on nothing can fail. */
	nksu_flex_free_ebitmaps(db->type_attr_map_array, value - 1);
	flex_array_free(db->type_val_to_struct_array);
	flex_array_free(db->sym_val_to_name[SYM_TYPES]);

	db->type_attr_map_array = attr_map;
	db->type_val_to_struct_array = val_to_struct;
	db->sym_val_to_name[SYM_TYPES] = val_to_name;

	type->primary = 1;
	type->value = value;
	type->attribute = attr;

	new_map = flex_array_get(db->type_attr_map_array, value - 1);
	if (new_map) {
		ebitmap_init(new_map);
		ebitmap_set_bit(new_map, value - 1, 1);
	}
	flex_array_put_ptr(db->type_val_to_struct_array, value - 1, type, GFP_KERNEL);
	flex_array_put_ptr(db->sym_val_to_name[SYM_TYPES], value - 1, key, GFP_KERNEL);

	for (i = 0; i < db->p_roles.nprim; ++i)
		ebitmap_set_bit(&db->role_val_to_struct[i]->types, value - 1, 1);

	return true;

err_free:
	nksu_flex_free_ebitmaps(attr_map, value - 1);
	flex_array_free(val_to_struct);
	flex_array_free(val_to_name);
	kfree(key);
	kfree(type);
	return false;
}
#endif

static bool rule_set_type_state(struct policydb *db, const char *type_name, bool permissive)
{
	struct type_datum *type;

	if (type_name == NULL || !*type_name) {
		struct hashtab_node *node;

		for_each_htable(db->p_types.table, node)
		{
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
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 7, 0)
	struct ebitmap *sattr = &db->type_attr_map_array[type->value - 1];
#else
	struct ebitmap *sattr = flex_array_get(db->type_attr_map_array, type->value - 1);
#endif
	struct hashtab_node *node;
	struct constraint_node *n;
	struct constraint_expr *e;

	if (!sattr)
		return;
	ebitmap_set_bit(sattr, attr->value - 1, 1);

	for_each_htable(db->p_classes.table, node)
	{
		struct class_datum *cls = node->datum;

		for (n = cls->constraints; n; n = n->next) {
			for (e = n->expr; e; e = e->next) {
				if (e->expr_type == CEXPR_NAMES && e->type_names &&
				    ebitmap_get_bit(&e->type_names->types, attr->value - 1))
					ebitmap_set_bit(&e->names, type->value - 1, 1);
			}
		}
	}
}

static bool rule_add_typeattribute(struct policydb *db, const char *type, const char *attr)
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

/*
 * avc_reset() is expensive: it flushes the AVC and broadcasts on netlink and
 * xfrm.  A batch of mutations therefore commits once at the end instead of
 * after every rule.  Batches are bracketed by sepolicy_batch_begin()/_end(); a
 * lone mutation (e.g. the manager's single IOC rule) resets immediately.
 */
static atomic_t rule_batch_depth = ATOMIC_INIT(0);
static bool rule_reset_pending;

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 7, 0)
/*
 * 4.9 policy mutations allocate memory (avtab nodes, hashtab nodes, grown
 * flex arrays) and must not do so under the policy rwlock, which is a
 * spinlock there.  Every mutation works on a private deep copy and installs it
 * with a short write-locked swap; a batch shares one copy.
 *
 * Batches are expected from a single context (boot rule load or one manager
 * IOC to ncore) as before.
 */
static DEFINE_MUTEX(nksu_rule_state_lock);
static struct policydb *nksu_rule_batch_pdb;
static unsigned int nksu_rule_batch_level;
static struct task_struct *nksu_rule_batch_owner;

static bool rule_in_batch(void)
{
	return nksu_rule_batch_level && nksu_rule_batch_owner == current;
}

static struct policydb *rule_batch_pdb(void)
{
	if (!nksu_rule_batch_pdb) {
		struct policydb *pdb = nksu_policydb_dup_live();

		if (IS_ERR(pdb))
			return NULL;
		nksu_rule_batch_pdb = pdb;
	}
	return nksu_rule_batch_pdb;
}
#endif

static void rule_mark_dirty(void)
{
	if (atomic_read(&rule_batch_depth))
		rule_reset_pending = true;
	else
		avc_reset();
}

void sepolicy_batch_begin(void)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 7, 0)
	if (nksu_rule_batch_level == 0) {
		mutex_lock(&nksu_rule_state_lock);
		nksu_rule_batch_owner = current;
	}
	nksu_rule_batch_level++;
#endif
	atomic_inc(&rule_batch_depth);
}

void sepolicy_batch_end(void)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 7, 0)
	bool outer;

	if (nksu_rule_batch_level)
		nksu_rule_batch_level--;
	outer = nksu_rule_batch_level == 0;
	if (outer) {
		if (nksu_rule_batch_pdb) {
			if (nksu_policydb_install(nksu_rule_batch_pdb))
				pr_err("[selinux]: cannot install updated policy\n");
			nksu_rule_batch_pdb = NULL;
		}
		nksu_rule_batch_owner = NULL;
		mutex_unlock(&nksu_rule_state_lock);
	}
#endif
	if (atomic_dec_and_test(&rule_batch_depth) && rule_reset_pending) {
		rule_reset_pending = false;
		avc_reset();
	}
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 7, 0)
#define RULE_LOCK_GET(db)                     \
	struct policydb *db;                  \
	mutex_lock(&selinux_state.policy_mutex); \
	db = fmac_get_pdb();                  \
	if (!db) {                            \
		mutex_unlock(&selinux_state.policy_mutex); \
		return -ENOENT;               \
	}

#define RULE_UNLOCK(ret)                           \
	mutex_unlock(&selinux_state.policy_mutex); \
	if ((ret) == 0)                            \
		rule_mark_dirty();
#else
#define RULE_LOCK_GET(db)                                             \
	struct policydb *db;                                          \
	bool __rule_batch = rule_in_batch();                          \
	if (__rule_batch) {                                           \
		db = rule_batch_pdb();                                \
		if (!db)                                              \
			return -ENOMEM;                               \
	} else {                                                      \
		mutex_lock(&nksu_rule_state_lock);                    \
		db = nksu_policydb_dup_live();                        \
		if (IS_ERR(db)) {                                     \
			mutex_unlock(&nksu_rule_state_lock);          \
			return PTR_ERR(db);                           \
		}                                                     \
	}

#define RULE_UNLOCK(ret)                                      \
	if (!__rule_batch) {                                  \
		if ((ret) == 0) {                             \
			int __inst = nksu_policydb_install(db); \
			if (__inst)                           \
				(ret) = __inst;               \
		} else {                                      \
			nksu_policydb_free(db);               \
		}                                             \
		mutex_unlock(&nksu_rule_state_lock);          \
		if ((ret) == 0)                               \
			rule_mark_dirty();                    \
	} else if ((ret) == 0) {                              \
		rule_mark_dirty();                            \
	}
#endif

int sepolicy_add_rule(const char *sname, const char *tname, const char *cname, const char *pname,
		      int effect, bool invert)
{
	int ret;
	RULE_LOCK_GET(db);

	ret = rule_add(db, sname, tname, cname, pname, effect, invert) ? 0 : -ENOENT;
	RULE_UNLOCK(ret);
	return ret;
}

int sepolicy_add_xperm(const char *s, const char *t, const char *c, const char *range, int effect,
		       bool invert)
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
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 7, 0)
	int ret;
	RULE_LOCK_GET(db);

	ret = symtab_search(&db->p_types, type_name) ? 1 : 0;
	mutex_unlock(&selinux_state.policy_mutex);
	return ret;
#else
	int ret;

	if (!selinux_state.ss)
		return -ENOENT;
	read_lock(&selinux_state.ss->policy_rwlock);
	ret = symtab_search(&selinux_state.ss->policydb.p_types, type_name) ? 1 : 0;
	read_unlock(&selinux_state.ss->policy_rwlock);
	return ret;
#endif
}

int sepolicy_add_type_transition(const char *s, const char *t, const char *c, const char *d,
				 const char *obj)
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
	struct type_datum *src = NULL;
	struct class_datum *cls = NULL;
	int ret = 0;

	RULE_LOCK_GET(db);

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
	RULE_UNLOCK(ret);
	return ret;
}

/* give sname every perm on every class to every type — basically unconfined */
int sepolicy_allow_any_any(const char *sname)
{
	struct type_datum *src = NULL;
	int ret = 0;

	RULE_LOCK_GET(db);

	if (sname && *sname && !(src = symtab_search(&db->p_types, sname))) {
		ret = -ENOENT;
		goto out;
	}

	rule_add_raw(db, src, NULL, NULL, NULL, AVTAB_ALLOWED, false);
	rule_add_raw(db, src, NULL, NULL, NULL, AVTAB_AUDITDENY, true);
out:
	RULE_UNLOCK(ret);
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
	struct avtab_node *node;
	int i, ret = 0;

	RULE_LOCK_GET(db);

	for (i = 0; i < db->te_avtab.nslot; i++) {
		for (node = nksu_avtab_bucket(&db->te_avtab, i); node; node = node->next) {
			if (node->key.specified & AVTAB_AUDITDENY)
				node->datum.u.data = ~0U;
		}
	}
	RULE_UNLOCK(ret);
	return ret;
}
#endif /* CONFIG_NKSU_DEBUG */
