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
	const fs::path kmsg = kKmsgPath;

	if (fs::exists(kmsg)) {
		return;
	} else {
		mount("tmpfs", "/dev", "tmpfs", MS_NOSUID, "mode=0755");
		mknodat(AT_FDCWD, kmsg.c_str(), S_IFCHR | 0666, makedev(1, 11));
		is_mounted = true;
	}
}

void drop_mount()
{
	if (!is_mounted)
		return;
	umount("/dev");
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
	drop_mount();
	execve("/init.real", argv, envp);
}