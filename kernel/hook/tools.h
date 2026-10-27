/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <linux/types.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <asm/ptrace.h>

// hook/get_arg.c

#define MAX_ARG_CNT 64
#define MAX_ARG_LEN 256

static inline bool argv_eq(const char *buf, int n, size_t buflen, const char *target)
{
	return n > 0 && (size_t)n < buflen && strcmp(buf, target) == 0;
}

int get_argvx(struct pt_regs *regs, unsigned int argno, unsigned int idx, char *buf, size_t len);

/* Copy a plain string argument (e.g. the exec filename) into buf; the result
 * is always NUL-terminated.  Bounded to len bytes. */
int get_path_arg(struct pt_regs *regs, unsigned int argno, char *buf, size_t len);

int get_argc(struct pt_regs *regs, unsigned int argno);
