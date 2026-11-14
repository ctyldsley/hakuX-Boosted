// hakuX-Boosted bench: force-included into every core source. Declares the
// two bionic calls the core's Android paths use that glibc does not have.
#ifndef BOOSTED_BENCH_COMPAT_H_
#define BOOSTED_BENCH_COMPAT_H_
#include <pthread.h>
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif
pid_t pthread_gettid_np(pthread_t thread);
#ifdef __cplusplus
}
#endif
#endif
