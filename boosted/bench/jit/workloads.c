/* hakuX-Boosted jit bench: CPU workloads in C, written for this rig (no
 * title's code). Compiled twice by build-workloads.py: for PowerPC (-mcpu=cell,
 * the code the translator is measured on) and for the host, where the same
 * functions give the expected results. So every run of the benchmark also
 * checks the translator against the host's C compiler.
 *
 * Rules that keep the two builds bit-identical: fixed-width integers only,
 * no pointer values in results, no libm, no float operation whose result
 * IEEE 754 leaves to the implementation (no NaN, no out-of-range conversion,
 * no contraction: -ffp-contract=off, fused multiply-adds are written out).
 *
 * Every workload is uint32_t wl_<name>(uint32_t n, uint32_t seed): n scales
 * the work, the result is a checksum of everything it computed.
 */
#include <stdint.h>
#include <stddef.h>

#define NOINLINE __attribute__((noinline))
#define ALIGN16 __attribute__((aligned(16)))

static inline uint32_t xs32(uint32_t *s) {
  uint32_t x = *s;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  return *s = x;
}

/* Data is big-endian in memory in both builds, so a checksum over bytes that
 * were written as words is the same on a little-endian host. */
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define BE32(x) __builtin_bswap32(x)
#define BE16(x) __builtin_bswap16(x)
#else
#define BE32(x) (x)
#define BE16(x) (x)
#endif

static inline uint32_t fbits(float f) {
  union { float f; uint32_t u; } v;
  v.f = f;
  return v.u;
}

static inline uint64_t dbits(double d) {
  union { double d; uint64_t u; } v;
  v.d = d;
  return v.u;
}

/* ---- integer: table CRC, hashing, shifts and rotates ------------------- */
static uint32_t crc_table[256];
static uint8_t crc_buf[4096];

NOINLINE uint32_t wl_integer(uint32_t n, uint32_t seed) {
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    crc_table[i] = c;
  }
  uint32_t s = seed | 1;
  for (uint32_t i = 0; i < sizeof crc_buf; ++i) crc_buf[i] = (uint8_t)xs32(&s);
  uint32_t crc = ~0u, h = 0x811C9DC5u, acc = 0;
  for (uint32_t pass = 0; pass < n; ++pass) {
    for (uint32_t i = 0; i < sizeof crc_buf; ++i) {
      uint8_t b = crc_buf[i];
      crc = crc_table[(crc ^ b) & 0xFF] ^ (crc >> 8);
      h = (h ^ b) * 0x01000193u;
      acc += (h << (b & 15)) | (h >> (32 - (b & 15) - 1) >> 1);
      acc ^= (uint32_t)((int32_t)crc >> 3) + (acc >> 7);
    }
    crc_buf[pass & 4095] ^= (uint8_t)crc;
  }
  return ~crc ^ h ^ acc;
}

/* ---- 64-bit integer: multiply high, divide, count leading zeros -------- */
NOINLINE uint32_t wl_int64(uint32_t n, uint32_t seed) {
  uint64_t a = 0x9E3779B97F4A7C15ull ^ seed, b = 0xD1B54A32D192ED03ull, sum = 0;
  for (uint32_t i = 0; i < n * 256u; ++i) {
    a ^= a >> 12;
    a ^= a << 25;
    a ^= a >> 27;
    uint64_t x = a * 0x2545F4914F6CDD1Dull;
    uint32_t xl = (uint32_t)x, xh = (uint32_t)(x >> 32);
    uint64_t wide = (uint64_t)xl * xh;
    b = (b << 7 | b >> 57) + wide;
    sum += x / ((b & 0xFFFF) + 3);
    sum ^= (uint64_t)(int64_t)((int32_t)xh >> (xl & 31));
    sum += (uint32_t)__builtin_clzll(b | 1) + (uint32_t)__builtin_clz(xl | 1);
    if ((int64_t)(a ^ b) < 0) sum -= b >> 3;
  }
  return (uint32_t)sum ^ (uint32_t)(sum >> 32) ^ (uint32_t)b;
}

/* ---- branchy: heapsort, binary search, a comparison-heavy classifier --- */
static uint32_t sort_buf[2048];

