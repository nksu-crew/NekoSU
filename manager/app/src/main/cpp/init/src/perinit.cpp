// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * NekoSU perinit runner.
 *
 * The init wrapper installed by the boot/init_boot patcher runs as PID 1 at
 * first stage and re-enters at selinux_setup, where first-stage mounts
 * (/metadata, /system, /vendor, /proc, /sys, /dev) are ready but no SELinux
 * policy is loaded and init.rc has not been parsed.  It calls perinit() and
 * then execs the original init.
 *
 * perinit() forks a worker and waits for it with a deadline, kills whatever is
 * left in the worker's process group and returns; the caller keeps booting.
 * Everything that can fail runs in the worker, so a crash there only costs the
 * preinit actions, never the boot.
 *
 * Actions come from /metadata/.../nksu/preinit/<module id>/preinit.conf:
 *
 *   insmod <ko> [params...]   load a kernel module
 *   exec [-t ms] <prog> ...   run a static program and wait for it
 *   write <path> <content>    write an existing procfs/sysfs file
 *
 * This is a userspace port of KernelSU's kernel/preinit/runner/runner.c.  It
 * deliberately uses libc instead of a nolibc runtime: the wrapper is a normal
 * static C++ executable and is already part of the ramdisk.
 */

#include "perinit.h"
#include "klog.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <dirent.h>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>
#include <utility>
#include <vector>

#ifndef AT_EMPTY_PATH
#define AT_EMPTY_PATH 0x1000
#endif

namespace
{

/* ---------------------------------------------------------------- layout */

/*
 * Preinit copies live on /metadata, the only writable filesystem available
 * this early (module directories on /data are mounted much later).  The vold
 * location is preferred, matching KernelSU; the other two are fallbacks for
 * devices where that directory does not exist.
 */
constexpr const char *kKsuDirs[] = {
	"/metadata/vold/nksu",
	"/metadata/watchdog/nksu",
	"/metadata/nksu",
};
constexpr const char *kPreinitSubdir = "preinit";
constexpr const char *kConfName = "preinit.conf";
constexpr const char *kBootcountName = ".preinit_bootcount";

constexpr std::size_t kConfMax = 64 * 1024;
constexpr std::size_t kLineMax = 4096;
constexpr std::size_t kMaxTokens = 128;
constexpr std::size_t kMaxModules = 256;
constexpr std::size_t kModIdMax = 256;
constexpr std::size_t kParamsMax = 8192;

constexpr long kTotalTimeoutMs = 30 * 1000;
constexpr long kExecTimeoutMs = 5 * 1000;
constexpr long kBootcountLimit = 3;
constexpr int kOwnerUid = 0;

/* ------------------------------------------------------------------ utils */

long now_ms()
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts))
		return 0;
	return static_cast<long>(ts.tv_sec) * 1000L + ts.tv_nsec / 1000000L;
}

void sleep_ms(long ms)
{
	struct timespec ts;

	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (ms % 1000) * 1000000L;
	while (nanosleep(&ts, &ts) && errno == EINTR)
		;
}

bool is_space(char c)
{
	return c == ' ' || c == '\t' || c == '\r';
}

bool safe_owner(const struct stat &st)
{
	return st.st_uid == kOwnerUid && !(st.st_mode & (S_IWGRP | S_IWOTH));
}

bool is_dir(const std::string &path)
{
	struct stat st;

	return !lstat(path.c_str(), &st) && S_ISDIR(st.st_mode);
}

std::string join_path(const std::string &dir, const std::string &name)
{
	std::string path = dir;

	if (!path.empty() && path.back() != '/')
		path.push_back('/');
	path += name;
	return path;
}

/*
 * Read up to max bytes into out.  Returns false when the file cannot be
 * opened; short reads (including an empty file) are not errors.
 */
bool read_file(const std::string &path, std::string &out, std::size_t max)
{
	int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
	char buf[4096];

	if (fd < 0)
		return false;
	out.clear();
	out.reserve(std::min<std::size_t>(max, 16384));
	while (out.size() < max) {
		ssize_t n = read(fd, buf, sizeof(buf));

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		std::size_t room =
			std::min<std::size_t>(static_cast<std::size_t>(n), max - out.size());
		out.append(buf, room);
		if (static_cast<std::size_t>(n) > room)
			break;
	}
	close(fd);
	return true;
}

