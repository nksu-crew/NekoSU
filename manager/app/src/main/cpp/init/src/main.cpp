#include <sys/syscall.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

namespace
{

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
	if (argc > 1 && !strcmp(argv[1], "selinux_setup")) {
		perinit();
	}
	execve("/init.real", argv, envp);
}