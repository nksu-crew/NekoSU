// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * nksu -- Magisk/KernelSU-style module loading from kernel context.
 *
 * Modules live under /data/adb/modules/<id>/ and ship shell hooks named after
 * the boot stage they belong to.  This file walks that directory *in the
 * kernel* and runs the hooks through nksu_spawn(), so bringing modules up no
 * longer depends on a userspace daemon (ksud/ncore) enumerating the directory
 * and forking the scripts.
 *
 * The two stages are kept strictly apart, matching Magisk:
 *
 *   post-fs-data   nksu_modules_post_fs_data()  -> post-fs-data.sh
 *                  (init second_stage: /data mounted, before the zygote)
 *   late_start     nksu_modules_service()       -> service.sh
 *                  (first zygote, i.e. the feature-component stage)
 *
 * A late load (insmod after boot) never saw the post-fs-data stage; the
 * late-load path calls nksu_modules_post_fs_data() before the feature stage
 * so both hooks still run once, in order.
 *
 * Metamodule
 * ----------
 * As in KernelSU-Next, mounting is delegated to a *metamodule*: a module
 * whose module.prop carries "metamodule=1" (or "=true").  Without one,
 * modules are never mounted.  The metamodule lifecycle scripts run before
 * regular modules' scripts, and its mount handler runs after all
 * post-fs-data scripts:
 *
 *   post-fs-data   post-fs-data.d/*.sh
 *                  metamodule/post-fs-data.sh
 *                  <regular modules>/post-fs-data.sh
 *                  metamodule/metamount.sh        <- mounts the modules
 *   late_start     service.d/*.sh
 *                  metamodule/service.sh
 *                  <regular modules>/service.sh
 *
 * Per-module layout handled here:
 *
 *   module.prop      may declare "metamodule=1"
 *   disable          present => module is disabled, skipped
 *   remove           present => module is pending removal, skipped
 *   post-fs-data.sh  post-fs-data hook
 *   service.sh       late_start hook
 *   metamount.sh     metamodule-only mount handler
 *
 * A hook is executed as
 *
 *   /system/bin/sh <module>/<hook>
 *
 * with root credentials in the "nksu" SELinux domain (unconfined), and its
 * exit status is waited on so hooks run in order.
 *
 * The two stages are dispatched to a dedicated "nksu-modload" kthread and
 * this file's public entry points return as soon as the work is queued, so
 * a slow module script can never stall init, the boot watcher, the zygote
 * feature stage or a late insmod.  The worker still runs each stage's hooks
 * one at a time, preserving the ordering above (metamodule before regular
 * modules, metamount.sh last).
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/dcache.h>
#include <linux/cred.h>
#include <linux/capability.h>

#include <fmac.h>

#define NKSU_MODULES_DIR "/data/adb/modules"
#define NKSU_POST_FS_DATA_DIR "/data/adb/post-fs-data.d"
#define NKSU_SERVICE_DIR "/data/adb/service.d"
#define NKSU_MODULE_PROP "module.prop"
#define NKSU_MODULE_DISABLE "disable"
#define NKSU_MODULE_REMOVE "remove"
#define NKSU_MODULE_SHELL "/system/bin/sh"
#define NKSU_MODULE_POST_FS_DATA "post-fs-data.sh"
#define NKSU_MODULE_SERVICE "service.sh"
#define NKSU_MODULE_METAMOUNT "metamount.sh"
#define NKSU_SCRIPT_SUFFIX ".sh"

/* module paths are bounded; keep the stack frames small */
#define NKSU_MODULE_PATH_MAX 320
#define NKSU_MODULE_PROP_MAX 4096

/* minimal environment so the shell and scripts have a usable PATH */
static const char *const nksu_module_envp[] = {
    "PATH=/sbin:/system/sbin:/system/bin:/system/xbin",
    "ANDROID_ROOT=/system",
    NULL,
};

/* set once the post-fs-data stage has been served */
static bool post_fs_data_done;

/*
 * Enumeration touches /data/adb from a kthread (kernel domain) or from the
 * manager's ioctl context, neither of which the policy may allow to read
 * there.  Borrow an unconfined nksu-domain cred while walking the tree.
 */
static struct cred *nksu_module_cred;

