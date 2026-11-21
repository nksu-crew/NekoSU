#include <sys/syscall.h>
#include <fcntl.h>
#include <unistd.h>

namespace
{

void load_module(void)
{
	int fd = open("/nksu.ko", O_RDONLY);
	if (fd < 0)
		return;
	syscall(SYS_finit_module, fd, "", 0);
	close(fd);
}

} // namespace

int main(int argc, char *argv[], char **envp)
{
	load_module();
	unlink("/init");
	symlink("/init.real", "/init");
	execve("/init", argv, envp);
}