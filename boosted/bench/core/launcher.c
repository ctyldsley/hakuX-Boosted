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