static const struct cred *nksu_module_creds_begin(void)
{
    if (!nksu_module_cred) {
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
        nksu_module_cred = cred;
    }

    return override_creds(nksu_module_cred);
}

static void nksu_module_creds_end(const struct cred *old)
{
    if (old)
        revert_creds(old);
}

struct nksu_module_dir {
    char **names;
    size_t count;
    size_t cap;
};

struct nksu_dir_ctx {
    struct dir_context ctx;
    struct nksu_module_dir dir;
    bool files_only;
};

static int nksu_path_join(char *buf, size_t size, const char *dir, const char *name)
{
    int n = snprintf(buf, size, "%s/%s", dir, name);

    if (n < 0 || (size_t)n >= size)
        return -ENAMETOOLONG;
    return 0;
}

static bool nksu_file_exists(const char *path)
{
    struct file *f = filp_open(path, O_RDONLY, 0);

    if (IS_ERR(f))
        return false;
    filp_close(f, NULL);
    return true;
}

static bool nksu_is_dir(const char *path)
{
    struct file *f = filp_open(path, O_RDONLY | O_DIRECTORY, 0);

    if (IS_ERR(f))
        return false;
    filp_close(f, NULL);
    return true;
}

/* Read a small text file into @buf (NUL-terminated); returns 0 on success. */
static int nksu_read_text(const char *path, char *buf, size_t size)
{
    struct file *f;
    loff_t pos = 0;
    ssize_t n;

    if (size == 0)
        return -EINVAL;

    f = filp_open(path, O_RDONLY, 0);
    if (IS_ERR(f))
        return PTR_ERR(f);

    n = kernel_read(f, buf, size - 1, &pos);
    filp_close(f, NULL);

    if (n < 0)
        return (int)n;

    buf[n] = '\0';
    return 0;
}

static int nksu_module_dir_add(struct nksu_module_dir *dir, const char *name, int namlen)
{
    char **names;
    char *copy;

    if (dir->count == dir->cap) {
        size_t cap = dir->cap ? dir->cap * 2 : 8;

        names = krealloc(dir->names, cap * sizeof(*names), GFP_KERNEL);
        if (!names)
            return -ENOMEM;
        dir->names = names;
        dir->cap = cap;
    }

    copy = kmalloc(namlen + 1, GFP_KERNEL);
    if (!copy)
        return -ENOMEM;

    memcpy(copy, name, namlen);
    copy[namlen] = '\0';
    dir->names[dir->count++] = copy;
    return 0;
}

static void nksu_module_dir_free(struct nksu_module_dir *dir)
{
    size_t i;

    for (i = 0; i < dir->count; i++)
        kfree(dir->names[i]);
    kfree(dir->names);
    dir->names = NULL;
    dir->count = 0;
    dir->cap = 0;
}

/*
 * filldir_t returned int (0 = keep going) before v6.1 and bool (true = keep
 * going) from v6.1 on; keep the callback in sync with the kernel we build for.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
typedef bool nksu_filldir_ret_t;
#define NKSU_FILLDIR_CONTINUE true
#define NKSU_FILLDIR_STOP false
#else
typedef int nksu_filldir_ret_t;
#define NKSU_FILLDIR_CONTINUE 0
#define NKSU_FILLDIR_STOP (-ENOMEM)
#endif

/* Stopping the walk early only happens on OOM. */
static nksu_filldir_ret_t nksu_filldir(struct dir_context *ctx, const char *name, int namlen, loff_t offset, u64 ino,
                                       unsigned int d_type)
{
    struct nksu_dir_ctx *dctx = container_of(ctx, struct nksu_dir_ctx, ctx);

    if (namlen == 1 && name[0] == '.')
        return NKSU_FILLDIR_CONTINUE;
    if (namlen == 2 && name[0] == '.' && name[1] == '.')
        return NKSU_FILLDIR_CONTINUE;

    if (dctx->files_only) {
        /* *.d directories: keep plain files, drop subdirectories. */
        if (d_type == DT_DIR)
            return NKSU_FILLDIR_CONTINUE;
    } else {
        if (d_type != DT_DIR && d_type != DT_UNKNOWN)
            return NKSU_FILLDIR_CONTINUE;
    }

    return nksu_module_dir_add(&dctx->dir, name, namlen) == 0 ? NKSU_FILLDIR_CONTINUE : NKSU_FILLDIR_STOP;
}

