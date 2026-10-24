/* SPDX-License-Identifier: GPL-2.0 */
/*
 * nksu's cleanup classes: the resources the module allocates most often, plus
 * a mutex guard, expressed with the macros from cleanup.h.
 *
 * Include this (not cleanup.h) from a .c that wants to use them.  The guard
 * classes are prefixed nksu_ on purpose: the kernel's own mutex/fd/etc. guards
 * only exist from v6.5, whereas these are defined here unconditionally, so the
 * same call sites compile on every supported KMI.
 *
 * Usage:
 *
 *	struct file *f __free(nksu_filp) = filp_open(...);
 *	guard(nksu_mutex)(&some_mutex);
 *	...
 */
#ifndef NKSU_GUARDS_H
#define NKSU_GUARDS_H

#include "cleanup.h"

#include <linux/file.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/slab.h>

/* Resource helpers. */
DEFINE_FREE(nksu_kfree, void *, kfree(_T))
DEFINE_FREE(nksu_kvfree, void *, kvfree(_T))
DEFINE_FREE(nksu_filp, struct file *, if (!IS_ERR_OR_NULL(_T)) filp_close(_T, NULL))
DEFINE_FREE(nksu_fput, struct file *, if (_T) fput(_T))
DEFINE_FREE(nksu_mmput, struct mm_struct *, if (_T) mmput(_T))

/* Lock guards. */
DEFINE_GUARD(nksu_mutex, struct mutex *, mutex_lock(_T), mutex_unlock(_T))

#endif /* NKSU_GUARDS_H */
