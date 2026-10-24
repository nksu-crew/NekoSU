#include "klog.h"
#include "guards.h"
#include "manager/manager.h"
#include "manager/abx.h"
#include "boot/safe_mode.h"
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/namei.h>
#include <linux/dcache.h>
#include <linux/mount.h>
#include <linux/path.h>
#include <linux/crypto.h>
#include <crypto/hash.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/atomic.h>
#include <linux/fsnotify_backend.h>
#include <linux/task_work.h>
#include <linux/cred.h>
#include <linux/capability.h>
#include <linux/workqueue.h>
#include <fmac.h>
#include "hook/handle.h"

/* Last: redirects vfs_mkdir/lookup_one_len to the resolved-symbol pointers. */
#include "symbol/symbol_compat.h"

/*
 * vfs_mkdir()/lookup_one_len() are reached through resolved pointers, so
 * disable CFI for this file: the type-hash the module emits for the indirect
 * calls may differ from the running kernel's.
 */
#if defined(__clang__)
#pragma clang attribute push(__attribute__((no_sanitize("cfi"))), apply_to = function)
#endif

/*
 * Manager identity.  The manager is recognised purely by its signing
 * certificate, never by its package name: it can be reinstalled under a new
 * name (rotation) as long as it keeps the signing key.  Its user id is read
 * straight from the matched packages.xml entry (the numeric sharedUserId), so
 * the whole shared uid -- not a single package -- gets manager rights.
 */
#define TARGET_HASH \
	"\xd8\x09\xb6\x34\x80\xbe\xea\x60\x86\xcd\x78\x68\xe4\x2f\x2e\xb2\xb9\x20\x2c\x67\x0c\x0d\x3d\xb0\x89\x4e\x9e\x43\x12\xf1\x28\xe9"

#define PACKAGES_XML_PATH "/data/system/packages.xml"
#define MAX_PACKAGES_XML_SIZE (8 * 1024 * 1024)

static kuid_t manager_kuid = INVALID_UID;

bool is_manager(void)
{
	return uid_valid(manager_kuid) && uid_eq(current_uid(), manager_kuid);
}

bool is_manager_uid(uid_t uid)
{
	return uid_valid(manager_kuid) && __kuid_val(manager_kuid) == uid;
}

/*
 * Drop the current manager's kernel identity and its profile entry.  Called
 * when a complete packages.xml scan proves no package is signed with the
 * manager certificate, or when the verified UID's new profile cannot be
 * installed.  Transient read/parse failures keep the last verified grant.
 */
static void revoke_manager(void)
{
	kuid_t old = manager_kuid;
	int ret;

	if (!uid_valid(old))
		return;

	manager_kuid = INVALID_UID;
	ret = nksu_profile_clear(__kuid_val(old));
	if (ret && ret != -ENOENT)
		pr_err("[manager] cannot revoke UID %u profile: %d\n", __kuid_val(old), ret);
	else
		pr_info("[manager] revoked manager UID %u\n", __kuid_val(old));
}

/*
 * Whether /data/adb/ncore is known to be present.  Set by the (sleepable)
 * manager scan; read by the su tracepoint hook, which must not sleep.
 */
static bool daemon_ready;

bool is_daemon_ready(void)
{
	return READ_ONCE(daemon_ready);
}

/* The ABX (Android Binary XML) parser lives in manager/abx.c. */

/*
 * One SHA-256 transform for the whole scan.  The certificate loop hashes every
 * candidate package, so allocating (and tearing down) a shash per call is pure
 * overhead; allocate it once and reset it per message.
 */
struct sha256_hasher {
	struct crypto_shash *tfm;
	struct shash_desc *desc;
};

static int sha256_hasher_init(struct sha256_hasher *h)
{
	h->tfm = crypto_alloc_shash("sha256", 0, 0);
	if (IS_ERR(h->tfm)) {
		h->tfm = NULL;
		return -1;
	}
	h->desc = kmalloc(sizeof(*h->desc) + crypto_shash_descsize(h->tfm), GFP_KERNEL);
	if (!h->desc) {
		crypto_free_shash(h->tfm);
		h->tfm = NULL;
		return -1;
	}
	h->desc->tfm = h->tfm;
	return 0;
}

static void sha256_hasher_exit(struct sha256_hasher *h)
{
	kfree(h->desc);
	if (h->tfm)
		crypto_free_shash(h->tfm);
}

static int sha256_hasher_run(struct sha256_hasher *h, const u8 *data, size_t len, u8 out[32])
{
	if (crypto_shash_init(h->desc) || crypto_shash_update(h->desc, data, len) ||
	    crypto_shash_final(h->desc, out))
		return -1;
	return 0;
}

