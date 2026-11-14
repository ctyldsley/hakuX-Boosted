// hakuX-Boosted bench: the two atomic operations the profile names
// (__aarch64_cas2_acq, a 16-bit compare-and-swap with acquire ordering, and
// __aarch64_ldadd8_relax, a relaxed 64-bit add), compiled three ways by
// prim/CMakeLists.txt: through the outline helpers (what an armv8-a build
// such as libe.so gets), as inline LSE instructions, and as an inline
// load-linked/store-conditional loop.
#include <atomic>
#include <cstdint>

#define CAT2(a, b) a##b
#define CAT(a, b) CAT2(a, b)
#define FN(name) CAT(PRIM_PREFIX, name)

extern "C" {

__attribute__((noinline)) bool FN(_cas16_acquire)(std::atomic<uint16_t>* value,
                                                  uint16_t expected,
                                                  uint16_t desired) {
  return value->compare_exchange_strong(expected, desired,
                                        std::memory_order_acquire,
                                        std::memory_order_relaxed);
}

__attribute__((noinline)) uint64_t FN(_add64_relaxed)(
    std::atomic<uint64_t>* value) {
  return value->fetch_add(1, std::memory_order_relaxed);
}

}  // extern "C"
