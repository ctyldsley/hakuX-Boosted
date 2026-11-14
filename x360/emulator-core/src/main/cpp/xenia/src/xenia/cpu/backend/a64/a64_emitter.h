/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_BACKEND_A64_A64_EMITTER_H_
#define XENIA_CPU_BACKEND_A64_A64_EMITTER_H_

#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "xenia/base/arena.h"
#include "xenia/cpu/backend/code_cache_base.h"
#include "xenia/cpu/function.h"
#include "xenia/cpu/function_trace_data.h"
#include "xenia/cpu/hir/hir_builder.h"
#include "xenia/cpu/hir/instr.h"
#include "xenia/cpu/hir/value.h"
#include "xenia/cpu/xex_module.h"
#include "xenia/memory.h"

#include "xbyak_aarch64.h"

namespace xe {
namespace cpu {
class Processor;
}  // namespace cpu
}  // namespace xe

namespace xe {
namespace cpu {
namespace backend {
namespace a64 {
using namespace arm64;
class A64Backend;
class A64CodeCache;

enum class FPCRMode : uint32_t { Unknown, Fpu, Vmx };

// Unfortunately due to the design of xbyak we have to pass this to the ctor.
class XbyakA64Allocator : public Xbyak_aarch64::Allocator {
 public:
  virtual bool useProtect() const { return false; }
};

class A64Emitter;
using TailEmitCallback =
    std::function<void(A64Emitter& e, Xbyak_aarch64::Label& lbl)>;
struct TailEmitter {
  Xbyak_aarch64::Label label;
  uint32_t alignment;
  TailEmitCallback func;
};

class A64Emitter : public Xbyak_aarch64::CodeGenerator {
 public:
  A64Emitter(A64Backend* backend, XbyakA64Allocator* allocator);
  virtual ~A64Emitter();

  Processor* processor() const { return processor_; }
  A64Backend* backend() const { return backend_; }

  bool Emit(GuestFunction* function, hir::HIRBuilder* builder,
            uint32_t debug_info_flags, FunctionDebugInfo* debug_info,
            void** out_code_address, size_t* out_code_size,
            std::vector<SourceMapEntry>* out_source_map);

 public:
  // Reserved: sp, x19 (backend context), x20 (context), x21 (membase)
  // Scratch: x0-x18 (caller-saved), v0-v3
  // Available GPRs for register allocator: x22-x28
  static constexpr int GPR_COUNT = 7;
  // Available VEC regs: v4-v15, v16-v31
  static constexpr int VEC_COUNT = 28;
  static constexpr size_t kStashOffset = 32;

  // A value that reached codegen without a register assignment indexes the maps
  // out of bounds, which otherwise surfaces far away as an unencodable-register
  // exception. Report the culprit here, where it is still in hand.
  static uint32_t MapReg(const hir::Value* v, const uint32_t* map, int count,
                         const char* set_name);

  static void SetupReg(const hir::Value* v, Xbyak_aarch64::WReg& r) {
    r = Xbyak_aarch64::WReg(MapReg(v, gpr_reg_map_, GPR_COUNT, "gpr"));
  }
  static void SetupReg(const hir::Value* v, Xbyak_aarch64::XReg& r) {
    r = Xbyak_aarch64::XReg(MapReg(v, gpr_reg_map_, GPR_COUNT, "gpr"));
  }
  static void SetupReg(const hir::Value* v, Xbyak_aarch64::SReg& r) {
    r = Xbyak_aarch64::SReg(MapReg(v, vec_reg_map_, VEC_COUNT, "vec"));
  }
  static void SetupReg(const hir::Value* v, Xbyak_aarch64::DReg& r) {
    r = Xbyak_aarch64::DReg(MapReg(v, vec_reg_map_, VEC_COUNT, "vec"));
  }
  static void SetupReg(const hir::Value* v, Xbyak_aarch64::QReg& r) {
    r = Xbyak_aarch64::QReg(MapReg(v, vec_reg_map_, VEC_COUNT, "vec"));
  }
  static void SetupReg(const hir::Value* v, Xbyak_aarch64::VReg& r) {
    r = Xbyak_aarch64::VReg(MapReg(v, vec_reg_map_, VEC_COUNT, "vec"));
  }

  Xbyak_aarch64::Label& epilog_label() { return *epilog_label_; }

