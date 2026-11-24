// hakuX-Boosted: the differential tester's runner. Generates the cases of
// dt_gen.h, runs each through the PowerPC front end and the ARM64 back end
// (the phone's translator, built headless as the instruction tests are) and
// compares everything the case left behind (general, float and vector
// registers, CR, XER, CTR, FPSCR, VSCR and the data window) with what the
// reference left (dt_ref.c under qemu-system-ppc64), read from --dt_ref.
//
//   xenia-jit-difftest --dt_seed=N --dt_tier=1|2 --dt_start=N --dt_count=N
//                      --dt_ref=<reference stream> --dt_out=<mismatches.jsonl>
//   xenia-jit-difftest --dt_regress=<regressions.txt>
//
// Mismatches are written one JSON object a line; dt_report.py turns the
// lines of a run into the report. The second form runs fixed cases whose
// expected results are in the file (no reference needed).
#include <unistd.h>

#include <cmath>
#include <csetjmp>
#include <csignal>
#include <cstring>
#include <string>
#include <vector>

#include "xenia/base/console_app_main.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/string_buffer.h"
#include "xenia/cpu/backend/a64/a64_backend.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/ppc/ppc_frontend.h"
#include "xenia/cpu/ppc/ppc_opcode_info.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/raw_module.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/memory.h"

extern "C" {
#include "dt_gen.h"
}

// Settings of other subsystems that the CPU sources name (as in the
// instruction tests' runner).
DEFINE_bool(mount_scratch, false, "Enable scratch mount", "Storage");
DEFINE_bool(mount_cache, false, "Enable cache mount", "Storage");
DEFINE_bool(mount_memory_unit, false, "Enable memory unit (MU) mount", "Storage");
DEFINE_string(apu, "nop", "Audio system stub.", "APU");
DEFINE_string(gpu, "null", "Graphics system stub.", "GPU");

DECLARE_path(log_file);

DEFINE_uint64(dt_seed, 1, "Seed of the run.", "Difftest");
DEFINE_uint64(dt_start, 0, "First case index.", "Difftest");
DEFINE_uint64(dt_count, 1000, "Number of cases.", "Difftest");
DEFINE_uint32(dt_tier, 1, "1: single instructions. 2: sequences.", "Difftest");
DEFINE_path(dt_ref, "", "The reference's result stream.", "Difftest");
DEFINE_path(dt_out, "", "Where mismatches are appended (JSON lines).", "Difftest");
DEFINE_path(dt_regress, "", "A file of fixed cases to run instead.", "Difftest");
DEFINE_bool(dt_dump, false, "Print every case, not only mismatches.", "Difftest");

