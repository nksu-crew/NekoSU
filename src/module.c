// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * nksu -- kernel side of Magisk/KernelSU-style modules.
 *
 * Module loading itself now lives in userspace: the rc that nksu injects into
 * init (src/init_rc.c) execs ncore (/data/adb/nksu/ncore) at `on post-fs-data`,
 * `services` and `boot-completed`, and ncore runs the KernelSU-compatible
 * module runtime.  See src/include/nksu_module.h.
 *
 * This file keeps only the two pieces that genuinely need the kernel:
 *
 *   - the module list the manager reads through IOC_LIST_MODULES
 *     (nksu_modules_emit_json), which walks /data/adb/modules in the
 *     unconfined nksu domain; and
 *   - nksu_modules_exit(), which drops the borrowed cred and the sepolicy
 *     sink on unload.
 *
 * A late load (nksu.ko insmod'ed after init has parsed its rc files) has no
 * rc left to fire, so it does not bring modules up at all: module loading is a
 * boot-time, init.rc-driven feature.  See src/include/nksu_module.h.
 *
 * Each module's sepolicy.rule is applied by the loader through the
 * /proc/nksu/sepolicy sink (src/selinux/rule_file.c), because only the kernel
 * can edit the live SELinux policy.  The old kernel-side enumeration, spawn
 * code and marker watcher are gone.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/dcache.h>
#include <linux/cred.h>
#include <linux/capability.h>
#include <linux/printk.h>

#include <fmac.h>

#include "nksu_module.h"

#define NKSU_MODULES_DIR "/data/adb/modules"
#define NKSU_MODULE_PROP "module.prop"
#define NKSU_MODULE_DISABLE "disable"
#define NKSU_MODULE_REMOVE "remove"

/* module paths are bounded; keep the stack frames small */
#define NKSU_MODULE_PATH_MAX 320
#define NKSU_MODULE_PROP_MAX 4096

/*
 * Reading /data/adb from the manager's ioctl context needs a cred the policy
 * allows there, so borrow an unconfined nksu-domain cred while walking the
 * tree.
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

/* ---- teardown ---- */

void nksu_modules_exit(void)
{
    if (nksu_module_cred) {
        put_cred(nksu_module_cred);
        nksu_module_cred = NULL;
    }

    nksu_sepolicy_sink_exit();
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
