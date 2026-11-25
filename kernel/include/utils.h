#include <linux/string.h>
#include <linux/uaccess.h>

static int rewrite_user_string(char __user *user_str,
                               const char *new_str)
{
    char old_str[PAGE_SIZE];
    size_t old_len;
    size_t new_len;

    if (!user_str || !new_str)
        return -EINVAL;

    old_len = strnlen_user(user_str, sizeof(old_str));
    if (!old_len)
        return -EFAULT;

    if (old_len > sizeof(old_str))
        return -E2BIG;

    new_len = strlen(new_str) + 1;

    if (new_len > old_len)
        return -ENOSPC;

    if (copy_to_user(user_str, new_str, new_len))
        return -EFAULT;

    return 0;
}