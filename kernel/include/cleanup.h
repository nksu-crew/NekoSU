/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Cleanup / scope-guard helpers for nksu, built on GCC/Clang's
 * __attribute__((__cleanup__(fn))) extension: fn(&var) runs automatically when
 * var goes out of scope.
 *
 * <linux/cleanup.h> only exists from v6.5, but the module is built against
 * 5.10 / 5.15 / 6.1 / 6.6 / 6.12 KMIs.  Where the tree already ships it we
 * include it verbatim; on the older trees we provide the same macros.  Both
 * paths rely only on __cleanup(), __UNIQUE_ID() and __maybe_unused(), which
 * every supported tree defines in its compiler headers.
 *
 * Usage:
 *
 *	DEFINE_FREE(nksu_kvfree, void *, kvfree(_T));
 *
 *	void foo(void)
 *	{
 *		void *p __free(nksu_kvfree) = kvmalloc(4096, GFP_KERNEL);
 *		if (!p)
 *			return;		// p is freed on every return
 *		...
 *	}
 */
#ifndef NKSU_CLEANUP_H
#define NKSU_CLEANUP_H

#include <linux/compiler.h>

/*
 * <linux/compiler_attributes.h> only exists from v4.14 (and cleanup.h from
 * v6.5).  Older trees (e.g. 4.9) do not ship either, so probe with
 * __has_include when the compiler supports it and fall back to the backport
 * below.
 */
#ifdef __has_include
#if __has_include(<linux/compiler_attributes.h>)
#include <linux/compiler_attributes.h>
#endif
#if __has_include(<linux/cleanup.h>)
#include <linux/cleanup.h>
#define NKSU_HAVE_LINUX_CLEANUP 1
#endif
#endif

#ifndef NKSU_HAVE_LINUX_CLEANUP
/* Backport of include/linux/cleanup.h (v6.5+). */

#ifndef __cleanup
#define __cleanup(func) __attribute__((__cleanup__(func)))
#endif

/*
 * DEFINE_FREE(name, type, free):
 *	define the wrapper __free(name) needs; @free is an expression using
 *	'_T' to access the variable.
 *
 * __free(name):
 *	attribute that scopes a variable's cleanup.
 *
 * no_free_ptr(var) / return_ptr(p):
 *	inhibit the cleanup (as if by xchg(var, NULL)) and hand the value out.
 */
#define DEFINE_FREE(_name, _type, _free)           \
	static inline void __free_##_name(void *p) \
	{                                          \
		_type _T = *(_type *)p;            \
		_free;                             \
	}

#define __free(_name) __cleanup(__free_##_name)

#define no_free_ptr(p)                   \
	({                               \
		__auto_type __ptr = (p); \
		(p) = NULL;              \
		__ptr;                   \
	})

#define return_ptr(p) return no_free_ptr(p)

/*
 * DEFINE_CLASS(name, type, exit, init, init_args...):
 *	define a destructor/constructor pair for a type; @exit uses '_T',
 *	@init is an expression over @init_args producing @type.
 *
 * EXTEND_CLASS(name, ext, init, init_args...):
 *	extend class @name to @name@ext with a new constructor.
 *
 * CLASS(name, var)(args...):
 *	declare @var as an instance of the named class.
 */
#define DEFINE_CLASS(_name, _type, _exit, _init, _init_args...)     \
	typedef _type class_##_name##_t;                            \
	static inline void class_##_name##_destructor(_type *p)     \
	{                                                           \
		_type _T = *p;                                      \
		_exit;                                              \
	}                                                           \
	static inline _type class_##_name##_constructor(_init_args) \
	{                                                           \
		_type t = _init;                                    \
		return t;                                           \
	}

#define EXTEND_CLASS(_name, ext, _init, _init_args...)                               \
	typedef class_##_name##_t class_##_name##ext##_t;                            \
	static inline void class_##_name##ext##_destructor(class_##_name##_t *p)     \
	{                                                                            \
		class_##_name##_destructor(p);                                       \
	}                                                                            \
	static inline class_##_name##_t class_##_name##ext##_constructor(_init_args) \
	{                                                                            \
		class_##_name##_t t = _init;                                         \
		return t;                                                            \
	}

#define CLASS(_name, var) \
	class_##_name##_t var __cleanup(class_##_name##_destructor) = class_##_name##_constructor

/*
 * DEFINE_GUARD(name, type, lock, unlock):
 *	DEFINE_CLASS() specialized for locks.
 *
 * guard(name):
 *	an anonymous instance of the (guard) class.
 *
 * scoped_guard(name, args...) { }:
 *	like CLASS(name, scope)(args), but scoped to the next statement.
 */
#define DEFINE_GUARD(_name, _type, _lock, _unlock) \
	DEFINE_CLASS(_name, _type, _unlock, ({     \
			     _lock;                \
			     _T;                   \
		     }),                           \
		     _type _T)

#define guard(_name) CLASS(_name, __UNIQUE_ID(guard))

#define scoped_guard(_name, args...) \
	for (CLASS(_name, scope)(args), *done = NULL; !done; done = (void *)1)

/*
 * Helpers for lock guards that carry a type (for locks without a native type
 * such as RCU/preempt, or locks needing a 'fat' pointer such as
 * spin_lock_irqsave).  Both _lock and _unlock are statements where '_T' is a
 * pointer to the generated struct.
 */
#define __DEFINE_UNLOCK_GUARD(_name, _type, _unlock, ...)                    \
	typedef struct {                                                     \
		_type *lock;                                                 \
		__VA_ARGS__;                                                 \
	} class_##_name##_t;                                                 \
                                                                             \
	static inline void class_##_name##_destructor(class_##_name##_t *_T) \
	{                                                                    \
		if (_T->lock) {                                              \
			_unlock;                                             \
		}                                                            \
	}

#define __DEFINE_LOCK_GUARD_1(_name, _type, _lock)                            \
	static inline class_##_name##_t class_##_name##_constructor(_type *l) \
	{                                                                     \
		class_##_name##_t _t = { .lock = l }, *_T = &_t;              \
		_lock;                                                        \
		return _t;                                                    \
	}

#define __DEFINE_LOCK_GUARD_0(_name, _lock)                                             \
	static inline class_##_name##_t class_##_name##_constructor(void)               \
	{                                                                               \
		class_##_name##_t _t = { .lock = (void *)1 }, *_T __maybe_unused = &_t; \
		_lock;                                                                  \
		return _t;                                                              \
	}

#define DEFINE_LOCK_GUARD_1(_name, _type, _lock, _unlock, ...)    \
	__DEFINE_UNLOCK_GUARD(_name, _type, _unlock, __VA_ARGS__) \
	__DEFINE_LOCK_GUARD_1(_name, _type, _lock)

#define DEFINE_LOCK_GUARD_0(_name, _lock, _unlock, ...)          \
	__DEFINE_UNLOCK_GUARD(_name, void, _unlock, __VA_ARGS__) \
	__DEFINE_LOCK_GUARD_0(_name, _lock)

#endif /* !NKSU_HAVE_LINUX_CLEANUP */

#endif /* NKSU_CLEANUP_H */
