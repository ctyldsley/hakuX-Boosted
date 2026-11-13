/* hakuX-Boosted: small C functions compiled to PowerPC for the translator's
 * sequence tests (gen-seq-tests.py beside this file). They exist to give the
 * translator the shapes compiled code has and single instruction tests do
 * not: loops, calls inside loops, values live across calls and blocks,
 * branches on flags set in another block, recursion, indirect calls.
 *
 * Rules, so that the same source gives the same answer compiled for the host
 * (which is where the expected values come from): fixed-width unsigned
 * arithmetic only, no undefined shifts or divisions, no NaN, no conversion
 * out of range, no memory that is not written first, no type punning.
 */
#include <stdint.h>

#define NOINLINE __attribute__((noinline))

/* --- integer ----------------------------------------------------------- */

NOINLINE uint32_t k_leaf(uint32_t a, uint32_t b) { return a * 31u + b; }

/* a call in every trip; the accumulator and the counter live across it */
NOINLINE uint32_t k_call_loop(uint32_t n, uint32_t seed) {
  uint32_t acc = seed;
  for (uint32_t i = 0; i < n; ++i) {
    acc = k_leaf(acc, i) ^ (acc >> 3);
  }
  return acc;
}

/* a call on some trips only */
NOINLINE uint32_t k_cond_call_loop(uint32_t n, uint32_t seed) {
  uint32_t acc = seed;
  for (uint32_t i = 0; i < n; ++i) {
    if (acc & 4u) {
      acc = k_leaf(acc, i);
    } else {
      acc = (acc << 1) | (acc >> 31);
      acc += i;
    }
  }
  return acc;
}

NOINLINE uint32_t k_collatz(uint32_t x, uint32_t limit) {
  uint32_t steps = 0;
  while (x != 1u && steps < limit) {
    if (x & 1u) {
      x = x * 3u + 1u;
    } else {
      x >>= 1;
    }
    ++steps;
  }
  return steps * 65536u + (x & 0xFFFFu);
}

NOINLINE uint32_t k_gcd(uint32_t a, uint32_t b) {
  while (b != 0u) {
    uint32_t t = a % b;
    a = b;
    b = t;
  }
  return a;
}