int write_all(int fd, const char *buf, std::size_t len)
{
	while (len) {
		ssize_t n = write(fd, buf, len);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return -1;
		buf += n;
		len -= static_cast<std::size_t>(n);
	}
	return 0;
}

/* ------------------------------------------------------------- boot mode */

/*
 * Find "androidboot.mode" in either bootconfig ("key = \"value\"") or cmdline
 * ("key=value") syntax and copy its value.
 */
bool find_boot_mode(const char *buf, char *out, std::size_t size)
{
	static const char key[] = "androidboot.mode";
	const char *p = buf;

	if (!size)
		return false;
	while ((p = strstr(p, key))) {
		const char *q = p + sizeof(key) - 1;
		std::size_t i;

		if (p != buf && !is_space(p[-1]) && p[-1] != '\n') {
			p = q;
			continue;
		}
		while (is_space(*q))
			q++;
		if (*q != '=') {
			p = q;
			continue;
		}
		q++;
		while (is_space(*q))
			q++;
		if (*q == '"')
			q++;
		for (i = 0; i + 1 < size && *q && *q != '"' && *q != '\n' && !is_space(*q); i++)
			out[i] = *q++;
		out[i] = '\0';
		return true;
	}
	return false;
}

bool is_charger_mode()
{
	constexpr std::size_t kBootInfoMax = 16384;
	char mode[32];
	std::string content;

	if ((read_file("/proc/bootconfig", content, kBootInfoMax - 1) && !content.empty() &&
	     find_boot_mode(content.c_str(), mode, sizeof(mode))) ||
	    (read_file("/proc/cmdline", content, kBootInfoMax - 1) && !content.empty() &&
	     find_boot_mode(content.c_str(), mode, sizeof(mode))))
		return !strcmp(mode, "charger");
	return false;
}

/* ------------------------------------------------------------- bootcount */

/*
 * Returns true when preinit may run.  The counter is bumped and synced before
 * any action runs.  If it cannot be persisted we refuse to run, otherwise a
 * bad module could loop forever.
 *
 * Once kBootcountLimit boots in a row did not complete, preinit is skipped
 * and the counter parks at kBootcountLimit + 1, so it stays off until the
 * userspace side resets it (on boot completed) or its content changes.
 */
bool bootcount_check_and_bump(const std::string &ksu_dir)
{
	std::string path = join_path(ksu_dir, kBootcountName);
	std::string content;
	char buf[16];
	long count = 0;
	bool run;
	int fd;

	if (read_file(path, content, sizeof(buf) - 1) && !content.empty())
		count = strtol(content.c_str(), nullptr, 10);
	if (count < 0)
		count = 0;

	run = count < kBootcountLimit;
	if (!run) {
		klog("bootcount %ld >= %ld, preinit disabled until reset", count, kBootcountLimit);
		if (count > kBootcountLimit)
			return false;
	}

	fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0) {
		klog("open %s failed: %d, skip all preinit", path.c_str(), errno);
		return false;
	}
	int n = snprintf(buf, sizeof(buf), "%ld\n", count + 1);
	if (n < 0 || write_all(fd, buf, static_cast<std::size_t>(n)) || fsync(fd)) {
		klog("write %s failed: %d, skip all preinit", path.c_str(), errno);
		close(fd);
		return false;
	}
	close(fd);

	fd = open(ksu_dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd >= 0) {
		fsync(fd);
		close(fd);
	}
	klog("bootcount %ld -> %ld", count, count + 1);
	return run;
}

/* -------------------------------------------------------------- parsing */

/*
 * Split a line in place.  Tokens are separated by blanks, double quotes group
 * blanks, \" and \\ are escapes.  No variable expansion.  Returns the token
 * count or -1 on syntax error.
 */
int tokenize(char *line, char **argv, int max)
{
	char *r = line, *w = line;
	bool in_quote;
	int argc = 0;

	for (;;) {
		while (is_space(*r))
			r++;
		if (!*r)
			break;
		if (argc >= max - 1)
			return -1;
		argv[argc++] = w;
		in_quote = false;
		while (*r) {
			if (*r == '\\' && (r[1] == '"' || r[1] == '\\')) {
				*w++ = r[1];
				r += 2;
				continue;
			}
			if (*r == '"') {
				in_quote = !in_quote;
				r++;
				continue;
			}
			if (!in_quote && is_space(*r))
				break;
			*w++ = *r++;
		}
		if (in_quote)
			return -1;
		if (*r)
			r++;
		*w++ = '\0';
	}
	argv[argc] = nullptr;
	return argc;
}

