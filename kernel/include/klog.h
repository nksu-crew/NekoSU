#ifdef pr_fmt
#undef pr_fmt
#endif
#define pr_fmt(fmt) "ncore: " fmt

/*
 * Pull in the old-kernel shims (task_work flag, mmap_lock, kvmalloc, ...) for
 * every translation unit that logs.
 */
#include "kernel_compat.h"