static void sift(uint32_t *a, uint32_t start, uint32_t end) {
  uint32_t root = start;
  while (root * 2 + 1 <= end) {
    uint32_t child = root * 2 + 1, sw = root;
    if (a[sw] < a[child]) sw = child;
    if (child + 1 <= end && a[sw] < a[child + 1]) sw = child + 1;
    if (sw == root) return;
    uint32_t t = a[root];
    a[root] = a[sw];
    a[sw] = t;
    root = sw;
  }
}

NOINLINE uint32_t wl_branchy(uint32_t n, uint32_t seed) {
  uint32_t s = seed | 1, sum = 0;
  const uint32_t count = sizeof sort_buf / sizeof sort_buf[0];
  for (uint32_t pass = 0; pass < n; ++pass) {
    for (uint32_t i = 0; i < count; ++i) sort_buf[i] = xs32(&s) >> 8;
    for (int32_t st = (int32_t)(count - 2) / 2; st >= 0; --st) sift(sort_buf, (uint32_t)st, count - 1);
    for (uint32_t end = count - 1; end > 0; --end) {
      uint32_t t = sort_buf[end];
      sort_buf[end] = sort_buf[0];
      sort_buf[0] = t;
      sift(sort_buf, 0, end - 1);
    }
    for (uint32_t q = 0; q < 512; ++q) {
      uint32_t key = xs32(&s) >> 8, lo = 0, hi = count;
      while (lo < hi) {
        uint32_t mid = (lo + hi) >> 1;
        if (sort_buf[mid] < key) lo = mid + 1; else hi = mid;
      }
      sum += lo;
      if (lo < count && sort_buf[lo] == key) sum ^= 0x55555555u;
      else if (key & 1) sum += key >> 3;
      else if ((int32_t)(key << 8) < 0) sum -= lo;
      else sum = sum << 1 | sum >> 31;
    }
    sum ^= sort_buf[pass % count];
  }
  return sum;
}

/* ---- interpreter: a switch in a loop (jump table through CTR) ---------- */
NOINLINE uint32_t wl_interp(uint32_t n, uint32_t seed) {
  static uint8_t code[512];
  uint32_t s = seed | 1;
  for (uint32_t i = 0; i < sizeof code; ++i) code[i] = (uint8_t)(xs32(&s) % 12);
  uint32_t reg[8] = {1, 2, 3, 5, 8, 13, 21, seed};
  uint32_t pc = 0;
  for (uint32_t steps = n * 4096u; steps; --steps) {
    uint8_t op = code[pc];
    uint32_t a = pc & 7, b = (pc >> 3) & 7;
    pc = (pc + 1) & 511;
    switch (op) {
      case 0: reg[a] += reg[b]; break;
      case 1: reg[a] -= reg[b]; break;
      case 2: reg[a] ^= reg[b] << 3; break;
      case 3: reg[a] = reg[a] * 5 + 1; break;
      case 4: if (reg[a] & 1) pc = (pc + 7) & 511; break;
      case 5: reg[a] = reg[b] >> (a + 1); break;
      case 6: reg[a] |= reg[b] & 0x0F0F0F0Fu; break;
      case 7: if ((int32_t)reg[a] < (int32_t)reg[b]) reg[a] = reg[b]; break;
      case 8: reg[a] = ~reg[a]; break;
      case 9: pc = (pc + (reg[a] & 3)) & 511; break;
      case 10: reg[b] = reg[a] + pc; break;
      default: reg[a] = (reg[a] << 5 | reg[a] >> 27) ^ steps; break;
    }
  }
  uint32_t sum = pc;
  for (int i = 0; i < 8; ++i) sum = sum * 31 + reg[i];
  return sum;
}

/* ---- memory: copies, fills, compares, at mixed alignments -------------- */
static uint8_t mem_a[8192 + 64], mem_b[8192 + 64];

static NOINLINE void copy_bytes(uint8_t *d, const uint8_t *s, uint32_t len) {
  while (len && ((uintptr_t)d & 3)) { *d++ = *s++; --len; }
  uint32_t words = len >> 2;
  for (uint32_t i = 0; i < words; ++i) {
    uint32_t w;
    __builtin_memcpy(&w, s + i * 4, 4);
    __builtin_memcpy(d + i * 4, &w, 4);
  }
  d += words * 4;
  s += words * 4;
  for (len &= 3; len; --len) *d++ = *s++;
}

static NOINLINE void fill_words(uint32_t *d, uint32_t value, uint32_t count) {
  for (uint32_t i = 0; i < count; ++i) d[i] = BE32(value + i);
}