namespace {

using xe::cpu::ppc::PPCContext;

constexpr uint32_t kCodeBase = 0x82000000u;
constexpr uint32_t kCodeSize = 0x00800000u;

// The state of a case in the console's terms.
struct XState {
  uint64_t gpr[32], ctr, cr, xer, fpscr, vscr;
  uint64_t fpr[32];
  uint64_t vr[128][2];
  uint8_t mem[DT_MEM_BYTES];
};
struct Skip {
  uint32_t gpr_hi = 0, fpr_hi = 0;
  bool vr[128] = {};
  uint32_t cr_mask = 0xFFFFFFFFu;
  uint32_t vscr_mask = DT_VSCR_NJ | DT_VSCR_SAT;
  bool tol = false;
  bool nan_equal = false;  // two NaNs are equal whatever their bits (sequences)
};
struct Diff {
  std::string name;  // r3, cr0, xer.ca, f1, v77, m0x1f8 ...
  std::string kind;  // gpr, gpr.hi32, cr0.., xer.ca, fpr, fpr.nan, vr, mem ...
  std::string ours, ref;
};

std::string Hex(uint64_t v, int digits = 16) {
  char b[40];
  snprintf(b, sizeof(b), "%0*llx", digits, static_cast<unsigned long long>(v));
  return b;
}

bool IsNaN64(uint64_t v) {
  return (v & 0x7FF0000000000000ull) == 0x7FF0000000000000ull && (v & 0x000FFFFFFFFFFFFFull);
}
bool IsNaN32(uint32_t v) { return (v & 0x7F800000u) == 0x7F800000u && (v & 0x007FFFFFu); }

// Estimates and the dot products: equal within 1/32, same class otherwise.
bool Close(double a, double b) {
  if (std::isnan(a) || std::isnan(b)) return std::isnan(a) && std::isnan(b);
  if (std::isinf(a) || std::isinf(b)) return a == b;
  if (std::fabs(a) < 1e-30 && std::fabs(b) < 1e-30) return true;
  return std::fabs(a - b) <= std::fabs(b) / 32;
}
bool Close64(uint64_t a, uint64_t b) {
  double x, y;
  std::memcpy(&x, &a, 8);
  std::memcpy(&y, &b, 8);
  return Close(x, y);
}
bool Close32(uint32_t a, uint32_t b) {
  float x, y;
  std::memcpy(&x, &a, 4);
  std::memcpy(&y, &b, 4);
  return Close(x, y);
}

void Compare(const XState& ours, const XState& ref, const Skip& skip, bool float_vectors,
             std::vector<Diff>* out) {
  for (int i = 0; i < 32; ++i) {
    uint64_t a = ours.gpr[i], b = ref.gpr[i];
    if (skip.gpr_hi >> i & 1) a &= 0xFFFFFFFFull, b &= 0xFFFFFFFFull;
    if (a != b) {
      out->push_back({"r" + std::to_string(i),
                      (uint32_t)a == (uint32_t)b ? "gpr.hi32" : "gpr", Hex(a), Hex(b)});
    }
  }
  if (ours.ctr != ref.ctr) out->push_back({"ctr", "ctr", Hex(ours.ctr), Hex(ref.ctr)});
  for (int i = 0; i < 8; ++i) {
    uint32_t m = (0xFu << (28 - 4 * i)) & skip.cr_mask;
    if ((ours.cr & m) != (ref.cr & m)) {
      // only the field's SO bit: its own kind (the translator keeps no SO)
      bool so_only = ((ours.cr ^ ref.cr) & m & 0xEEEEEEEEu) == 0;
      out->push_back({"cr" + std::to_string(i), so_only ? "cr.so" : "cr" + std::to_string(i),
                      Hex((ours.cr >> (28 - 4 * i)) & 15, 1), Hex((ref.cr >> (28 - 4 * i)) & 15, 1)});
    }
  }
  static const char* const xer_names[] = {"xer.so", "xer.ov", "xer.ca"};
  for (int i = 0; i < 3; ++i) {
    uint32_t m = 0x80000000u >> i;
    if ((ours.xer & m) != (ref.xer & m)) {
      out->push_back({xer_names[i], xer_names[i], (ours.xer & m) ? "1" : "0", (ref.xer & m) ? "1" : "0"});
    }
  }
  if ((ours.fpscr & DT_FPSCR_MASK) != (ref.fpscr & DT_FPSCR_MASK)) {
    out->push_back({"fpscr", "fpscr", Hex(ours.fpscr, 8), Hex(ref.fpscr, 8)});
  }
  if ((ours.vscr ^ ref.vscr) & DT_VSCR_SAT & skip.vscr_mask) {
    out->push_back({"vscr.sat", "vscr.sat", Hex(ours.vscr, 8), Hex(ref.vscr, 8)});
  }
  if ((ours.vscr ^ ref.vscr) & DT_VSCR_NJ & skip.vscr_mask) {
    out->push_back({"vscr.nj", "vscr.nj", Hex(ours.vscr, 8), Hex(ref.vscr, 8)});
  }
  for (int i = 0; i < 32; ++i) {
    uint64_t a = ours.fpr[i], b = ref.fpr[i];
    if (skip.fpr_hi >> i & 1) a &= 0xFFFFFFFFull, b &= 0xFFFFFFFFull;
    if (a == b) continue;
    if (skip.tol && Close64(a, b)) continue;
    if (skip.nan_equal && IsNaN64(a) && IsNaN64(b)) continue;
    out->push_back({"f" + std::to_string(i), IsNaN64(a) && IsNaN64(b) ? "fpr.nan" : "fpr", Hex(a), Hex(b)});
  }
  for (int i = 0; i < 128; ++i) {
    if (skip.vr[i]) continue;
    if (ours.vr[i][0] == ref.vr[i][0] && ours.vr[i][1] == ref.vr[i][1]) continue;
    bool all_close = skip.tol, all_nan = float_vectors;
    for (int k = 0; k < 4; ++k) {
      uint32_t a = (uint32_t)(ours.vr[i][k / 2] >> (k % 2 ? 0 : 32));
      uint32_t b = (uint32_t)(ref.vr[i][k / 2] >> (k % 2 ? 0 : 32));
      if (a == b) continue;
      if (!Close32(a, b)) all_close = false;
      if (!(IsNaN32(a) && IsNaN32(b))) all_nan = false;
    }
    if (all_close || (all_nan && skip.nan_equal)) continue;
    out->push_back({"v" + std::to_string(i), all_nan ? "vr.nan" : "vr",
                    Hex(ours.vr[i][0]) + Hex(ours.vr[i][1]), Hex(ref.vr[i][0]) + Hex(ref.vr[i][1])});
  }
  for (uint32_t i = 0; i < DT_MEM_BYTES; i += 8) {
    if (std::memcmp(ours.mem + i, ref.mem + i, 8) == 0) continue;
    uint64_t a = 0, b = 0;
    for (int k = 0; k < 8; ++k) a = (a << 8) | ours.mem[i + k], b = (b << 8) | ref.mem[i + k];
    char name[24];
    snprintf(name, sizeof(name), "m%03x", i);
    out->push_back({name, "mem", Hex(a), Hex(b)});
  }
}

void WordsToState(const DtCase& c, const uint64_t* w, XState* s) {
  std::memset(s, 0, sizeof(*s));
  for (int i = 0; i < 32; ++i) s->gpr[i] = w[W_GPR + i];
  s->ctr = w[W_CTR];
  s->cr = w[W_CR];
  s->xer = w[W_XER];
  s->fpscr = w[W_FPSCR];
  s->vscr = w[W_VSCR];
  for (int i = 0; i < 32; ++i) s->fpr[i] = w[W_FPR + i];
  for (int i = 0; i < 32; ++i) {
    s->vr[c.vmap[i]][0] = w[W_VR + 2 * i];
    s->vr[c.vmap[i]][1] = w[W_VR + 2 * i + 1];
  }
  for (uint32_t i = 0; i < DT_MEM_BYTES; ++i) {
    s->mem[i] = (uint8_t)(w[W_MEM + i / 8] >> (56 - 8 * (i % 8)));
  }
}

sigjmp_buf g_crash_jump;
volatile sig_atomic_t g_in_case = 0;
void CrashHandler(int sig) {
  if (g_in_case) siglongjmp(g_crash_jump, sig);
  signal(sig, SIG_DFL);
  raise(sig);
}

class Runner {
 public:
  bool Setup() {
    memory_.reset(new xe::Memory());
    memory_->Initialize();
    memory_->Reset();
    processor_.reset(new xe::cpu::Processor(memory_.get(), nullptr));
    if (!processor_->Setup(std::make_unique<xe::cpu::backend::a64::A64Backend>())) return false;
    const uint32_t rw = xe::kMemoryProtectRead | xe::kMemoryProtectWrite;
    const uint32_t commit = xe::kMemoryAllocationReserve | xe::kMemoryAllocationCommit;
    if (!memory_->LookupHeap(kCodeBase)->AllocFixed(kCodeBase, kCodeSize, 0, commit, rw)) return false;
    if (!memory_->LookupHeap(DT_MEM_BASE)->AllocFixed(DT_MEM_BASE, 0x1000, 0, commit, rw)) return false;
    auto module = std::make_unique<xe::cpu::RawModule>(processor_.get());
    module->set_name("difftest");
    module->SetAddressRange(kCodeBase, kCodeSize);
    module->set_executable(true);
    processor_->AddModule(std::move(module));
    processor_->backend()->CommitExecutableRange(kCodeBase, kCodeBase + kCodeSize);
    // A thread: its stack and PCR below the code, as the instruction tests.
    uint32_t stack_size = 64 * 1024;
    uint32_t stack_address = kCodeBase - stack_size;
    memory_->LookupHeap(stack_address - 0x2000)
        ->AllocFixed(stack_address - 0x2000, stack_size + 0x2000, 0, commit, rw);
    thread_state_.reset(new xe::cpu::ThreadState(processor_.get(), 0x100, stack_address,
                                                 stack_address - 0x1000));
    struct sigaction sa = {};
    sa.sa_handler = CrashHandler;
    sa.sa_flags = SA_NODEFER;
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGFPE}) sigaction(sig, &sa, nullptr);
    return true;
  }

  // Runs code (console instruction words, no blr) from the state `in`.
  // Returns "" or what went wrong ("nocode", "signal 11").
  std::string Run(const uint32_t* code, uint32_t count, const XState& in, XState* out) {
    if (code_at_ + (count + 1) * 4 > kCodeSize) return "code region full";
    uint32_t address = kCodeBase + code_at_;
    auto p = memory_->TranslateVirtual<uint8_t*>(address);
    for (uint32_t i = 0; i <= count; ++i) {
      xe::store_and_swap<uint32_t>(p + 4 * i, i < count ? code[i] : 0x4E800020u);
    }
    code_at_ += (count + 1) * 4;
    code_at_ = (code_at_ + 15) & ~15u;

    auto ctx = thread_state_->context();
    for (int i = 0; i < 32; ++i) ctx->r[i] = in.gpr[i];
    ctx->ctr = in.ctr;
    for (int i = 0; i < 32; ++i) {
      (&ctx->cr0.cr0_lt)[i] = (in.cr >> (31 - i)) & 1;
    }
    ctx->xer_so = (in.xer >> 31) & 1;
    ctx->xer_ov = (in.xer >> 30) & 1;
    ctx->xer_ca = (in.xer >> 29) & 1;
    ctx->fpscr.value = (uint32_t)in.fpscr;
    ctx->vscr_vec.u32[0] = ctx->vscr_vec.u32[1] = ctx->vscr_vec.u32[2] = 0;
    ctx->vscr_vec.u32[3] = (uint32_t)in.vscr;
    ctx->vscr_sat = in.vscr & DT_VSCR_SAT;
    for (int i = 0; i < 32; ++i) std::memcpy(&ctx->f[i], &in.fpr[i], 8);
    for (int i = 0; i < 128; ++i) {
      ctx->v[i].u32[0] = (uint32_t)(in.vr[i][0] >> 32);
      ctx->v[i].u32[1] = (uint32_t)in.vr[i][0];
      ctx->v[i].u32[2] = (uint32_t)(in.vr[i][1] >> 32);
      ctx->v[i].u32[3] = (uint32_t)in.vr[i][1];
    }
    std::memcpy(memory_->TranslateVirtual<uint8_t*>(DT_MEM_BASE), in.mem, DT_MEM_BYTES);

    // The back end's own per-thread state: scalar float mode, no
    // reservation, the guest's rounding mode.
    auto backend = static_cast<xe::cpu::backend::a64::A64Backend*>(processor_->backend());
    auto bctx = backend->BackendContextForGuestContext(ctx);
    bctx->flags &= ~((1u << xe::cpu::backend::a64::kA64BackendFPCRModeBit) |
                     (1u << xe::cpu::backend::a64::kA64BackendHasReserveBit));
    bctx->flags |= 1u << xe::cpu::backend::a64::kA64BackendNJMOn;
    backend->SetGuestRoundingMode(ctx, (uint32_t)in.fpscr & 7);

    std::string status;
    g_in_case = 1;
    int sig = sigsetjmp(g_crash_jump, 1);
    if (sig == 0) {
      auto fn = processor_->ResolveFunction(address);
      if (!fn) {
        status = "nocode";
      } else {
        ctx->lr = 0xBCBCBCBC;
        fn->Call(thread_state_.get(), uint32_t(ctx->lr));
      }
    } else {
      status = "signal " + std::to_string(sig);
    }
    g_in_case = 0;

    for (int i = 0; i < 32; ++i) out->gpr[i] = ctx->r[i];
    out->ctr = ctx->ctr;
    out->cr = 0;
    for (int i = 0; i < 32; ++i) {
      out->cr |= (uint64_t)((&ctx->cr0.cr0_lt)[i] & 1) << (31 - i);
    }
    out->xer = ((uint64_t)(ctx->xer_so & 1) << 31) | ((uint64_t)(ctx->xer_ov & 1) << 30) |
               ((uint64_t)(ctx->xer_ca & 1) << 29);
    out->fpscr = ctx->fpscr.value;
    out->vscr = (ctx->vscr_vec.u32[3] & DT_VSCR_NJ) | (ctx->vscr_sat & 1);
    for (int i = 0; i < 32; ++i) std::memcpy(&out->fpr[i], &ctx->f[i], 8);
    for (int i = 0; i < 128; ++i) {
      out->vr[i][0] = ((uint64_t)ctx->v[i].u32[0] << 32) | ctx->v[i].u32[1];
      out->vr[i][1] = ((uint64_t)ctx->v[i].u32[2] << 32) | ctx->v[i].u32[3];
    }
    std::memcpy(out->mem, memory_->TranslateVirtual<uint8_t*>(DT_MEM_BASE), DT_MEM_BYTES);
    return status;
  }

 private:
  std::unique_ptr<xe::Memory> memory_;
  std::unique_ptr<xe::cpu::Processor> processor_;
  std::unique_ptr<xe::cpu::ThreadState> thread_state_;
  uint32_t code_at_ = 0;
};

