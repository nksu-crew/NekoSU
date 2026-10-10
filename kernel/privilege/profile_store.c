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
 *   <uid> <caps_hex> <ns> <selinux_domain>
 *
 * File I/O runs under a borrowed cred switched to the nksu domain (which the
 * policy makes unconfined) because the mutating callers — the manager's ioctl
 * handler in particular — run with an app's credentials that cannot touch
 * /data/adb.
 */

#include <linux/capability.h>
#include <linux/cred.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/minmax.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>

#include "manager/manager.h"
#include "privilege/profile.h"
#include "privilege/profile_store.h"
#include "selinux/selinux.h"

#define NKSU_PROFILE_FILE "/data/adb/nksu/allow.profile"
#define NKSU_PROFILE_FILE_MAX (256 * 1024)

static DEFINE_MUTEX(store_lock);
static struct cred *store_cred;
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
 * The store's file operations need a cred that can reach /data/adb.  Borrow
 * the nksu domain with the full capability set, like the manager scan does.
 * The first caller builds it; later callers reuse it.
 */
static const struct cred *store_creds_begin(void)
{
	if (!store_cred) {
		struct cred *cred = prepare_creds();

		if (!cred)
			return NULL;

		cred->cap_effective = CAP_FULL_SET;
		cred->cap_permitted = CAP_FULL_SET;
		cred->cap_bset = CAP_FULL_SET;
		cred->cap_inheritable = CAP_FULL_SET;

		if (set_domain(DOMAIN_CTX, cred)) {
			abort_creds(cred);
			return NULL;
		}
		store_cred = cred;
	}

	return override_creds(store_cred);
}

static void store_creds_end(const struct cred *old)
{
	if (old)
		revert_creds(old);
}

/* ── serialization ── */

struct save_ctx {
	char *buf;
	size_t cap;
	size_t len;
	bool overflow;
};

static void save_entry(uid_t uid, kernel_cap_t caps, int ns, const char *domain,
		       size_t domain_len, void *ctx)
{
	struct save_ctx *s = ctx;
	char line[160];
	int n;

	/* The manager's entry is re-granted by the boot scan, never restored. */
	if (is_manager_uid(uid))
		return;

	n = scnprintf(line, sizeof(line), "%u %016llx %d %.*s\n", uid,
		      (unsigned long long)encode_caps(caps), ns,
		      (int)min_t(size_t, domain_len, 64), domain);
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
			 "# <uid> <caps_hex> <ns> <selinux_domain>\n");
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
	struct file *fp;
	loff_t pos = 0;
	size_t cap;
	char *buf;
	int len;

	if (READ_ONCE(store_suppress))
		return;

	mutex_lock(&store_lock);

	cap = 64 + ((size_t)nksu_profile_count() + 1) * 160;
	if (cap > NKSU_PROFILE_FILE_MAX)
		cap = NKSU_PROFILE_FILE_MAX;

	buf = kvmalloc(cap, GFP_KERNEL);
	if (!buf) {
		mutex_unlock(&store_lock);
		return;
	}

	len = nksu_profile_to_text(buf, cap);
	if (len < 0) {
		/* Should not happen: cap is sized from the entry count. */
		pr_warn("[profile] serialize for %s failed: %d\n",
			NKSU_PROFILE_FILE, len);
		kvfree(buf);
		mutex_unlock(&store_lock);
		return;
	}

	old = store_creds_begin();
	if (!old) {
		kvfree(buf);
		mutex_unlock(&store_lock);
		return;
	}

	fp = filp_open(NKSU_PROFILE_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (!IS_ERR(fp)) {
		kernel_write(fp, buf, (size_t)len, &pos);
		filp_close(fp, NULL);
	} else if (PTR_ERR(fp) != -ENOENT) {
		pr_warn("[profile] save %s failed: %ld\n", NKSU_PROFILE_FILE,
			PTR_ERR(fp));
	}

	store_creds_end(old);
	kvfree(buf);
	mutex_unlock(&store_lock);
}

/* ── parsing ── */

/* Copy `src` (not necessarily NUL-terminated) into `dst`, dropping blanks. */
static void copy_trimmed(char *dst, size_t dst_size, const char *src,
			 size_t src_len)
{
	size_t start = 0, end = src_len;

	while (start < end && (src[start] == ' ' || src[start] == '\t'))
		start++;
	while (end > start && (src[end - 1] == ' ' || src[end - 1] == '\t' ||
			       src[end - 1] == '\r'))
		end--;

	if (end - start >= dst_size)
		end = start + dst_size - 1;
	memcpy(dst, src + start, end - start);
	dst[end - start] = '\0';
}

static int apply_line(char *line)
{
	char *cursor = line;
	char *token;
	char domain[64];
	unsigned int uid;
	unsigned long long caps;
	int ns;
	int ret;

	while (*cursor == ' ' || *cursor == '\t')
		cursor++;
	if (*cursor == '\0' || *cursor == '#' || *cursor == '\r')
		return 0;

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

	copy_trimmed(domain, sizeof(domain), cursor ? cursor : "", cursor ? strlen(cursor) : 0);
	if (domain[0] == '-' && domain[1] == '\0')
		domain[0] = '\0';

	return nksu_profile_set((uid_t)uid, decode_caps(caps), domain, ns);
}

int nksu_profile_store_load(void)
{
	const struct cred *old;
	struct file *fp;
	loff_t pos = 0;
	loff_t size;
	ssize_t rd;
	char *buf;
	char *cursor;
	int ret = 0;

	old = store_creds_begin();
	if (!old)
		return -ENOMEM;

	fp = filp_open(NKSU_PROFILE_FILE, O_RDONLY, 0);
	if (IS_ERR(fp)) {
		store_creds_end(old);
		return PTR_ERR(fp) == -ENOENT ? 0 : PTR_ERR(fp);
	}

	size = i_size_read(fp->f_inode);
	if (size <= 0 || size > NKSU_PROFILE_FILE_MAX) {
		filp_close(fp, NULL);
		store_creds_end(old);
		return size > NKSU_PROFILE_FILE_MAX ? -EFBIG : 0;
	}

	buf = kvmalloc((size_t)size + 1, GFP_KERNEL);
	if (!buf) {
		filp_close(fp, NULL);
		store_creds_end(old);
		return -ENOMEM;
	}

	rd = kernel_read(fp, buf, (size_t)size, &pos);
	filp_close(fp, NULL);
	store_creds_end(old);

	if (rd <= 0) {
		kvfree(buf);
		return rd < 0 ? (int)rd : 0;
	}
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
	kvfree(buf);

	if (ret)
		pr_warn("[profile] %s had an entry that failed to load: %d\n",
			NKSU_PROFILE_FILE, ret);
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
		pr_warn("[profile] failed to restore %s: %d\n", NKSU_PROFILE_FILE,
			ret);
	else
		pr_info("[profile] restored %u entries from %s\n",
			nksu_profile_count(), NKSU_PROFILE_FILE);
	return 0;
}

void nksu_profile_store_exit(void)
{
	if (store_cred) {
		put_cred(store_cred);
		store_cred = NULL;
	}
}
