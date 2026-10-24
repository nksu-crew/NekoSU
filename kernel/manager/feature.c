// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Kernel feature registry, driven by the manager over IOC_FEATURE_*.
 *
 * KernelSU keeps an equivalent table (policy/feature.c) and lets the daemon flip the
 * switches; here the manager talks to the kernel directly through the control
 * fd, so there is no userspace daemon in the loop.
 */
#include "klog.h"
#include "guards.h"
#include <linux/cred.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>

#include "manager/feature.h"
#include "selinux/selinux.h"

/* Last: redirects filp_open/kernel_read/kernel_write to resolved pointers. */
#include "symbol/symbol_compat.h"

/*
 * The registry's file I/O goes through resolved pointers, so disable CFI for
 * this file: the type-hash the module emits for the indirect calls may differ
 * from the running kernel's.
 */
#if defined(__clang__)
#pragma clang attribute push(__attribute__((no_sanitize("cfi"))), apply_to = function)
#endif

#define NKSU_MAX_FEATURES 16

static DEFINE_MUTEX(nksu_feature_mutex);
static const struct nksu_feature *nksu_features[NKSU_MAX_FEATURES];
static int nksu_feature_count;

static const struct nksu_feature *nksu_feature_find(u32 id)
{
	int i;

	for (i = 0; i < nksu_feature_count; i++) {
		if (nksu_features[i]->id == id)
			return nksu_features[i];
	}
	return NULL;
}

int nksu_feature_register(const struct nksu_feature *feature)
{
	int i;

	if (!feature || !feature->name)
		return -EINVAL;

	scoped_guard(nksu_mutex, &nksu_feature_mutex)
	{
		for (i = 0; i < nksu_feature_count; i++) {
			if (nksu_features[i]->id == feature->id)
				return -EEXIST;
		}
		if (nksu_feature_count >= NKSU_MAX_FEATURES)
			return -ENOSPC;
		nksu_features[nksu_feature_count++] = feature;
	}
	return 0;
}

int nksu_feature_get(u32 id, u64 *value)
{
	const struct nksu_feature *feature;
	int ret = -ENOENT;

	if (!value)
		return -EINVAL;

	guard(nksu_mutex)(&nksu_feature_mutex);
	feature = nksu_feature_find(id);
	if (feature && feature->get)
		ret = feature->get(value);
	return ret;
}

int nksu_feature_set(u32 id, u64 value)
{
	const struct nksu_feature *feature;
	int ret = -ENOENT;

	guard(nksu_mutex)(&nksu_feature_mutex);
	feature = nksu_feature_find(id);
	if (feature && feature->set)
		ret = feature->set(value);
	return ret;
}

int nksu_feature_render(char *buf, size_t size)
{
	int i, len = 0;

	if (!buf || size == 0)
		return -EINVAL;

	guard(nksu_mutex)(&nksu_feature_mutex);
	for (i = 0; i < nksu_feature_count; i++) {
		const struct nksu_feature *feature = nksu_features[i];
		u64 value = 0;

		if (feature->get)
			feature->get(&value);

		len += scnprintf(buf + len, size - len, "%u %s %llu\n", feature->id, feature->name,
				 (unsigned long long)value);
		if ((size_t)len >= size)
			break;
	}

	return len;
}

/*
 * ── persistent enabled-feature set ──
 *
 * /data/adb/nksu/feature holds one enabled feature name per line.  The kernel
 * owns it and edits only its own entry, so several features can share the file.
 * File I/O borrows the nksu domain (unconfined, and the only domain allowed to
 * touch /data/adb); the cred is built per operation, since toggles and the boot
 * read are rare.
 */
#define NKSU_FEATURE_FILE "/data/adb/nksu/feature"
#define NKSU_FEATURE_FILE_MAX 4096

/* True when `line` is exactly `name` (surrounding blanks ignored). */
static bool nksu_feature_name_in_line(const char *line, const char *name)
{
	size_t nlen = strlen(name), llen;

	while (*line == ' ' || *line == '\t' || *line == '\r')
		line++;
	llen = strlen(line);
	while (llen && (line[llen - 1] == ' ' || line[llen - 1] == '\t' || line[llen - 1] == '\r'))
		llen--;

	return llen == nlen && memcmp(line, name, nlen) == 0;
}