std::string Disasm(uint32_t word) {
  xe::StringBuffer sb;
  if (!xe::cpu::ppc::DisasmPPC(0, word, &sb)) return "?";
  return sb.to_string();
}

uint32_t Extract(uint16_t pl, uint32_t w) {
  if (pl < 0x400) return (w >> (pl & 31)) & ((1u << (pl >> 5)) - 1);
  switch (pl) {
    case PL_VD128: return dt_vd128(w);
    case PL_VA128: return ((w >> 16) & 31) | (((w >> 5) & 1) << 5) | (((w >> 10) & 1) << 6);
    case PL_VB128: return dt_vb128(w);
  }
  return 0;
}

std::string JsonEscape(const std::string& s) {
  std::string o;
  for (char ch : s) {
    if (ch == '"' || ch == '\\') o += '\\';
    if ((unsigned char)ch < 0x20) continue;
    o += ch;
  }
  return o;
}

// The registers a case's instructions name, for the report and for the
// regression line: "r3", "f1", "v77".
void NamedRegisters(const DtCase& c, std::vector<std::string>* names) {
  auto add = [&](const std::string& n) {
    for (auto& e : *names) if (e == n) return;
    names->push_back(n);
  };
  for (uint32_t i = 0; i < c.ninsn; ++i) {
    if (c.insn[i] == DT_INSN_BC) continue;
    const DtInsn& in = dt_insns[c.insn[i]];
    uint32_t w = c.codex[i];
    if (in.flags & (IF_RESL | IF_RESS)) {
      add("r" + std::to_string((w >> 21) & 31));
      add("r" + std::to_string((w >> 16) & 31));
      add("r" + std::to_string((w >> 11) & 31));
      continue;
    }
    for (int k = 0; k < in.nops; ++k) {
      uint32_t v = Extract(in.ops[k].px, w);
      switch (in.ops[k].type) {
        case T_GD: case T_GS: case T_GDS: case T_PTRU: case T_IDX:
          add("r" + std::to_string(v));
          break;
        case T_PTR:
          if (v) add("r" + std::to_string(v));
          break;
        case T_FD: case T_FS: add("f" + std::to_string(v)); break;
        case T_VD: case T_VS: case T_VDS: case T_VS8: case T_VSSH: case T_VSNJ:
          add("v" + std::to_string(v));
          break;
      }
    }
    if (in.flags & IF_MTFSF) add("f31");
    if (in.name[0] == 's' && in.name[1] == 't' && in.name[2] == 'm') {
      for (uint32_t r = (w >> 21) & 31; r < 32; ++r) add("r" + std::to_string(r));
    }
  }
}

