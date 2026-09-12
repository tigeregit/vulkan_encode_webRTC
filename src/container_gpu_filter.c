/* Optional, process-local workaround for NVIDIA container-toolkit #1249.
 * Only removes GPUs without an accessible /dev/nvidiaN; never grants access.
 * ABI: NVIDIA open-gpu-kernel-modules 580.142, ctrl0000gpu.h and nvos.h.
 * Inspired by the approach discussed in NVIDIA/k8s-device-plugin#1282.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
struct Control {
  uint32_t client, object, cmd, flags;
  void *params __attribute__((aligned(8)));
  uint32_t size, status;
};
struct IdInfo {
  uint32_t id, flags, device, subdevice;
  void *name __attribute__((aligned(8)));
  uint32_t sli, board, instance;
  int32_t numa;
};
static int (*next_ioctl)(int, unsigned long, ...);
static pthread_once_t initialized = PTHREAD_ONCE_INIT;
static void initialize(void) {
  next_ioctl = dlsym(RTLD_NEXT, "ioctl");
  if (!next_ioctl)
    _exit(127);
}
int ioctl(int fd, unsigned long request, ...) {
  pthread_once(&initialized, initialize);
  va_list ap;
  va_start(ap, request);
  void *arg = va_arg(ap, void *);
  va_end(ap);
  int result = next_ioctl(fd, request, arg), saved_errno = errno;
  unsigned long rm = _IOWR('F', 0x2a, struct Control);
  if (result || request != rm || !arg) {
    errno = saved_errno;
    return result;
  }
  struct Control *c = arg;
  if (c->cmd != 0x201 || c->status || !c->params || c->size != 32 * sizeof(uint32_t)) {
    errno = saved_errno;
    return result;
  }
  uint32_t *ids = c->params, filtered[32];
  unsigned n = 0;
  int unresolved = 0;
  for (unsigned i = 0; i < 32 && ids[i] != UINT32_MAX; i++) {
    char name[128] = {0};
    struct IdInfo info = {.id = ids[i], .name = name};
    struct Control query = {.client = c->client,
                            .object = c->client,
                            .cmd = 0x202,
                            .params = &info,
                            .size = sizeof(info)};
    if (next_ioctl(fd, rm, &query) || query.status) {
      unresolved = 1;
      break;
    }
    char path[64];
    snprintf(path, sizeof(path), "/dev/nvidia%u", info.device);
    struct stat st;
    if (stat(path, &st) || !S_ISCHR(st.st_mode) || major(st.st_rdev) != 195 ||
        minor(st.st_rdev) != info.device)
      continue;
    int dev = open(path, O_RDWR | O_CLOEXEC);
    if (dev < 0)
      continue;
    close(dev);
    filtered[n++] = ids[i];
  }
  // Leave the original response untouched if mapping cannot be proven.
  if (!unresolved && n) {
    for (unsigned i = 0; i < 32; i++)
      ids[i] = i < n ? filtered[i] : UINT32_MAX;
  }
  errno = saved_errno;
  return result;
}
