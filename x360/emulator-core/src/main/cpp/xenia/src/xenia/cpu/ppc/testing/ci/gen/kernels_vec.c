/* hakuX-Boosted: vector loops written with AltiVec intrinsics. No expected
 * values come with these (the host has no AltiVec): each run's result is
 * compared with the run that has every translator setting off. See
 * kernels.c for the rules.
 */
#include <altivec.h>
#include <stdint.h>

#define NOINLINE __attribute__((noinline))

/* the vector form of the mixer: out += in * volume, clamped */
NOINLINE uint32_t kv_mixer(uint32_t n, uint32_t seed) {
  float in[64] __attribute__((aligned(16)));
  float out[64] __attribute__((aligned(16)));
  for (uint32_t i = 0; i < 64u; ++i) {
    in[i] = (float)((int32_t)((seed + i * 37u) % 64u) - 32) * 0.03125f;
    out[i] = (float)(i & 7u) * 0.25f;
  }
  vector float volume = {0.5f, 0.75f, 1.0f, 1.25f};
  const vector float step = {0.125f, 0.125f, 0.125f, 0.125f};
  const vector float hi = {1.0f, 1.0f, 1.0f, 1.0f};
  const vector float lo = {-1.0f, -1.0f, -1.0f, -1.0f};
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t k = (i & 15u) * 4u;
    vector float a = vec_ld(0, &in[k]);
    vector float o = vec_ld(0, &out[k]);
    o = vec_madd(a, volume, o);
    o = vec_min(vec_max(o, lo), hi);
    vec_st(o, 0, &out[k]);
    volume = vec_add(volume, step);
  }
  uint32_t sum = 0;
  for (uint32_t i = 0; i < 64u; ++i) {
    sum = sum * 31u + (uint32_t)(int32_t)(out[i] * 4096.0f);
  }
  return sum;
}

/* integer vectors: add, compare, select, shift, with a call in the loop */
NOINLINE vector unsigned int kv_leaf(vector unsigned int a,
                                     vector unsigned int b) {
  return vec_xor(vec_add(a, b), vec_sl(a, vec_splat_u32(3)));
}

NOINLINE uint32_t kv_int(uint32_t n, uint32_t seed) {
  uint32_t buf[4] __attribute__((aligned(16)));
  buf[0] = seed;
  buf[1] = seed * 3u + 1u;
  buf[2] = ~seed;
  buf[3] = seed >> 5;
  vector unsigned int acc = vec_ld(0, buf);
  vector unsigned int inc = vec_splat_u32(7);
  vector unsigned int lim = vec_sl(vec_splat_u32(1), vec_splat_u32(15));
  lim = vec_sl(lim, vec_splat_u32(14));
  for (uint32_t i = 0; i < n; ++i) {
    vector bool int big = vec_cmpgt(acc, lim);
    vector unsigned int alt = vec_sr(acc, vec_splat_u32(2));
    acc = vec_sel(vec_add(acc, inc), alt, big);
    if (i & 1u) {
      acc = kv_leaf(acc, inc);
    }
    inc = vec_add(inc, vec_splat_u32(1));
  }
  vec_st(acc, 0, buf);
  return buf[0] ^ (buf[1] * 3u) ^ (buf[2] * 5u) ^ (buf[3] * 7u);
}