NOINLINE uint32_t k_crc32(uint32_t n, uint32_t seed) {
  uint8_t buf[64];
  for (uint32_t i = 0; i < 64u; ++i) {
    buf[i] = (uint8_t)(seed * (i + 1u) + (i << 3));
  }
  uint32_t crc = 0xFFFFFFFFu;
  for (uint32_t i = 0; i < n; ++i) {
    crc ^= buf[i & 63u];
    for (uint32_t k = 0; k < 8u; ++k) {
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
  }
  return ~crc;
}

NOINLINE uint32_t k_sort(uint32_t n, uint32_t seed) {
  uint32_t a[32];
  uint32_t x = seed;
  if (n > 32u) n = 32u;
  for (uint32_t i = 0; i < n; ++i) {
    x = x * 1664525u + 1013904223u;
    a[i] = x >> 8;
  }
  for (uint32_t i = 1; i < n; ++i) {
    uint32_t v = a[i];
    uint32_t j = i;
    while (j > 0u && a[j - 1u] > v) {
      a[j] = a[j - 1u];
      --j;
    }
    a[j] = v;
  }
  uint32_t sum = 0;
  for (uint32_t i = 0; i < n; ++i) {
    sum = sum * 16777619u ^ a[i];
  }
  return sum;
}

NOINLINE uint32_t k_matrix(uint32_t n, uint32_t seed) {
  uint32_t m[8][8];
  uint32_t r[8];
  if (n > 8u) n = 8u;
  for (uint32_t i = 0; i < 8u; ++i) {
    for (uint32_t j = 0; j < 8u; ++j) {
      m[i][j] = seed + i * 7u + j * 13u + (i ^ j);
    }
  }
  for (uint32_t i = 0; i < 8u; ++i) {
    uint32_t acc = 0;
    for (uint32_t j = 0; j < n; ++j) {
      acc += m[i][j] * m[j][i];
    }
    r[i] = acc;
  }
  uint32_t out = 0;
  for (uint32_t i = 0; i < 8u; ++i) {
    out ^= r[i] + (out << 5);
  }
  return out;
}

NOINLINE uint32_t k_fib(uint32_t n) {
  if (n < 2u) return n;
  return k_fib(n - 1u) + k_fib(n - 2u);
}

NOINLINE uint32_t k_ack(uint32_t m, uint32_t n) {
  if (m == 0u) return n + 1u;
  if (n == 0u) return k_ack(m - 1u, 1u);
  return k_ack(m - 1u, k_ack(m, n - 1u));
}

NOINLINE uint32_t k_op_add(uint32_t a, uint32_t b) { return a + b; }
NOINLINE uint32_t k_op_xor(uint32_t a, uint32_t b) { return a ^ (b << 1); }
NOINLINE uint32_t k_op_mul(uint32_t a, uint32_t b) { return a * (b | 1u); }
NOINLINE uint32_t k_op_rot(uint32_t a, uint32_t b) {
  uint32_t s = b & 31u;
  return s ? (a << s) | (a >> (32u - s)) : a;
}

typedef uint32_t (*k_op)(uint32_t, uint32_t);

/* indirect calls through a table built on the stack */
NOINLINE uint32_t k_dispatch(uint32_t n, uint32_t seed) {
  k_op ops[4];
  ops[0] = k_op_add;
  ops[1] = k_op_xor;
  ops[2] = k_op_mul;
  ops[3] = k_op_rot;
  uint32_t acc = seed;
  for (uint32_t i = 0; i < n; ++i) {
    acc = ops[(acc >> 5) & 3u](acc, i + 1u);
  }
  return acc;
}

/* a dense switch in a loop: a jump table and bctr */
NOINLINE uint32_t k_switch(uint32_t n, uint32_t seed) {
  uint32_t acc = seed;
  for (uint32_t i = 0; i < n; ++i) {
    switch (acc & 7u) {
      case 0: acc += 0x9E3779B9u; break;
      case 1: acc ^= acc >> 7; break;
      case 2: acc = acc * 5u + 1u; break;
      case 3: acc -= i; break;
      case 4: acc = (acc << 3) | (acc >> 29); break;
      case 5: acc ^= 0xA5A5A5A5u; break;
      case 6: acc += acc >> 11; break;
      default: acc = ~acc + i; break;
    }
  }
  return acc;
}

/* 64-bit arithmetic on a 32-bit ABI: carries between the halves */
NOINLINE uint64_t k_mix64(uint64_t a, uint64_t b) {
  uint64_t x = a;
  for (uint32_t i = 0; i < 9u; ++i) {
    x = (x + b) * 0x9E3779B97F4A7C15ull;
    x ^= x >> 29;
    b = b - x + (b << 3);
  }
  return x ^ b;
}

/* leaves the loop from its middle */
NOINLINE uint32_t k_search(uint32_t needle, uint32_t seed) {
  uint32_t x = seed;
  for (uint32_t i = 0; i < 200u; ++i) {
    x = x * 1103515245u + 12345u;
    if (((x >> 16) & 0xFFu) == (needle & 0xFFu)) {
      return i * 3u + (x & 1u);
    }
    if ((x & 0x7FFu) == 0x7FFu) {
      return 0xFFFF0000u + i;
    }
  }
  return 0xFFFFFFFFu;
}

/* more values live at once than there are host registers to hold them */
NOINLINE uint32_t k_pressure(uint32_t n, uint32_t seed) {
  uint32_t a = seed, b = seed ^ 0x1111u, c = seed + 0x2222u, d = seed * 3u;
  uint32_t e = ~seed, f = seed >> 3, g = seed << 5, h = seed ^ 0xDEADBEEFu;
  uint32_t p = 1u, q = 2u, r = 3u, s = 5u;
  for (uint32_t i = 0; i < n; ++i) {
    a += b ^ i;
    b = (b << 1) + c;
    c ^= d + p;
    d += e >> 2;
    e -= f;
    f = f * 3u + g;
    g ^= h << 1;
    h += a;
    p += q ^ h;
    q = q * 7u + r;
    r ^= s + a;
    s += p >> 1;
  }
  return a ^ b ^ c ^ d ^ e ^ f ^ g ^ h ^ p ^ q ^ r ^ s;
}

/* flags of one compare used by several later branches */
NOINLINE uint32_t k_classify(uint32_t n, uint32_t seed) {
  uint32_t x = seed;
  uint32_t lo = 0, mid = 0, hi = 0, eq = 0;
  for (uint32_t i = 0; i < n; ++i) {
    x = x * 69069u + 1u;
    int32_t v = (int32_t)(x >> 4) - 0x8000000;
    int32_t w = (int32_t)(i * 1000003u) - 0x4000000;
    if (v < w) {
      ++lo;
      if (v < -0x4000000) lo += 2u;
    } else if (v == w) {
      ++eq;
    } else if ((uint32_t)v > (uint32_t)w + 0x1000u) {
      ++hi;
    } else {
      ++mid;
    }
  }
  return lo + mid * 257u + hi * 65537u + eq * 16777259u;
}

/* signed arithmetic the way compiled code does it: division, remainders,
   arithmetic shifts, sign extension */
NOINLINE uint32_t k_signed(uint32_t n, uint32_t seed) {
  int32_t acc = (int32_t)(seed & 0x7FFFFFu) + 17;
  uint32_t out = 0;
  for (uint32_t i = 1; i <= n; ++i) {
    int32_t d = (int32_t)(i & 15u) + 3;
    int32_t q = acc / d;
    int32_t r = acc % d;
    int16_t s16 = (int16_t)(acc + (int32_t)i);
    int8_t s8 = (int8_t)(acc >> 3);
    out = out * 33u + (uint32_t)(q - r + s16 * 3 + s8);
    acc = (acc >> 1) + (int32_t)(i * 977u) - q;
    if (acc < 0) acc = -acc;
    acc &= 0xFFFFFF;
    acc += 5;
  }
  return out;
}

/* --- float ------------------------------------------------------------- */

/* a mixer: samples scaled by a ramping volume and summed into a buffer */
NOINLINE float k_mixer(uint32_t n, float volume, float step) {
  float in[32];
  float out[32];
  for (uint32_t i = 0; i < 32u; ++i) {
    in[i] = (float)((int32_t)(i * 37u % 64u) - 32) * 0.03125f;
    out[i] = (float)(i & 7u) * 0.25f;
  }
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t k = i & 31u;
    float s = out[k] + in[k] * volume;
    if (s > 1.0f) s = 1.0f;
    if (s < -1.0f) s = -1.0f;
    out[k] = s;
    volume += step;
  }
  float sum = 0.0f;
  for (uint32_t i = 0; i < 32u; ++i) {
    sum += out[i] * (float)(i + 1u);
  }
  return sum;
}

