/* SPDX-License-Identifier: GPL-3.0 */
#pragma once

#include <linux/types.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <asm/ptrace.h>

// hook/get_arg.c

#define MAX_ARG_CNT   64
#define MAX_ARG_LEN   256

static inline bool argv_eq(const char *buf, int n, size_t buflen,
                           const char *target)
{
    return n > 0 && (size_t)n < buflen && strcmp(buf, target) == 0;
}

int get_argvx(struct pt_regs *regs, unsigned int argno,
              unsigned int idx, char *buf, size_t len);

int get_argc(struct pt_regs *regs, unsigned int argno);