std::string RegValue(const XState& s, const std::string& n) {
  int i = atoi(n.c_str() + 1);
  if (n[0] == 'r') return Hex(s.gpr[i]);
  if (n[0] == 'f') return Hex(s.fpr[i]);
  return Hex(s.vr[i][0]) + Hex(s.vr[i][1]);
}

bool TouchesMemory(const DtCase& c) {
  for (uint32_t i = 0; i < c.ninsn; ++i) {
    if (c.insn[i] == DT_INSN_BC) continue;
    uint8_t cls = dt_insns[c.insn[i]].cls;
    if (cls == CL_MEM || cls == CL_RESV || cls == CL_VMEM || cls == CL_V128M) return true;
  }
  return false;
}

// The mismatch's key: what dt_report.py groups by and known-issues.txt
// names. A single instruction is its mnemonic with "+oe" and "+rc" when
// those bits are set; a sequence is "seq".
std::string CaseKey(const DtCase& c) {
  if (c.tier != 1) return "seq";
  const DtInsn& in = dt_insns[c.insn[0]];
  if (in.flags & (IF_RESL | IF_RESS)) {
    std::string k = "resv:";
    for (uint32_t i = 0; i < c.ninsn; ++i) {
      const DtInsn& s = dt_insns[c.insn[i]];
      if (s.flags & (IF_RESL | IF_RESS)) k += std::string(k.size() > 5 ? "+" : "") + s.name;
    }
    return k;
  }
  std::string k = in.name;
  uint32_t w = c.codex[0];
  for (int i = 0; i < in.nops; ++i) {
    if (in.ops[i].type == T_OE && Extract(in.ops[i].px, w)) k += "+oe";
    if ((in.ops[i].type == T_RC || in.ops[i].type == T_RC6 || in.ops[i].type == T_FRC) &&
        Extract(in.ops[i].px, w)) k += "+rc";
  }
  return k;
}