/*
 * Accept a package whose signing certificate hashes to TARGET_HASH.  This is
 * the only thing that identifies the manager, so its package name is free to
 * change between installs.  `ctx` is the scan-wide sha256_hasher.
 */
static bool manager_cert_match(const u8 *cert, size_t cert_len, void *ctx)
{
	struct sha256_hasher *h = ctx;
	u8 hash[32];

	return sha256_hasher_run(h, cert, cert_len, hash) == 0 && !memcmp(hash, TARGET_HASH, 32);
}

/*
 * Find the manager package in /data/system/packages.xml and capture its user
 * id, on-disk locations and signing certificate.  The ABX parsing lives in
 * manager/abx.c; here we only supply the certificate predicate and, on a hit,
 * take the uid the entry carries.  nativeLibraryPath is the extracted jniLibs
 * directory (<codePath>/lib/<abi>); codePath is the fallback for images that do
 * not write it.
 *
 * The file and the XML buffer are released by __free() on every exit path.
 */
enum manager_verify_result {
	MANAGER_VERIFY_OK,
	MANAGER_VERIFY_ABSENT,
	MANAGER_VERIFY_ERROR,
};

static enum manager_verify_result verify_package_signature(struct abx_package *paths)
{
	loff_t pos = 0;
	ssize_t rd;
	size_t fsize;
	struct file *fp __free(nksu_filp) = filp_open(PACKAGES_XML_PATH, O_RDONLY, 0);
	struct sha256_hasher hasher;
	enum abx_result abx_ret;

	if (IS_ERR(fp)) {
		pr_err("[manager] cannot open %s: %ld\n", PACKAGES_XML_PATH, PTR_ERR(fp));
		return MANAGER_VERIFY_ERROR;
	}

	fsize = i_size_read(fp->f_inode);
	if (fsize < 4 || fsize > MAX_PACKAGES_XML_SIZE) {
		pr_err("[manager] invalid %s size: %zu\n", PACKAGES_XML_PATH, fsize);
		return MANAGER_VERIFY_ERROR;
	}

	u8 *buf __free(nksu_kvfree) = kvmalloc(fsize, GFP_KERNEL);
	if (!buf) {
		pr_err("[manager] cannot allocate %zu bytes for %s\n", fsize, PACKAGES_XML_PATH);
		return MANAGER_VERIFY_ERROR;
	}

	rd = nksu_kread(fp, buf, fsize, &pos);
	if (rd != (ssize_t)fsize) {
		pr_err("[manager] short read from %s: %zd of %zu bytes\n", PACKAGES_XML_PATH, rd,
		       fsize);
		return MANAGER_VERIFY_ERROR;
	}

	if (sha256_hasher_init(&hasher)) {
		pr_err("[manager] cannot initialize SHA-256 for manager scan\n");
		return MANAGER_VERIFY_ERROR;
	}

	abx_ret = abx_find_package_by_cert(buf, fsize, manager_cert_match, &hasher, paths);
	sha256_hasher_exit(&hasher);

	switch (abx_ret) {
	case ABX_PACKAGE_FOUND:
		pr_info("[manager] %s (uid %u) has the manager signature\n", paths->name,
			paths->uid);
		return MANAGER_VERIFY_OK;
	case ABX_PACKAGE_NOT_FOUND:
		pr_err("[manager] no package signed with the manager certificate\n");
		return MANAGER_VERIFY_ABSENT;
	default:
		pr_err("[manager] cannot parse %s\n", PACKAGES_XML_PATH);
		return MANAGER_VERIFY_ERROR;
	}
}

static uid_t get_uid_from_packages_list(const char *package_name)
{
	char *line, *p, *token;
	loff_t pos = 0;
	uid_t target_uid = (uid_t)-1;
	size_t fsize;
	ssize_t read_size;
	struct file *file __free(nksu_filp) = filp_open("/data/system/packages.list", O_RDONLY, 0);

	if (IS_ERR(file))
		return (uid_t)-1;

	fsize = i_size_read(file->f_inode);
	if (fsize == 0 || fsize > MAX_PACKAGES_XML_SIZE)
		return (uid_t)-1;

	/* The list can exceed one read buffer; read it whole. */
	char *buf __free(nksu_kvfree) = kvmalloc(fsize + 1, GFP_KERNEL);
	if (!buf)
		return (uid_t)-1;

	read_size = nksu_kread(file, buf, fsize, &pos);
	if (read_size <= 0)
		return (uid_t)-1;
	buf[read_size] = '\0';

	p = buf;
	while ((line = strsep(&p, "\n")) != NULL) {
		token = strsep(&line, " ");
		if (token && strcmp(token, package_name) == 0) {
			token = strsep(&line, " ");
			if (token && kstrtouint(token, 10, &target_uid) == 0)
				break;
		}
	}

	return target_uid;
}