/* fused multiply-add in a loop with a float accumulator */
NOINLINE float k_dot(uint32_t n, float scale) {
  float buf[16];
  for (uint32_t i = 0; i < 16u; ++i) {
    buf[i] = scale * (float)i - 1.5f;
  }
  float acc = 0.0f;
  for (uint32_t i = 0; i < n; ++i) {
    acc = __builtin_fmaf(buf[i & 15u], buf[(i + 3u) & 15u], acc);
  }
  return acc;
}

NOINLINE double k_horner(uint32_t n, double x) {
  double acc = 1.0;
  for (uint32_t i = 0; i < n; ++i) {
    acc = acc * x + (double)(i & 7u) * 0.125;
    if (acc > 1048576.0) acc = acc * 0.0009765625;
    if (acc < -1048576.0) acc = acc * 0.0009765625;
  }
  return acc;
}

NOINLINE double k_fleaf(double a, double b) { return a * 0.5 + b; }

/* float values live across calls */
NOINLINE double k_fcall_loop(uint32_t n, double seed) {
  double a = seed, b = 1.0, c = 0.25;
  for (uint32_t i = 0; i < n; ++i) {
    a = k_fleaf(a, b);
    b = b + c;
    c = c * 0.75 + 0.125;
    if (a > 100.0) a -= 100.0;
  }
  return a + b + c;
}

/* integer and float mixed, conversions both ways */
NOINLINE uint32_t k_convert(uint32_t n, float gain) {
  uint32_t acc = 0;
  float f = 0.5f;
  for (uint32_t i = 0; i < n; ++i) {
    f = f * gain + (float)(i & 31u);
    if (f > 30000.0f) f = f * 0.001f;
    int32_t v = (int32_t)f;
    acc = acc * 31u + (uint32_t)v;
    double d = (double)(int32_t)(acc & 0xFFFFu) * 0.0625;
    acc ^= (uint32_t)(int32_t)d;
  }
  return acc;
}

NOINLINE uint32_t k_fcompare(uint32_t n, double seed) {
  uint32_t lt = 0, gt = 0, eq = 0;
  double a = seed, b = 0.0;
  for (uint32_t i = 0; i < n; ++i) {
    a = a * 1.25 - 3.0;
    if (a > 64.0) a -= 80.0;
    b = (double)(int32_t)(i % 9u) - 4.0;
    if (a < b) {
      ++lt;
    } else if (a > b) {
      ++gt;
    } else {
      ++eq;
    }
    if (a >= b && b <= 0.0) gt += 16u;
  }
  return lt + gt * 1024u + eq * 1048576u;
}
