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

#ifdef __cplusplus
// std::atomic_ref came to libc++ in version 19; the runner's is 18, the
// NDK's is newer. One line of the core uses it (cpu/xex_module.h, a
// fetch_or on a word of guest memory). This is that much of it and no more,
// on the compiler's atomic builtins, which is what the real one is made of.
#include <atomic>
#if defined(_LIBCPP_VERSION) && !defined(__cpp_lib_atomic_ref)
namespace std {
template <typename T>
class atomic_ref {
 public:
  explicit atomic_ref(T& object) : pointer_(&object) {}
  T load(memory_order = memory_order_seq_cst) const {
    return __atomic_load_n(pointer_, __ATOMIC_SEQ_CST);
  }
  void store(T value, memory_order = memory_order_seq_cst) const {
    __atomic_store_n(pointer_, value, __ATOMIC_SEQ_CST);
  }
  T fetch_or(T value, memory_order = memory_order_seq_cst) const {
    return __atomic_fetch_or(pointer_, value, __ATOMIC_SEQ_CST);
  }
  T fetch_and(T value, memory_order = memory_order_seq_cst) const {
    return __atomic_fetch_and(pointer_, value, __ATOMIC_SEQ_CST);
  }
  T fetch_add(T value, memory_order = memory_order_seq_cst) const {
    return __atomic_fetch_add(pointer_, value, __ATOMIC_SEQ_CST);
  }

 private:
  T* pointer_;
};
}  // namespace std
#endif
#endif  // __cplusplus
#endif
