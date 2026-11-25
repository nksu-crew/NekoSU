#include <sys/syscall.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <cstring>

#include "perinit.h"

namespace
{

void setup_kmsg()
{
	const char *const KMSG = "/kmsg";

	if (access(KMSG, F_OK) == 0) {
		return;
	} else {
		mknodat(AT_FDCWD, "/kmsg", S_IFCHR | 0666, makedev(1, 11));
	}
}

void load_module(void)
{
	int fd = open("/nksu.ko", O_RDONLY);
	if (fd < 0)
		return;
	syscall(SYS_finit_module, fd, "", 0);
	close(fd);
	unlink("/nksu.ko");
}

} // namespace

int main(int argc, char *argv[], char **envp)
{
	load_module();
	if (argc > 1) {
		if (!strcmp(argv[1], "selinux_setup")) {
			setup_kmsg();
			perinit();
		}
	}
	execve("/init.real", argv, envp);
}