struct RefStream {
  std::vector<uint8_t> data;
  size_t at = 0;
  bool Load(const std::filesystem::path& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.insert(data.end(), buf, buf + n);
    fclose(f);
    if (data.size() < 16 || std::memcmp(data.data(), "DTR1", 4) != 0) return false;
    if (U32(4) != DT_VERSION || U32(8) != 1 || U32(12) != (uint32_t)DT_NINSN) return false;
    at = 16;
    return true;
  }
  uint32_t U16(size_t o) const { return (data[o] << 8) | data[o + 1]; }
  uint32_t U32(size_t o) const { return (U16(o) << 16) | U16(o + 2); }
  // The next case's result: the initial words with the changes applied.
  bool Next(const uint64_t* init, uint64_t* words, uint32_t* trap) {
    if (at + 4 > data.size()) return false;
    *trap = U16(at);
    uint32_t n = U16(at + 2);
    at += 4;
    if (at + 10 * (size_t)n > data.size()) return false;
    std::memcpy(words, init, sizeof(uint64_t) * DT_NW);
    for (uint32_t i = 0; i < n; ++i, at += 10) {
      uint32_t k = U16(at);
      if (k >= DT_NW) return false;
      words[k] = ((uint64_t)U32(at + 2) << 32) | U32(at + 6);
    }
    return true;
  }
};

