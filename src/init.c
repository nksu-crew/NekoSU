#include "init.h"

int init_nksu(void)
{
    int ret = hook_init();
    return ret;
}

void exit_nksu(void)
{
    hook_exit();
}