/* -------------------------------------------------------------- actions */

struct ModuleContext {
	const char *id;
	const char *dir;
};

int open_checked(const ModuleContext &m, const char *path)
{
	struct stat st;
	int fd = open(path, O_RDONLY | O_CLOEXEC);

	if (fd < 0) {
		klog("[%s] open %s failed: %d", m.id, path, errno);
		return -1;
	}
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || !safe_owner(st)) {
		klog("[%s] %s: not a root owned, non group/other writable regular file", m.id,
		     path);
		close(fd);
		return -1;
	}
	return fd;
}

bool has_space(const char *s)
{
	for (; *s; s++)
		if (is_space(*s))
			return true;
	return false;
}

bool append(std::string &out, const char *s, std::size_t n)
{
	if (n >= kParamsMax || out.size() + n >= kParamsMax)
		return false;
	out.append(s, n);
	return true;
}

/*
 * Join module parameters with blanks.  Tokens that contain blanks are quoted
 * again (key="a b") so the kernel's parse_args sees a single argument.
 */
bool build_params(char **argv, int argc, std::string &out)
{
	out.clear();
	for (int i = 0; i < argc; i++) {
		const char *a = argv[i];
		const char *eq = strchr(a, '=');
		std::size_t key = eq ? static_cast<std::size_t>(eq - a + 1) : 0;
		if (strchr(a, '"'))
			return false;
		if (i && !append(out, " ", 1))
			return false;
		if (!has_space(a)) {
			if (!append(out, a, strlen(a)))
				return false;
		} else if (!append(out, a, key) || !append(out, "\"", 1) ||
			   !append(out, a + key, strlen(a + key)) || !append(out, "\"", 1)) {
			return false;
		}
	}
	return true;
}

/*
 * NekoSU does not expose a symbol-lookup ioctl, so unlike KernelSU's runner
 * this cannot bind a module's non-exported symbols before loading it.  A module
 * that uses them fails here with the kernel's "Unknown symbol" error, which is
 * logged like any other insmod failure; exported symbols work normally.
 */
int do_insmod(const ModuleContext &m, int argc, char **argv)
{
	std::string params;
	struct stat st;
	int fd, ret;

	if (argc < 2) {
		klog("[%s] insmod: missing path", m.id);
		return -1;
	}
	if (!build_params(argv + 2, argc - 2, params)) {
		klog("[%s] insmod %s: bad parameters", m.id, argv[1]);
		return -1;
	}
	fd = open_checked(m, argv[1]);
	if (fd < 0)
		return -1;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode)) {
		klog("[%s] insmod %s: stat failed", m.id, argv[1]);
		close(fd);
		return -1;
	}

	ret = static_cast<int>(syscall(SYS_finit_module, fd, params.c_str(), 0));
	int saved_errno = errno;

	if (ret < 0 && saved_errno == ENOSYS) {
		/*
		 * finit_module is missing on very old kernels.  Read the image
		 * into memory and use the legacy init_module syscall.
		 */
		std::string image;

		if (st.st_size <= 0 ||
		    !read_file(argv[1], image, static_cast<std::size_t>(st.st_size))) {
			klog("[%s] insmod %s: cannot read image", m.id, argv[1]);
			close(fd);
			return -1;
		}
		ret = static_cast<int>(
			syscall(SYS_init_module, image.data(), image.size(), params.c_str()));
		saved_errno = errno;
	}
	close(fd);

	if (ret && saved_errno == EEXIST) {
		klog("[%s] insmod %s: already loaded", m.id, argv[1]);
		return 0;
	}
	if (ret) {
		klog("[%s] insmod %s failed: %d", m.id, argv[1], saved_errno);
		return -1;
	}
	klog("[%s] insmod %s ok", m.id, argv[1]);
	return 0;
}

