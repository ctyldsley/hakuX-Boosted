// hakuX-Boosted: the differential tester's case generator.
//
// Plain C with no library, so that the same text is compiled twice: into the
// reference harness that runs on the emulated PowerPC (dt_ref.c) and into the
// runner that drives the ARM64 back end (dt_jit.cc). Both call dt_gen_case()
// with the same seed, index and tier and get the same case: the instruction
// words, the whole machine state before them, and what to leave out of the
// comparison. Nothing is exchanged between the two but the reference's
// results.
//
// A case's state, before and after, is DT_NW 64-bit words (the W_ indices).
// Vector registers are "slots" 0 to 31: the reference runs them as v0 to v31,
// the console side as the registers vmap[] names (VMX128 has 128).
#ifndef DT_GEN_H_
#define DT_GEN_H_

#include <stdint.h>

#define DT_VERSION 3u

// ---- state layout
#define W_GPR 0
#define W_CTR 32
#define W_CR 33
#define W_XER 34    // SO, OV, CA in their XER positions (bits 31, 30, 29)
#define W_FPSCR 35
#define W_VSCR 36
#define W_FPR 40
#define W_VR 72     // two words a slot, bytes 0-7 then 8-15
#define W_MEM 136
#define DT_MEM_BYTES 2048u
#define DT_NW (W_MEM + DT_MEM_BYTES / 8)

// The data window: at the same guest address on both sides.
#define DT_MEM_BASE 0x10010000u
#define DT_MEM_MID (DT_MEM_BASE + DT_MEM_BYTES / 2)

#define DT_XER_MASK 0xE0000000u
// FPSCR: the translator keeps the control bits (enables, NI, RN) and none of
// the status bits, so only the low byte is compared.
#define DT_FPSCR_MASK 0x000000FFu
#define DT_VSCR_NJ 0x00010000u
#define DT_VSCR_SAT 0x00000001u

// ---- the instruction table
enum {
  T_GD, T_GS, T_GDS, T_PTR, T_PTRU, T_IDX, T_DISP, T_DISPU, T_DISP4, T_DISPU4,
  T_SI16, T_UI16, T_IMM, T_IMM6, T_IMM8, T_RC, T_OE, T_FRC, T_RC6, T_CRFD,
  T_CRBD, T_CRBS, T_FXM, T_SPR, T_FD, T_FS, T_FM, T_RN, T_VD, T_VS, T_VDS, T_VS8,
  T_VSSH, T_VSNJ
};
#define PL_NONE 0xFFFF
#define PL_SH6 0x400
#define PL_MB6 0x401
#define PL_SPR 0x402
#define PL_VD128 0x403
#define PL_VA128 0x404
#define PL_VB128 0x405
#define PL_PERM 0x406

enum {
  CL_INT, CL_ROT, CL_CR, CL_MEM, CL_RESV, CL_FPU, CL_VMX, CL_VMXF, CL_VMEM,
  CL_V128, CL_V128F, CL_V128M, CL_COUNT
};
static const char* const dt_class_names[CL_COUNT] = {
    "int", "rot", "cr", "mem", "resv", "fpu", "vmx", "vmxf", "vmem",
    "v128", "v128f", "v128m"};

#define IF_HI32 0x0001u    // upper 32 bits of the result are undefined
#define IF_DIVW 0x0002u
#define IF_DIVWU 0x0004u
#define IF_DIVD 0x0008u
#define IF_DIVDU 0x0010u
#define IF_TOL 0x0020u     // an estimate: compared with a tolerance
#define IF_FHI32 0x0040u   // upper 32 bits of the float register undefined
#define IF_MTFSF 0x0080u
#define IF_FIRST 0x0100u   // only as a case's first instruction
#define IF_RESL 0x0200u
#define IF_RESS 0x0400u
#define IF_ELEM 0x0800u    // loads one element, the rest undefined
#define IF_VINT 0x1000u    // integer result of a float vector instruction
#define IF_VMULFP 0x2000u  // the reference is three instructions
#define IF_T1 0x4000u      // single-instruction cases only
#define IF_KNOWN 0x8000u   // has an entry in known-issues.txt: not in sequences
#define IF_DIV (IF_DIVW | IF_DIVWU | IF_DIVD | IF_DIVDU)

enum { PS_NONE, PS_LVLX, PS_LVRX, PS_STVLX, PS_STVRX, PS_VMSUM3FP,
       PS_VMSUM4FP, PS_VPERMWI, PS_VRLIMI };

typedef struct {
  uint8_t type;
  uint16_t px, pr, pr2;
} DtOp;
typedef struct {
  const char* name;
  uint32_t opx, opr;
  uint8_t cls;
  uint32_t flags;
  uint8_t pseudo;
  uint8_t nops;
  DtOp ops[7];
} DtInsn;

static const DtInsn dt_insns[] = {
#define DT_ROWS
#include "dt_table.inc"
#undef DT_ROWS
};
#define DT_NINSN ((int)(sizeof(dt_insns) / sizeof(dt_insns[0])))

// ---- a case
#define CF_PSEUDO 0x1u    // the reference is dt_pseudo(), not instructions
#define CF_TOL 0x2u       // float results compared with a tolerance
#define DT_MAX_INSN 8
#define DT_INSN_BC 0xFFFFu