static int nksu_read_dir_entries(const char *path, struct nksu_module_dir *out, bool files_only)
{
    struct nksu_dir_ctx dctx = {
        .ctx.actor = nksu_filldir,
        .files_only = files_only,
    };
    struct file *f;
    int ret;

    f = filp_open(path, O_RDONLY | O_DIRECTORY, 0);
    if (IS_ERR(f))
        return PTR_ERR(f);

    ret = iterate_dir(f, &dctx.ctx);
    filp_close(f, NULL);

    if (ret)
        nksu_module_dir_free(&dctx.dir);
    else
        *out = dctx.dir;

    return ret;
}

static void nksu_spawn_script(const char *script)
{
    char *argv[3];
    struct nksu_spawn_args args;

    if (!nksu_file_exists(script))
        return;

    argv[0] = (char *)NKSU_MODULE_SHELL;
    argv[1] = (char *)script;
    argv[2] = NULL;

    memset(&args, 0, sizeof(args));
    args.path = NKSU_MODULE_SHELL;
    args.argv = (char *const *)argv;
    args.envp = (char *const *)nksu_module_envp;
    args.domain = DOMAIN_CTX;
    args.wait = NKSU_SPAWN_WAIT_PROC;

    pr_info("nksu: running module hook %s\n", script);
    nksu_spawn(&args);
}

static void nksu_module_run_hook(const char *moddir, const char *hook)
{
    char script[NKSU_MODULE_PATH_MAX];

    if (nksu_path_join(script, sizeof(script), moddir, hook))
        return;

    nksu_spawn_script(script);
}

static bool nksu_has_suffix(const char *name, const char *suffix)
{
    size_t n = strlen(name);
    size_t s = strlen(suffix);

    return n >= s && strcmp(name + n - s, suffix) == 0;
}

/*
 * Run every "*.sh" in a Magisk-style script directory (post-fs-data.d,
 * service.d).  The directory is optional; a missing one is not an error.
 */
static void nksu_run_scripts_dir(const char *dir)
{
    struct nksu_module_dir files;
    const struct cred *old;
    size_t i;

    old = nksu_module_creds_begin();

    memset(&files, 0, sizeof(files));
    if (nksu_read_dir_entries(dir, &files, true)) {
        nksu_module_creds_end(old);
        return;
    }

    for (i = 0; i < files.count; i++) {
        char script[NKSU_MODULE_PATH_MAX];

        if (!nksu_has_suffix(files.names[i], NKSU_SCRIPT_SUFFIX))
            continue;
        if (nksu_path_join(script, sizeof(script), dir, files.names[i]))
            continue;

        nksu_spawn_script(script);
    }

    nksu_module_dir_free(&files);
    nksu_module_creds_end(old);
}

/* A module participates in a stage unless it is disabled or being removed. */
static bool nksu_module_is_enabled(const char *moddir, const char *name)
{
    char flag[NKSU_MODULE_PATH_MAX];

    if (!nksu_is_dir(moddir))
        return false;

    if (!nksu_path_join(flag, sizeof(flag), moddir, NKSU_MODULE_DISABLE) && nksu_file_exists(flag)) {
        pr_info("nksu: module '%s' is disabled, skipping\n", name);
        return false;
    }

    if (!nksu_path_join(flag, sizeof(flag), moddir, NKSU_MODULE_REMOVE) && nksu_file_exists(flag)) {
        pr_info("nksu: module '%s' is marked for removal, skipping\n", name);
        return false;
    }

    return true;
}

/* Extract "<key>=<value>" from a module.prop body into @out (trimmed). */
static bool nksu_prop_get(const char *text, const char *key, char *out, size_t outsz)
{
    size_t klen = strlen(key);
    const char *p = text;

    if (out && outsz)
        out[0] = '\0';

    while (*p) {
        const char *eol = strchrnul(p, '\n');
        const char *eq = memchr(p, '=', eol - p);

        if (eq) {
            const char *k = p, *kend = eq;
            const char *v = eq + 1, *vend = eol;

            while (k < kend && (*k == ' ' || *k == '\t'))
                k++;
            while (kend > k && (kend[-1] == ' ' || kend[-1] == '\t' || kend[-1] == '\r'))
                kend--;
            while (v < vend && (*v == ' ' || *v == '\t'))
                v++;
            while (vend > v && (vend[-1] == ' ' || vend[-1] == '\t' || vend[-1] == '\r'))
                vend--;

            if ((size_t)(kend - k) == klen && memcmp(k, key, klen) == 0) {
                size_t vlen = (size_t)(vend - v);

                if (out && outsz) {
                    if (vlen >= outsz)
                        vlen = outsz - 1;
                    memcpy(out, v, vlen);
                    out[vlen] = '\0';
                }
                return true;
            }
        }

        p = *eol ? eol + 1 : eol;
    }

    return false;
}