int do_write(const ModuleContext &m, int argc, char **argv)
{
	int fd, ret;

	if (argc != 3) {
		klog("[%s] write: usage: write <path> <content>", m.id);
		return -1;
	}
	fd = open(argv[1], O_WRONLY | O_CLOEXEC);
	if (fd < 0) {
		klog("[%s] write %s: open failed: %d", m.id, argv[1], errno);
		return -1;
	}
	ret = write_all(fd, argv[2], strlen(argv[2]));
	if (ret)
		klog("[%s] write %s failed: %d", m.id, argv[1], errno);
	close(fd);
	return ret;
}

struct LineForward {
	const char *id;
	std::string buf;
};

void fwd_flush(LineForward &f)
{
	char out[kLogLineMax + kModIdMax + 32];
	int n;

	if (f.buf.empty())
		return;
	n = snprintf(out, sizeof(out), "nksu_perinit[%s]: %s\n", f.id, f.buf.c_str());
	if (n < 0)
		return;
	if (static_cast<std::size_t>(n) >= sizeof(out)) {
		n = sizeof(out) - 1;
		out[n - 1] = '\n';
	}
	kmsg_write(out, static_cast<std::size_t>(n));
	f.buf.clear();
}

void fwd_feed(LineForward &f, const char *data, std::size_t len)
{
	for (; len; data++, len--) {
		if (*data == '\n') {
			fwd_flush(f);
			continue;
		}
		if (f.buf.size() == kLogLineMax)
			fwd_flush(f);
		f.buf.push_back(*data);
	}
}

/*
 * Forward the child's output until it exits or times out.  Returns its wait
 * status or -1 on timeout.
 */
int wait_forward(const ModuleContext &m, pid_t pid, int out_fd, long timeout_ms)
{
	LineForward fwd;

	fwd.id = m.id;
	long deadline = now_ms() + timeout_ms;
	struct pollfd pfd;

	pfd.fd = out_fd;
	pfd.events = POLLIN;
	pfd.revents = 0;
	bool exited = false, eof = false;
	int status = 0;
	char buf[1024];

	for (;;) {
		if (!eof) {
			/* After exit, only drain what is buffered: grandchildren may hold the pipe. */
			int ready = poll(&pfd, 1, exited ? 0 : 20);

			if (ready > 0) {
				ssize_t n = read(out_fd, buf, sizeof(buf));

				if (n > 0)
					fwd_feed(fwd, buf, static_cast<std::size_t>(n));
				else if (n == 0 || errno != EINTR)
					eof = true;
			} else if (exited) {
				break;
			}
		} else if (!exited) {
			sleep_ms(20);
		}
		if (!exited) {
			pid_t r = waitpid(pid, &status, WNOHANG);

			if (r == pid || (r < 0 && errno != EINTR))
				exited = true;
		}
		if (exited && eof)
			break;
		if (now_ms() >= deadline) {
			if (exited)
				break;
			kill(pid, SIGKILL);
			/*
			 * A child stuck in D state ignores SIGKILL: wait at most a
			 * second, the supervisor kills and reaps whatever is left.
			 */
			long kill_deadline = now_ms() + 1000;
			while (waitpid(pid, &status, WNOHANG) == 0 && now_ms() < kill_deadline)
				sleep_ms(10);
			fwd_flush(fwd);
			return -1;
		}
	}
	fwd_flush(fwd);
	return status;
}