int Fuzz() {
  RefStream ref;
  if (!ref.Load(cvars::dt_ref)) {
    fprintf(stderr, "difftest: cannot read the reference stream (or it is of another build)\n");
    return 2;
  }
  // Never destroyed: see Main.
  Runner& runner = *new Runner();
  if (!runner.Setup()) {
    fprintf(stderr, "difftest: setup failed\n");
    return 2;
  }
  FILE* out = cvars::dt_out.empty() ? stdout : fopen(cvars::dt_out.c_str(), "a");
  if (!out) return 2;
  static DtCase c;
  static uint64_t ref_words[DT_NW];
  static XState init, ours, expect;
  uint64_t mismatches = 0;
  uint64_t class_cases[CL_COUNT + 1] = {}, class_mismatches[CL_COUNT + 1] = {};
  for (uint64_t index = cvars::dt_start; index < cvars::dt_start + cvars::dt_count; ++index) {
    dt_gen_case(cvars::dt_seed, index, cvars::dt_tier, &c);
    uint32_t trap = 0;
    if (!ref.Next(c.init, ref_words, &trap)) {
      fprintf(stderr, "difftest: the reference stream ends at case %llu\n", (unsigned long long)index);
      return 2;
    }
    WordsToState(c, c.init, &init);
    WordsToState(c, ref_words, &expect);
    std::string status = runner.Run(c.codex, c.nx, init, &ours);
    if (status == "code region full") {
      fprintf(stderr, "difftest: code region full at case %llu\n", (unsigned long long)index);
      return 2;
    }
    Skip skip;
    skip.gpr_hi = c.gpr_hi_skip;
    skip.fpr_hi = c.fpr_hi_skip;
    for (int i = 0; i < 32; ++i) {
      if (c.vr_skip >> i & 1) skip.vr[c.vmap[i]] = true;
    }
    skip.cr_mask = c.cr_mask;
    skip.vscr_mask = c.vscr_mask;
    skip.tol = (c.flags & CF_TOL) != 0;
    skip.nan_equal = c.tier != 1;
    bool float_vectors = false;
    for (uint32_t i = 0; i < c.ninsn; ++i) {
      if (c.insn[i] == DT_INSN_BC) continue;
      uint8_t cls = dt_insns[c.insn[i]].cls;
      if (cls == CL_VMXF || cls == CL_V128F) float_vectors = true;
    }
    std::vector<Diff> diffs;
    if (!status.empty()) {
      diffs.push_back({"run", status.rfind("signal", 0) == 0 ? "crash" : status, status, "ran"});
    } else if (trap) {
      diffs.push_back({"run", "reftrap", "ran", "exception " + Hex(trap, 4)});
    } else {
      Compare(ours, expect, skip, float_vectors, &diffs);
    }
    ++class_cases[c.tier == 1 ? c.theme : CL_COUNT];
    if (!diffs.empty()) ++class_mismatches[c.tier == 1 ? c.theme : CL_COUNT];
    if (diffs.empty() && !cvars::dt_dump) continue;
    if (!diffs.empty()) ++mismatches;

    // One JSON line.
    std::vector<std::string> regs;
    NamedRegisters(c, &regs);
    std::string j = "{\"seed\":" + std::to_string(cvars::dt_seed) + ",\"tier\":" +
                    std::to_string(c.tier) + ",\"index\":" + std::to_string(index) +
                    ",\"key\":\"" + CaseKey(c) + "\",\"class\":\"" +
                    (c.tier == 1 ? dt_class_names[c.theme] : "seq") + "\",\"code\":[";
    for (uint32_t i = 0; i < c.nx; ++i) j += std::string(i ? "," : "") + "\"" + Hex(c.codex[i], 8) + "\"";
    j += "],\"asm\":[";
    for (uint32_t i = 0; i < c.nx; ++i) j += std::string(i ? "," : "") + "\"" + JsonEscape(Disasm(c.codex[i])) + "\"";
    j += "],\"in\":{";
    for (auto& n : regs) j += "\"" + n + "\":\"" + RegValue(init, n) + "\",";
    j += "\"cr\":\"" + Hex(init.cr, 8) + "\",\"xer\":\"" + Hex(init.xer, 8) + "\",\"ctr\":\"" +
         Hex(init.ctr) + "\",\"fpscr\":\"" + Hex(init.fpscr, 8) + "\",\"vscr\":\"" + Hex(init.vscr, 8) + "\"";
    if (TouchesMemory(c)) {
      j += ",\"mem\":\"";
      for (uint32_t i = 0; i < DT_MEM_BYTES; ++i) j += Hex(init.mem[i], 2);
      j += "\"";
    }
    j += "},\"diff\":[";
    for (size_t i = 0; i < diffs.size(); ++i) {
      j += std::string(i ? "," : "") + "{\"name\":\"" + diffs[i].name + "\",\"kind\":\"" + diffs[i].kind +
           "\",\"ours\":\"" + diffs[i].ours + "\",\"ref\":\"" + diffs[i].ref + "\"}";
    }
    // What the reference changed: the regression line's expected results.
    j += "],\"refout\":{";
    bool first = true;
    auto put = [&](const std::string& n, const std::string& v) {
      j += std::string(first ? "" : ",") + "\"" + n + "\":\"" + v + "\"";
      first = false;
    };
    for (int i = 0; i < 32; ++i) {
      if (expect.gpr[i] != init.gpr[i]) put("r" + std::to_string(i), Hex(expect.gpr[i]));
      if (expect.fpr[i] != init.fpr[i]) put("f" + std::to_string(i), Hex(expect.fpr[i]));
    }
    for (int i = 0; i < 128; ++i) {
      if (skip.vr[i]) continue;
      if (expect.vr[i][0] != init.vr[i][0] || expect.vr[i][1] != init.vr[i][1])
        put("v" + std::to_string(i), Hex(expect.vr[i][0]) + Hex(expect.vr[i][1]));
    }
    if (expect.cr != init.cr) put("cr", Hex(expect.cr, 8));
    if (expect.xer != init.xer) put("xer", Hex(expect.xer, 8));
    if (expect.ctr != init.ctr) put("ctr", Hex(expect.ctr));
    if ((expect.fpscr ^ init.fpscr) & DT_FPSCR_MASK) put("fpscr", Hex(expect.fpscr & DT_FPSCR_MASK, 8));
    if (expect.vscr != init.vscr) put("vscr", Hex(expect.vscr, 8));
    for (uint32_t i = 0; i < DT_MEM_BYTES; i += 8) {
      if (std::memcmp(expect.mem + i, init.mem + i, 8) == 0) continue;
      uint64_t b = 0;
      for (int k = 0; k < 8; ++k) b = (b << 8) | expect.mem[i + k];
      char name[24];
      snprintf(name, sizeof(name), "m%03x", i);
      put(name, Hex(b));
    }
    j += "},\"skip\":{\"gprhi\":\"" + Hex(skip.gpr_hi, 8) + "\",\"fprhi\":\"" + Hex(skip.fpr_hi, 8) +
         "\",\"crmask\":\"" + Hex(skip.cr_mask, 8) + "\",\"vscrmask\":\"" + Hex(skip.vscr_mask, 8) + "\",\"tol\":" + (skip.tol ? "1" : "0") +
         ",\"nanequal\":" + (skip.nan_equal ? "1" : "0") + ",\"floatvec\":" + (float_vectors ? "1" : "0") + ",\"vr\":[";
    first = true;
    for (int i = 0; i < 128; ++i) {
      if (!skip.vr[i]) continue;
      bool named = false;
      for (auto& n : regs) named |= n == "v" + std::to_string(i);
      if (!named) continue;
      j += std::string(first ? "" : ",") + std::to_string(i);
      first = false;
    }
    j += "]}}\n";
    fputs(j.c_str(), out);
  }
  {
    std::string j = "{\"stats\":1,\"seed\":" + std::to_string(cvars::dt_seed) + ",\"tier\":" +
                    std::to_string(cvars::dt_tier) + ",\"start\":" + std::to_string(cvars::dt_start) +
                    ",\"count\":" + std::to_string(cvars::dt_count) + ",\"classes\":{";
    for (int i = 0; i <= CL_COUNT; ++i) {
      j += std::string(i ? "," : "") + "\"" + (i < CL_COUNT ? dt_class_names[i] : "seq") + "\":[" +
           std::to_string(class_cases[i]) + "," + std::to_string(class_mismatches[i]) + "]";
    }
    j += "}}\n";
    fputs(j.c_str(), out);
  }
  if (out != stdout) fclose(out);
  fprintf(stderr, "difftest: seed %llu tier %u cases %llu..%llu: %llu mismatches\n",
          (unsigned long long)cvars::dt_seed, cvars::dt_tier, (unsigned long long)cvars::dt_start,
          (unsigned long long)(cvars::dt_start + cvars::dt_count - 1), (unsigned long long)mismatches);
  return 0;
}