/* module.prop "metamodule=1" (or =true) marks a metamodule. */
static bool nksu_prop_has_metamodule(const char *text)
{
    char v[16];

    if (!nksu_prop_get(text, "metamodule", v, sizeof(v)))
        return false;

    return strcmp(v, "1") == 0 || strcasecmp(v, "true") == 0;
}

/*
 * A metamodule declares itself with "metamodule=1" in module.prop.  The
 * /data/adb/metamodule symlink (created at install time) is only a cache of
 * the same information, so scanning module.prop is enough to find it here.
 */
static bool nksu_module_is_metamodule(const char *moddir)
{
    char path[NKSU_MODULE_PATH_MAX];
    char *buf;
    bool is_meta;

    if (nksu_path_join(path, sizeof(path), moddir, NKSU_MODULE_PROP))
        return false;

    buf = kvmalloc(NKSU_MODULE_PROP_MAX, GFP_KERNEL);
    if (!buf)
        return false;

    if (nksu_read_text(path, buf, NKSU_MODULE_PROP_MAX))
        is_meta = false;
    else
        is_meta = nksu_prop_has_metamodule(buf);

    kvfree(buf);
    return is_meta;
}

/*
 * Run @hook for every enabled module of one kind, in directory order:
 * want_meta=false runs regular modules, want_meta=true the metamodule.
 */
static void nksu_modules_foreach(const char *hook, bool want_meta)
{
    struct nksu_module_dir dirs;
    const struct cred *old;
    size_t i;

    old = nksu_module_creds_begin();

    memset(&dirs, 0, sizeof(dirs));
    if (nksu_read_dir_entries(NKSU_MODULES_DIR, &dirs, false)) {
        nksu_module_creds_end(old);
        return;
    }

    for (i = 0; i < dirs.count; i++) {
        char moddir[NKSU_MODULE_PATH_MAX];
        const char *name = dirs.names[i];

        if (nksu_path_join(moddir, sizeof(moddir), NKSU_MODULES_DIR, name))
            continue;
        if (!nksu_module_is_enabled(moddir, name))
            continue;
        if (nksu_module_is_metamodule(moddir) != want_meta)
            continue;

        nksu_module_run_hook(moddir, hook);
    }

    nksu_module_dir_free(&dirs);
    nksu_module_creds_end(old);
}

/* ---- staged module bring-up (non-blocking) ---- */

/*
 * Both stages run on this worker so the callers (boot watcher, feature stage,
 * late insmod) never wait on a module script.  A stage is a single boolean
 * request; the worker drains post-fs-data before service, which keeps the
 * boot-time ordering intact even if the zygote stage arrives early.
 */
static struct task_struct *modload_thread;
static DECLARE_WAIT_QUEUE_HEAD(modload_wq);
static bool post_fs_data_pending;
static bool service_pending;

static void nksu_modules_do_post_fs_data(void)
{
    pr_info("nksu: post-fs-data module stage\n");

    /* Magisk-compatible extra scripts run before the modules. */
    nksu_run_scripts_dir(NKSU_POST_FS_DATA_DIR);

    nksu_modules_foreach(NKSU_MODULE_POST_FS_DATA, true);
    nksu_modules_foreach(NKSU_MODULE_POST_FS_DATA, false);

    pr_info("nksu: metamodule mount stage\n");
    nksu_modules_foreach(NKSU_MODULE_METAMOUNT, true);
}

static void nksu_modules_do_service(void)
{
    pr_info("nksu: service module stage\n");

    nksu_run_scripts_dir(NKSU_SERVICE_DIR);

    nksu_modules_foreach(NKSU_MODULE_SERVICE, true);
    nksu_modules_foreach(NKSU_MODULE_SERVICE, false);
}

