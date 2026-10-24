// SPDX-License-Identifier: GPL-2.0
/*
 * profile_store — persist the uid profile table between reboots.
 *
 * The manager no longer re-pushes profiles on every startup; instead the
 * kernel mirrors each mutation to a plain-text file and restores it once at
 * feature-init time.  The format is deliberately simple so userspace (the
 * manager / ncore) can read it too:
 *
 *   # comment
 *   <uid> <caps_hex> <ns> <root_uid> <selinux_domain>
 *
 * <root_uid> is the uid the authorized process switches to after escalation
 * (0 = real root).  Lines written by older builds have no <root_uid> field and
 * are still accepted, defaulting it to 0.
 *
 * File I/O runs under a borrowed cred switched to the nksu domain (which the
 * policy makes unconfined) because the mutating callers — the manager's ioctl
 * handler in particular — run with an app's credentials that cannot touch
 * /data/adb.
 */

#include "klog.h"
#include "guards.h"
#include <linux/capability.h>
#include <linux/cred.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 8, 0)
#include <linux/minmax.h>
#endif
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>

#include "manager/manager.h"
#include "privilege/profile.h"
#include "privilege/profile_store.h"
#include "selinux/selinux.h"

/* Last: redirects filp_open/kernel_read/kernel_write to resolved pointers. */
#include "symbol/symbol_compat.h"

/*
 * The store's file I/O goes through resolved pointers, so disable CFI for this
 * file: the type-hash the module emits for the indirect calls may differ from
 * the running kernel's.
 */
#if defined(__clang__)
#pragma clang attribute push(__attribute__((no_sanitize("cfi"))), apply_to = function)
#endif

#define NKSU_PROFILE_FILE "/data/adb/nksu/allow.profile"
#define NKSU_PROFILE_FILE_MAX (256 * 1024)

static DEFINE_MUTEX(store_lock);
static bool store_suppress;

/* A kernel_cap_t is an 8-byte opaque struct on every supported kernel. */
static inline u64 encode_caps(kernel_cap_t caps)
{
	u64 value = 0;

	memcpy(&value, &caps, min_t(size_t, sizeof(value), sizeof(caps)));
	return value;
}

static inline kernel_cap_t decode_caps(u64 value)
{
	kernel_cap_t caps;

	memset(&caps, 0, sizeof(caps));
	memcpy(&caps, &value, min_t(size_t, sizeof(value), sizeof(caps)));
	return caps;
}

/*
 * The store's file operations need a cred that can reach /data/adb; they run
 * with one borrowed from the nksu domain (see nksu_borrow_creds_begin()).
 */

/* ── serialization ── */

struct save_ctx {
	char *buf;
	size_t cap;
	size_t len;
	bool overflow;
};

static void save_entry(uid_t uid, uid_t root_uid, kernel_cap_t caps, int ns, const char *domain,
		       size_t domain_len, void *ctx)
{
	struct save_ctx *s = ctx;
	char line[160];
	int n;

	/* The manager's entry is re-granted by the boot scan, never restored. */
	if (is_manager_uid(uid))
		return;

	if (domain_len > 0)
		n = scnprintf(line, sizeof(line), "%u %016llx %d %u %.*s\n", uid,
			      (unsigned long long)encode_caps(caps), ns, (unsigned)root_uid,
			      (int)min_t(size_t, domain_len, 64), domain);
	else
		n = scnprintf(line, sizeof(line), "%u %016llx %d %u -\n", uid,
			      (unsigned long long)encode_caps(caps), ns, (unsigned)root_uid);
	if (n <= 0)
		return;
	if (s->len + (size_t)n >= s->cap) {
		s->overflow = true;
		return;
	}
	memcpy(s->buf + s->len, line, (size_t)n);
	s->len += (size_t)n;
}

int nksu_profile_to_text(char *buf, size_t cap)
{
	struct save_ctx s = { .buf = buf, .cap = cap, .len = 0, .overflow = false };
	int head;

	if (!buf || cap == 0)
		return -ENOSPC;

	head = scnprintf(buf, cap,
			 "# nksu allow.profile v1\n"
			 "# <uid> <caps_hex> <ns> <root_uid> <selinux_domain>\n");
	if (head > 0)
		s.len = (size_t)head;

	nksu_profile_foreach(save_entry, &s);

	if (s.overflow || s.len + 1 > cap)
		return -ENOSPC;
	buf[s.len] = '\0';
	return (int)s.len;
}