static NOINLINE uint32_t sum_halves(const uint16_t *p, uint32_t count) {
  uint32_t sum = 0;
  for (uint32_t i = 0; i < count; ++i) sum = sum * 3 + BE16(p[i]);
  return sum;
}

NOINLINE uint32_t wl_memory(uint32_t n, uint32_t seed) {
  uint32_t s = seed | 1, sum = 0;
  for (uint32_t i = 0; i < sizeof mem_a; ++i) mem_a[i] = (uint8_t)xs32(&s);
  for (uint32_t pass = 0; pass < n; ++pass) {
    uint32_t off_d = xs32(&s) & 31, off_s = xs32(&s) & 31, len = 4096 + (xs32(&s) & 4095);
    copy_bytes(mem_b + off_d, mem_a + off_s, len);
    fill_words((uint32_t *)(void *)(mem_a + 4096), sum, 64);
    sum += sum_halves((const uint16_t *)(const void *)(mem_b + (off_d & ~1u)), 256);
    for (uint32_t i = 0; i < len; i += 61) {
      if (mem_b[off_d + i] != mem_a[off_s + i] && i < 4000) sum ^= 0xBAD00000u | i;
      sum += mem_b[off_d + i];
    }
    mem_a[pass & 4095] += (uint8_t)sum;
  }
  return sum;
}

/* ---- strings: byte loads, compares, a tokeniser ------------------------ */
static char text[4096];

static NOINLINE uint32_t str_len(const char *p) {
  const char *q = p;
  while (*q) ++q;
  return (uint32_t)(q - p);
}

static NOINLINE int32_t str_cmp(const char *a, const char *b) {
  while (*a && *a == *b) { ++a; ++b; }
  return (int32_t)(uint8_t)*a - (int32_t)(uint8_t)*b;
}

NOINLINE uint32_t wl_strings(uint32_t n, uint32_t seed) {
  uint32_t s = seed | 1, sum = 0;
  for (uint32_t i = 0; i < sizeof text - 1; ++i) {
    uint32_t r = xs32(&s) % 29;
    text[i] = r < 26 ? (char)('a' + r) : r == 26 ? ' ' : r == 27 ? ',' : '\0';
  }
  text[sizeof text - 1] = '\0';
  for (uint32_t pass = 0; pass < n; ++pass) {
    uint32_t pos = 0, words = 0, hash = 5381;
    const char *prev = text;
    while (pos < sizeof text - 1) {
      uint32_t len = str_len(text + pos);
      for (uint32_t i = 0; i < len; ++i) {
        char c = text[pos + i];
        if (c == ' ' || c == ',') { ++words; hash ^= hash >> 5; }
        else hash = hash * 33 + (uint8_t)(c >= 'a' && c <= 'm' ? c - 32 : c);
      }
      if (str_cmp(prev, text + pos) < 0) sum += len;
      else sum ^= hash;
      prev = text + pos;
      pos += len + 1;
    }
    sum += words * 7 + hash;
    text[(pass * 37) % (sizeof text - 1)] ^= 1;
  }
  return sum;
}

/* ---- calls: recursion, small callees in loops -------------------------- */
static NOINLINE uint32_t fib(uint32_t k) { return k < 2 ? k : fib(k - 1) + fib(k - 2); }
static NOINLINE uint32_t leaf_mix(uint32_t a, uint32_t b) { return (a ^ b) * 0x9E3779B1u + (a >> 5); }
static NOINLINE uint32_t mid_call(uint32_t a, uint32_t b, uint32_t c) {
  uint32_t t[4] = {a, b, c, a + b};
  return leaf_mix(t[0], t[1]) + leaf_mix(t[2], t[3]);
}

NOINLINE uint32_t wl_calls(uint32_t n, uint32_t seed) {
  uint32_t sum = seed;
  for (uint32_t pass = 0; pass < n; ++pass) {
    sum += fib(12 + (pass & 3));
    for (uint32_t i = 0; i < 256; ++i) sum = mid_call(sum, i, pass);
  }
  return sum;
}

