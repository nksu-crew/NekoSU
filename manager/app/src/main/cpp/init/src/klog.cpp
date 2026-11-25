#include "klog.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstddef>
#include <algorithm>

void kmsg_write(const char *buf, std::size_t len)
{
	int fd = open(kKmsgPath, O_WRONLY | O_CLOEXEC);

	if (fd < 0)
		fd = STDERR_FILENO;
	while (len) {
		ssize_t n = write(fd, buf, len);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		buf += n;
		len -= static_cast<std::size_t>(n);
	}
	if (fd != STDERR_FILENO)
		close(fd);
}

__attribute__((format(printf, 1, 2))) void klog(const char *fmt, ...)
{
	char buf[kLogLineMax];
	std::size_t used;
	va_list args;

	int prefix = snprintf(buf, sizeof(buf), "NekoSU: ");
	if (prefix < 0)
		return;
	used = std::min(static_cast<std::size_t>(prefix), sizeof(buf) - 2);

	va_start(args, fmt);
	int n = vsnprintf(buf + used, sizeof(buf) - used - 1, fmt, args);
	va_end(args);
	if (n > 0)
		used += std::min(static_cast<std::size_t>(n), sizeof(buf) - used - 2);
	buf[used++] = '\n';
	kmsg_write(buf, used);
}