void nksu_profile_store_save(void)
{
	const struct cred *old;
	struct file *fp __free(nksu_filp) = NULL;
	loff_t pos = 0;
	size_t cap;
	char *buf __free(nksu_kvfree) = NULL;
	int len;

	if (READ_ONCE(store_suppress))
		return;

	guard(nksu_mutex)(&store_lock);

	cap = 64 + ((size_t)nksu_profile_count() + 1) * 160;
	if (cap > NKSU_PROFILE_FILE_MAX)
		cap = NKSU_PROFILE_FILE_MAX;

	buf = kvmalloc(cap, GFP_KERNEL);
	if (!buf)
		return;

	len = nksu_profile_to_text(buf, cap);
	if (len < 0) {
		/* Should not happen: cap is sized from the entry count. */
		pr_warn("[profile] serialize for %s failed: %d\n", NKSU_PROFILE_FILE, len);
		return;
	}

	old = nksu_borrow_creds_begin();
	if (!old)
		return;

	fp = filp_open(NKSU_PROFILE_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (!IS_ERR(fp)) {
		while (pos < len) {
			ssize_t written = nksu_kwrite(fp, buf + pos, (size_t)(len - pos), &pos);

			if (written < 0) {
				pr_err("[profile] save %s failed: %d\n", NKSU_PROFILE_FILE,
				       (int)written);
				break;
			}
			if (written == 0) {
				pr_err("[profile] save %s: short write at %lld\n",
				       NKSU_PROFILE_FILE, pos);
				break;
			}
		}
	} else if (PTR_ERR(fp) != -ENOENT) {
		pr_err("[profile] save %s failed: %ld\n", NKSU_PROFILE_FILE, PTR_ERR(fp));
	}

	nksu_borrow_creds_end(old);
}

/* ── parsing ── */

/* Copy `src` (not necessarily NUL-terminated) into `dst`, dropping blanks. */
static void copy_trimmed(char *dst, size_t dst_size, const char *src, size_t src_len)
{
	size_t start = 0, end = src_len;

	while (start < end && (src[start] == ' ' || src[start] == '\t'))
		start++;
	while (end > start && (src[end - 1] == ' ' || src[end - 1] == '\t' || src[end - 1] == '\r'))
		end--;

	if (end - start >= dst_size)
		end = start + dst_size - 1;
	memcpy(dst, src + start, end - start);
	dst[end - start] = '\0';
}

/* Count whitespace-separated tokens, used to tell the old 3/4-field lines
 * (no <root_uid>) apart from the 5-field lines written by current builds. */
static int count_tokens(const char *line)
{
	int n = 0;
	const char *p = line;

	while (*p) {
		while (*p == ' ' || *p == '\t' || *p == '\r')
			p++;
		if (!*p)
			break;
		n++;
		while (*p && *p != ' ' && *p != '\t' && *p != '\r')
			p++;
	}
	return n;
}

static int apply_line(char *line)
{
	char *cursor = line;
	char *token;
	char domain[64];
	unsigned int uid;
	unsigned int root_uid = 0;
	unsigned long long caps;
	int ns;
	int ret;
	bool has_root_uid;

	while (*cursor == ' ' || *cursor == '\t')
		cursor++;
	if (*cursor == '\0' || *cursor == '#' || *cursor == '\r')
		return 0;

	/* 5 tokens means a <root_uid> field is present (current format). */
	has_root_uid = count_tokens(cursor) >= 5;

	token = strsep(&cursor, " \t");
	if (!token)
		return -EINVAL;
	ret = kstrtouint(token, 10, &uid);
	if (ret)
		return ret;

	token = strsep(&cursor, " \t");
	if (!token)
		return -EINVAL;
	ret = kstrtoull(token, 16, &caps);
	if (ret)
		return ret;

	token = strsep(&cursor, " \t");
	if (!token)
		return -EINVAL;
	ret = kstrtoint(token, 10, &ns);
	if (ret)
		return ret;

	if (has_root_uid) {
		token = strsep(&cursor, " \t");
		if (!token)
			return -EINVAL;
		ret = kstrtouint(token, 10, &root_uid);
		if (ret)
			return ret;
	}

	copy_trimmed(domain, sizeof(domain), cursor ? cursor : "", cursor ? strlen(cursor) : 0);
	if (domain[0] == '-' && domain[1] == '\0')
		domain[0] = '\0';

	return nksu_profile_set((uid_t)uid, (uid_t)root_uid, decode_caps(caps), domain, ns);
}

int nksu_profile_store_load(void)
{
	const struct cred *old;
	loff_t pos = 0;
	loff_t size;
	ssize_t rd;
	char *buf __free(nksu_kvfree) = NULL;
	char *cursor;
	int ret = 0;
	struct file *fp __free(nksu_filp) = NULL;

	old = nksu_borrow_creds_begin();
	if (!old)
		return -ENOMEM;

	fp = filp_open(NKSU_PROFILE_FILE, O_RDONLY, 0);
	if (IS_ERR(fp)) {
		nksu_borrow_creds_end(old);
		return PTR_ERR(fp) == -ENOENT ? 0 : PTR_ERR(fp);
	}

	size = i_size_read(fp->f_inode);
	if (size <= 0 || size > NKSU_PROFILE_FILE_MAX) {
		nksu_borrow_creds_end(old);
		return size > NKSU_PROFILE_FILE_MAX ? -EFBIG : 0;
	}

	buf = kvmalloc((size_t)size + 1, GFP_KERNEL);
	if (!buf) {
		nksu_borrow_creds_end(old);
		return -ENOMEM;
	}

	rd = nksu_kread(fp, buf, (size_t)size, &pos);
	nksu_borrow_creds_end(old);

	if (rd <= 0)
		return rd < 0 ? (int)rd : 0;
	buf[rd] = '\0';

	/*
	 * Suppress persistence while replaying: applying each line goes through
	 * nksu_profile_set(), which would otherwise rewrite the file per entry.
	 */
	WRITE_ONCE(store_suppress, true);

	cursor = buf;
	while (cursor && *cursor) {
		char *line = strsep(&cursor, "\n");

		if (line && *line) {
			int line_ret = apply_line(line);

			if (line_ret && !ret)
				ret = line_ret;
		}
	}

	WRITE_ONCE(store_suppress, false);

	if (ret)
		pr_warn("[profile] %s had an entry that failed to load: %d\n", NKSU_PROFILE_FILE,
			ret);
	return ret;
}

/* Persistence hook called from profile.c after every successful mutation. */
void nksu_profile_persist(void)
{
	nksu_profile_store_save();
}

int nksu_profile_store_init(void)
{
	int ret = nksu_profile_store_load();

	if (ret)
		pr_warn("[profile] failed to restore %s: %d\n", NKSU_PROFILE_FILE, ret);
	else
		pr_info("[profile] restored %u entries from %s\n", nksu_profile_count(),
			NKSU_PROFILE_FILE);
	return 0;
}

void nksu_profile_store_exit(void)
{
	nksu_borrow_creds_release();
}

#if defined(__clang__)
#pragma clang attribute pop
#endif