static int get_task_cmdline(struct task_struct *task, char *buffer, int buflen)
{
	unsigned long arg_start, arg_end, len;
	int res = 0;
	struct mm_struct *mm __free(nksu_mmput) = get_task_mm(task);

	if (!mm)
		return 0;

	mmap_read_lock(mm);
	arg_start = mm->arg_start;
	arg_end = mm->arg_end;
	mmap_read_unlock(mm);

	len = arg_end - arg_start;
	if (len > buflen - 1)
		len = buflen - 1;

	if (len > 0) {
		res = access_process_vm(task, arg_start, buffer, len, 0);
		if (res > 0)
			buffer[res] = '\0';
		else
			buffer[0] = '\0';
	}

	return res;
}

/* Cap the thread-group snapshot; the process list is far smaller. */
#define NKSU_ZYGOTE_SNAPSHOT_MAX 4096

static int mark_zygote(void)
{
	struct task_struct *p;
	int n = 0, i;
	char *cmdline_buf __free(nksu_kfree) = kmalloc(256, GFP_KERNEL);

	if (!cmdline_buf)
		return -ENOMEM;

	struct task_struct **tasks __free(nksu_kfree) =
		kmalloc_array(NKSU_ZYGOTE_SNAPSHOT_MAX, sizeof(*tasks), GFP_KERNEL);
	if (!tasks)
		return -ENOMEM;

	/*
     * get_task_cmdline() sleeps (it takes mmap_lock and calls
     * access_process_vm()), so it must not run under rcu_read_lock() -- that
     * warns "Voluntary context switch within RCU read-side critical section".
     * Pin every candidate under RCU, then read the cmdlines after unlocking.
     *
     * The tracepoint flag is only ever set (and new threads inherit it), so a
     * process already carrying it needs no re-check: skip it here and never
     * pay for its cmdline copy.  This makes every scan after the first one
     * touch only newly created processes.
     */
	rcu_read_lock();
	for_each_process(p) {
		if (p->flags & PF_KTHREAD)
			continue;
		if (test_tsk_thread_flag(p, TIF_SYSCALL_TRACEPOINT))
			continue;
		if (n == NKSU_ZYGOTE_SNAPSHOT_MAX)
			break;
		get_task_struct(p);
		tasks[n++] = p;
	}
	rcu_read_unlock();

	for (i = 0; i < n; i++) {
		pid_t pid;
		uid_t uid;
		const char *label;

		p = tasks[i];
		pid = p->pid;
		rcu_read_lock();
		uid = __kuid_val(task_uid(p));
		rcu_read_unlock();

		/*
		 * Refresh an already-running authorized process: the tracepoint
		 * flag can be cleared by other tracing users between scans, so
		 * its absence does not prove this process is irrelevant.
		 *
		 * App workers may also be pre-forked (USAP on devices where it
		 * is enabled).  Match their short comm first so a failed cmdline
		 * read cannot silently leave the whole app pool unmarked.
		 */
		if (is_manager_uid(uid) || nksu_profile_has_uid(uid)) {
			label = "authorized process";
		} else if (!strncmp(p->comm, "zygote", 6) || !strncmp(p->comm, "usap", 4) ||
			   strstr(p->comm, "app_process")) {
			label = p->comm;
		} else {
			if (get_task_cmdline(p, cmdline_buf, 256) <= 0) {
				pr_warn_ratelimited(
					"[manager] cannot read cmdline for pid=%d uid=%u\n", pid,
					uid);
				goto next;
			}
			if (strncmp(cmdline_buf, "zygote", 6) != 0 &&
			    strncmp(cmdline_buf, "usap", 4) != 0 &&
			    !strstr(cmdline_buf, "app_process"))
				goto next;
			label = cmdline_buf;
		}

		nksu_mark_threads_by_pid(pid);
		pr_info("[manager] : marked %s (pid=%d, uid=%u)\n", label, pid, uid);
	next:
		put_task_struct(p);
	}

	return 0;
}

/*
 * The scan runs from a kthread, i.e. in the kernel SELinux domain, which is
 * not allowed to read /data/system, so it runs with a cred borrowed from the
 * nksu domain via nksu_borrow_creds_begin().
 */