/* ---- virtual: function pointers in tables, pointer chasing ------------- */
struct node;
typedef uint32_t (*visit_fn)(struct node *, uint32_t);
struct node {
  uint32_t left, right; /* indices, so the host build has the same layout */
  uint32_t key, kind;
};
static struct node pool[1024];
static uint32_t v_add(struct node *nd, uint32_t acc) { return acc + nd->key; }
static uint32_t v_xor(struct node *nd, uint32_t acc) { return acc ^ (nd->key << 3); }
static uint32_t v_rot(struct node *nd, uint32_t acc) { return (acc << 7 | acc >> 25) + nd->kind; }
static uint32_t v_mul(struct node *nd, uint32_t acc) { return acc * 31 + (nd->key & 0xFF); }
static const visit_fn vtable[4] = {v_add, v_xor, v_rot, v_mul};

static NOINLINE uint32_t walk(uint32_t index, uint32_t acc, uint32_t depth) {
  while (index != 0xFFFFFFFFu) {
    struct node *nd = &pool[index];
    acc = vtable[nd->kind](nd, acc);
    if (nd->left != 0xFFFFFFFFu && depth < 48) acc = walk(nd->left, acc, depth + 1);
    index = nd->right;
  }
  return acc;
}

NOINLINE uint32_t wl_virtual(uint32_t n, uint32_t seed) {
  uint32_t s = seed | 1, sum = 0;
  const uint32_t count = sizeof pool / sizeof pool[0];
  for (uint32_t pass = 0; pass < n; ++pass) {
    for (uint32_t i = 0; i < count; ++i) {
      pool[i].left = pool[i].right = 0xFFFFFFFFu;
      pool[i].key = xs32(&s);
      pool[i].kind = pool[i].key >> 30;
    }
    for (uint32_t i = 1; i < count; ++i) { /* binary search tree insert */
      uint32_t at = 0;
      for (;;) {
        uint32_t *next = pool[i].key < pool[at].key ? &pool[at].left : &pool[at].right;
        if (*next == 0xFFFFFFFFu) { *next = i; break; }
        at = *next;
      }
    }
    sum = walk(0, sum, 0);
  }
  return sum;
}

/* ---- float: double precision, an n-body step with a Newton 1/sqrt ------ */
static NOINLINE double rsqrt_newton(double x) {
  double y = 1.0;
  if (x > 4.0) y = 0.25;
  if (x > 64.0) y = 0.0625;
  if (x > 1024.0) y = 0.015625;
  for (int i = 0; i < 6; ++i) y = y * (1.5 - 0.5 * x * y * y);
  return y;
}

NOINLINE uint32_t wl_float(uint32_t n, uint32_t seed) {
  enum { BODIES = 8 };
  double px[BODIES], py[BODIES], pz[BODIES], vx[BODIES], vy[BODIES], vz[BODIES], m[BODIES];
  uint32_t s = seed | 1;
  for (int i = 0; i < BODIES; ++i) {
    px[i] = (double)(int32_t)(xs32(&s) & 1023) * 0.015625 - 8.0;
    py[i] = (double)(int32_t)(xs32(&s) & 1023) * 0.015625 - 8.0;
    pz[i] = (double)(int32_t)(xs32(&s) & 1023) * 0.015625 - 8.0;
    vx[i] = vy[i] = vz[i] = 0.0;
    m[i] = 1.0 + (double)(i & 3) * 0.5;
  }
  const double dt = 0.001953125;
  for (uint32_t step = 0; step < n * 16u; ++step) {
    for (int i = 0; i < BODIES; ++i) {
      double ax = 0.0, ay = 0.0, az = 0.0;
      for (int j = 0; j < BODIES; ++j) {
        if (i == j) continue;
        double dx = px[j] - px[i], dy = py[j] - py[i], dz = pz[j] - pz[i];
        double d2 = dx * dx + dy * dy + dz * dz + 0.5;
        double inv = rsqrt_newton(d2);
        double f = m[j] * inv * inv * inv;
        ax += dx * f;
        ay += dy * f;
        az += dz * f;
      }
      vx[i] += ax * dt;
      vy[i] += ay * dt;
      vz[i] += az * dt;
    }
    for (int i = 0; i < BODIES; ++i) {
      px[i] = __builtin_fma(vx[i], dt, px[i]);
      py[i] = __builtin_fma(vy[i], dt, py[i]);
      pz[i] = __builtin_fma(vz[i], dt, pz[i]);
      if (px[i] > 16.0 || px[i] < -16.0) vx[i] = -vx[i];
      if (__builtin_fabs(py[i]) > 16.0) vy[i] = -vy[i];
    }
  }
  uint64_t sum = 0;
  for (int i = 0; i < BODIES; ++i) sum = sum * 31 + (dbits(px[i]) ^ dbits(vy[i]) ^ dbits(pz[i]));
  return (uint32_t)sum ^ (uint32_t)(sum >> 32);
}