typedef struct {
  uint32_t nx, nr;
  uint32_t codex[DT_MAX_INSN + 4];
  uint32_t coder[DT_MAX_INSN * 3 + 4];
  uint64_t init[DT_NW];
  uint8_t vmap[32];
  uint32_t flags;
  uint8_t pseudo;
  uint32_t gpr_hi_skip;  // registers whose upper 32 bits are not compared
  uint32_t fpr_hi_skip;
  uint32_t vr_skip;      // slots not compared
  uint32_t cr_mask;      // condition register bits compared
  uint32_t vscr_mask;    // VSCR bits compared
  uint32_t ninsn;
  uint16_t insn[DT_MAX_INSN + 4];  // table index of each console instruction
  uint8_t tier;
  uint8_t theme;
} DtCase;

// ---- random numbers (splitmix64)
typedef struct { uint64_t s; } DtRng;
static inline uint64_t dt_next(DtRng* r) {
  uint64_t z = (r->s += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}
static inline uint32_t dt_below(DtRng* r, uint32_t n) {
  return (uint32_t)((dt_next(r) >> 32) * (uint64_t)n >> 32);
}
static inline int dt_chance(DtRng* r, uint32_t num, uint32_t den) {
  return dt_below(r, den) < num;
}
static inline int32_t dt_range(DtRng* r, int32_t lo, int32_t hi) {
  return lo + (int32_t)dt_below(r, (uint32_t)(hi - lo + 1));
}

// The data window from its seed. Frozen: the regression cases
// (regressions.txt) name a window by this seed.
static inline void dt_fill_window(uint64_t seed, uint64_t* words) {
  DtRng r;
  r.s = seed;
  for (uint32_t i = 0; i < DT_MEM_BYTES / 8; ++i) words[i] = dt_next(&r);
}

// ---- values
static inline uint64_t dt_gpr_value(DtRng* r) {
  static const uint64_t edge[] = {
      0, 1, 2, 0xFFFFFFFFFFFFFFFFull, 0x7FFFFFFFull, 0x80000000ull,
      0xFFFFFFFFull, 0x100000000ull, 0xFFFFFFFF80000000ull,
      0x8000000000000000ull, 0x7FFFFFFFFFFFFFFFull, 0xFFFFFFFF7FFFFFFFull,
      0x00000000FFFF8000ull, 0x0000000000007FFFull, 0x5555555555555555ull,
      0xAAAAAAAAAAAAAAAAull, 0x0000000100000001ull, 0x8000000080000000ull,
      31, 32, 63, 64};
  uint64_t v = dt_next(r);
  switch (dt_below(r, 8)) {
    case 0: return edge[dt_below(r, sizeof(edge) / sizeof(edge[0]))];
    case 1: return v & 0xFFFFFFFFull;
    case 2: return (uint64_t)(int64_t)(int32_t)v;
    case 3: return (uint64_t)(int64_t)dt_range(r, -130, 130);
    case 4: return v & 0xFFull;
    case 5: return v & (v >> dt_below(r, 64));
    default: return v;
  }
}

static inline uint64_t dt_double_bits_any(DtRng* r);
// nan: whether NaNs may be among the values. Sequences start without them:
// the two sides agree on which results are NaNs, not always on a NaN's sign
// and payload (known-issues.txt), and in a sequence that difference would
// travel into everything computed from it.
static inline uint64_t dt_double_bits(DtRng* r, int nan) {
  uint64_t v = dt_double_bits_any(r);
  if (!nan && (v & 0x7FF0000000000000ull) == 0x7FF0000000000000ull &&
      (v & 0x000FFFFFFFFFFFFFull)) {
    v &= 0xBFFFFFFFFFFFFFFFull;
  }
  return v;
}
static inline uint64_t dt_double_bits_any(DtRng* r) {
  static const uint64_t edge[] = {
      0x0000000000000000ull, 0x8000000000000000ull, 0x7FF0000000000000ull,
      0xFFF0000000000000ull, 0x7FF8000000000000ull, 0xFFF8000000000001ull,
      0x7FF4000000000000ull, 0xFFF0000000000001ull, 0x3FF0000000000000ull,
      0xBFF0000000000000ull, 0x0000000000000001ull, 0x800FFFFFFFFFFFFFull,
      0x0010000000000000ull, 0x7FEFFFFFFFFFFFFFull, 0x47EFFFFFE0000000ull,
      0x36A0000000000000ull, 0x3810000000000000ull, 0x41DFFFFFFFC00000ull,
      0xC1E0000000000000ull, 0x43E0000000000000ull, 0x4000000000000000ull,
      0x3FE0000000000000ull, 0x4059000000000000ull, 0x3FB999999999999Aull};
  uint64_t v = dt_next(r);
  switch (dt_below(r, 8)) {
    case 0: return edge[dt_below(r, sizeof(edge) / sizeof(edge[0]))];
    case 1: {  // a small integer
      // built without the host's float code: sign, exponent, mantissa
      uint32_t n = 1 + dt_below(r, 1000), e = 0;
      uint64_t m = n;
      while (m >> 1 >> e) ++e;
      m = (m << (52 - e)) & 0x000FFFFFFFFFFFFFull;
      return ((v & 1) << 63) | ((uint64_t)(1023 + e) << 52) | m;
    }
    case 2:  // near 1, exactly a single
      return (v & 0x8000000000000000ull) |
             ((uint64_t)(1023 + dt_range(r, -8, 8)) << 52) |
             ((v >> 12) & 0x000FFFFFE0000000ull);
    case 3:  // the range of singles
      return (v & 0x8000000000000000ull) |
             ((uint64_t)(1023 + dt_range(r, -160, 130)) << 52) |
             ((v >> 12) & 0x000FFFFFFFFFFFFFull);
    case 4:  // near 1
      return (v & 0x8000000000000000ull) |
             ((uint64_t)(1023 + dt_range(r, -3, 3)) << 52) |
             ((v >> 12) & 0x000FFFFFFFFFFFFFull);
    default: return v;  // anything: all exponents, NaNs included
  }
}

// A single for a vector lane. No denormals: the reference CPU model does not
// treat denormal vector inputs as the console does (see the notes).
static inline uint32_t dt_float_bits_any(DtRng* r);
static inline uint32_t dt_float_bits(DtRng* r, int nan) {
  uint32_t v = dt_float_bits_any(r);
  if (!nan && (v & 0x7F800000u) == 0x7F800000u && (v & 0x007FFFFFu)) v &= 0xBFFFFFFFu;
  return v;
}
static inline uint32_t dt_float_bits_any(DtRng* r) {
  static const uint32_t edge[] = {
      0x00000000u, 0x80000000u, 0x7F800000u, 0xFF800000u, 0x7FC00000u,
      0xFFC00001u, 0x7FA00000u, 0x3F800000u, 0xBF800000u, 0x7F7FFFFFu,
      0x00800000u, 0x4F000000u, 0xCF000000u, 0x4F800000u, 0x3F000000u,
      0x40000000u, 0x42C80000u, 0x3DCCCCCDu};
  uint32_t v = (uint32_t)dt_next(r);
  uint32_t out;
  switch (dt_below(r, 6)) {
    case 0: return edge[dt_below(r, sizeof(edge) / sizeof(edge[0]))];
    case 1: out = (v & 0x807FFFFFu) | ((uint32_t)(127 + dt_range(r, -4, 4)) << 23); break;
    case 2: out = (v & 0x80700000u) | ((uint32_t)(127 + dt_range(r, 0, 10)) << 23); break;
    case 3: out = (v & 0x807FFFFFu) | ((uint32_t)(127 + dt_range(r, -40, 40)) << 23); break;
    default: out = v; break;
  }
  if ((out & 0x7F800000u) == 0) out &= 0x80000000u;  // denormal to zero
  return out;
}

// Two lanes. (One call a statement throughout this file: C leaves the order
// of two calls in one expression to the compiler, and the two sides are
// built by two compilers.)
static inline uint64_t dt_float_pair(DtRng* r, int nan) {
  uint64_t hi = dt_float_bits(r, nan);
  uint64_t lo = dt_float_bits(r, nan);
  return (hi << 32) | lo;
}

// ---- field placement
static inline uint32_t dt_place(uint16_t pl, uint32_t v) {
  if (pl == PL_NONE) return 0;
  if (pl < 0x400) {
    uint32_t shift = pl & 31, width = pl >> 5;
    return (v & ((1u << width) - 1)) << shift;
  }
  switch (pl) {
    case PL_SH6: return ((v & 31) << 11) | (((v >> 5) & 1) << 1);
    case PL_MB6: return ((v & 31) << 6) | (((v >> 5) & 1) << 5);
    case PL_SPR: return ((v & 31) << 16) | (((v >> 5) & 31) << 11);
    case PL_VD128: return ((v & 31) << 21) | (((v >> 5) & 3) << 2);
    case PL_VA128:
      return ((v & 31) << 16) | (((v >> 5) & 1) << 5) | (((v >> 6) & 1) << 10);
    case PL_VB128: return ((v & 31) << 11) | ((v >> 5) & 3);
    case PL_PERM: return ((v & 31) << 16) | (((v >> 5) & 7) << 6);
  }
  return 0;
}

// The console's vector register fields of an instruction word (for
// dt_pseudo.h, which works on the word).
static inline uint32_t dt_vd128(uint32_t w) { return ((w >> 21) & 31) | (((w >> 2) & 3) << 5); }
static inline uint32_t dt_vb128(uint32_t w) { return ((w >> 11) & 31) | ((w & 3) << 5); }

// ---- the generator
typedef struct {
  DtRng r;
  DtCase* c;
  uint8_t ptr[3], idx[2];
  uint8_t gpool[6], fpool[6], vpool[6];
  uint32_t reserved_gpr;    // pointer and index registers
  uint32_t gpr_taint, fpr_taint, vr_nosrc, vr_taint;
  int uses_v128;
  int under_branch;
} DtGen;

// Slots with a fixed role: 29 holds a VSCR value (for mtvscr), 30 a shift
// count splat (for vsl and vsr), 31 is the reference's scratch. Float
// register 31 holds an FPSCR value (for mtfsf).
#define DT_VSLOTS 29
#define DT_FREGS 31

static inline uint32_t dt_pick_gpr_src(DtGen* g) {
  for (;;) {
    uint32_t reg = dt_chance(&g->r, 3, 4) ? g->gpool[dt_below(&g->r, 6)]
                                          : dt_below(&g->r, 32);
    if (!(g->gpr_taint >> reg & 1)) return reg;
  }
}
static inline uint32_t dt_pick_gpr_dst(DtGen* g) {
  for (;;) {
    uint32_t reg = dt_chance(&g->r, 3, 4) ? g->gpool[dt_below(&g->r, 6)]
                                          : dt_below(&g->r, 32);
    if (!(g->reserved_gpr >> reg & 1)) return reg;
  }
}
static inline uint32_t dt_pick_fpr(DtGen* g, int src) {
  for (;;) {
    uint32_t reg = dt_chance(&g->r, 3, 4) ? g->fpool[dt_below(&g->r, 6)]
                                          : dt_below(&g->r, DT_FREGS);
    if (!src || !(g->fpr_taint >> reg & 1)) return reg;
  }
}
static inline uint32_t dt_pick_vr(DtGen* g, int src, uint32_t limit) {
  for (;;) {
    uint32_t slot = dt_chance(&g->r, 3, 4) ? g->vpool[dt_below(&g->r, 6)]
                                           : dt_below(&g->r, limit);
    if (slot >= limit) slot = dt_below(&g->r, limit);
    if (!src || !((g->vr_nosrc | g->vr_taint) >> slot & 1)) return slot;
  }
}

// Appends one instruction of the table. Returns 0 if it cannot be placed.
static inline int dt_emit(DtGen* g, int index) {
  const DtInsn* in = &dt_insns[index];
  DtCase* c = g->c;
  uint32_t wx = in->opx, wr = in->opr;
  uint32_t gd = 32, fd = 32, vd = 32, slots[7];
  int ra0_variant = 0, has_ptr = 0, has_idx = 0;
  for (int i = 0; i < in->nops; ++i) {
    if (in->ops[i].type == T_PTR) has_ptr = 1;
    if (in->ops[i].type == T_IDX) has_idx = 1;
  }
  if (has_ptr && has_idx && dt_chance(&g->r, 1, 8)) ra0_variant = 1;
  uint32_t ptr = g->ptr[dt_below(&g->r, 3)];
  for (int i = 0; i < in->nops; ++i) {
    const DtOp* op = &in->ops[i];
    uint32_t v = 0, vx;
    int is_slot = 0;
    switch (op->type) {
      case T_GD: v = gd = dt_pick_gpr_dst(g); break;
      case T_GS: v = dt_pick_gpr_src(g); break;
      case T_GDS:
        do { v = dt_pick_gpr_dst(g); } while (g->gpr_taint >> v & 1);
        gd = v;
        break;
      case T_PTR: v = ra0_variant ? 0 : ptr; break;
      case T_PTRU: v = ptr; break;
      case T_IDX: v = ra0_variant ? ptr : g->idx[dt_below(&g->r, 2)]; break;
      case T_DISP: v = (uint32_t)dt_range(&g->r, -120, 120); break;
      case T_DISPU: v = (uint32_t)dt_range(&g->r, -48, 48); break;
      case T_DISP4: v = (uint32_t)dt_range(&g->r, -120, 120) & ~3u; break;
      case T_DISPU4: v = (uint32_t)dt_range(&g->r, -48, 48) & ~3u; break;
      case T_SI16:
      case T_UI16: {
        static const uint16_t edge[] = {0, 1, 0xFFFF, 0x7FFF, 0x8000, 0x8001, 2, 0xFF, 0x100};
        v = dt_chance(&g->r, 1, 3) ? edge[dt_below(&g->r, 9)] : (uint32_t)dt_next(&g->r);
        break;
      }
      case T_IMM: case T_IMM6: case T_IMM8: case T_CRFD:
        v = (uint32_t)dt_next(&g->r);
        break;
      case T_CRBD: case T_CRBS:
        v = (uint32_t)dt_next(&g->r);
        if (c->tier != 1 && (v & 3) == 3) v ^= 1 + dt_below(&g->r, 3);  // not SO
        break;
      case T_FXM: v = dt_chance(&g->r, 1, 4) ? 0xFF : (uint32_t)dt_next(&g->r); break;
      case T_RC: v = dt_chance(&g->r, 1, 3); break;
      case T_RC6: v = dt_chance(&g->r, 1, 3); break;
      case T_OE: v = c->tier == 1 && dt_chance(&g->r, 1, 4); break;
      case T_FRC:
        // Record forms copy FPSCR status into CR1; the translator does not
        // keep that status, so CR1 is left out of the comparison.
        // (Not in sequences: a later instruction could read CR1.)
        v = c->tier == 1 && dt_chance(&g->r, 1, 8);
        if (v) c->cr_mask &= 0xF0FFFFFFu;
        break;
      case T_SPR:  // XER or CTR
        v = dt_chance(&g->r, 1, 2) && (c->tier == 1 || gd < 32) ? 1 : 9;
        break;
      case T_FD: v = fd = dt_pick_fpr(g, 0); break;
      case T_FS: v = dt_pick_fpr(g, 1); break;
      case T_FM: v = dt_chance(&g->r, 1, 2) ? 0x01 : 0xFF; break;
      case T_RN: v = dt_below(&g->r, 4); wx |= 7u << 23; wr |= 7u << 23; break;
      // A plain VMX instruction's fields have five bits: in a case that
      // spreads the slots over the 128 registers it takes one of the slots
      // that kept their own number (see vmap below).
      case T_VD:
        v = vd = dt_pick_vr(g, 0, g->uses_v128 && op->px < 0x400 ? 16 : DT_VSLOTS);
        is_slot = 1;
        break;
      case T_VS:  // not 29: its VSCR image is a denormal as a float
        do {
          v = dt_pick_vr(g, 1, g->uses_v128 && op->px < 0x400 ? 16 : 31);
        } while (v == 29);
        is_slot = 1;
        break;
      case T_VDS: v = vd = dt_pick_vr(g, 1, DT_VSLOTS); is_slot = 1; break;
      case T_VS8: v = dt_pick_vr(g, 1, 8); is_slot = 1; break;
      case T_VSSH: v = 30; is_slot = 1; break;
      case T_VSNJ: v = 29; is_slot = 1; break;
    }
    slots[i] = v;
    vx = is_slot ? c->vmap[v] : v;
    wx |= dt_place(op->px, vx);
    wr |= dt_place(op->pr, v) | dt_place(op->pr2, v);
  }
  if (in->flags & IF_MTFSF) {
    wx = (wx & ~(31u << 11)) | (31u << 11);
    wr = wx;
  }
  // Taints: a register an instruction leaves partly undefined is not read
  // again and its undefined part is not compared.
  if (gd < 32) {
    if (!g->under_branch) g->gpr_taint &= ~(1u << gd);
    if (in->flags & IF_HI32) g->gpr_taint |= 1u << gd;
  }
  if (fd < 32) {
    if (!g->under_branch) g->fpr_taint &= ~(1u << fd);
    if (in->flags & IF_FHI32) g->fpr_taint |= 1u << fd;
  }
  if (vd < 32) {
    if (!g->under_branch) {
      g->vr_taint &= ~(1u << vd);
      g->vr_nosrc &= ~(1u << vd);
    }
    if (in->flags & IF_ELEM) g->vr_taint |= 1u << vd;
    if (in->flags & IF_VINT) g->vr_nosrc |= 1u << vd;
  }
  if (in->flags & IF_TOL) c->flags |= CF_TOL;
  c->insn[c->ninsn++] = (uint16_t)index;
  c->codex[c->nx++] = wx;
  if (in->pseudo != PS_NONE) {
    c->flags |= CF_PSEUDO;
    c->pseudo = in->pseudo;
  } else if (in->flags & IF_VMULFP) {
    // a * b as a * b + (-0.0), -0.0 made in the scratch slot
    c->coder[c->nr++] = 0x1000038Cu | (31u << 21) | (31u << 16);  // vspltisw v31,-1
    c->coder[c->nr++] = 0x10000184u | (31u << 21) | (31u << 16) | (31u << 11);  // vslw
    c->coder[c->nr++] = 0x1000002Eu | (slots[0] << 21) | (slots[1] << 16) |
                        (31u << 11) | (slots[2] << 6);  // vmaddfp d,a,c=b,b=v31
  } else {
    c->coder[c->nr++] = wr;
  }
  return 1;
}

static inline void dt_emit_raw(DtGen* g, uint32_t word, uint16_t tag) {
  DtCase* c = g->c;
  c->insn[c->ninsn++] = tag;
  c->codex[c->nx++] = word;
  c->coder[c->nr++] = word;
}

// A division whose result the architecture leaves undefined (by zero, or
// the most negative number by -1) gets new operands: the two sides need not
// agree on it. Only for a case's first instruction, whose operands are the
// initial state.
static inline void dt_fix_division(DtCase* c, const DtInsn* in, uint32_t w) {
  uint32_t ra = (w >> 16) & 31, rb = (w >> 11) & 31;
  uint64_t* a = &c->init[W_GPR + ra];
  uint64_t* b = &c->init[W_GPR + rb];
  if (in->flags & (IF_DIVW | IF_DIVWU)) {
    if ((uint32_t)*b == 0) *b = (*b & ~0xFFFFFFFFull) | 3;
    if ((in->flags & IF_DIVW) && (uint32_t)*a == 0x80000000u && (uint32_t)*b == 0xFFFFFFFFu)
      *b = (*b & ~0xFFFFFFFFull) | 5, *a = (ra == rb) ? *b : *a;
  } else {
    if (*b == 0) *b = 3;
    if ((in->flags & IF_DIVD) && *a == 0x8000000000000000ull && *b == ~0ull)
      *b = 5, *a = (ra == rb) ? *b : *a;
  }
}

static inline int dt_pick_insn(DtGen* g, uint32_t class_mask, uint32_t forbid) {
  for (;;) {
    int index = (int)dt_below(&g->r, (uint32_t)DT_NINSN);
    const DtInsn* in = &dt_insns[index];
    if (!(class_mask >> in->cls & 1)) continue;
    if (in->flags & forbid) continue;
    return index;
  }
}

static inline int dt_find_insn(const char* name) {
  for (int i = 0; i < DT_NINSN; ++i) {
    const char* a = dt_insns[i].name;
    const char* b = name;
    while (*a && *a == *b) ++a, ++b;
    if (!*a && !*b) return i;
  }
  return 0;
}

#define DT_CLASSMASK(c) (1u << (c))
#define DT_INT_CLASSES (DT_CLASSMASK(CL_INT) | DT_CLASSMASK(CL_ROT) | DT_CLASSMASK(CL_CR))
#define DT_V128_CLASSES (DT_CLASSMASK(CL_V128) | DT_CLASSMASK(CL_V128F) | DT_CLASSMASK(CL_V128M))

// A load-and-reserve, up to two plain integer instructions, and the store
// conditional to the same address: the pairing titles use. A lone store
// conditional (which must fail) one time in four.
static inline void dt_emit_reservation(DtGen* g, int index) {
  const DtInsn* in = &dt_insns[index];
  int wide = in->name[1] == 'd' || in->name[2] == 'd';  // ldarx, stdcx
  uint32_t p = g->ptr[dt_below(&g->r, 3)], x = g->idx[dt_below(&g->r, 2)];
  uint32_t d = dt_pick_gpr_dst(g), s = dt_pick_gpr_src(g);
  uint32_t ab = (p << 16) | (x << 11);
  uint16_t li = 0, si = 0;
  for (int i = 0; i < DT_NINSN; ++i) {
    const char* n = dt_insns[i].name;
    if (dt_insns[i].flags & IF_RESL) { if ((n[1] == 'd') == wide) li = (uint16_t)i; }
    if (dt_insns[i].flags & IF_RESS) { if ((n[2] == 'd') == wide) si = (uint16_t)i; }
  }
  // The address must be aligned to the access: move the pointer and the
  // index so that their sum is.
  g->c->init[W_GPR + p] &= ~7ull;
  g->c->init[W_GPR + x] &= ~7ull;
  if (!dt_chance(&g->r, 1, 4)) {
    dt_emit_raw(g, dt_insns[li].opx | (d << 21) | ab, li);
    g->gpr_taint &= ~(1u << d);
    for (uint32_t n = dt_below(&g->r, 3); n; --n)
      // (no CR logic between them: a single-instruction case may read the
      // SO bits, and here that would mix into the pattern's own CR0)
      dt_emit(g, dt_pick_insn(g, DT_CLASSMASK(CL_INT) | DT_CLASSMASK(CL_ROT),
                              IF_DIV | IF_FIRST | IF_T1 | IF_KNOWN));
    // The stored register must be whole (stdcx. stores all 64 bits).
    while (g->gpr_taint >> s & 1) s = dt_below(&g->r, 32);
  }
  dt_emit_raw(g, dt_insns[si].opx | (s << 21) | ab, si);
}

// tier 1: one instruction (or one reservation pattern). tier 2: a sequence
// of two to six around a theme, with forward branches.
static inline void dt_gen_case(uint64_t seed, uint64_t index, uint32_t tier, DtCase* c) {
  DtGen gen;
  DtGen* g = &gen;
  DtRng* r = &g->r;
  // Seed, index and tier are mixed one after the other: added together,
  // neighbouring seeds gave the same stream a few draws apart.
  r->s = seed;
  r->s = dt_next(r) ^ (index * 0xD1B54A32D192ED03ull);
  r->s = dt_next(r) + tier;
  dt_next(r);
  g->c = c;
  g->gpr_taint = g->fpr_taint = g->vr_nosrc = g->vr_taint = 0;
  g->under_branch = 0;
  c->nx = c->nr = c->ninsn = 0;
  c->flags = 0;
  c->pseudo = PS_NONE;
  c->tier = (uint8_t)tier;
  // What the translator does not keep at all (the notes list each): the
  // summary overflow bit and its copy in every CR field, the overflow bit,
  // and VSCR's saturation bit. Single instructions are compared in full, so
  // that the report shows them; in sequences they are neither compared nor
  // allowed to reach anything that is (see T_CRBD, T_OE, T_SPR, IF_T1 and
  // the branch conditions below), so that a sequence's mismatch is always
  // something else.
  c->cr_mask = tier == 1 ? 0xFFFFFFFFu : 0xEEEEEEEEu;
  c->vscr_mask = tier == 1 ? (DT_VSCR_NJ | DT_VSCR_SAT) : DT_VSCR_NJ;

  // What the case is about.
  static const uint32_t themes[] = {
      DT_INT_CLASSES,
      DT_INT_CLASSES | DT_CLASSMASK(CL_MEM),
      DT_INT_CLASSES | DT_CLASSMASK(CL_MEM) | DT_CLASSMASK(CL_RESV),
      DT_CLASSMASK(CL_FPU),
      DT_CLASSMASK(CL_FPU) | DT_CLASSMASK(CL_MEM),
      DT_CLASSMASK(CL_FPU) | DT_INT_CLASSES,
      DT_CLASSMASK(CL_VMX) | DT_CLASSMASK(CL_VMEM),
      DT_CLASSMASK(CL_VMX) | DT_CLASSMASK(CL_V128) | DT_CLASSMASK(CL_V128M),
      DT_CLASSMASK(CL_VMXF) | DT_CLASSMASK(CL_V128F),
      DT_CLASSMASK(CL_V128) | DT_CLASSMASK(CL_V128M) | DT_CLASSMASK(CL_VMEM) | DT_CLASSMASK(CL_MEM),
      DT_INT_CLASSES | DT_CLASSMASK(CL_MEM) | DT_CLASSMASK(CL_FPU) | DT_CLASSMASK(CL_VMX) | DT_CLASSMASK(CL_VMEM),
  };
  int first = -1;
  uint32_t class_mask;
  if (tier == 1) {
    first = (int)dt_below(r, (uint32_t)DT_NINSN);
    class_mask = DT_CLASSMASK(dt_insns[first].cls);
    c->theme = dt_insns[first].cls;
  } else {
    c->theme = (uint8_t)dt_below(r, sizeof(themes) / sizeof(themes[0]));
    class_mask = themes[c->theme];
  }
  int float_vectors = (class_mask & (DT_CLASSMASK(CL_VMXF) | DT_CLASSMASK(CL_V128F))) != 0;
  g->uses_v128 = (class_mask & DT_V128_CLASSES) != 0;

  // The state.
  uint64_t* in = c->init;
  for (int i = 0; i < 32; ++i) in[W_GPR + i] = dt_gpr_value(r);
  in[W_CTR] = dt_chance(r, 1, 2) ? dt_below(r, 4) : dt_gpr_value(r);
  in[W_CR] = (uint32_t)dt_next(r);
  in[W_XER] = dt_chance(r, 1, 2) ? 0x20000000u : 0;
  if (tier == 1 && dt_chance(r, 1, 4)) in[W_XER] |= (uint32_t)dt_next(r) & 0xC0000000u;
  in[W_FPSCR] = dt_below(r, 4);
  in[W_VSCR] = DT_VSCR_NJ | (dt_chance(r, 1, 4) ? DT_VSCR_SAT : 0);
  in[37] = in[38] = in[39] = 0;
  int nan = tier == 1;
  for (int i = 0; i < 32; ++i) in[W_FPR + i] = dt_double_bits(r, nan);
  in[W_FPR + 31] = dt_next(r) & 0xFFFFFFFFFFFFFF03ull;
  int vstyle = float_vectors ? 1 : (int)dt_below(r, 3);
  for (int i = 0; i < 32; ++i) {
    uint64_t hi, lo;
    if (vstyle == 1) {
      hi = dt_float_pair(r, nan);
      lo = dt_float_pair(r, nan);
    } else if (vstyle == 2) {  // lanes near the saturation limits
      hi = dt_gpr_value(r);
      hi ^= dt_gpr_value(r) << 32;
      lo = dt_gpr_value(r);
      lo ^= dt_gpr_value(r) << 32;
    } else {
      hi = dt_next(r);
      lo = dt_next(r);
    }
    in[W_VR + 2 * i] = hi;
    in[W_VR + 2 * i + 1] = lo;
  }
  in[W_VR + 2 * 29] = 0;
  in[W_VR + 2 * 29 + 1] = DT_VSCR_NJ | (dt_chance(r, 1, 2) ? DT_VSCR_SAT : 0);
  in[W_VR + 2 * 30] = in[W_VR + 2 * 30 + 1] = (dt_next(r) & 0xFF) * 0x0101010101010101ull;
  dt_fill_window(dt_next(r), &in[W_MEM]);
  // Floats in memory, for the float loads: half the single instructions,
  // and every sequence that has float instructions (random bytes are a NaN
  // one time in 256, or a denormal for a float vector instruction).
  int float_case = (class_mask & (DT_CLASSMASK(CL_FPU) | DT_CLASSMASK(CL_VMXF) | DT_CLASSMASK(CL_V128F))) != 0;
  if (tier == 1 ? dt_chance(r, 1, 2) : float_case) {
    for (uint32_t i = 0; i < DT_MEM_BYTES / 8; ++i) {
      if (dt_chance(r, 1, 2) && !float_vectors) in[W_MEM + i] = dt_double_bits(r, nan);
      else in[W_MEM + i] = dt_float_pair(r, nan);
    }
  }

  // Pointer and index registers: every memory access goes through these,
  // and nothing but an update form writes them.
  g->reserved_gpr = 0;
  for (int i = 0; i < 3; ++i) {
    uint32_t reg;
    do {
      reg = (i == 0 && dt_chance(r, 1, 2)) ? 1 : 1 + dt_below(r, 31);
    } while (g->reserved_gpr >> reg & 1);
    g->ptr[i] = (uint8_t)reg;
    g->reserved_gpr |= 1u << reg;
    static const uint32_t align[] = {~15u, ~15u, ~7u, ~3u, ~1u, ~0u};
    uint32_t address = DT_MEM_MID + (uint32_t)dt_range(r, -64, 64);
    in[W_GPR + reg] = address & align[dt_below(r, 6)];
  }
  for (int i = 0; i < 2; ++i) {
    uint32_t reg;
    do { reg = dt_below(r, 32); } while (g->reserved_gpr >> reg & 1);
    g->idx[i] = (uint8_t)reg;
    g->reserved_gpr |= 1u << reg;
    int32_t v = dt_range(r, -64, 64);
    if (dt_chance(r, 1, 3)) v &= ~15;
    in[W_GPR + reg] = (uint64_t)(int64_t)v;
  }
  for (int i = 0; i < 6; ++i) {
    do { g->gpool[i] = (uint8_t)dt_below(r, 32); } while (g->reserved_gpr >> g->gpool[i] & 1);
    g->fpool[i] = (uint8_t)dt_below(r, DT_FREGS);
    g->vpool[i] = (uint8_t)dt_below(r, DT_VSLOTS);
  }
  // Slot to console register. VMX128 cases spread slots 16 to 28 over the
  // registers only VMX128 reaches; slots 0 to 15 and the three with a fixed
  // role keep their number, for the plain VMX instructions of the case and
  // for vperm128's third operand (three bits).
  for (int i = 0; i < 32; ++i) c->vmap[i] = (uint8_t)i;
  if (g->uses_v128) {
    uint8_t used[128];
    for (int i = 0; i < 128; ++i) used[i] = 0;
    for (int i = 16; i < DT_VSLOTS; ++i) {
      uint32_t reg;
      do { reg = 32 + dt_below(r, 96); } while (used[reg]);
      used[reg] = 1;
      c->vmap[i] = (uint8_t)reg;
    }
  }

  // The instructions.
  if (tier == 1) {
    const DtInsn* fi = &dt_insns[first];
    if (fi->flags & (IF_RESL | IF_RESS)) {
      dt_emit_reservation(g, first);
    } else {
      dt_emit(g, first);
      if (fi->flags & IF_DIV) dt_fix_division(c, fi, c->codex[0]);
    }
  } else {
    uint32_t count = 2 + dt_below(r, 5);
    // A constant first, one time in three: a register of the pool the later
    // instructions favour is loaded with an immediate, so that they are
    // translated with a constant operand (the translator folds those and
    // the back end has separate code for them).
    // (Not in the float vector themes: a vspltis* result is a denormal or a
    // NaN as a float, which the sequences keep out of float instructions.)
    int fed = 0;
    if (!float_vectors && dt_chance(r, 1, 3)) {
      uint32_t imm = (uint32_t)dt_next(r);
      fed = 1;
      if (class_mask & (DT_CLASSMASK(CL_VMX) | DT_V128_CLASSES | DT_CLASSMASK(CL_VMEM))) {
        static const char* const names[] = {"vspltisb", "vspltish", "vspltisw"};
        uint32_t which = dt_below(r, 3);
        uint32_t slot = g->vpool[0] % 16;
        g->vpool[0] = g->vpool[1] = (uint8_t)slot;
        int index = dt_find_insn(names[which]);
        dt_emit_raw(g, dt_insns[index].opx | (slot << 21) | ((imm & 31) << 16), (uint16_t)index);
      } else if (class_mask & DT_INT_CLASSES) {
        int index = dt_find_insn(dt_chance(r, 1, 4) ? "addis" : "addi");
        g->gpool[1] = g->gpool[0];
        dt_emit_raw(g, dt_insns[index].opx | ((uint32_t)g->gpool[0] << 21) | (imm & 0xFFFF),
                    (uint16_t)index);
      }
    }
    for (uint32_t n = 0; n < count && c->nx < DT_MAX_INSN; ++n) {
      uint32_t forbid = IF_TOL | IF_ELEM | IF_FIRST | IF_VMULFP | IF_T1 | IF_KNOWN;
      // A division only as the very first instruction: its operands are
      // then the initial state, which dt_fix_division can see.
      if (n || fed) forbid |= IF_DIV | IF_MTFSF;
      int index;
      do {
        index = dt_pick_insn(g, class_mask, forbid);
      } while (dt_insns[index].pseudo != PS_NONE);
      const DtInsn* pi = &dt_insns[index];
      if (pi->flags & (IF_RESL | IF_RESS)) {
        // only first: an update form before it could leave the address
        // unaligned, which traps
        if (n) continue;
        dt_emit_reservation(g, index);
        continue;
      }
      // A forward branch over this instruction, one time in five.
      if (n && n + 1 < count && dt_chance(r, 1, 5) && c->nx + 2 <= DT_MAX_INSN) {
        static const uint8_t bo[] = {12, 4, 12, 4, 16, 18, 8, 0, 10, 2, 20};
        uint32_t bi = dt_below(r, 32);
        if ((bi & 3) == 3) bi -= 1 + dt_below(r, 3);  // not SO
        dt_emit_raw(g, 0x40000008u | ((uint32_t)bo[dt_below(r, 11)] << 21) | (bi << 16),
                    DT_INSN_BC);
        g->under_branch = 1;
      }
      dt_emit(g, index);
      g->under_branch = 0;
      if (n == 0 && (pi->flags & IF_DIV)) dt_fix_division(c, pi, c->codex[0]);
    }
  }
  c->gpr_hi_skip = g->gpr_taint;
  c->fpr_hi_skip = g->fpr_taint;
  c->vr_skip = g->vr_taint | (1u << 31);
}

#endif  // DT_GEN_H_