/*
 * Bootstrap /data/adb/nksu/ncore straight from the verified manager package, so root works on the very first boot without the manager
 * having to run `ncore install` from a root shell it does not have yet.
 *
 * The manager ships libncore.so as a jniLib; the kernel only sees the
 * extracted nativeLibraryPath directory from packages.xml.  This mirrors KernelSU's daemon-exists gate and the
 * manager-provided libncore.so, but installs from the kernel so no userspace
 * round-trip is needed.
 */
#define NKSU_DAEMON_BOOT_PATH "/data/adb/ncore"
#define NKSU_DAEMON_BIN_DIR "/data/adb/nksu/bin"
#define NKSU_DAEMON_LIB_NAME "libncore.so"

static bool kfile_exists(const char *path)
{
	struct file *f __free(nksu_filp) = filp_open(path, O_RDONLY, 0);

	return !IS_ERR(f);
}

/* mkdir one leaf whose parent already exists. */
static int kdir_mkdir(const char *path, umode_t mode)
{
	char parent[256];
	const char *slash;
	struct path p;
	struct inode *dir;
	struct dentry *dentry;
	size_t len;
	int err;

	slash = strrchr(path, '/');
	if (!slash || slash == path)
		return -EINVAL;

	len = (size_t)(slash - path);
	if (len >= sizeof(parent))
		return -ENAMETOOLONG;
	memcpy(parent, path, len);
	parent[len] = '\0';

	err = kern_path(parent, LOOKUP_DIRECTORY | LOOKUP_FOLLOW, &p);
	if (err)
		return err;

	dir = d_inode(p.dentry);
	inode_lock_nested(dir, I_MUTEX_PARENT);
	dentry = lookup_one_len(slash + 1, p.dentry, (int)strlen(slash + 1));
	if (IS_ERR(dentry)) {
		err = PTR_ERR(dentry);
	} else {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
		err = vfs_mkdir(mnt_idmap(p.mnt), dir, dentry, mode);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 12, 0)
		err = vfs_mkdir(mnt_user_ns(p.mnt), dir, dentry, mode);
#else
		err = vfs_mkdir(dir, dentry, mode);
#endif
		if (err == -EEXIST)
			err = 0;
		dput(dentry);
	}
	inode_unlock(dir);
	path_put(&p);
	return err;
}

static int kdir_mkdir_p(const char *dir)
{
	char tmp[256];
	size_t n = strlen(dir);
	int err = 0;

	if (n == 0 || n >= sizeof(tmp))
		return -EINVAL;
	memcpy(tmp, dir, n + 1);

	for (char *q = tmp + 1;; q++) {
		if (*q == '/' || *q == '\0') {
			char saved = *q;

			*q = '\0';
			if (!kfile_exists(tmp)) {
				int e = kdir_mkdir(tmp, 0700);

				if (e && !err)
					err = e;
			}
			*q = saved;
			if (saved == '\0')
				break;
		}
	}
	return err;
}

int nksu_kpath_mkdir_p(const char *dir)
{
	return kdir_mkdir_p(dir);
}

static void kfile_remove(struct file *file);

static int kfile_copy(const char *src, const char *dst)
{
	loff_t ipos = 0, opos = 0;
	loff_t src_size, dst_size;
	ssize_t off;
	int err = 0;
	struct file *in __free(nksu_filp) = filp_open(src, O_RDONLY, 0);

	if (IS_ERR(in))
		return PTR_ERR(in);

	char *buf __free(nksu_kfree) = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	struct file *out __free(nksu_filp) = filp_open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0755);
	if (IS_ERR(out))
		return PTR_ERR(out);

	for (;;) {
		ssize_t r = nksu_kread(in, buf, PAGE_SIZE, &ipos);

		if (r < 0) {
			err = (int)r;
			break;
		}
		if (r == 0)
			break;

		off = 0;
		while (off < r) {
			ssize_t w = nksu_kwrite(out, buf + off, (size_t)(r - off), &opos);

			if (w <= 0) {
				err = w ? (int)w : -EIO;
				break;
			}
			off += w;
		}
		if (err)
			break;
	}

	if (!err) {
		src_size = i_size_read(file_inode(in));
		dst_size = i_size_read(file_inode(out));
		if (src_size <= 0 || opos != src_size || dst_size != src_size)
			err = -EIO;
	}

	if (err)
		pr_err("[manager] copy %s -> %s failed: %d\n", src, dst, err);
	if (err)
		kfile_remove(out);

	return err;
}