int do_exec(const ModuleContext &m, int argc, char **argv)
{
	long timeout = kExecTimeoutMs;
	int idx = 1, pfd[2], status, fd;
	pid_t pid;

	if (argc > 1 && !strcmp(argv[1], "-t")) {
		char *end;
		if (argc < 3 || (timeout = strtol(argv[2], &end, 10)) <= 0 || *end) {
			klog("[%s] exec: bad -t value", m.id);
			return -1;
		}
		idx = 3;
	}
	if (idx >= argc) {
		klog("[%s] exec: missing program", m.id);
		return -1;
	}
	/* Exec the checked file itself, not whatever the path resolves to later. */
	fd = open_checked(m, argv[idx]);
	if (fd < 0)
		return -1;

	std::string env_modid = std::string("MODID=") + m.id;
	std::string env_dir = std::string("PREINIT_DIR=") + m.dir;
	std::vector<std::string> env = {
		"KSU=true",
		"KSU_PREINIT=1",
		"NKSU=true",
		"NKSU_PREINIT=1",
		env_modid,
		env_dir,
		"PATH=/system/bin:/vendor/bin",
	};
	std::vector<char *> envp;

	envp.reserve(env.size() + 1);
	for (auto &entry : env)
		envp.push_back(entry.data());
	envp.push_back(nullptr);

	if (pipe2(pfd, O_CLOEXEC)) {
		klog("[%s] exec: pipe failed: %d", m.id, errno);
		close(fd);
		return -1;
	}
	pid = fork();
	if (pid < 0) {
		klog("[%s] exec: fork failed: %d", m.id, errno);
		close(fd);
		close(pfd[0]);
		close(pfd[1]);
		return -1;
	}
	if (pid == 0) {
		int nullfd = open("/dev/null", O_RDONLY | O_CLOEXEC);

		if (nullfd >= 0) {
			dup2(nullfd, STDIN_FILENO);
			if (nullfd != STDIN_FILENO)
				close(nullfd);
		} else {
			close(STDIN_FILENO);
		}
		dup2(pfd[1], STDOUT_FILENO);
		dup2(pfd[1], STDERR_FILENO);
		if (pfd[1] != STDOUT_FILENO && pfd[1] != STDERR_FILENO)
			close(pfd[1]);
		close(pfd[0]);
		/* Static ELF only, so a close-on-exec fd is fine for AT_EMPTY_PATH. */
		syscall(SYS_execveat, fd, "", argv + idx, envp.data(), AT_EMPTY_PATH);
		_exit(127);
	}
	close(fd);
	close(pfd[1]);
	status = wait_forward(m, pid, pfd[0], timeout);
	close(pfd[0]);

	if (status == -1) {
		klog("[%s] exec %s: timed out after %ld ms", m.id, argv[idx], timeout);
		return -1;
	}
	if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
		klog("[%s] exec %s ok", m.id, argv[idx]);
		return 0;
	}
	if (WIFEXITED(status))
		klog("[%s] exec %s exited with %d", m.id, argv[idx], WEXITSTATUS(status));
	else if (WIFSIGNALED(status))
		klog("[%s] exec %s killed by signal %d", m.id, argv[idx], WTERMSIG(status));
	else
		klog("[%s] exec %s failed", m.id, argv[idx]);
	return -1;
}

/* Returns 0 on success, -1 when the action failed. */
int run_action(const ModuleContext &m, int argc, char **argv)
{
	if (!strcmp(argv[0], "insmod"))
		return do_insmod(m, argc, argv);
	if (!strcmp(argv[0], "exec"))
		return do_exec(m, argc, argv);
	if (!strcmp(argv[0], "write"))
		return do_write(m, argc, argv);
	klog("[%s] unknown action: %s", m.id, argv[0]);
	return -1;
}

/* -------------------------------------------------------------- modules */

void run_module(const std::string &preinit_dir, const std::string &id)
{
	std::string dir = join_path(preinit_dir, id);
	std::string conf;
	struct stat st;
	char *argv[kMaxTokens];
	int fd, lineno = 0;

	if (lstat(dir.c_str(), &st) || !S_ISDIR(st.st_mode) || !safe_owner(st)) {
		klog("[%s] skip: directory not root owned or group/other writable", id.c_str());
		return;
	}
	if (chdir(dir.c_str())) {
		klog("[%s] skip: chdir failed: %d", id.c_str(), errno);
		return;
	}
	fd = open(kConfName, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0) {
		klog("[%s] skip: no %s", id.c_str(), kConfName);
		return;
	}
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || !safe_owner(st) ||
	    st.st_size > static_cast<off_t>(kConfMax)) {
		klog("[%s] skip: bad %s", id.c_str(), kConfName);
		close(fd);
		return;
	}
	conf.resize(static_cast<std::size_t>(st.st_size));
	std::size_t total = 0;
	while (total < conf.size()) {
		ssize_t n = read(fd, conf.data() + total, conf.size() - total);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		total += static_cast<std::size_t>(n);
	}
	close(fd);
	conf.resize(total);
	conf.push_back('\0');

	ModuleContext m;

	m.id = id.c_str();
	m.dir = dir.c_str();
	klog("[%s] start", id.c_str());
	char *line = conf.data();
	while (line && *line) {
		char *next = strchr(line, '\n');
		bool optional;
		int argc;
		std::size_t len;

		lineno++;
		if (next)
			*next++ = '\0';
		len = strlen(line);
		if (len > kLineMax) {
			klog("[%s] line %d: too long, abort module", id.c_str(), lineno);
			return;
		}
		while (is_space(*line))
			line++;
		if (*line == '#' || !*line) {
			line = next;
			continue;
		}

		argc = tokenize(line, argv, static_cast<int>(kMaxTokens));
		if (argc < 0) {
			klog("[%s] line %d: syntax error, abort module", id.c_str(), lineno);
			return;
		}
		if (!argc) {
			line = next;
			continue;
		}
		len = strlen(argv[0]);
		optional = len > 1 && argv[0][len - 1] == '?';
		if (optional)
			argv[0][len - 1] = '\0';

		if (run_action(m, argc, argv) && !optional) {
			klog("[%s] line %d: %s failed, abort module", id.c_str(), lineno, argv[0]);
			return;
		}
		line = next;
	}
	klog("[%s] done", id.c_str());
}

