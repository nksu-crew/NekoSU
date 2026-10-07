#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "kmod.h"

/*
 * nksu.ko 不再编译进 init, 而是随 ramdisk 放在文件系统中。
 * 依次尝试常见位置, 找到第一个可用者加载。
 */
static const char *ko_paths[] = {
    "/nksu.ko",
    "/data/adb/nksu/nksu.ko",
    "/data/local/tmp/nksu.ko",
    "/vendor/lib/modules/nksu.ko",
};

static unsigned char *read_file(const char *path, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long size = ftell(f);
    if (size <= 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);

    unsigned char *buf = malloc((size_t)size);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return NULL;
    }

    fclose(f);
    *out_size = (size_t)size;
    return buf;
}

/* 解析 uname release，如 "5.10.198-android12-9-g1234567" */
static void parse_kernel_version(const char *release, int *major, int *minor, int *android)
{
    *major = *minor = *android = 0;

    const char *p = release;
    *major = (int)strtol(p, (char **)&p, 10);
    if (*p == '.') {
        p++;
        *minor = (int)strtol(p, (char **)&p, 10);
    }

    const char *tag = strstr(release, "-android");
    if (tag) {
        tag += strlen("-android");
        *android = (int)strtol(tag, NULL, 10);
    }
}

int main(int argc, char *argv[], char *envp[]) {

  mount("proc", "/proc", "proc", MS_NODEV | MS_NOEXEC | MS_NOSUID, NULL);

  const char *init = "/init.real";
  if (access(init, F_OK) != 0) {
    init = "/system/bin/init";
    if (access(init, F_OK) != 0) {
      return -1; // can't find out real init, panic.
    }
  }
  unlink("/init");
  int result = link(init, "/init");
  if (result != 0) {
    perror("link");
  }

  struct utsname uts;
  int major = 0, minor = 0, android = 0;
  if (uname(&uts) == 0) {
    parse_kernel_version(uts.release, &major, &minor, &android);
    fprintf(stderr, "nksu: kernel release=%s parsed=%d.%d android=%d\n",
            uts.release, major, minor, android);
  } else {
    perror("uname");
  }

  int loaded = 0;
  for (size_t i = 0; i < sizeof(ko_paths) / sizeof(ko_paths[0]); i++) {
    size_t size = 0;
    unsigned char *image = read_file(ko_paths[i], &size);
    if (!image)
      continue;

    fprintf(stderr, "nksu: loading %s (%zu bytes)\n", ko_paths[i], size);
    if (kmod_load(image, size) == 0) {
      fprintf(stderr, "nksu: loaded %s\n", ko_paths[i]);
      free(image);
      loaded = 1;
      break;
    }
    fprintf(stderr, "nksu: failed to load %s\n", ko_paths[i]);
    free(image);
  }

  if (!loaded)
    fprintf(stderr, "nksu: no module could be loaded\n");

  umount2("/proc", MNT_DETACH);
  execve("/init", argv, envp);
  return 0;
}