static int kfile_size(const char *path, loff_t *size)
{
	struct file *file __free(nksu_filp) = filp_open(path, O_RDONLY, 0);

	if (IS_ERR(file))
		return PTR_ERR(file);
	*size = i_size_read(file_inode(file));
	return 0;
}

/* A failed copy must not leave a truncated file that the next boot trusts. */
static void kfile_remove(struct file *file)
{
	struct dentry *parent;
	struct inode *dir;
	int ret;

	if (!file || !file->f_path.dentry)
		return;

	parent = dget_parent(file->f_path.dentry);
	dir = d_inode(parent);
	if (!dir) {
		dput(parent);
		return;
	}

	inode_lock_nested(dir, I_MUTEX_PARENT);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
	ret = vfs_unlink(mnt_idmap(file->f_path.mnt), dir, file->f_path.dentry, NULL);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 12, 0)
	ret = vfs_unlink(mnt_user_ns(file->f_path.mnt), dir, file->f_path.dentry, NULL);
#else
	ret = vfs_unlink(dir, file->f_path.dentry, NULL);
#endif
	inode_unlock(dir);
	dput(parent);

	if (ret && ret != -ENOENT)
		pr_warn("[manager] cannot remove partial %s: %d\n",
			file->f_path.dentry->d_name.name, ret);
}

/* codePath is either the package dir or .../base.apk; reduce it to the dir. */
static void code_path_base(const char *code, char *out, size_t cap)
{
	size_t n = strlen(code);

	if (n > 4 && strcmp(code + n - 4, ".apk") == 0) {
		const char *slash = strrchr(code, '/');

		if (slash) {
			n = (size_t)(slash - code);
			if (n >= cap)
				n = cap - 1;
			memcpy(out, code, n);
			out[n] = '\0';
			return;
		}
	}
	strscpy(out, code, cap);
}

static bool resolve_lib_dir(const struct abx_package *paths, char *out, size_t cap)
{
	char cand[320];
	char base[256];

	pr_info("[manager] package paths: native='%s' code='%s'\n", paths->native_lib_path,
		paths->code_path);

	if (paths->native_lib_path[0]) {
		snprintf(cand, sizeof(cand), "%s/%s", paths->native_lib_path, NKSU_DAEMON_LIB_NAME);
		pr_info("[manager] trying %s\n", cand);
		if (kfile_exists(cand)) {
			strscpy(out, paths->native_lib_path, cap);
			return true;
		}
	}

	if (paths->code_path[0]) {
		code_path_base(paths->code_path, base, sizeof(base));

		snprintf(cand, sizeof(cand), "%s/lib/arm64/%s", base, NKSU_DAEMON_LIB_NAME);
		pr_info("[manager] trying %s\n", cand);
		if (kfile_exists(cand)) {
			snprintf(out, cap, "%s/lib/arm64", base);
			return true;
		}
		snprintf(cand, sizeof(cand), "%s/lib/arm/%s", base, NKSU_DAEMON_LIB_NAME);
		pr_info("[manager] trying %s\n", cand);
		if (kfile_exists(cand)) {
			snprintf(out, cap, "%s/lib/arm", base);
			return true;
		}
	}
	return false;
}

static int install_daemon_from_manager(const struct abx_package *paths)
{
	char lib_dir[256];
	char src[320];
	loff_t src_size, dst_size;
	int err;

	if (!resolve_lib_dir(paths, lib_dir, sizeof(lib_dir))) {
		/*
		 * The installed copy is still usable when the manager's APK no
		 * longer ships libncore.so.  Keep it, but only when it exists.
		 */
		if (!kfile_exists(NKSU_DAEMON_BOOT_PATH)) {
			pr_err("[manager] cannot locate %s in the manager install\n",
			       NKSU_DAEMON_LIB_NAME);
			return -ENOENT;
		}

		pr_warn("[manager] cannot verify %s, keeping the installed copy\n",
			NKSU_DAEMON_BOOT_PATH);
		nksu_relabel_path(NKSU_DAEMON_BOOT_PATH);
		WRITE_ONCE(daemon_ready, true);
		return 0;
	}

	snprintf(src, sizeof(src), "%s/%s", lib_dir, NKSU_DAEMON_LIB_NAME);
	if (!kfile_size(src, &src_size) && !kfile_size(NKSU_DAEMON_BOOT_PATH, &dst_size) &&
	    src_size > 0 && src_size == dst_size) {
		/*
		 * Already bootstrapped and complete.  Re-assert the label every
		 * boot: an older build created this as adb_data_file, which init
		 * cannot execute, and this also repairs a lost xattr.
		 */
		nksu_relabel_path(NKSU_DAEMON_BOOT_PATH);
		WRITE_ONCE(daemon_ready, true);
		return 0;
	}

	if (kfile_exists(NKSU_DAEMON_BOOT_PATH))
		pr_warn("[manager] %s is incomplete, reinstalling\n", NKSU_DAEMON_BOOT_PATH);

	err = kdir_mkdir_p("/data/adb/nksu");
	if (err && err != -EEXIST) {
		pr_err("[manager] mkdir /data/adb/nksu failed: %d\n", err);
		return err;
	}
	err = kdir_mkdir_p(NKSU_DAEMON_BIN_DIR);
	if (err && err != -EEXIST) {
		pr_err("[manager] mkdir %s failed: %d\n", NKSU_DAEMON_BIN_DIR, err);
		return err;
	}

	err = kfile_copy(src, NKSU_DAEMON_BOOT_PATH);
	if (err) {
		return err;
	}
	/* Label it nksu_file so init's injected init.rc can exec it. */
	nksu_relabel_path(NKSU_DAEMON_BOOT_PATH);

	pr_info("[manager] installed ncore from %s\n", lib_dir);
	WRITE_ONCE(daemon_ready, true);
	return 0;
}

