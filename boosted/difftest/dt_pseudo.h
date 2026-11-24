// hakuX-Boosted: reference results for the console's instructions that the
// reference CPU does not have and no plain VMX instruction equals. Written
// from the instruction descriptions (the load/store left/right family as on
// Cell) and, for the VMX128-only ones, from what the core's own front end
// documents in cpu/ppc/ppc_emit_altivec.cc. These check the ARM64 back end
// against that reading, not against a console.
//
// dt_pseudo() works on a case's state words in place. Compiled into the
// reference harness only (the float ones use the emulated CPU's arithmetic).
#ifndef DT_PSEUDO_H_
#define DT_PSEUDO_H_

#include "dt_gen.h"

static inline uint8_t dt_mem_get(const uint64_t* w, uint32_t ea) {
  uint32_t off = ea - DT_MEM_BASE;
  return (uint8_t)(w[W_MEM + off / 8] >> (56 - 8 * (off % 8)));
}
static inline void dt_mem_put(uint64_t* w, uint32_t ea, uint8_t v) {
  uint32_t off = ea - DT_MEM_BASE;
  uint32_t sh = 56 - 8 * (off % 8);
  w[W_MEM + off / 8] = (w[W_MEM + off / 8] & ~(0xFFull << sh)) | ((uint64_t)v << sh);
}
static inline void dt_vr_get(const uint64_t* w, uint32_t slot, uint8_t* b) {
  for (int i = 0; i < 8; ++i) {
    b[i] = (uint8_t)(w[W_VR + 2 * slot] >> (56 - 8 * i));
    b[8 + i] = (uint8_t)(w[W_VR + 2 * slot + 1] >> (56 - 8 * i));
  }
}
static inline void dt_vr_put(uint64_t* w, uint32_t slot, const uint8_t* b) {
  uint64_t hi = 0, lo = 0;
  for (int i = 0; i < 8; ++i) {
    hi = (hi << 8) | b[i];
    lo = (lo << 8) | b[8 + i];
  }
  w[W_VR + 2 * slot] = hi;
  w[W_VR + 2 * slot + 1] = lo;
}
static inline uint32_t dt_slot_of(const DtCase* c, uint32_t reg) {
  for (uint32_t i = 0; i < 32; ++i)
    if (c->vmap[i] == reg) return i;
  return 31;
}

#ifdef DT_PSEUDO_FLOAT
// Single-precision arithmetic of the emulated CPU, round to nearest.
static inline uint32_t dt_fmul(uint32_t a, uint32_t b) {
  union { float f; uint32_t u; } x, y, z;
  x.u = a; y.u = b;
  z.f = x.f * y.f;
  return z.u;
}
static inline uint32_t dt_fadd(uint32_t a, uint32_t b) {
  union { float f; uint32_t u; } x, y, z;
  x.u = a; y.u = b;
  z.f = x.f + y.f;
  return z.u;
}
#endif

static inline void dt_pseudo(const DtCase* c, uint64_t* w) {
  uint32_t word = c->codex[0];
  uint32_t ra = (word >> 16) & 31, rb = (word >> 11) & 31;
  uint32_t ea = (uint32_t)((ra ? w[W_GPR + ra] : 0) + w[W_GPR + rb]);
  uint32_t eb = ea & 15;
  // The vector register is in the 5-bit field for the plain forms and in
  // the 7-bit one for the 128 forms (primary opcode 4, 5 or 6).
  uint32_t primary = word >> 26;
  uint32_t vreg = primary == 31 ? (word >> 21) & 31 : dt_vd128(word);
  uint32_t slot = dt_slot_of(c, vreg);
  uint8_t v[16], s[16];
  switch (c->pseudo) {
    case PS_LVLX:
      for (uint32_t i = 0; i < 16; ++i) v[i] = i < 16 - eb ? dt_mem_get(w, ea + i) : 0;
      dt_vr_put(w, slot, v);
      break;
    case PS_LVRX:
      for (uint32_t i = 0; i < 16; ++i) v[i] = 0;
      for (uint32_t i = 0; i < eb; ++i) v[16 - eb + i] = dt_mem_get(w, ea - eb + i);
      dt_vr_put(w, slot, v);
      break;
    case PS_STVLX:
      dt_vr_get(w, slot, v);
      for (uint32_t i = 0; i < 16 - eb; ++i) dt_mem_put(w, ea + i, v[i]);
      break;
    case PS_STVRX:
      dt_vr_get(w, slot, v);
      for (uint32_t i = 0; i < eb; ++i) dt_mem_put(w, ea - eb + i, v[16 - eb + i]);
      break;
    case PS_VPERMWI: {
      uint32_t perm = ((word >> 16) & 31) | (((word >> 6) & 7) << 5);
      dt_vr_get(w, dt_slot_of(c, dt_vb128(word)), s);
      for (uint32_t i = 0; i < 4; ++i) {
        uint32_t from = (perm >> (6 - 2 * i)) & 3;
        for (uint32_t k = 0; k < 4; ++k) v[4 * i + k] = s[4 * from + k];
      }
      dt_vr_put(w, slot, v);
      break;
    }
    case PS_VRLIMI: {
      uint32_t imm = (word >> 16) & 31, z = (word >> 6) & 3;
      dt_vr_get(w, dt_slot_of(c, dt_vb128(word)), s);
      dt_vr_get(w, slot, v);
      for (uint32_t i = 0; i < 4; ++i) {
        if (!((imm >> (3 - i)) & 1)) continue;
        uint32_t from = (i + z) & 3;
        for (uint32_t k = 0; k < 4; ++k) v[4 * i + k] = s[4 * from + k];
      }
      dt_vr_put(w, slot, v);
      break;
    }
#ifdef DT_PSEUDO_FLOAT
    case PS_VMSUM3FP:
    case PS_VMSUM4FP: {
      uint32_t va = ((word >> 16) & 31) | (((word >> 5) & 1) << 5) | (((word >> 10) & 1) << 6);
      uint32_t sa = dt_slot_of(c, va), sb = dt_slot_of(c, dt_vb128(word));
      uint32_t a[4], b[4];
      for (uint32_t i = 0; i < 4; ++i) {
        a[i] = (uint32_t)(w[W_VR + 2 * sa + i / 2] >> (i % 2 ? 0 : 32));
        b[i] = (uint32_t)(w[W_VR + 2 * sb + i / 2] >> (i % 2 ? 0 : 32));
      }
      uint32_t sum = dt_fadd(dt_fmul(a[0], b[0]), dt_fmul(a[1], b[1]));
      if (c->pseudo == PS_VMSUM3FP) {
        sum = dt_fadd(sum, dt_fmul(a[2], b[2]));
      } else {
        sum = dt_fadd(sum, dt_fadd(dt_fmul(a[2], b[2]), dt_fmul(a[3], b[3])));
      }
      if ((sum & 0x7F800000u) == 0) sum &= 0x80000000u;  // denormal to zero
      w[W_VR + 2 * slot] = w[W_VR + 2 * slot + 1] = ((uint64_t)sum << 32) | sum;
      break;
    }
#endif
    default: break;
  }
}

#endif  // DT_PSEUDO_H_