/* ---- single precision: matrices, transforms, clamps, conversions ------- */
static float verts[256][4];
static float outv[256][4];

static NOINLINE void mat_mul(float r[4][4], const float a[4][4], const float b[4][4]) {
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
      float t = 0.0f;
      for (int k = 0; k < 4; ++k) t += a[i][k] * b[k][j];
      r[i][j] = t;
    }
}

NOINLINE uint32_t wl_single(uint32_t n, uint32_t seed) {
  uint32_t s = seed | 1, sum = 0;
  for (int i = 0; i < 256; ++i)
    for (int k = 0; k < 4; ++k) verts[i][k] = k == 3 ? 1.0f : (float)(int32_t)(xs32(&s) & 255) * 0.0625f - 8.0f;
  float rot[4][4] = {{0.96875f, -0.25f, 0.0f, 0.0f}, {0.25f, 0.96875f, 0.0f, 0.0f},
                     {0.0f, 0.0f, 1.0f, 0.0f}, {0.125f, -0.0625f, 0.03125f, 1.0f}};
  float m[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
  for (uint32_t pass = 0; pass < n; ++pass) {
    float t[4][4];
    mat_mul(t, m, rot);
    for (int i = 0; i < 4; ++i)
      for (int j = 0; j < 4; ++j) {
        float v = t[i][j];
        m[i][j] = v > 4.0f ? 4.0f : v < -4.0f ? -4.0f : v;
      }
    for (int i = 0; i < 256; ++i) {
      const float *v = verts[i];
      float x = v[0] * m[0][0] + v[1] * m[1][0] + v[2] * m[2][0] + m[3][0];
      float y = v[0] * m[0][1] + v[1] * m[1][1] + v[2] * m[2][1] + m[3][1];
      float z = __builtin_fmaf(v[0], m[0][2], __builtin_fmaf(v[1], m[1][2], v[2] * m[2][2] + m[3][2]));
      float w = 1.0f / (2.0f + __builtin_fabsf(z) * 0.125f);
      outv[i][0] = x * w;
      outv[i][1] = y * w;
      outv[i][2] = z;
      outv[i][3] = w;
      int32_t sx = (int32_t)(x * w * 16.0f), sy = (int32_t)(y * w * 16.0f);
      if (sx >= -320 && sx < 320 && sy >= -180 && sy < 180) sum += (uint32_t)(sx * 3 + sy);
      else sum ^= (uint32_t)i;
      sum += (uint32_t)(double)(z * z < 100.0f ? z * z : 100.0f);
    }
    sum = sum * 31 + fbits(outv[pass & 255][0]) + fbits(outv[(pass * 7) & 255][3]);
  }
  return sum;
}

/* ---- particles: the mix a game's update loop has ------------------------ */
struct particle {
  float pos[3], vel[3];
  float life;
  uint32_t flags;
};
static struct particle parts[512];

static NOINLINE uint32_t spawn(struct particle *p, uint32_t *s) {
  for (int k = 0; k < 3; ++k) {
    p->pos[k] = (float)(int32_t)(xs32(s) & 127) * 0.125f - 8.0f;
    p->vel[k] = (float)(int32_t)(xs32(s) & 63) * 0.03125f - 1.0f;
  }
  p->life = 1.0f + (float)(xs32(s) & 15) * 0.25f;
  p->flags = xs32(s) & 7;
  return p->flags;
}

NOINLINE uint32_t wl_particles(uint32_t n, uint32_t seed) {
  uint32_t s = seed | 1, sum = 0, alive = 0;
  const uint32_t count = sizeof parts / sizeof parts[0];
  for (uint32_t i = 0; i < count; ++i) sum += spawn(&parts[i], &s);
  const float dt = 0.015625f, gravity = -0.25f;
  for (uint32_t frame = 0; frame < n; ++frame) {
    alive = 0;
    for (uint32_t i = 0; i < count; ++i) {
      struct particle *p = &parts[i];
      if (p->life <= 0.0f) {
        if ((i + frame) & 3) continue;
        sum += spawn(p, &s);
      }
      if (p->flags & 1) p->vel[1] += gravity * dt;
      if (p->flags & 2) { p->vel[0] *= 0.984375f; p->vel[2] *= 0.984375f; }
      for (int k = 0; k < 3; ++k) {
        p->pos[k] += p->vel[k] * dt;
        if (p->pos[k] < -8.0f) { p->pos[k] = -8.0f; p->vel[k] = -p->vel[k] * 0.5f; p->flags ^= 4; }
        else if (p->pos[k] > 8.0f) { p->pos[k] = 8.0f; p->vel[k] = -p->vel[k] * 0.5f; }
      }
      p->life -= dt * ((p->flags & 4) ? 2.0f : 1.0f);
      ++alive;
    }
    sum = sum * 33 + alive + fbits(parts[frame % count].pos[1]);
  }
  return sum ^ alive;
}

/* ---- vector: AltiVec on PowerPC, the same element by element on the host */
#ifdef __ALTIVEC__
#include <altivec.h>
typedef vector float vf;
typedef vector unsigned int vu;
#define VF_LOAD(p) vec_ld(0, (const float *)(p))
#define VF_STORE(v, p) vec_st((v), 0, (float *)(p))
#define VU_LOAD(p) vec_ld(0, (const unsigned int *)(p))
#define VU_STORE(v, p) vec_st((v), 0, (unsigned int *)(p))
#define VF_MADD(a, b, c) vec_madd((a), (b), (c))
#define VF_ADD(a, b) vec_add((a), (b))
#define VF_SUB(a, b) vec_sub((a), (b))
#define VF_MIN(a, b) vec_min((a), (b))
#define VF_MAX(a, b) vec_max((a), (b))
#define VU_ADD(a, b) vec_add((a), (b))
#define VU_XOR(a, b) vec_xor((a), (b))
#define VU_AND(a, b) vec_and((a), (b))
#define VU_SL(a, b) vec_sl((a), (b))
#define VU_SR(a, b) vec_sr((a), (b))
#define VU_CMPGT(a, b) ((vu)vec_cmpgt((a), (b)))
#define VU_SEL(a, b, m) vec_sel((a), (b), (m))
#define VF_CMPGT(a, b) ((vu)vec_cmpgt((a), (b)))
#define VF_SEL(a, b, m) vec_sel((a), (b), (m))
#define VU_MERGEH(a, b) vec_mergeh((a), (b))
#define VF_SPLAT(a, i) vec_splat((a), (i))
#else
typedef struct { float e[4]; } vf;
typedef struct { uint32_t e[4]; } vu;
#define EACH for (int i = 0; i < 4; ++i)
static inline vf VF_LOAD(const void *p) { vf r; __builtin_memcpy(&r, p, 16); return r; }
static inline void VF_STORE(vf v, void *p) { __builtin_memcpy(p, &v, 16); }
static inline vu VU_LOAD(const void *p) { vu r; __builtin_memcpy(&r, p, 16); return r; }
static inline void VU_STORE(vu v, void *p) { __builtin_memcpy(p, &v, 16); }
static inline vf VF_MADD(vf a, vf b, vf c) { vf r; EACH r.e[i] = __builtin_fmaf(a.e[i], b.e[i], c.e[i]); return r; }
static inline vf VF_ADD(vf a, vf b) { vf r; EACH r.e[i] = a.e[i] + b.e[i]; return r; }
static inline vf VF_SUB(vf a, vf b) { vf r; EACH r.e[i] = a.e[i] - b.e[i]; return r; }
static inline vf VF_MIN(vf a, vf b) { vf r; EACH r.e[i] = a.e[i] < b.e[i] ? a.e[i] : b.e[i]; return r; }
static inline vf VF_MAX(vf a, vf b) { vf r; EACH r.e[i] = a.e[i] > b.e[i] ? a.e[i] : b.e[i]; return r; }
static inline vu VU_ADD(vu a, vu b) { vu r; EACH r.e[i] = a.e[i] + b.e[i]; return r; }
static inline vu VU_XOR(vu a, vu b) { vu r; EACH r.e[i] = a.e[i] ^ b.e[i]; return r; }
static inline vu VU_AND(vu a, vu b) { vu r; EACH r.e[i] = a.e[i] & b.e[i]; return r; }
static inline vu VU_SL(vu a, vu b) { vu r; EACH r.e[i] = a.e[i] << (b.e[i] & 31); return r; }
static inline vu VU_SR(vu a, vu b) { vu r; EACH r.e[i] = a.e[i] >> (b.e[i] & 31); return r; }
static inline vu VU_CMPGT(vu a, vu b) { vu r; EACH r.e[i] = a.e[i] > b.e[i] ? ~0u : 0; return r; }
static inline vu VU_SEL(vu a, vu b, vu m) { vu r; EACH r.e[i] = (a.e[i] & ~m.e[i]) | (b.e[i] & m.e[i]); return r; }
static inline vu VF_CMPGT(vf a, vf b) { vu r; EACH r.e[i] = a.e[i] > b.e[i] ? ~0u : 0; return r; }
static inline vf VF_SEL(vf a, vf b, vu m) { vf r; EACH r.e[i] = m.e[i] ? b.e[i] : a.e[i]; return r; }
static inline vu VU_MERGEH(vu a, vu b) { vu r = {{a.e[0], b.e[0], a.e[1], b.e[1]}}; return r; }
static inline vf VF_SPLAT(vf a, int k) { vf r; EACH r.e[i] = a.e[k]; return r; }
#endif

static float vin[1024] ALIGN16, vout[1024] ALIGN16;
static uint32_t uin[1024] ALIGN16, uout[1024] ALIGN16;
static const float vconst[5][4] ALIGN16 = {{0.5f, 0.75f, 1.0f, 1.25f}, {0.0078125f, 0.0078125f, 0.0078125f, 0.0078125f},
                                           {4.0f, 4.0f, 4.0f, 4.0f}, {-4.0f, -4.0f, -4.0f, -4.0f},
                                           {0.25f, 0.25f, 0.25f, 0.25f}};
static const uint32_t uconst[3][4] ALIGN16 = {{1, 2, 3, 4}, {0x00FF00FFu, 0x00FF00FFu, 0x00FF00FFu, 0x00FF00FFu},
                                              {0x80000000u, 0x80000000u, 0x80000000u, 0x80000000u}};

NOINLINE uint32_t wl_vector(uint32_t n, uint32_t seed) {
  uint32_t s = seed | 1, sum = 0;
  for (uint32_t i = 0; i < 1024; ++i) {
    vin[i] = (float)(int32_t)(xs32(&s) & 255) * 0.03125f - 4.0f;
    vout[i] = (float)(i & 7) * 0.25f;
    uin[i] = xs32(&s);
    uout[i] = 0;
  }
  vf volume = VF_LOAD(vconst[0]);
  const vf step = VF_LOAD(vconst[1]), hi = VF_LOAD(vconst[2]), lo = VF_LOAD(vconst[3]), quarter = VF_LOAD(vconst[4]);
  const vu shifts = VU_LOAD(uconst[0]), mask = VU_LOAD(uconst[1]), top = VU_LOAD(uconst[2]);
  for (uint32_t pass = 0; pass < n; ++pass) {
    vu acc = VU_LOAD(uconst[1]);
    for (uint32_t i = 0; i < 1024; i += 4) {
      vf a = VF_LOAD(&vin[i]), o = VF_LOAD(&vout[i]);
      o = VF_MADD(a, volume, o);
      o = VF_MIN(VF_MAX(o, lo), hi);
      vf damp = VF_SUB(o, VF_MADD(o, quarter, VF_SPLAT(a, 0)));
      o = VF_SEL(o, damp, VF_CMPGT(o, quarter));
      VF_STORE(o, &vout[i]);
      vu u = VU_LOAD(&uin[i]);
      vu t = VU_XOR(VU_SL(u, shifts), VU_SR(u, shifts));
      t = VU_ADD(t, VU_AND(u, mask));
      t = VU_SEL(t, VU_XOR(t, top), VU_CMPGT(u, acc));
      acc = VU_ADD(acc, VU_MERGEH(t, u));
      VU_STORE(t, &uout[i]);
    }
    volume = VF_ADD(volume, step);
    uint32_t tmp[4] ALIGN16;
    VU_STORE(acc, tmp);
    sum = sum * 31 + tmp[0] + tmp[1] + tmp[2] + tmp[3];
    uin[pass & 1023] ^= sum;
  }
  for (uint32_t i = 0; i < 1024; i += 17) sum = sum * 33 + fbits(vout[i]) + uout[i];
  return sum;
}