/*
 * Scans can be triggered concurrently: the initial one from appscan_init() and
 * one task_work per packages.xml change.  Serialize them (the scan mutates the
 * profile table and caches the borrowed nksu creds).
 */
static DEFINE_MUTEX(appscan_mutex);

static int scan_and_apply(void)
{
	const struct cred *old;
	kuid_t kuid, previous;
	uid_t uid;
	int attempt, install_ret, ret = 0;
	enum manager_verify_result verify = MANAGER_VERIFY_ERROR;

	/*
     * Off the stack: abx_package carries several 256-byte path buffers and
     * would otherwise push this (heavily inlined) frame past the kernel's
     * limit.
     */
	struct abx_package *paths __free(nksu_kfree) = kzalloc(sizeof(*paths), GFP_KERNEL);
	if (!paths) {
		pr_err("[manager] cannot allocate manager scan state\n");
		return -ENOMEM;
	}

	guard(nksu_mutex)(&appscan_mutex);
	old = nksu_borrow_creds_begin();
	if (!old) {
		pr_err("[manager] cannot borrow credentials for manager scan\n");
		return -ENOMEM;
	}

	/*
     * Find the manager by signing certificate -- never by package name -- and
     * take the uid its packages.xml entry carries (the numeric sharedUserId),
     * so the whole shared uid, not a single package, gets manager rights.  Fall
     * back to packages.list if the entry somehow has no uid.
     */
	for (attempt = 0; attempt < 3; attempt++) {
		verify = verify_package_signature(paths);
		if (verify != MANAGER_VERIFY_ERROR)
			break;
		if (attempt < 2)
			msleep(20);
	}
	if (verify != MANAGER_VERIFY_OK) {
		if (verify == MANAGER_VERIFY_ABSENT) {
			ret = -EACCES;
			revoke_manager();
		} else {
			/*
			 * A transient read failure is not proof that the manager
			 * disappeared.  Keep the last verified grant and let the
			 * next packages.xml event retry the scan.
			 */
			ret = -EIO;
			pr_err("[manager] verification unavailable; keeping current authorization\n");
		}
		goto out;
	}

	uid = paths->has_uid ? (uid_t)paths->uid : get_uid_from_packages_list(paths->name);
	if (uid == (uid_t)-1) {
		pr_err("[manager] Could not find UID for %s\n", paths->name);
		ret = -ENOENT;
		revoke_manager();
		goto out;
	}

	kuid = make_kuid(current_user_ns(), uid);
	if (!uid_valid(kuid)) {
		pr_err("[manager] invalid manager UID %u\n", uid);
		ret = -EINVAL;
		revoke_manager();
		goto out;
	}

	pr_info("[manager] Verification passed. "
		"Granting privileges to UID %u\n",
		uid);
	/*
	 * Record the UID before granting the profile so the store can leave
	 * the manager's own entry out of allow.profile: it is re-created on
	 * every boot from this verified scan, never restored from disk.  A
	 * changed UID also revokes the previous manager's profile first.
	 */
	previous = manager_kuid;
	if (!uid_eq(kuid, previous)) {
		int clear_ret;

		manager_kuid = INVALID_UID;
		if (uid_valid(previous)) {
			clear_ret = nksu_profile_clear(__kuid_val(previous));
			if (clear_ret && clear_ret != -ENOENT)
				pr_err("[manager] cannot revoke previous UID %u profile: %d\n",
				       __kuid_val(previous), clear_ret);
			else
				pr_info("[manager] revoked previous manager UID %u\n",
					__kuid_val(previous));
		}
		manager_kuid = kuid;
	}

	/* Install ncore/busybox before the profile store writes its file so
	 * /data/adb/nksu exists for the first save. */
	install_ret = install_daemon_from_manager(paths);
	if (install_ret) {
		pr_err("[manager] ncore bootstrap failed: %d\n", install_ret);
		ret = install_ret;
	}

	install_ret = nksu_profile_set_default(uid);
	if (install_ret) {
		pr_err("[manager] cannot grant profile to UID %u: %d\n", uid, install_ret);
		revoke_manager();
		ret = install_ret;
		goto out;
	}

	install_ret = mark_zygote();
	if (install_ret)
		pr_warn("[manager] cannot mark zygote: %d\n", install_ret);
	nksu_path_sync_all();

out:
	nksu_borrow_creds_end(old);

	/*
     * No ncore means no module can run this boot, so volume-key safe mode has
     * nothing to protect.  Idempotent; the safe-mode watcher is armed earlier
     * (second_stage), so this simply stops it counting.
     */
	if (!READ_ONCE(daemon_ready))
		nksu_safe_mode_disable();

	return ret;
}

