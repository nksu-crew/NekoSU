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
 *   post-fs-data   metamodule/post-fs-data.sh
 *                  <regular modules>/post-fs-data.sh
 *                  metamodule/metamount.sh        <- mounts the modules
 *   late_start     metamodule/service.sh
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
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/dcache.h>

#include <fmac.h>

#define NKSU_MODULES_DIR "/data/adb/modules"
#define NKSU_MODULE_PROP "module.prop"
#define NKSU_MODULE_DISABLE "disable"
#define NKSU_MODULE_REMOVE "remove"
#define NKSU_MODULE_SHELL "/system/bin/sh"
#define NKSU_MODULE_POST_FS_DATA "post-fs-data.sh"
#define NKSU_MODULE_SERVICE "service.sh"
#define NKSU_MODULE_METAMOUNT "metamount.sh"

/* module paths are bounded; keep the stack frames small */
#define NKSU_MODULE_PATH_MAX 512
#define NKSU_MODULE_PROP_MAX 4096

/* minimal environment so the shell and scripts have a usable PATH */
static const char *const nksu_module_envp[] = {
    "PATH=/sbin:/system/sbin:/system/bin:/system/xbin",
    "ANDROID_ROOT=/system",
    NULL,
};

/* set once the post-fs-data stage has been served */
static bool post_fs_data_done;

struct nksu_module_dir {
    char **names;
    size_t count;
    size_t cap;
};

struct nksu_dir_ctx {
    struct dir_context ctx;
    struct nksu_module_dir dir;
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

/* Returning false stops the walk; only OOM makes us do that. */
static bool nksu_filldir(struct dir_context *ctx, const char *name, int namlen, loff_t offset, u64 ino,
                         unsigned int d_type)
{
    struct nksu_dir_ctx *dctx = container_of(ctx, struct nksu_dir_ctx, ctx);

    if (namlen == 1 && name[0] == '.')
        return true;
    if (namlen == 2 && name[0] == '.' && name[1] == '.')
        return true;
    if (d_type != DT_DIR && d_type != DT_UNKNOWN)
        return true;

    return nksu_module_dir_add(&dctx->dir, name, namlen) == 0;
}

static int nksu_read_module_dirs(const char *path, struct nksu_module_dir *out)
{
    struct nksu_dir_ctx dctx = {
        .ctx.actor = nksu_filldir,
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

static void nksu_module_run_hook(const char *moddir, const char *hook)
{
    char script[NKSU_MODULE_PATH_MAX];
    char *argv[3];
    struct nksu_spawn_args args;

    if (nksu_path_join(script, sizeof(script), moddir, hook))
        return;

    if (!nksu_file_exists(script))
        return;

    argv[0] = (char *)NKSU_MODULE_SHELL;
    argv[1] = script;
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

/* Find "<key>=<value>" in a module.prop body, values "1" and "true". */
static bool nksu_prop_has_metamodule(const char *text)
{
    static const char key[] = "metamodule";
    const char *p = text;

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

            if ((size_t)(kend - k) == sizeof(key) - 1 && memcmp(k, key, sizeof(key) - 1) == 0) {
                size_t vlen = (size_t)(vend - v);

                if (vlen == 1 && v[0] == '1')
                    return true;
                if (vlen == 4 && strncasecmp(v, "true", 4) == 0)
                    return true;
            }
        }

        p = *eol ? eol + 1 : eol;
    }

    return false;
}

/*
 * A metamodule declares itself with "metamodule=1" in module.prop.  The
 * /data/adb/metamodule symlink (created at install time) is only a cache of
 * the same information, so scanning module.prop is enough to find it here.
 */
static bool nksu_module_is_metamodule(const char *moddir)
{
    char path[NKSU_MODULE_PATH_MAX];
    char buf[NKSU_MODULE_PROP_MAX];

    if (nksu_path_join(path, sizeof(path), moddir, NKSU_MODULE_PROP))
        return false;

    if (nksu_read_text(path, buf, sizeof(buf)))
        return false;

    return nksu_prop_has_metamodule(buf);
}

/*
 * Run @hook for every enabled module of one kind, in directory order:
 * want_meta=false runs regular modules, want_meta=true the metamodule.
 */
static void nksu_modules_foreach(const char *hook, bool want_meta)
{
    struct nksu_module_dir dirs;
    size_t i;

    memset(&dirs, 0, sizeof(dirs));
    if (nksu_read_module_dirs(NKSU_MODULES_DIR, &dirs))
        return;

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
}

/*
 * post-fs-data stage: called from the boot watcher once init reaches
 * second_stage (and from the late-load path).  Idempotent so the boot
 * watcher may announce the stage more than once.
 *
 * Order matches KernelSU-Next: the metamodule's own post-fs-data.sh runs
 * first, then regular modules', and only then the metamodule's metamount.sh
 * mounts everything.  With no metamodule nothing is mounted.
 */
void nksu_modules_post_fs_data(void)
{
    if (READ_ONCE(post_fs_data_done))
        return;
    WRITE_ONCE(post_fs_data_done, true);

    pr_info("nksu: post-fs-data module stage\n");

    nksu_modules_foreach(NKSU_MODULE_POST_FS_DATA, true);
    nksu_modules_foreach(NKSU_MODULE_POST_FS_DATA, false);

    pr_info("nksu: metamodule mount stage\n");
    nksu_modules_foreach(NKSU_MODULE_METAMOUNT, true);
}

/* late_start stage: called from the feature-component stage. */
int nksu_modules_service(void)
{
    pr_info("nksu: service module stage\n");

    nksu_modules_foreach(NKSU_MODULE_SERVICE, true);
    nksu_modules_foreach(NKSU_MODULE_SERVICE, false);

    return 0;
}

void nksu_modules_exit(void)
{
    /* Hooks are waited for and no state is retained. */
}