bool nksu_feature_persist_has(const char *name)
{
	const struct cred *old;
	loff_t pos = 0, size;
	bool found = false;
	struct file *fp __free(nksu_filp) = NULL;
	char *buf __free(nksu_kvfree) = NULL;

	if (!name || !name[0])
		return false;

	old = nksu_borrow_creds_begin();
	if (!old)
		return false;

	fp = filp_open(NKSU_FEATURE_FILE, O_RDONLY, 0);
	if (IS_ERR(fp))
		goto out;

	size = i_size_read(fp->f_inode);
	if (size <= 0 || size > NKSU_FEATURE_FILE_MAX)
		goto out;

	buf = kvmalloc((size_t)size + 1, GFP_KERNEL);
	if (!buf)
		goto out;

	{
		ssize_t rd = nksu_kread(fp, buf, (size_t)size, &pos);

		if (rd > 0) {
			char *cursor = buf;

			buf[rd] = '\0';
			while (cursor && *cursor) {
				char *line = strsep(&cursor, "\n");

				if (line && nksu_feature_name_in_line(line, name)) {
					found = true;
					break;
				}
			}
		}
	}

out:
	nksu_borrow_creds_end(old);
	return found;
}

int nksu_feature_persist_set(const char *name, bool enabled)
{
	const struct cred *old;
	loff_t pos = 0, size;
	size_t out_cap, out_len = 0;
	bool present = false;
	int ret = 0;
	struct file *rf __free(nksu_filp) = NULL;
	struct file *wf __free(nksu_filp) = NULL;
	char *in __free(nksu_kvfree) = NULL;
	char *out __free(nksu_kvfree) = NULL;

	if (!name || !name[0])
		return -EINVAL;

	old = nksu_borrow_creds_begin();
	if (!old)
		return -ENOMEM;

	rf = filp_open(NKSU_FEATURE_FILE, O_RDONLY, 0);
	if (!IS_ERR(rf)) {
		size = i_size_read(rf->f_inode);
		if (size > 0 && size <= NKSU_FEATURE_FILE_MAX) {
			in = kvmalloc((size_t)size + 1, GFP_KERNEL);
			if (in) {
				ssize_t rd = nksu_kread(rf, in, (size_t)size, &pos);

				if (rd > 0)
					in[rd] = '\0';
				else {
					kvfree(in);
					in = NULL;
				}
			}
		}
	} else if (PTR_ERR(rf) != -ENOENT) {
		ret = PTR_ERR(rf);
		goto out;
	}

	out_cap = NKSU_FEATURE_FILE_MAX + strlen(name) + 2;
	out = kvmalloc(out_cap, GFP_KERNEL);
	if (!out) {
		ret = -ENOMEM;
		goto out;
	}

	if (in) {
		char *cursor = in;

		while (cursor && *cursor) {
			char *line = strsep(&cursor, "\n");

			if (!line || !*line)
				continue;
			if (nksu_feature_name_in_line(line, name)) {
				present = true;
				if (!enabled)
					continue; /* drop our own entry */
			}
			out_len += scnprintf(out + out_len, out_cap - out_len, "%s\n", line);
		}
	}

	if (enabled && !present)
		out_len += scnprintf(out + out_len, out_cap - out_len, "%s\n", name);

	wf = filp_open(NKSU_FEATURE_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (IS_ERR(wf)) {
		ret = PTR_ERR(wf);
	} else {
		pos = 0;
		while (pos < (loff_t)out_len) {
			ssize_t written = nksu_kwrite(wf, out + pos, out_len - (size_t)pos, &pos);

			if (written < 0) {
				ret = (int)written;
				break;
			}
			if (written == 0) {
				ret = -EIO;
				break;
			}
		}
		if (ret)
			pr_err("[feature] save %s failed: %d\n", NKSU_FEATURE_FILE, ret);
	}

out:
	nksu_borrow_creds_end(old);
	return ret;
}

#if defined(__clang__)
#pragma clang attribute pop
#endif
