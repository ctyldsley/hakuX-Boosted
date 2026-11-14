// hakuX-Boosted bench: stands in for the NDK header on Linux. The core asks
// Android for anonymous shared memory for the guest address space; memfd is
// the same kind of object (a file descriptor that can be mapped many times).
#ifndef BOOSTED_BENCH_COMPAT_ANDROID_SHAREDMEM_H_
#define BOOSTED_BENCH_COMPAT_ANDROID_SHAREDMEM_H_
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
int ASharedMemory_create(const char* name, size_t size);
size_t ASharedMemory_getSize(int fd);
int ASharedMemory_setProt(int fd, int prot);
#ifdef __cplusplus
}
#endif
#endif
