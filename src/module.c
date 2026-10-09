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
 * Per-module layout handled here:
 *
 *   disable          present => module is disabled, skipped
 *   remove           present => module is pending removal, skipped
 *   post-fs-data.sh  boot hook
 *   service.sh       boot hook
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
#define NKSU_MODULE_DISABLE "disable"
#define NKSU_MODULE_REMOVE "remove"
#define NKSU_MODULE_SHELL "/system/bin/sh"

/* module paths are bounded; keep the stack frames small */
#define NKSU_MODULE_PATH_MAX 512

/* boot hooks, in execution order */
static const char *const nksu_module_hooks[] = {
    "post-fs-data.sh",
    "service.sh",
};

/* minimal environment so the shell and scripts have a usable PATH */
static const char *const nksu_module_envp[] = {
    "PATH=/sbin:/system/sbin:/system/bin:/system/xbin",
    "ANDROID_ROOT=/system",
    NULL,
};

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

static void nksu_module_load_one(const char *name)
{
    char moddir[NKSU_MODULE_PATH_MAX];
    char flag[NKSU_MODULE_PATH_MAX];
    size_t i;

    if (nksu_path_join(moddir, sizeof(moddir), NKSU_MODULES_DIR, name))
        return;

    if (!nksu_is_dir(moddir))
        return;

    if (!nksu_path_join(flag, sizeof(flag), moddir, NKSU_MODULE_DISABLE) && nksu_file_exists(flag)) {
        pr_info("nksu: module '%s' is disabled, skipping\n", name);
        return;
    }

    if (!nksu_path_join(flag, sizeof(flag), moddir, NKSU_MODULE_REMOVE) && nksu_file_exists(flag)) {
        pr_info("nksu: module '%s' is marked for removal, skipping\n", name);
        return;
    }

    pr_info("nksu: loading module '%s'\n", name);

    for (i = 0; i < ARRAY_SIZE(nksu_module_hooks); i++)
        nksu_module_run_hook(moddir, nksu_module_hooks[i]);
}

int nksu_modules_init(void)
{
    struct nksu_module_dir dirs;
    size_t i;

    memset(&dirs, 0, sizeof(dirs));

    if (nksu_read_module_dirs(NKSU_MODULES_DIR, &dirs)) {
        pr_info("nksu: no module directory %s\n", NKSU_MODULES_DIR);
        return 0;
    }

    pr_info("nksu: %zu module(s) found in %s\n", dirs.count, NKSU_MODULES_DIR);

    for (i = 0; i < dirs.count; i++)
        nksu_module_load_one(dirs.names[i]);

    nksu_module_dir_free(&dirs);
    return 0;
}

void nksu_modules_exit(void)
{
    /* Hooks are waited for and no state is retained. */
}