  FunctionDebugInfo* debug_info() const { return debug_info_; }
  size_t stack_size() const { return stack_size_; }

  void MarkSourceOffset(const hir::Instr* i);

  void DebugBreak();
  void Trap(uint16_t trap_type = 0);
  void UnimplementedInstr(const hir::Instr* i);

  void Call(const hir::Instr* instr, GuestFunction* function);
  void CallIndirect(const hir::Instr* instr, int reg_index);
  void CallExtern(const hir::Instr* instr, const Function* function);
  void CallNative(void* fn);
  void CallNativeSafe(void* fn);
  void SetReturnAddress(uint64_t value);

  // Backend context register = x19.
  // Points to A64BackendContext (immediately before PPCContext in memory).
  const Xbyak_aarch64::XReg& GetBackendCtxReg() const { return x19; }
  // Context register = x20.
  const Xbyak_aarch64::XReg& GetContextReg() const { return x20; }
  // Memory base register = x21.
  const Xbyak_aarch64::XReg& GetMembaseReg() const { return x21; }

  void ReloadMembase();

  void PushStackpoint();
  void PopStackpoint();
  void EnsureSynchronizedGuestAndHostStack();

  static void HandleStackpointOverflowError(ppc::PPCContext* context);

  void ForgetFpcrMode() {
    if (fpcr_mode_ == FPCRMode::Vmx) {
      ChangeFpcrMode(FPCRMode::Fpu);
    }
    fpcr_mode_ = FPCRMode::Unknown;
  }
  bool ChangeFpcrMode(FPCRMode new_mode, bool already_set = false);
  bool IsFeatureEnabled(uint64_t feature_flag) const {
    return (feature_flags_ & feature_flag) == feature_flag;
  }

  Xbyak_aarch64::Label& AddToTail(TailEmitCallback callback,
                                  uint32_t alignment = 0);
  Xbyak_aarch64::Label& NewCachedLabel();

  // Emits a cooperative-scheduler preemption safepoint: yields the fiber once
  // the context's preempt_requested flag is raised. Only valid at a block head.
  // guest_address is stamped into the context for wedge diagnosis when
  // log_safepoint_pc is on; 0 means unknown.
  void EmitPreemptCheck(uint32_t guest_address = 0);

  // ARM64 conditional branches (cbz/cbnz: ±1 MiB, tbz/tbnz: ±32 KiB,
  // b.cond: ±1 MiB) can fall short of their target in large guest functions.
  // These shadows emit the safe pattern `<inverse> skip; b target; skip:`,
  // routing the long branch through unconditional b (±128 MiB). The
  // int64_t-immediate overloads remain available via the using-declarations
  // for hand-tuned thunks that pass literal byte offsets.
  using Xbyak_aarch64::CodeGenerator::b;
  using Xbyak_aarch64::CodeGenerator::cbnz;
  using Xbyak_aarch64::CodeGenerator::cbz;
  using Xbyak_aarch64::CodeGenerator::tbnz;
  using Xbyak_aarch64::CodeGenerator::tbz;
  void b(const Xbyak_aarch64::Cond cond, const Xbyak_aarch64::Label& label);
  void cbz(const Xbyak_aarch64::WReg& rt, const Xbyak_aarch64::Label& label);
  void cbz(const Xbyak_aarch64::XReg& rt, const Xbyak_aarch64::Label& label);
  void cbnz(const Xbyak_aarch64::WReg& rt, const Xbyak_aarch64::Label& label);
  void cbnz(const Xbyak_aarch64::XReg& rt, const Xbyak_aarch64::Label& label);
  void tbz(const Xbyak_aarch64::WReg& rt, uint32_t imm,
           const Xbyak_aarch64::Label& label);
  void tbz(const Xbyak_aarch64::XReg& rt, uint32_t imm,
           const Xbyak_aarch64::Label& label);
  void tbnz(const Xbyak_aarch64::WReg& rt, uint32_t imm,
            const Xbyak_aarch64::Label& label);
  void tbnz(const Xbyak_aarch64::XReg& rt, uint32_t imm,
            const Xbyak_aarch64::Label& label);

  // Get or create a xbyak_aarch64 label for a HIR label ID.
  Xbyak_aarch64::Label& GetLabel(uint32_t label_id);