static int nksu_modload_thread_fn(void *data)
{
    while (!kthread_should_stop()) {
        wait_event_interruptible(modload_wq, READ_ONCE(post_fs_data_pending) || READ_ONCE(service_pending) ||
                                                 kthread_should_stop());
        if (kthread_should_stop())
            break;

        if (READ_ONCE(post_fs_data_pending)) {
            WRITE_ONCE(post_fs_data_pending, false);
            nksu_modules_do_post_fs_data();
        }

        if (READ_ONCE(service_pending)) {
            WRITE_ONCE(service_pending, false);
            nksu_modules_do_service();
        }
    }

    return 0;
}

static int nksu_modload_ensure(void)
{
    if (modload_thread)
        return 0;

    modload_thread = kthread_run(nksu_modload_thread_fn, NULL, "nksu-modload");
    if (IS_ERR(modload_thread)) {
        int ret = PTR_ERR(modload_thread);

        modload_thread = NULL;
        return ret;
    }

    return 0;
}

/*
 * post-fs-data stage: called from the boot watcher once init reaches
 * second_stage (and from the late-load path).  Idempotent so the boot
 * watcher may announce the stage more than once, and non-blocking: the work
 * is queued to the module loader and this returns at once.
 *
 * Order matches KernelSU-Next: post-fs-data.d/*.sh, the metamodule's own
 * post-fs-data.sh, then regular modules', and only then the metamodule's
 * metamount.sh mounts everything.  With no metamodule nothing is mounted.
 */
void nksu_modules_post_fs_data(void)
{
    if (READ_ONCE(post_fs_data_done))
        return;

    if (nksu_modload_ensure()) {
        pr_err("nksu: failed to start module loader\n");
        return;
    }

    WRITE_ONCE(post_fs_data_done, true);
    WRITE_ONCE(post_fs_data_pending, true);
    wake_up_interruptible(&modload_wq);
}

/* late_start stage: called from the feature-component stage. */
int nksu_modules_service(void)
{
    if (nksu_modload_ensure())
        return -ENOMEM;

    WRITE_ONCE(service_pending, true);
    wake_up_interruptible(&modload_wq);
    return 0;
}

void nksu_modules_exit(void)
{
    if (modload_thread) {
        kthread_stop(modload_thread);
        modload_thread = NULL;
    }

    if (nksu_module_cred) {
        put_cred(nksu_module_cred);
        nksu_module_cred = NULL;
    }
}

/* ---- module list JSON (the kernel-side module interface) ---- */

static bool nksu_module_has_flag(const char *moddir, const char *flag)
{
    char path[NKSU_MODULE_PATH_MAX];

    if (nksu_path_join(path, sizeof(path), moddir, flag))
        return false;
    return nksu_file_exists(path);
}

static bool nksu_module_has_system(const char *moddir)
{
    char path[NKSU_MODULE_PATH_MAX];

    if (nksu_path_join(path, sizeof(path), moddir, "system"))
        return false;
    return nksu_is_dir(path);
}

struct nksu_json {
    char *buf;
    size_t cap;
    size_t len;
};

static void nksu_json_putc(struct nksu_json *j, char c)
{
    if (j->len + 1 < j->cap)
        j->buf[j->len] = c;
    j->len++;
}

static void nksu_json_puts(struct nksu_json *j, const char *s)
{
    while (*s)
        nksu_json_putc(j, *s++);
}

static void nksu_json_str(struct nksu_json *j, const char *s)
{
    nksu_json_putc(j, '"');
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;

        switch (c) {
        case '"':
            nksu_json_puts(j, "\\\"");
            break;
        case '\\':
            nksu_json_puts(j, "\\\\");
            break;
        case '\n':
            nksu_json_puts(j, "\\n");
            break;
        case '\r':
            nksu_json_puts(j, "\\r");
            break;
        case '\t':
            nksu_json_puts(j, "\\t");
            break;
        default:
            if (c < 0x20) {
                char esc[8];

                snprintf(esc, sizeof(esc), "\\u%04x", c);
                nksu_json_puts(j, esc);
            } else {
                nksu_json_putc(j, (char)c);
            }
        }
    }
    nksu_json_putc(j, '"');
}

static void nksu_json_field(struct nksu_json *j, const char *key, const char *val)
{
    nksu_json_str(j, key);
    nksu_json_putc(j, ':');
    nksu_json_str(j, val ? val : "");
}