std::vector<std::string> collect_modules(const std::string &preinit_dir)
{
	std::vector<std::string> modules;
	DIR *dir = opendir(preinit_dir.c_str());

	if (!dir)
		return modules;
	while (struct dirent *entry = readdir(dir)) {
		if (entry->d_name[0] == '.')
			continue;
		if (strlen(entry->d_name) >= kModIdMax)
			continue;
		if (modules.size() == kMaxModules) {
			klog("too many modules, ignoring %s", entry->d_name);
			continue;
		}
		modules.emplace_back(entry->d_name);
	}
	closedir(dir);

	/* Run order is the lexical order of module ids. */
	std::sort(modules.begin(), modules.end());
	return modules;
}

int worker_main()
{
	const char *ksu_dir = nullptr;
	std::string preinit_dir;
	struct stat st;
	std::vector<std::string> modules;

	if (is_charger_mode()) {
		klog("charger mode, skip");
		return 0;
	}

	for (const char *candidate : kKsuDirs) {
		std::string dir = join_path(candidate, kPreinitSubdir);

		if (is_dir(dir)) {
			ksu_dir = candidate;
			preinit_dir = std::move(dir);
			break;
		}
	}
	if (!ksu_dir)
		return 0;

	if (lstat(preinit_dir.c_str(), &st) || !S_ISDIR(st.st_mode) || !safe_owner(st)) {
		klog("%s is not root owned or is group/other writable, skip", preinit_dir.c_str());
		return 0;
	}

	modules = collect_modules(preinit_dir);
	if (modules.empty())
		return 0;
	if (!bootcount_check_and_bump(ksu_dir))
		return 0;

	for (const std::string &id : modules)
		run_module(preinit_dir, id);
	return 0;
}

/* ----------------------------------------------------------------- PID 1 */

void supervise(pid_t worker)
{
	long deadline = now_ms() + kTotalTimeoutMs;
	pid_t r;

	for (;;) {
		r = waitpid(worker, nullptr, WNOHANG);
		if (r == worker || (r < 0 && errno != EINTR))
			break;
		if (now_ms() >= deadline) {
			klog("timed out after %ld ms, killing worker", kTotalTimeoutMs);
			kill(-worker, SIGKILL);
			kill(worker, SIGKILL);
			/*
			 * SIGKILL cannot interrupt a worker stuck in a module's
			 * init, so do not block on it: boot must go on.
			 */
			for (int i = 0; i < 100 && waitpid(worker, nullptr, WNOHANG) != worker; i++)
				sleep_ms(10);
			break;
		}
		sleep_ms(10);
	}

	/*
	 * Nothing may outlive preinit: once the policy loads, leftovers would
	 * keep running in the kernel domain.  Orphans are reparented to us;
	 * reap what has already exited.  Do not wait for the rest: first stage
	 * init may have left children of its own (e.g. snapuserd).
	 */
	kill(-worker, SIGKILL);
	while ((r = waitpid(-1, nullptr, WNOHANG)) > 0 || (r < 0 && errno == EINTR))
		;
}

} // namespace

void perinit()
{
	static bool started;

	if (started)
		return;
	started = true;

	klog("start perinit");

	if (getpid() != 1) {
		klog("not PID 1, skip");
		return;
	}

	pid_t pid = fork();
	if (pid == 0) {
		setpgid(0, 0);
		_exit(worker_main());
	}
	if (pid > 0) {
		setpgid(pid, pid);
		supervise(pid);
	} else {
		klog("fork failed: %d", errno);
	}
}