  XexModule* GuestModule() { return guest_module_; }

  // hakuX-Boosted: guest registers held in host registers for the whole of
  // the function being emitted (pin_guest_registers). See the block comment
  // above A64Emitter::EmitPinnedFlush in a64_emitter.cc for the rules.
  struct PinnedGuestRegister {
    uint32_t offset;    // offset in PPCContext
    hir::TypeName type;  // INT64_TYPE, INT8_TYPE, FLOAT64_TYPE or VEC128_TYPE
    uint32_t host_reg;  // x or v register number
    bool written;       // the function stores to it
    uint32_t bit;       // its bit in the masks below
  };
  // The pinned register for a context access of exactly this offset and
  // type, or null. Always null when nothing is pinned.
  const PinnedGuestRegister* FindPinnedGuestRegister(uint32_t offset,
                                                     hir::TypeName type) const {
    for (const auto& pin : pins_) {
      if (pin.offset == offset && pin.type == type) {
        return &pin;
      }
    }
    return nullptr;
  }
  // hakuX-Boosted: a64_fuse_compare_branch. An integer compare whose only
  // use is the conditional branch that follows it (with nothing between
  // that could change the flags) leaves its answer in the flags instead of
  // a register: the compare's sequence calls EmitCompareResult, the
  // branch's asks TakeFusedCompare.
  void EmitCompareResult(const Xbyak_aarch64::WReg& dest,
                         Xbyak_aarch64::Cond cond) {
    if (fuse_compare_pending_) {
      fuse_compare_pending_ = false;
      fused_cond_ = cond;
      fused_cond_valid_ = true;
      return;
    }
    cset(dest, cond);
  }
  bool TakeFusedCompare(const hir::Instr* branch, Xbyak_aarch64::Cond* cond) {
    if (!fused_cond_valid_ || fused_branch_ != branch) {
      return false;
    }
    fused_cond_valid_ = false;
    *cond = fused_cond_;
    return true;
  }

  // a64_pin_verify: stores to pinned registers also go to the context, and
  // loads compare the two.
  bool pin_verify() const { return pin_verify_; }
  // Context <- the pinned registers named by the mask.
  void EmitPinnedFlush(uint32_t mask);
  // The pinned registers named by the mask <- context.
  void EmitPinnedReload(uint32_t mask);
  // What a write-back and a reload at the instruction being emitted cover:
  // without pin_guest_registers_wide every register the function writes and
  // every register; with it, what the pass worked out for this instruction.
  uint32_t pin_flush_mask() const { return pin_cur_dirty_; }
  uint32_t pin_reload_mask() const { return pin_cur_live_after_; }
  // Round a host function called with a bare blr instead of through the
  // guest-to-host thunk (which is where CallNativeSafe does this itself).
  void EmitPinnedSyncBeforeBareHostCall() {
    if (pins_live_) {
      EmitPinnedFlush(pin_cur_dirty_);
    }
  }
  void EmitPinnedSyncAfterBareHostCall() {
    if (pins_live_) {
      EmitPinnedReload(pin_cur_live_after_);
    }
  }
  // a64_pin_verify: compare the pinned register with the context copy and
  // report a difference. Clobbers x0, x1, v0 and nothing else.
  void EmitPinnedVerify(const PinnedGuestRegister& pin);
  // hakuX-Boosted: dead_context_store_verify. After a context load of an
  // 8-bit or 64-bit integer slot: report if the value is the marker that
  // mode stores in place of a store it took for dead. Clobbers x0 to x2 and
  // x17; no flag.
  void EmitDeadStoreVerify(const Xbyak_aarch64::XReg& value, uint32_t offset,
                           uint32_t size);

 protected:
  void* Emplace(const EmitFunctionInfo& func_info,
                GuestFunction* function = nullptr);
  // Drops the code buffer, tail entries and both label pools. Both the success
  // path and a failed compile must run it, or stale labels carry over.
  void ResetPerFunctionState();
  bool Emit(hir::HIRBuilder* builder, EmitFunctionInfo& func_info);

 protected:
  Processor* processor_ = nullptr;
  A64Backend* backend_ = nullptr;
  A64CodeCache* code_cache_ = nullptr;
  XbyakA64Allocator* allocator_ = nullptr;
  XexModule* guest_module_ = nullptr;
  uint64_t feature_flags_ = 0;
  uint32_t current_guest_function_ = 0;
  bool log_guest_call_ = false;

