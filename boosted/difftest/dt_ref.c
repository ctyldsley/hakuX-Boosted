// hakuX-Boosted: the reference side of the differential tester.
//
// A bare-metal PowerPC program, run by qemu-system-ppc64 as the firmware of
// its pseries machine with a PowerPC 970 (the closest CPU model to the
// console's: 64-bit, big-endian, AltiVec, no later extensions). It generates
// the same cases as the runner (dt_gen.h), runs each on the emulated CPU in
// 32-bit mode with 64-bit registers, which is how the console runs titles
// (carry, CR0 and CTR tests take the low 32 bits; QEMU's user mode cannot do
// that, which is why this is not a Linux program), and writes what each
// case changed to the console as a stream of binary records.
//
// Parameters arrive in memory at DT_PARAMS (QEMU's -device loader).
#define DT_PSEUDO_FLOAT
#include "dt_gen.h"
#include "dt_pseudo.h"

#define TRAPVEC (*(volatile uint64_t*)0x2140)
#define VSCRTMP ((volatile uint32_t*)0x2180)
#define ST ((volatile uint64_t*)0x2200)
#define DT_PARAMS ((volatile uint64_t*)0x00800000)
#define CODE_BASE 0x01000000u
#define CODE_SIZE 0x00100000u

uint64_t dt_hcall(uint64_t token, uint64_t a, uint64_t b, uint64_t c, uint64_t d);
void dt_run_case(void* entry);

void* memcpy(void* d, const void* s, unsigned long n) {
  char* dp = d;
  const char* sp = s;
  while (n--) *dp++ = *sp++;
  return d;
}
void* memset(void* d, int v, unsigned long n) {
  char* dp = d;
  while (n--) *dp++ = (char)v;
  return d;
}

// Output: the hypervisor's console call takes 16 bytes at a time.
static uint8_t out_buf[16];
static uint32_t out_len;
static void out_flush(void) {
  if (!out_len) return;
  uint64_t hi = 0, lo = 0;
  for (int i = 0; i < 8; ++i) {
    hi = (hi << 8) | out_buf[i];
    lo = (lo << 8) | out_buf[8 + i];
  }
  dt_hcall(0x58, 0, out_len, hi, lo);  // H_PUT_TERM_CHAR
  out_len = 0;
}
static void out8(uint8_t v) {
  out_buf[out_len++] = v;
  if (out_len == 16) out_flush();
}
static void out16(uint32_t v) { out8((uint8_t)(v >> 8)); out8((uint8_t)v); }
static void out32(uint32_t v) { out16(v >> 16); out16(v & 0xFFFF); }
static void out64(uint64_t v) { out32((uint32_t)(v >> 32)); out32((uint32_t)v); }

static DtCase c;
static uint64_t res[DT_NW];

void dt_main(void) {
  volatile uint64_t* p = DT_PARAMS;
  uint64_t seed = p[1], start = p[2], count = p[3], tier = p[4];
  out32(0x44545231);  // "DTR1"
  out32(DT_VERSION);
  out32(p[0] == 0x4454504152414D53ull ? 1 : 0);  // "DTPARAMS" seen
  out32((uint32_t)DT_NINSN);
  uint32_t code_at = 0;
  for (uint64_t i = start; i < start + count; ++i) {
    dt_gen_case(seed, i, (uint32_t)tier, &c);
    uint32_t trap = 0;
    if (c.flags & CF_PSEUDO) {
      for (uint32_t k = 0; k < DT_NW; ++k) res[k] = c.init[k];
      dt_pseudo(&c, res);
    } else {
      uint64_t* mem = (uint64_t*)(uint64_t)DT_MEM_BASE;
      for (uint32_t k = 0; k < DT_MEM_BYTES / 8; ++k) mem[k] = c.init[W_MEM + k];
      for (uint32_t k = 0; k < W_MEM; ++k) ST[k] = c.init[k];
      VSCRTMP[0] = VSCRTMP[1] = VSCRTMP[2] = 0;
      VSCRTMP[3] = (uint32_t)c.init[W_VSCR];
      if (code_at + (c.nr + 1) * 4 > CODE_SIZE) code_at = 0;
      uint32_t* code = (uint32_t*)(uint64_t)(CODE_BASE + code_at);
      for (uint32_t k = 0; k < c.nr; ++k) code[k] = c.coder[k];
      code[c.nr] = 0x4E800020;  // blr
      code_at += (c.nr + 1) * 4;
      // No reservation is carried from one case to the next.
      {
        static uint32_t scratch;
        uint32_t tmp = 0;
        __asm__ volatile("stwcx. %0, 0, %1" : : "r"(tmp), "r"(&scratch) : "cr0", "memory");
      }
      TRAPVEC = 0;
      dt_run_case(code);
      trap = (uint32_t)TRAPVEC;
      for (uint32_t k = 0; k < W_MEM; ++k) res[k] = ST[k];
      res[W_CR] &= 0xFFFFFFFFull;
      res[W_XER] &= DT_XER_MASK;
      res[W_FPSCR] &= 0xFFFFFFFFull;
      res[W_VSCR] = VSCRTMP[3];
      res[37] = res[38] = res[39] = 0;
      for (uint32_t k = 0; k < DT_MEM_BYTES / 8; ++k) res[W_MEM + k] = mem[k];
    }
    uint32_t n = 0;
    for (uint32_t k = 0; k < DT_NW; ++k) n += res[k] != c.init[k];
    out16(trap);
    out16(n);
    for (uint32_t k = 0; k < DT_NW; ++k) {
      if (res[k] != c.init[k]) {
        out16(k);
        out64(res[k]);
      }
    }
  }
  out32(0x44544E44);  // "DTND"
  out_flush();
  // Power off through RTAS (QEMU's token for "power-off").
  static uint32_t rtas[8];
  rtas[0] = 0x2003;
  rtas[1] = 2;
  rtas[2] = 1;
  dt_hcall(0xF000, (uint64_t)rtas, 0, 0, 0);
  for (;;) {
  }
}