// ---- fixed cases
//
// One case a line of regressions.txt:
//   name code=<word>,<word> in: r3=<hex> f1=<hex> v77=<hex> cr=.. xer=.. ctr=..
//     fpscr=.. vscr=.. mem=<the whole window> out: r3=<hex> cr=.. m1f8=<hex>
//     skip: gprhi=<mask> fprhi=<mask> crmask=<mask> tol=1 v77
// Registers not named start as zero (VSCR as non-Java mode), the window as
// zero. After the run every register and the window must be as "out:" says,
// or unchanged where it says nothing, except what "skip:" leaves out.
uint64_t ParseHex(const std::string& s) { return strtoull(s.c_str(), nullptr, 16); }

bool SetField(XState* s, const std::string& key, const std::string& value) {
  if (key == "cr") s->cr = ParseHex(value);
  else if (key == "xer") s->xer = ParseHex(value);
  else if (key == "ctr") s->ctr = ParseHex(value);
  else if (key == "fpscr") s->fpscr = (s->fpscr & ~(uint64_t)DT_FPSCR_MASK) | (ParseHex(value) & DT_FPSCR_MASK);
  else if (key == "vscr") s->vscr = ParseHex(value);
  else if (key == "mem") {
    if (value.size() != DT_MEM_BYTES * 2) return false;
    for (uint32_t i = 0; i < DT_MEM_BYTES; ++i) s->mem[i] = (uint8_t)ParseHex(value.substr(2 * i, 2));
  } else if (key[0] == 'm') {
    uint32_t off = (uint32_t)ParseHex(key.substr(1));
    uint64_t v = ParseHex(value);
    if (off + 8 > DT_MEM_BYTES) return false;
    for (int k = 0; k < 8; ++k) s->mem[off + k] = (uint8_t)(v >> (56 - 8 * k));
  } else if (key[0] == 'r') s->gpr[atoi(key.c_str() + 1) & 31] = ParseHex(value);
  else if (key[0] == 'f') s->fpr[atoi(key.c_str() + 1) & 31] = ParseHex(value);
  else if (key[0] == 'v') {
    if (value.size() != 32) return false;
    int n = atoi(key.c_str() + 1) & 127;
    s->vr[n][0] = ParseHex(value.substr(0, 16));
    s->vr[n][1] = ParseHex(value.substr(16));
  } else return false;
  return true;
}

