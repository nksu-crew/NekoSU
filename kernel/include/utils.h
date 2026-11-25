#include <linux/string.h>
#include <linux/uaccess.h>

static inline int rewrite_user_string(char __user *user_str,
                               const char *new_str)
{
    if (!user_str || !new_str)
        return -EINVAL;

    if (copy_to_user(user_str, new_str, strlen(new_str) + 1))
        return -EFAULT;

    return 0;
}