/*
 * packages.xml observer.
 *
 * PackageManagerService republishes /data/system/packages.xml whenever the set
 * of installed packages changes.  It normally writes a reserve copy and renames
 * it into place; some boot paths instead rewrite the file directly.  Watch the
 * completed rename and close-for-write events rather than FS_CREATE: a create
 * can fire while the writer still has an empty or partial file.  The callback
 * runs under the fsnotify SRCU lock and the rename's directory locks, while the
 * scan sleeps and reads files, so the work is deferred with task_work
 * (KernelSU's ksu_defer_track_throne): it then runs in the renamer's context and
 * finishes before the rename returns, so a freshly installed manager is verified
 * and granted before it can be opened.
 */
#define APPSCAN_WATCH_DIR "/data/system"
#define APPSCAN_WATCH_NAME "packages.xml"
#define APPSCAN_WATCH_MASK (FS_MOVE | FS_CLOSE_WRITE | FS_EVENT_ON_CHILD)

static struct fsnotify_group *appscan_group;
static struct fsnotify_mark *appscan_mark;
static bool appscan_shutdown;

static void appscan_work_fn(struct work_struct *work)
{
	if (!READ_ONCE(appscan_shutdown))
		scan_and_apply();
	module_put(THIS_MODULE);
}

static DECLARE_WORK(appscan_work, appscan_work_fn);

static void appscan_task_work(struct callback_head *cb)
{
	/* Runs after exit_fs() if the task is exiting; path lookups would oops. */
	if (!READ_ONCE(appscan_shutdown) && !(current->flags & PF_EXITING))
		scan_and_apply();

	kfree(cb);
	module_put(THIS_MODULE);
}

static void appscan_defer_scan(void)
{
	struct callback_head *cb;

	if (READ_ONCE(appscan_shutdown))
		return;

	if (!try_module_get(THIS_MODULE)) {
		pr_warn_ratelimited("[manager] cannot pin module for package scan\n");
		return;
	}

	if (!(current->flags & PF_KTHREAD)) {
		/*
		 * The fsnotify callback may run under rename locks.  Allocate
		 * atomically and never run the sleeping scan inline; on failure
		 * fall back to the workqueue, which can be cancelled at exit.
		 */
		cb = kzalloc(sizeof(*cb), GFP_ATOMIC);
		if (cb) {
			init_task_work(cb, appscan_task_work);
			if (!task_work_add(current, cb, TWA_RESUME))
				return;
			kfree(cb);
		}
	}

	/* kthread, allocation failure or exiting task: defer off this context. */
	if (!schedule_work(&appscan_work)) {
		pr_debug("[manager] package scan already pending\n");
		module_put(THIS_MODULE);
	}
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 9, 0)
static int appscan_handle_inode_event(struct fsnotify_mark *mark, u32 mask, struct inode *inode,
				      struct inode *dir, const struct qstr *file_name, u32 cookie)
{
	(void)mark;
	(void)mask;
	(void)inode;
	(void)dir;
	(void)cookie;

	if (!file_name || file_name->len != sizeof(APPSCAN_WATCH_NAME) - 1)
		return 0;
	if (memcmp(file_name->name, APPSCAN_WATCH_NAME, sizeof(APPSCAN_WATCH_NAME) - 1))
		return 0;

