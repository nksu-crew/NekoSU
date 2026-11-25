#ifndef NKSU_UTILS_H
#define NKSU_UTILS_H

#include <linux/ptrace.h>
#include <linux/string.h>
#include <linux/uaccess.h>

/*
 * Stage a short string on the current process's user stack.
 *
 * A syscall argument (the execve filename, for instance) can point at a
 * read-only string literal, so it cannot be rewritten in place.  Writing
 * below the user stack pointer gives the syscall a private, writable copy
 * without allocating a page or touching the original string.
 */
static inline void __user *nksu_user_stack_buffer(const void *data, size_t len)
{
	char __user *p = (char __user *)current_user_stack_pointer() - len;

	return copy_to_user(p, data, len) ? NULL : p;
}

#endif /* NKSU_UTILS_H */