static void nksu_json_bool(struct nksu_json *j, const char *key, bool val)
{
    nksu_json_str(j, key);
    nksu_json_putc(j, ':');
    nksu_json_puts(j, val ? "true" : "false");
}

size_t nksu_modules_emit_json(char *buf, size_t size)
{
    struct nksu_module_dir dirs;
    struct nksu_json j = { .buf = buf, .cap = size, .len = 0 };
    const struct cred *old;
    char *text;
    size_t i;
    size_t ret;
    bool first = true;

    if (!buf || size < 3)
        return 0;

    old = nksu_module_creds_begin();

    memset(&dirs, 0, sizeof(dirs));
    if (nksu_read_dir_entries(NKSU_MODULES_DIR, &dirs, false)) {
        nksu_json_puts(&j, "[]");
        ret = j.len < size ? j.len : size - 1;
        j.buf[ret] = '\0';
        nksu_module_creds_end(old);
        return ret;
    }

    text = kvmalloc(NKSU_MODULE_PROP_MAX, GFP_KERNEL);
    if (!text) {
        nksu_json_puts(&j, "[]");
        ret = j.len < size ? j.len : size - 1;
        j.buf[ret] = '\0';
        nksu_module_creds_end(old);
        return ret;
    }

    nksu_json_putc(&j, '[');

    for (i = 0; i < dirs.count; i++) {
        char moddir[NKSU_MODULE_PATH_MAX];
        char proppath[NKSU_MODULE_PATH_MAX];
        char id[64] = { 0 };
        char value[256];
        const char *name = dirs.names[i];

        if (nksu_path_join(moddir, sizeof(moddir), NKSU_MODULES_DIR, name))
            continue;
        if (!nksu_is_dir(moddir))
            continue;

        if (nksu_path_join(proppath, sizeof(proppath), moddir, NKSU_MODULE_PROP) == 0 &&
            nksu_read_text(proppath, text, NKSU_MODULE_PROP_MAX) == 0) {
            /* text holds module.prop */
        } else {
            text[0] = '\0';
        }

        if (!nksu_prop_get(text, "id", id, sizeof(id)) || !id[0])
            strscpy(id, name, sizeof(id));

        if (!first)
            nksu_json_putc(&j, ',');
        first = false;

        nksu_json_putc(&j, '{');
        nksu_json_field(&j, "id", id);
        nksu_json_putc(&j, ',');

        nksu_prop_get(text, "name", value, sizeof(value));
        nksu_json_field(&j, "name", value);
        nksu_json_putc(&j, ',');

        nksu_prop_get(text, "version", value, sizeof(value));
        nksu_json_field(&j, "version", value);
        nksu_json_putc(&j, ',');

        nksu_prop_get(text, "versionCode", value, sizeof(value));
        nksu_json_field(&j, "versionCode", value);
        nksu_json_putc(&j, ',');

        nksu_prop_get(text, "author", value, sizeof(value));
        nksu_json_field(&j, "author", value);
        nksu_json_putc(&j, ',');

        nksu_prop_get(text, "description", value, sizeof(value));
        nksu_json_field(&j, "description", value);
        nksu_json_putc(&j, ',');

        nksu_json_bool(&j, "enabled", !nksu_module_has_flag(moddir, NKSU_MODULE_DISABLE));
        nksu_json_putc(&j, ',');
        nksu_json_bool(&j, "metamodule", nksu_prop_has_metamodule(text));
        nksu_json_putc(&j, ',');
        nksu_json_bool(&j, "update", nksu_module_has_flag(moddir, "update"));
        nksu_json_putc(&j, ',');
        nksu_json_bool(&j, "remove", nksu_module_has_flag(moddir, NKSU_MODULE_REMOVE));
        nksu_json_putc(&j, ',');
        nksu_json_bool(&j, "skipMount", nksu_module_has_flag(moddir, "skip_mount"));
        nksu_json_putc(&j, ',');
        nksu_json_bool(&j, "hasSystem", nksu_module_has_system(moddir));
        nksu_json_putc(&j, '}');
    }

    nksu_json_putc(&j, ']');

    kvfree(text);
    nksu_module_creds_end(old);

    if (j.len >= size)
        j.len = size - 1;
    j.buf[j.len] = '\0';
    return j.len;
}