	appscan_defer_scan();
	return 0;
}

static const struct fsnotify_ops appscan_ops = {
	.handle_inode_event = appscan_handle_inode_event,
};
#else
/* 4.9 has the old group-wide handle_event callback and group-owned marks. */
static void appscan_free_mark(struct fsnotify_mark *mark)
{
	kfree(mark);
}

static int appscan_handle_event(struct fsnotify_group *group, struct inode *inode,
				struct fsnotify_mark *inode_mark, struct fsnotify_mark *vfsmount_mark,
				u32 mask, void *data, int data_type,
				const unsigned char *file_name, u32 cookie)
{
	(void)group;
	(void)inode;
	(void)inode_mark;
	(void)vfsmount_mark;
	(void)mask;
	(void)data;
	(void)data_type;
	(void)cookie;

	if (!file_name || strlen((const char *)file_name) != sizeof(APPSCAN_WATCH_NAME) - 1)
		return 0;
	if (memcmp(file_name, APPSCAN_WATCH_NAME, sizeof(APPSCAN_WATCH_NAME) - 1))
		return 0;

	appscan_defer_scan();
	return 0;
}

static const struct fsnotify_ops appscan_ops = {
	.handle_event = appscan_handle_event,
};
#endif

static void appscan_observer_exit(void)
{
	if (appscan_mark) {
		fsnotify_destroy_mark(appscan_mark, appscan_group);
		fsnotify_put_mark(appscan_mark);
		appscan_mark = NULL;
	}
	if (appscan_group) {
		fsnotify_put_group(appscan_group);
		appscan_group = NULL;
	}
}

static int appscan_observer_init(void)
{
	const struct cred *old;
	struct fsnotify_mark *mark;
	struct path kpath;
	int ret;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 0, 0)
	appscan_group = fsnotify_alloc_group(&appscan_ops, 0);
#else
	appscan_group = fsnotify_alloc_group(&appscan_ops);
#endif
	if (IS_ERR(appscan_group)) {
		ret = PTR_ERR(appscan_group);
		appscan_group = NULL;
		return ret;
	}

	/* /data/system is only reachable under nksu's domain + full caps. */
	old = nksu_borrow_creds_begin();
	if (!old) {
		ret = -ENOMEM;
		goto out_group;
	}
	ret = kern_path(APPSCAN_WATCH_DIR, LOOKUP_FOLLOW, &kpath);
	nksu_borrow_creds_end(old);
	if (ret)
		goto out_group;

	mark = kzalloc(sizeof(*mark), GFP_KERNEL);
	if (!mark) {
		ret = -ENOMEM;
		path_put(&kpath);
		goto out_group;
	}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 9, 0)
	fsnotify_init_mark(mark, appscan_group);
#else
	fsnotify_init_mark(mark, appscan_free_mark);
#endif
	mark->mask = APPSCAN_WATCH_MASK;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 9, 0)
	ret = nksu_fsnotify_add_inode_mark(mark, d_inode(kpath.dentry), 0);
#else
	ret = nksu_fsnotify_add_mark(mark, appscan_group, d_inode(kpath.dentry), NULL, 0);
#endif
	path_put(&kpath);
	if (ret) {
		fsnotify_put_mark(mark);
		goto out_group;
	}

	appscan_mark = mark;
	pr_info("[manager] watching %s/%s\n", APPSCAN_WATCH_DIR, APPSCAN_WATCH_NAME);
	return 0;

out_group:
	pr_warn("[manager] package observer unavailable (%d)\n", ret);
	fsnotify_put_group(appscan_group);
	appscan_group = NULL;
	return ret;
}

int appscan_init(void)
{
	int ret;

	pr_info("[manager] Module starting scan...\n");

	/*
     * Install the observer first, then scan once.  A first-stage (vendor_boot)
     * load can reach this before PackageManagerService has written
     * packages.xml; the observer then triggers the scan as soon as it appears,
     * so no retry loop is needed.
     */
	if (appscan_observer_init())
		pr_warn("[manager] package observer unavailable, scanning boot only\n");

	ret = scan_and_apply();
	if (ret)
		pr_err("[manager] initial manager scan failed: %d\n", ret);
	return 0;
}

void appscan_exit(void)
{
	WRITE_ONCE(appscan_shutdown, true);
	appscan_observer_exit();
	if (cancel_work_sync(&appscan_work))
		module_put(THIS_MODULE);
	nksu_borrow_creds_release();
}

#if defined(__clang__)
#pragma clang attribute pop
#endif
