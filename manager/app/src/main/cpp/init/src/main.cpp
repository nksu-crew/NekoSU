#include <sys/syscall.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <cstring>
#include <filesystem>

#include "perinit.h"
#include "klog.hpp"

namespace fs = std::filesystem;

namespace
{

void setup_kmsg()
{
	const fs::path kmsg = "/kmsg";

	if (fs::exists(kmsg)) {
		return;
	} else {
		mknodat(AT_FDCWD, kmsg.c_str(), S_IFCHR | 0666, makedev(1, 11));
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
	setup_kmsg();
	klog("Hello NekoSU");
	load_module();
	if (argc > 1 && !strcmp(argv[1], "selinux_setup")) {
		perinit();
		if (fs::exists("/system/bin/init")) {
			execve("/system/bin/init", argv, envp);
		}
	}
	execve("/init.real", argv, envp);
}