  Xbyak_aarch64::Label* epilog_label_ = nullptr;

  hir::Instr* current_instr_ = nullptr;

  FunctionDebugInfo* debug_info_ = nullptr;
  uint32_t debug_info_flags_ = 0;
  FunctionTraceData* trace_data_ = nullptr;
  Arena source_map_arena_;

  size_t stack_size_ = 0;

  static const uint32_t gpr_reg_map_[GPR_COUNT];
  static const uint32_t vec_reg_map_[VEC_COUNT];

  std::vector<TailEmitter> tail_code_;
  std::vector<Xbyak_aarch64::Label*> label_cache_;

  // Map from HIR label IDs to xbyak_aarch64 Labels.
  std::unordered_map<uint32_t, Xbyak_aarch64::Label*> label_map_;

  // hakuX-Boosted: code offsets of the block labels placed so far in the
  // function being emitted, for one-instruction backward branches
  // (a64_short_backward_branches). Cleared with the labels.
  std::unordered_map<const Xbyak_aarch64::Label*, size_t> bound_label_offsets_;
  // Reach of b.cond/cbz/cbnz (imm19) and tbz/tbnz (imm14) backwards, less a
  // margin of four instructions.
  static constexpr size_t kNearBranch19 = (size_t(1) << 20) - 16;
  static constexpr size_t kNearBranch14 = (size_t(1) << 15) - 16;
  bool IsNearBoundLabel(const Xbyak_aarch64::Label& label,
                        size_t max_distance) const;
  // hakuX-Boosted: a64_short_forward_branches. True if a conditional branch
  // to this label may take the one-instruction form: it is near and placed
  // (as above), or it is a block label of this function not yet placed and
  // this attempt at the function allows that (see Emit: a function too
  // large for it is emitted a second time without).
  bool UseShortBranch(const Xbyak_aarch64::Label& label, size_t max_distance);
  std::unordered_set<const Xbyak_aarch64::Label*> block_labels_;
  bool short_forward_ok_ = false;
  // The locals' stack offsets are assigned once per function (see Emit).
  bool locals_laid_out_ = false;
  size_t locals_stack_size_ = 0;
  bool used_short_forward19_ = false;
  bool used_short_forward14_ = false;

  // hakuX-Boosted: what Emit's loop does round an instruction when guest
  // registers are pinned.
  enum class PinSync { kNone, kFlush, kFlushAndReload };
  static PinSync ClassifyPinSync(const hir::Instr* instr);
  // hakuX-Boosted: see FindPinnedGuestRegister.
  std::vector<PinnedGuestRegister> pins_;
  // True while emitting code that runs with the pinned registers loaded:
  // from the end of the prologue to the last body instruction.
  bool pins_live_ = false;
  bool pin_verify_ = false;
  // hakuX-Boosted: the masks for the instruction being emitted (see
  // pin_flush_mask), and the reload mask of the last instruction that was a
  // way out and back in, for the resume stub of its return site.
  uint32_t pin_cur_dirty_ = 0;
  uint32_t pin_cur_live_after_ = 0;
  uint32_t pin_last_sync_reload_ = 0;
  // log_translation_stats: count executed blocks (see EmitBlockCounter).
  bool count_blocks_ = false;
  void EmitBlockCounter();
  struct PinnedResumeStub {
    uint32_t guest_address;
    Xbyak_aarch64::Label* target;
    uint32_t code_offset;
    uint32_t reload_mask;
  };
  std::vector<PinnedResumeStub> pinned_resume_stubs_;

  // hakuX-Boosted: see EmitCompareResult.
  bool fuse_compare_pending_ = false;
  bool fused_cond_valid_ = false;
  Xbyak_aarch64::Cond fused_cond_ = Xbyak_aarch64::EQ;
  const hir::Instr* fused_branch_ = nullptr;
  // The branch a compare may be fused with, or null.
  const hir::Instr* FindFusableBranch(const hir::Instr* compare) const;

  FPCRMode fpcr_mode_ = FPCRMode::Unknown;
  bool synchronize_stack_on_next_instruction_ = false;
};

}  // namespace a64
}  // namespace backend
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_BACKEND_A64_A64_EMITTER_H_
