// hakuX-Boosted bench: starts the core benchmarks. Built twice.
//
// bench_core loads libxecore.so with dlopen, as Android loads libe.so, so the
// library's thread-locals go through the loader's dynamic TLS resolver.
// bench_core_static links the library in the ordinary way, which gives it
// static TLS: a load Android never gives the app, kept to show how much of a
// path's cost is thread-local access.
//
// This executable's only thread-local is the word below, which puts it first
// in the static TLS block: on AArch64 glibc that is the thread pointer plus
// 16, where bionic keeps TLS_SLOT_APP. host_thread_state_slot uses it.
#include <dlfcn.h>
#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <unistd.h>

__thread void* bench_slot_app;

// The allocator abort under qemu-user with a page size that is not the
// host's (qemu-aarch64 -p 16384 on a 4 KiB machine): the same measure as in
// the translator tests' build, cpu/ppc/testing/ci/stubs.cc, where the
// comment has the whole story (and boosted/notes/translator-overhaul.md,
// section 0). qemu starts the program with a break that is not page
// aligned; glibc's allocator, once its first brk() has failed, trusts a
// later successful brk() to be page aligned and aborts ("sysmalloc:
// assertion failed") when it is not. So, as early as an executable can run
// anything: align the break and block it with an inaccessible page. Nothing
// is done on a real kernel, where the break is aligned. (It has to be here
// and not in the library: the library's constructors run after dlopen has
// allocated a good deal.)
#include <stdint.h>
#include <sys/mman.h>
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
__attribute__((constructor(101))) static void align_program_break(void) {
  const uintptr_t page = (uintptr_t)sysconf(_SC_PAGESIZE);
  const uintptr_t brk_now = (uintptr_t)sbrk(0);
  if (!page || brk_now == (uintptr_t)-1 || !(brk_now & (page - 1))) {
    return;
  }
  const uintptr_t aligned = (brk_now + page - 1) & ~(page - 1);
  if (sbrk((intptr_t)(aligned - brk_now)) == (void*)-1) {
    return;
  }
  mmap((void*)aligned, page, PROT_NONE,
       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
}

#ifdef BENCH_STATIC
int bench_core_main(int argc, char** argv, void* slot_address);
int main(int argc, char** argv) {
  return bench_core_main(argc, argv, &bench_slot_app);
}
#else
int main(int argc, char** argv) {
  // The library sits beside this executable.
  char self[PATH_MAX];
  ssize_t length = readlink("/proc/self/exe", self, sizeof(self) - 1);
  if (length <= 0) {
    fprintf(stderr, "bench_core: cannot find its own path\n");
    return 1;
  }
  self[length] = 0;
  char path[PATH_MAX + 32];
  snprintf(path, sizeof(path), "%s/libxecore.so", dirname(self));
  void* handle = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
  if (!handle) {
    fprintf(stderr, "bench_core: %s\n", dlerror());
    return 1;
  }
  int (*entry)(int, char**, void*) =
      (int (*)(int, char**, void*))dlsym(handle, "bench_core_main");
  if (!entry) {
    fprintf(stderr, "bench_core: %s\n", dlerror());
    return 1;
  }
  return entry(argc, argv, &bench_slot_app);
}
#endif
