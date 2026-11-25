#include <sys/syscall.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <sys/mount.h>

#include "perinit.h"
#include "klog.hpp"

namespace fs = std::filesystem;

namespace
{
bool is_mounted = false;

void setup_kmsg()
{
	const fs::path kmsg = kKmsgPath;

	if (fs::exists(kmsg)) {
		return;
	} else {
		if (mount("tmpfs", "/dev", "tmpfs", MS_NOSUID, "mode=0755") == 0)
			is_mounted = true;
		mknodat(AT_FDCWD, kmsg.c_str(), S_IFCHR | 0666, makedev(1, 11));
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
	klog("Hello NekoSU (argv1=%s)", argc > 1 ? argv[1] : "");
	load_module();
	if (argc > 1 && !strcmp(argv[1], "selinux_setup")) {
		perinit();
		drop_mount();
		// Prefer the preserved original init: exec'ing /system/bin/init
		// again relies on the kernel's one-shot redirect guard.
		if (fs::exists("/init.real")) {
			klog("perinit done, exec /init.real");
			execve("/init.real", argv, envp);
		}
		if (fs::exists("/system/bin/init")) {
			klog("perinit done, exec /system/bin/init");
			execve("/system/bin/init", argv, envp);
		}
	}
	drop_mount();
	if (fs::exists("/init.real")) {
		execve("/init.real", argv, envp);
	}
	if (fs::exists("/system/bin/init")) {
		execve("/system/bin/init", argv, envp);
	}
	klog("failed to exec the real init: %s", strerror(errno));
	return 1;
}