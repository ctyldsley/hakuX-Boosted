/* hakuX-Boosted: leaf functions compiled with 64-bit instructions on the
 * 32-bit ABI (-mpowerpc64), the mix the console's own compiler emits.
 * Leaves only: with this ABI a call preserves the low half of a register.
 * Arguments and results are 32-bit so that they pass in one register each;
 * the 64-bit work happens inside. See kernels.c for the rules.
 */
#include <stdint.h>

#define NOINLINE __attribute__((noinline))

NOINLINE uint32_t k64_mul(uint32_t a, uint32_t b, uint32_t n) {
  uint64_t x = ((uint64_t)a << 32) | b;
  uint64_t y = 0x9E3779B97F4A7C15ull;
  for (uint32_t i = 0; i < n; ++i) {
    x = x * y + i;
    y ^= x >> 31;
    x = (x << 13) | (x >> 51);
  }
  x ^= y;
  return (uint32_t)(x >> 32) ^ (uint32_t)x;
}

NOINLINE uint32_t k64_div(uint32_t a, uint32_t b, uint32_t n) {
  uint64_t x = ((uint64_t)a << 32) | 0x12345678u;
  uint64_t acc = 0;
  for (uint32_t i = 0; i < n; ++i) {
    uint64_t d = (uint64_t)(b | 1u) + i * 7919u;
    acc += x / d;
    acc ^= (x % d) << 17;
    x = x * 3u + acc;
  }
  return (uint32_t)(acc >> 32) + (uint32_t)acc;
}

NOINLINE uint32_t k64_shift(uint32_t a, uint32_t b, uint32_t n) {
  int64_t x = (int64_t)(((uint64_t)a << 32) | b);
  uint64_t acc = 0;
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t s = (i * 5u + 1u) & 63u;
    acc += (uint64_t)(x >> s);
    acc ^= (uint64_t)x << s;
    acc += __builtin_clzll(acc | 1u);
    x = (int64_t)((uint64_t)x * 6364136223846793005ull + 1442695040888963407ull);
  }
  return (uint32_t)(acc >> 32) ^ (uint32_t)acc;
}

NOINLINE uint32_t k64_compare(uint32_t a, uint32_t b, uint32_t n) {
  int64_t x = (int64_t)(((uint64_t)a << 32) | b);
  uint32_t lo = 0, hi = 0;
  for (uint32_t i = 0; i < n; ++i) {
    x = (int64_t)((uint64_t)x * 2862933555777941757ull + 3037000493ull);
    int64_t y = (int64_t)((uint64_t)i << 40) - (int64_t)1099511627776ll;
    if (x < y) ++lo;
    if ((uint64_t)x > (uint64_t)y) hi += 3u;
    if ((x >> 20) == (y >> 20)) lo += 100u;
  }
  return lo * 65536u + hi;
}
