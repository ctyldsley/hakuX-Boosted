// hakuX-Boosted bench: see bench_compat.h and android/sharedmem.h.

#include "bench_compat.h"

#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "android/sharedmem.h"

pid_t pthread_gettid_np(pthread_t thread) {
  if (pthread_equal(thread, pthread_self())) {
    return gettid();
  }
  // A thread's CPU clock id encodes its kernel thread id.
  clockid_t clock;
  if (pthread_getcpuclockid(thread, &clock) != 0) {
    return -1;
  }
  return (pid_t) ~(clock >> 3);
}

int ASharedMemory_create(const char* name, size_t size) {
  int fd = memfd_create(name ? name : "bench", MFD_CLOEXEC);
  if (fd < 0) {
    return -1;
  }
  if (ftruncate(fd, (off_t)size) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

size_t ASharedMemory_getSize(int fd) {
  struct stat st;
  return fstat(fd, &st) == 0 ? (size_t)st.st_size : 0;
}

int ASharedMemory_setProt(int fd, int prot) {
  (void)fd;
  (void)prot;
  return 0;
}
