#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <linux/errno.h>
#include <asm/ptrace.h>

#define MAX_ARG_CNT   64
#define MAX_ARG_LEN   256

static inline unsigned long regs_arg(struct pt_regs *regs, unsigned int argno)
{
#if defined(__aarch64__)
    if (unlikely(argno > 5))
        return 0;
    return regs->regs[argno];
#elif defined(__x86_64__)
    switch (argno) {
    case 0: return regs->di;
    case 1: return regs->si;
    case 2: return regs->dx;
    case 3: return regs->r10;
    case 4: return regs->r8;
    case 5: return regs->r9;
    default: return 0;
    }
#else
# error "unsupported arch"
#endif
}

static inline const char __user *const __user *
regs_uargv(struct pt_regs *regs, unsigned int argno)
{
    return (const char __user *const __user *)regs_arg(regs, argno);
}

static int get_argvx_at(const char __user *const __user *uargv,
                        unsigned int idx, char *buf, size_t len)
{
    const char __user *uarg;
    long n;

    if (unlikely(!uargv || !buf || len < 2))
        return -EINVAL;

    if (unlikely(get_user(uarg, &uargv[idx])))
        return -EFAULT;

    if (!uarg)
        return 0;

    n = strncpy_from_user(buf, uarg, len);
    if (unlikely(n < 0))
        return -EFAULT;

    return (int)n;
}

static int get_argc_at(const char __user *const __user *uargv)
{
    int i;

    if (unlikely(!uargv))
        return -EINVAL;

    for (i = 0; i < MAX_ARG_CNT; i++) {
        const char __user *uarg;
        if (unlikely(get_user(uarg, &uargv[i])))
            return -EFAULT;
        if (!uarg)
            return i;
    }
    return MAX_ARG_CNT;
}

 int get_argvx(struct pt_regs *regs, unsigned int argno,
                     unsigned int idx, char *buf, size_t len)
{
    if (unlikely(!current->mm || idx >= MAX_ARG_CNT))
        return -EINVAL;

    return get_argvx_at(regs_uargv(regs, argno), idx, buf, len);
}

 int get_argc(struct pt_regs *regs, unsigned int argno)
{
    if (unlikely(!current->mm))
        return -EINVAL;

    return get_argc_at(regs_uargv(regs, argno));
}