int Regress() {
  FILE* f = fopen(cvars::dt_regress.c_str(), "r");
  if (!f) {
    fprintf(stderr, "difftest: cannot open %s\n", cvars::dt_regress.c_str());
    return 2;
  }
  // Never destroyed: see Main.
  Runner& runner = *new Runner();
  if (!runner.Setup()) return 2;
  static char line[3 * DT_MEM_BYTES + 8192];
  int failed = 0, ran = 0;
  static XState init, expect, ours;
  while (fgets(line, sizeof(line), f)) {
    if (line[0] == '#' || line[0] == '\n') continue;
    std::vector<std::string> tok;
    for (char* t = strtok(line, " \t\r\n"); t; t = strtok(nullptr, " \t\r\n")) tok.push_back(t);
    if (tok.size() < 2) continue;
    std::memset(&init, 0, sizeof(init));
    init.vscr = DT_VSCR_NJ;
    Skip skip;
    std::vector<uint32_t> code;
    std::vector<std::pair<std::string, std::string>> outs;
    int section = 0;
    bool ok = true, float_vectors = false;
    for (size_t i = 1; i < tok.size() && ok; ++i) {
      const std::string& t = tok[i];
      if (t == "in:") { section = 1; continue; }
      if (t == "out:") { section = 2; continue; }
      if (t == "skip:") { section = 3; continue; }
      size_t eq = t.find('=');
      std::string key = t.substr(0, eq), value = eq == std::string::npos ? "" : t.substr(eq + 1);
      if (section == 0 && key == "code") {
        for (size_t at = 0; at < value.size(); at += 9) code.push_back((uint32_t)ParseHex(value.substr(at, 8)));
      } else if (section == 0 && key == "floatvec") {
        float_vectors = true;
      } else if (section == 1) {
        ok = SetField(&init, key, value);
      } else if (section == 2) {
        outs.emplace_back(key, value);
      } else if (section == 3) {
        if (key == "gprhi") skip.gpr_hi = (uint32_t)ParseHex(value);
        else if (key == "fprhi") skip.fpr_hi = (uint32_t)ParseHex(value);
        else if (key == "crmask") skip.cr_mask = (uint32_t)ParseHex(value);
        else if (key == "vscrmask") skip.vscr_mask = (uint32_t)ParseHex(value);
        else if (key == "tol") skip.tol = true;
        else if (key == "nanequal") skip.nan_equal = true;
        else if (key[0] == 'v') skip.vr[atoi(key.c_str() + 1) & 127] = true;
        else ok = false;
      }
    }
    expect = init;
    for (auto& o : outs) ok = ok && SetField(&expect, o.first, o.second);
    if (!ok || code.empty()) {
      fprintf(stderr, "FAIL %s: the line does not parse\n", tok[0].c_str());
      ++failed;
      continue;
    }
    std::string status = runner.Run(code.data(), (uint32_t)code.size(), init, &ours);
    std::vector<Diff> diffs;
    if (!status.empty()) diffs.push_back({"run", status, status, "ran"});
    else Compare(ours, expect, skip, float_vectors, &diffs);
    ++ran;
    if (diffs.empty()) {
      printf("ok   %s\n", tok[0].c_str());
      continue;
    }
    ++failed;
    printf("FAIL %s:", tok[0].c_str());
    for (uint32_t w : code) printf(" [%s]", Disasm(w).c_str());
    printf("\n");
    for (auto& d : diffs) printf("       %-8s ours %s  expected %s\n", d.name.c_str(), d.ours.c_str(), d.ref.c_str());
  }
  fclose(f);
  printf("regressions: %d run, %d failed\n", ran, failed);
  return failed ? 1 : 0;
}

int Main(const std::vector<std::string>& args) {
  if (!cvars::log_file.empty()) xe::InitializeLogging("xenia-jit-difftest");
  int status = cvars::dt_regress.empty() ? Fuzz() : Regress();
  // Leave without taking the processor down: nothing is left to save, and
  // its teardown has been seen to spin after a case that ended in a signal.
  if (!cvars::log_file.empty()) xe::ShutdownLogging();
  fflush(nullptr);
  _exit(status);
}

}  // namespace

XE_DEFINE_CONSOLE_APP("xenia-jit-difftest", Main, "", "");
