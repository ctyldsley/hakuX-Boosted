/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/backend/a64/a64_emitter.h"

#include <cstring>

#include "xenia/base/debugging.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/profiling.h"
#include "xenia/cpu/backend/a64/a64_backend.h"
#include "xenia/cpu/backend/a64/a64_code_cache.h"
#include "xenia/cpu/backend/a64/a64_function.h"
#include "xenia/cpu/backend/a64/a64_sequences.h"
#include "xenia/cpu/backend/a64/a64_stack_layout.h"
#include "xenia/cpu/backend/a64/a64_tracers.h"
#include "xenia/cpu/compiler/translation_stats.h"
#include "xenia/cpu/cpu_flags.h"
#include "xenia/cpu/hir/hir_builder.h"
#include "xenia/cpu/hir/label.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/processor.h"

DECLARE_int64(a64_max_stackpoints);
DECLARE_bool(a64_enable_host_guest_stack_synchronization);

DECLARE_bool(a64_lean_extern_thunk);

// hakuX-Boosted: see A64Emitter::IsNearBoundLabel.
DEFINE_bool(
    a64_short_backward_branches, false,
    "A conditional branch (b.cond, cbz, cbnz, tbz, tbnz) to a block label "
    "already placed earlier in the same function is emitted as the one "
    "instruction it is when the distance is known to be in range, instead of "
    "an inverted branch over an unconditional one. Loop back-edges are the "
    "common case. Forward branches, whose distance is not known when they are "
    "emitted, keep the two-instruction form. Takes effect for functions "
    "translated after it is set.",
    "a64");

// hakuX-Boosted: see A64Emitter::PushStackpoint.
DEFINE_bool(
    a64_compact_stackpoint_push, false,
    "The guest function prologue's stackpoint push indexes its 16-byte "
    "entries with a shifted add instead of materialising the entry size and "
    "multiplying: two instructions fewer in every translated function's "
    "prologue, the same address. Takes effect for functions translated after "
    "it is set.",
    "a64");

// hakuX-Boosted: see A64Emitter::EmitPinnedVerify.
DEFINE_bool(
    a64_pin_verify, false,
    "Self-check for pin_guest_registers, for a debugging run only: every "
    "store to a pinned guest register also goes to the guest context as it "
    "would without pinning, and every load of one compares the host register "
    "with the context copy first. A difference means a pinned register was "
    "lost or went stale; it is logged (\"PinVerify:\", with the guest "
    "function and the context offset) and the register is reloaded from the "
    "context, which stays authoritative in this mode. Slower than not "
    "pinning at all. Takes effect for functions translated after it is set.",
    "a64");

DEFINE_bool(
    log_safepoint_pc, false,
    "Record the guest address of every JIT safepoint a fiber passes, so the "
    "cooperative scheduler's no-progress report can name where a wedged "
    "fiber last checked in rather than only its link register. Costs a "
    "materialize and a store on every loop back-edge; diagnostic only.",
    "CPU");

namespace xe {
namespace cpu {
namespace backend {
namespace a64 {

using namespace Xbyak_aarch64;

// Defined in a64_backend.cc.
extern uint64_t ResolveFunction(void* raw_context, uint64_t target_address);

static uint64_t UndefinedCallExtern(void* raw_context, uint64_t function_ptr) {
  auto function = reinterpret_cast<Function*>(function_ptr);
  XELOGE("undefined extern call to {:08X} {}", function->address(),
         function->name());
  return 0;
}

// Max host code per translated guest function. 1 MiB was too small for large,
// FP-heavy guest functions (each PowerPC FP op emits NaN-handling overhead),
// which overflowed the Xbyak buffer -> uncaught Xbyak_aarch64::Error "code is
// too big" -> terminate. The buffer is a single per-emitter allocation reused
// (reset) across functions, so enlarging it is cheap.
static constexpr size_t kMaxCodeSize = 4_MiB;

// Register maps:
// GPR allocatable registers: x22, x23, x24, x25, x26, x27, x28
// (x19=backend context, x20=context, x21=membase are reserved)
const uint32_t A64Emitter::gpr_reg_map_[GPR_COUNT] = {
    22, 23, 24, 25, 26, 27, 28,
};

// VEC allocatable registers: v4-v15, v16-v31
// (v0-v3 are scratch)
const uint32_t A64Emitter::vec_reg_map_[VEC_COUNT] = {
    4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17,
    18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
};

A64Emitter::A64Emitter(A64Backend* backend, XbyakA64Allocator* allocator)
    : CodeGenerator(kMaxCodeSize, Xbyak_aarch64::AutoGrow, allocator),
      processor_(backend->processor()),
      backend_(backend),
      code_cache_(backend->code_cache()),
      allocator_(allocator),
      feature_flags_(arm64::GetFeatureFlags()) {}

A64Emitter::~A64Emitter() = default;

bool A64Emitter::Emit(GuestFunction* function, hir::HIRBuilder* builder,
                      uint32_t debug_info_flags, FunctionDebugInfo* debug_info,
                      void** out_code_address, size_t* out_code_size,
                      std::vector<SourceMapEntry>* out_source_map) {
  SCOPE_profile_cpu_f("cpu");

  guest_module_ = dynamic_cast<XexModule*>(function->module());

  debug_info_ = debug_info;
  debug_info_flags_ = debug_info_flags;
  trace_data_ = &function->trace_data();

  current_guest_function_ = function->address();
  log_guest_call_ = !cvars::log_guest_calls_at.empty() &&
                    GuestAddressInList(cvars::log_guest_calls_at,
                                       current_guest_function_);

  // Reset state.
  stack_size_ = StackLayout::GUEST_STACK_SIZE;
  source_map_arena_.Reset();
  tail_code_.clear();
  fpcr_mode_ = FPCRMode::Unknown;

  // The prolog, epilog and helpers emit outside the per-opcode guard below, so
  // an unencodable operand needs catching here too.
  EmitFunctionInfo func_info = {};
  bool emitted = false;
  try {
    emitted = Emit(builder, func_info);
  } catch (const Xbyak_aarch64::Error& e) {
    XELOGE("A64: assembler error while emitting guest function {:08X}: {}",
           current_guest_function_, e.what());
    emitted = false;
  }
  if (!emitted) {
    // Emplace only runs on success, so a failed compile has to reset too.
    ResetPerFunctionState();
    return false;
  }

  // hakuX-Boosted: a function with pinned guest registers tells
  // ResolveFunction where its return sites can be re-entered, before the code
  // becomes reachable.
  if (!pins_.empty()) {
    std::vector<std::pair<uint32_t, uint32_t>> stubs;
    stubs.reserve(pinned_resume_stubs_.size());
    for (const auto& stub : pinned_resume_stubs_) {
      stubs.emplace_back(stub.guest_address, stub.code_offset);
    }
    static_cast<A64Function*>(function)->SetPinnedResumeStubs(
        std::move(stubs));
  }

  // Emplace the code into the code cache.
  *out_code_address = Emplace(func_info, function);
  *out_code_size = func_info.code_size.total;

  // Copy source map.
  source_map_arena_.CloneContents(out_source_map);

  return *out_code_address != nullptr;
}

bool A64Emitter::Emit(hir::HIRBuilder* builder, EmitFunctionInfo& func_info) {
  // hakuX-Boosted: the guest registers this function keeps in host
  // registers, as GuestRegisterPinningPass chose them. Empty unless
  // pin_guest_registers is on.
  pins_.clear();
  pins_live_ = false;
  pin_verify_ = cvars::a64_pin_verify;
  pinned_resume_stubs_.clear();
  for (const auto& slot : builder->pinned_context_slots()) {
    PinnedGuestRegister pin;
    pin.offset = slot.offset;
    pin.type = slot.type;
    pin.written = slot.written;
    if (slot.reg_set == 0) {
      if (slot.reg_index >= GPR_COUNT || slot.type != hir::INT64_TYPE) {
        XELOGE("A64: bad pinned integer slot in {:08X}",
               current_guest_function_);
        return false;
      }
      pin.host_reg = gpr_reg_map_[slot.reg_index];
    } else {
      if (slot.reg_index >= VEC_COUNT || (slot.type != hir::FLOAT64_TYPE &&
                                          slot.type != hir::VEC128_TYPE)) {
        XELOGE("A64: bad pinned vector slot in {:08X}",
               current_guest_function_);
        return false;
      }
      pin.host_reg = vec_reg_map_[slot.reg_index];
    }
    pins_.push_back(pin);
  }

  // Calculate local variable stack offsets.
  auto locals = builder->locals();
  size_t stack_offset = StackLayout::GUEST_STACK_SIZE;
  for (auto it = locals.begin(); it != locals.end(); ++it) {
    auto slot = *it;
    size_t type_size = hir::GetTypeSize(slot->type);
    // Align to natural size (at least 4 bytes for ARM64 alignment).
    size_t align_size = xe::round_up(type_size, static_cast<size_t>(4));
    stack_offset = xe::align(stack_offset, align_size);
    slot->set_constant(static_cast<uint32_t>(stack_offset));
    stack_offset += type_size;
  }
  // Align total stack offset to 16 bytes (ARM64 ABI requirement).
  stack_offset -= StackLayout::GUEST_STACK_SIZE;
  stack_offset = xe::align(stack_offset, static_cast<size_t>(16));

  const size_t stack_size = StackLayout::GUEST_STACK_SIZE + stack_offset;
  // ARM64 ABI: SP must always be 16-byte aligned.
  assert_true(stack_size % 16 == 0);
  func_info.stack_size = stack_size;
  func_info.lr_save_offset = StackLayout::HOST_RET_ADDR;
  stack_size_ = stack_size;

  struct {
    size_t prolog;
    size_t body;
    size_t epilog;
    size_t tail;
    size_t prolog_stack_alloc;
  } code_offsets = {};

  // ========================================================================
  // PROLOG
  // ========================================================================
  code_offsets.prolog = getSize();

  // sub sp, sp, #stack_size
  if (stack_size <= 4095) {
    sub(sp, sp, static_cast<uint32_t>(stack_size));
  } else {
    mov(x17, static_cast<uint64_t>(stack_size));
    sub(sp, sp, x17, UXTX);
  }
  code_offsets.prolog_stack_alloc = getSize();

  // Store host return address (x30/LR) so the epilog can restore it.
  str(x30, ptr(sp, static_cast<uint32_t>(StackLayout::HOST_RET_ADDR)));
  // Store guest PPC return address (passed in x0 by convention).
  str(x0, ptr(sp, static_cast<uint32_t>(StackLayout::GUEST_RET_ADDR)));
  // Store zero for call return address (we haven't made a call yet).
  str(xzr, ptr(sp, static_cast<uint32_t>(StackLayout::GUEST_CALL_RET_ADDR)));

  // Record stackpoint for longjmp recovery.
  PushStackpoint();

  // ========================================================================
  // BODY
  // ========================================================================
  code_offsets.body = getSize();

  // FTrace: log guest function entry when the backend was built with
  // function tracing available (gated at runtime by the trace_func flag).
  if (IsTracingFunc()) {
    mov(x1, static_cast<uint64_t>(current_guest_function_));
    CallNative(reinterpret_cast<void*>(TraceFunctionEntry));
  }
  if (log_guest_call_) {
    mov(x1, static_cast<uint64_t>(current_guest_function_));
    CallNative(reinterpret_cast<void*>(LogGuestCallEntry));
  }

  // hakuX-Boosted: the pinned guest registers are read from the context
  // once, here. The context is current on entry: every way into a function's
  // first instruction is a guest call or the host-to-guest thunk.
  if (!pins_.empty()) {
    EmitPinnedReload();
    pins_live_ = true;
  }

  // Allocate the epilog label (owned by label_cache_ for cleanup).
  auto epilog_label_ptr = new Label();
  label_cache_.push_back(epilog_label_ptr);
  epilog_label_ = epilog_label_ptr;

  // Walk HIR blocks and emit ARM64 instructions.
  auto block = builder->first_block();
  synchronize_stack_on_next_instruction_ = false;
  hir::Opcode last_opcode = hir::OPCODE_NOP;  // hakuX-Boosted
  while (block) {
    // Reset FPCR tracking on each block entry (we don't know which
    // predecessor ran, so mode is unknown).
    ForgetFpcrMode();

    // Bind all labels targeting this block.
    auto label = block->label_head;
    while (label) {
      auto& host_label = GetLabel(label->id);
      L(host_label);
      // hakuX-Boosted: where it was placed, for IsNearBoundLabel.
      bound_label_offsets_[&host_label] = getSize();
      label = label->next;
    }

    // Process each instruction in the block.
    const hir::Instr* instr = block->instr_head;
    while (instr) {
      // After a guest call, check for longjmp on the next real instruction.
      // Skip SOURCE_OFFSET because the return address from the call would
      // point past the check, so it would never execute.
      if (synchronize_stack_on_next_instruction_) {
        if (instr->GetOpcodeNum() != hir::OPCODE_SOURCE_OFFSET) {
          synchronize_stack_on_next_instruction_ = false;
          EnsureSynchronizedGuestAndHostStack();
        } else if (pins_live_) {
          // hakuX-Boosted: this source offset is the host address
          // ResolveFunction would resume at for a longjmp to the return site
          // of the call just emitted. With pinned registers it resumes at a
          // stub instead (emitted with the tail code), which reloads them
          // and jumps here, to the stack check that follows.
          auto& resume_target = NewCachedLabel();
          L(resume_target);
          pinned_resume_stubs_.push_back(
              {static_cast<uint32_t>(instr->src1.offset), &resume_target, 0});
        }
      }
      // hakuX-Boosted: bring the context up to date before any instruction
      // that leaves this function's code, and read the pinned registers back
      // after any that returns here. See EmitPinnedFlush.
      const PinSync pin_sync =
          pins_live_ ? ClassifyPinSync(instr) : PinSync::kNone;
      if (pin_sync != PinSync::kNone) {
        EmitPinnedFlush();
        compiler::translation_stats.pin_sync_sites.fetch_add(
            1, std::memory_order_relaxed);
      }
      last_opcode = instr->GetOpcodeNum();
      const hir::Instr* new_tail = instr;
      bool selected = false;
      try {
        selected = SelectSequence(this, instr, &new_tail);
        if (selected && pin_sync == PinSync::kFlushAndReload) {
          EmitPinnedReload();
        }
      } catch (const Xbyak_aarch64::Error& e) {
        // Uncaught this aborts the process with no context, so name the opcode
        // and the guest function and fail just this compile.
        XELOGE(
            "A64: assembler rejected HIR opcode {} in guest function {:08X}: "
            "{}",
            hir::GetOpcodeName(instr->GetOpcodeInfo()), current_guest_function_,
            e.what());
        return false;
      }
      if (!selected) {
        // No sequence matched — this is expected in Phase 1 before
        // sequences are implemented.
        XELOGE("A64: Unable to process HIR opcode {}",
               hir::GetOpcodeName(instr->GetOpcodeInfo()));
        return false;
      }
      instr = new_tail;
    }

    block = block->next;
  }

  // hakuX-Boosted: every branch to the epilog wrote the pinned registers
  // back first (returns are sync points). Falling off the last block into it
  // must do the same; after a plain return it already has.
  if (pins_live_ && last_opcode != hir::OPCODE_RETURN) {
    EmitPinnedFlush();
  }
  // From here on nothing may assume the pinned registers: the epilog's and
  // the tail code's host calls run with the context already current (or,
  // for the prologue's stackpoint overflow, before the registers were
  // loaded).
  pins_live_ = false;

  // ========================================================================
  // EPILOG
  // ========================================================================
  L(*epilog_label_);
  epilog_label_ = nullptr;
  // FTrace: log the guest return value (r3) on normal return.
  if (IsTracingFunc()) {
    mov(x1, static_cast<uint64_t>(current_guest_function_));
    CallNative(reinterpret_cast<void*>(TraceFunctionReturn));
  }
  if (log_guest_call_) {
    mov(x1, static_cast<uint64_t>(current_guest_function_));
    CallNative(reinterpret_cast<void*>(LogGuestCallReturn));
  }
  code_offsets.epilog = getSize();

  // Pop stackpoint before leaving.
  PopStackpoint();

  // Restore host return address and deallocate stack.
  ldr(x30, ptr(sp, static_cast<uint32_t>(StackLayout::HOST_RET_ADDR)));
  if (stack_size <= 4095) {
    add(sp, sp, static_cast<uint32_t>(stack_size));
  } else {
    mov(x17, static_cast<uint64_t>(stack_size));
    add(sp, sp, x17, UXTX);
  }
  ret();

  // ========================================================================
  // TAIL CODE
  // ========================================================================
  for (auto& tail_item : tail_code_) {
    // ARM64 instructions are always 4-byte aligned, so alignment is mostly
    // a no-op unless we want cache-line alignment for hot paths.
    L(tail_item.label);
    try {
      tail_item.func(*this, tail_item.label);
    } catch (const Xbyak_aarch64::Error& e) {
      XELOGE("A64: assembler rejected tail code in guest function {:08X}: {}",
             current_guest_function_, e.what());
      return false;
    }
  }
  // hakuX-Boosted: one resume stub per return site, see the body loop.
  for (auto& stub : pinned_resume_stubs_) {
    stub.code_offset = static_cast<uint32_t>(getSize());
    try {
      EmitPinnedReload();
      CodeGenerator::b(*stub.target);
    } catch (const Xbyak_aarch64::Error& e) {
      XELOGE("A64: assembler rejected a resume stub in guest function "
             "{:08X}: {}",
             current_guest_function_, e.what());
      return false;
    }
  }
  if (!pinned_resume_stubs_.empty()) {
    compiler::translation_stats.pin_resume_stubs.fetch_add(
        pinned_resume_stubs_.size(), std::memory_order_relaxed);
  }
  code_offsets.tail = getSize();

  // Fill in EmitFunctionInfo metrics.
  assert_zero(code_offsets.prolog);
  func_info.code_size.total = getSize();
  func_info.code_size.prolog = code_offsets.body - code_offsets.prolog;
  func_info.code_size.body = code_offsets.epilog - code_offsets.body;
  func_info.code_size.epilog = code_offsets.tail - code_offsets.epilog;
  func_info.code_size.tail = getSize() - code_offsets.tail;
  func_info.prolog_stack_alloc_offset =
      code_offsets.prolog_stack_alloc - code_offsets.prolog;

  return true;
}

void* A64Emitter::Emplace(const EmitFunctionInfo& func_info,
                          GuestFunction* function) {
  assert_true(func_info.code_size.total == getSize());

  void* new_execute_address;
  void* new_write_address;

  if (function) {
    code_cache_->PlaceGuestCode(
        function->address(),
        const_cast<void*>(static_cast<const void*>(getCode())), func_info,
        function, new_execute_address, new_write_address);
  } else {
    code_cache_->PlaceHostCode(
        0, const_cast<void*>(static_cast<const void*>(getCode())), func_info,
        new_execute_address, new_write_address);
  }

  // In xbyak_aarch64, labels are resolved at define time (backpatching),
  // so all relative offsets are already correct. We just need to reset
  // the codegen state for the next function.
  ResetPerFunctionState();

  return new_execute_address;
}

void A64Emitter::ResetPerFunctionState() {
  reset();
  tail_code_.clear();

  // Clean up cached labels.
  epilog_label_ = nullptr;
  for (auto* cached_label : label_cache_) {
    delete cached_label;
  }
  label_cache_.clear();

  // hakuX-Boosted: keyed by the labels deleted below.
  bound_label_offsets_.clear();

  // Clean up HIR->xbyak label map. HIR label ids restart at each function, so
  // stale entries would hand the next function this one's labels.
  for (auto& pair : label_map_) {
    delete pair.second;
  }
  label_map_.clear();
}

void A64Emitter::MarkSourceOffset(const hir::Instr* i) {
  auto entry = source_map_arena_.Alloc<SourceMapEntry>();
  entry->guest_address = static_cast<uint32_t>(i->src1.offset);
  entry->hir_offset = uint32_t(i->block->ordinal << 16) | i->ordinal;
  entry->code_offset = static_cast<uint32_t>(getSize());
}

void A64Emitter::DebugBreak() { brk(0xF000); }

void A64Emitter::Trap(uint16_t trap_type) { brk(trap_type); }

// hakuX-Boosted: true when `label` is a block label of this function that has
// already been placed and lies no more than max_distance bytes behind the
// instruction about to be emitted, so a one-instruction branch to it is known
// to encode. Only labels recorded where Emit binds them are considered; any
// other label (forward, tail code, the epilog) is unknown here and keeps the
// far form. The assembler checks the range again when it encodes.
bool A64Emitter::IsNearBoundLabel(const Xbyak_aarch64::Label& label,
                                  size_t max_distance) const {
  if (!cvars::a64_short_backward_branches) {
    return false;
  }
  auto it = bound_label_offsets_.find(&label);
  if (it == bound_label_offsets_.end()) {
    return false;
  }
  const size_t here = getSize();
  return here >= it->second && here - it->second <= max_distance;
}

void A64Emitter::b(const Xbyak_aarch64::Cond cond,
                   const Xbyak_aarch64::Label& label) {
  if (IsNearBoundLabel(label, kNearBranch19)) {
    CodeGenerator::b(cond, label);
    return;
  }
  Xbyak_aarch64::Label skip;
  CodeGenerator::b(static_cast<Xbyak_aarch64::Cond>(cond ^ 1), skip);
  CodeGenerator::b(label);
  L(skip);
}

void A64Emitter::cbz(const Xbyak_aarch64::WReg& rt,
                     const Xbyak_aarch64::Label& label) {
  if (IsNearBoundLabel(label, kNearBranch19)) {
    CodeGenerator::cbz(rt, label);
    return;
  }
  Xbyak_aarch64::Label skip;
  CodeGenerator::cbnz(rt, skip);
  CodeGenerator::b(label);
  L(skip);
}

void A64Emitter::cbz(const Xbyak_aarch64::XReg& rt,
                     const Xbyak_aarch64::Label& label) {
  if (IsNearBoundLabel(label, kNearBranch19)) {
    CodeGenerator::cbz(rt, label);
    return;
  }
  Xbyak_aarch64::Label skip;
  CodeGenerator::cbnz(rt, skip);
  CodeGenerator::b(label);
  L(skip);
}

void A64Emitter::cbnz(const Xbyak_aarch64::WReg& rt,
                      const Xbyak_aarch64::Label& label) {
  if (IsNearBoundLabel(label, kNearBranch19)) {
    CodeGenerator::cbnz(rt, label);
    return;
  }
  Xbyak_aarch64::Label skip;
  CodeGenerator::cbz(rt, skip);
  CodeGenerator::b(label);
  L(skip);
}

void A64Emitter::cbnz(const Xbyak_aarch64::XReg& rt,
                      const Xbyak_aarch64::Label& label) {
  if (IsNearBoundLabel(label, kNearBranch19)) {
    CodeGenerator::cbnz(rt, label);
    return;
  }
  Xbyak_aarch64::Label skip;
  CodeGenerator::cbz(rt, skip);
  CodeGenerator::b(label);
  L(skip);
}

void A64Emitter::tbz(const Xbyak_aarch64::WReg& rt, uint32_t imm,
                     const Xbyak_aarch64::Label& label) {
  if (IsNearBoundLabel(label, kNearBranch14)) {
    CodeGenerator::tbz(rt, imm, label);
    return;
  }
  Xbyak_aarch64::Label skip;
  CodeGenerator::tbnz(rt, imm, skip);
  CodeGenerator::b(label);
  L(skip);
}

void A64Emitter::tbz(const Xbyak_aarch64::XReg& rt, uint32_t imm,
                     const Xbyak_aarch64::Label& label) {
  if (IsNearBoundLabel(label, kNearBranch14)) {
    CodeGenerator::tbz(rt, imm, label);
    return;
  }
  Xbyak_aarch64::Label skip;
  CodeGenerator::tbnz(rt, imm, skip);
  CodeGenerator::b(label);
  L(skip);
}

void A64Emitter::tbnz(const Xbyak_aarch64::WReg& rt, uint32_t imm,
                      const Xbyak_aarch64::Label& label) {
  if (IsNearBoundLabel(label, kNearBranch14)) {
    CodeGenerator::tbnz(rt, imm, label);
    return;
  }
  Xbyak_aarch64::Label skip;
  CodeGenerator::tbz(rt, imm, skip);
  CodeGenerator::b(label);
  L(skip);
}

void A64Emitter::tbnz(const Xbyak_aarch64::XReg& rt, uint32_t imm,
                      const Xbyak_aarch64::Label& label) {
  if (IsNearBoundLabel(label, kNearBranch14)) {
    CodeGenerator::tbnz(rt, imm, label);
    return;
  }
  Xbyak_aarch64::Label skip;
  CodeGenerator::tbz(rt, imm, skip);
  CodeGenerator::b(label);
  L(skip);
}

void A64Emitter::UnimplementedInstr(const hir::Instr* i) {
  XELOGE("A64: Unimplemented HIR instruction: {}",
         hir::GetOpcodeName(i->GetOpcodeInfo()));
  DebugBreak();
}

void A64Emitter::Call(const hir::Instr* instr, GuestFunction* function) {
  assert_not_null(function);
  ForgetFpcrMode();
  auto fn = static_cast<A64Function*>(function);

  if (fn->machine_code()) {
    // Direct call — function is already compiled.
    mov(x9, reinterpret_cast<uint64_t>(fn->machine_code()));
    if (!(instr->flags & hir::CALL_TAIL)) {
      // Pass the next call's guest return address in x0.
      ldr(x0, ptr(sp, static_cast<uint32_t>(StackLayout::GUEST_CALL_RET_ADDR)));
      blr(x9);
      synchronize_stack_on_next_instruction_ = true;
    } else {
      // Tail call: pass our return address to the callee.
      PopStackpoint();
      ldr(x0, ptr(sp, static_cast<uint32_t>(StackLayout::GUEST_RET_ADDR)));
      ldr(x30, ptr(sp, static_cast<uint32_t>(StackLayout::HOST_RET_ADDR)));
      if (stack_size() <= 4095) {
        add(sp, sp, static_cast<uint32_t>(stack_size()));
      } else {
        mov(x17, static_cast<uint64_t>(stack_size()));
        add(sp, sp, x17, UXTX);
      }
      br(x9);
    }
    return;
  }

  if (code_cache_->has_indirection_table()) {
    // Must leave the guest address in w16 for the resolve thunk to read.
    mov(w16, function->address());
    if (!code_cache_->encoded_indirection()) {
      // Fast path: table mapped at host VA == guest addr; slot holds raw
      // 32-bit host target.
      ldr(w9, ptr(x16, static_cast<uint32_t>(0)));
    } else {
      // Encoded path: see A64CodeCache for the entry format.
      Label external_target;
      Label indirection_ready;

      mov(x14, code_cache_->indirection_table_base_bias());
      add(x14, x14, w16, UXTW);
      ldr(w9, ptr(x14, static_cast<uint32_t>(0)));
      tbnz(w9, 31, external_target);

      // Internal: rel32 from code cache base.
      mov(x14, code_cache_->execute_base_address());
      add(x9, x14, w9, UXTW);
      b(indirection_ready);

      // External: tagged index into the side table.
      L(external_target);
      and_(w15, w9, A64CodeCache::kIndirectionExternalIndexMask);
      mov(x14, code_cache_->external_indirection_table_base_address());
      lsl(x15, x15, 3);
      add(x14, x14, x15);
      ldr(x9, ptr(x14, static_cast<uint32_t>(0)));

      L(indirection_ready);
    }
  } else {
    // No indirection table: resolve at runtime.
    mov(x0, x20);  // context
    mov(x1, static_cast<uint64_t>(function->address()));
    mov(x9, reinterpret_cast<uint64_t>(&ResolveFunction));
    blr(x9);
    mov(x9, x0);  // resolved address in x9
  }

  if (instr->flags & hir::CALL_TAIL) {
    PopStackpoint();
    ldr(x0, ptr(sp, static_cast<uint32_t>(StackLayout::GUEST_RET_ADDR)));
    ldr(x30, ptr(sp, static_cast<uint32_t>(StackLayout::HOST_RET_ADDR)));
    if (stack_size() <= 4095) {
      add(sp, sp, static_cast<uint32_t>(stack_size()));
    } else {
      mov(x17, static_cast<uint64_t>(stack_size()));
      add(sp, sp, x17, UXTX);
    }
    br(x9);
  } else {
    ldr(x0, ptr(sp, static_cast<uint32_t>(StackLayout::GUEST_CALL_RET_ADDR)));
    blr(x9);
    synchronize_stack_on_next_instruction_ = true;
  }
}

void A64Emitter::CallIndirect(const hir::Instr* instr, int reg_index) {
  ForgetFpcrMode();
  auto target_w = WReg(reg_index);

  // Check if this is a possible return (e.g., PPC blr).
  if (instr->flags & hir::CALL_POSSIBLE_RETURN) {
    // Compare target guest address with our function's return address.
    ldr(w0, ptr(sp, static_cast<uint32_t>(StackLayout::GUEST_RET_ADDR)));
    cmp(target_w, w0);
    b(EQ, epilog_label());
  }

  // Load host code address from indirection table.
  if (code_cache_->has_indirection_table()) {
    // Must leave the guest address in w16 for the resolve thunk to read.
    if (target_w.getIdx() != w16.getIdx()) {
      mov(w16, target_w);
    }
    if (!code_cache_->encoded_indirection()) {
      // Fast path: table mapped at host VA == guest addr; slot holds raw
      // 32-bit host target.
      ldr(w9, ptr(x16, static_cast<uint32_t>(0)));
    } else {
      // Encoded path: see A64CodeCache for the entry format.
      Label external_target;
      Label indirection_ready;

      mov(x14, code_cache_->indirection_table_base_bias());
      add(x14, x14, w16, UXTW);
      ldr(w9, ptr(x14, static_cast<uint32_t>(0)));
      tbnz(w9, 31, external_target);

      // Internal: rel32 from code cache base.
      mov(x14, code_cache_->execute_base_address());
      add(x9, x14, w9, UXTW);
      b(indirection_ready);

      // External: tagged index into the side table.
      L(external_target);
      and_(w15, w9, A64CodeCache::kIndirectionExternalIndexMask);
      mov(x14, code_cache_->external_indirection_table_base_address());
      lsl(x15, x15, 3);
      add(x14, x14, x15);
      ldr(x9, ptr(x14, static_cast<uint32_t>(0)));

      L(indirection_ready);
    }
  } else {
    // No indirection table: resolve at runtime.
    mov(w16, target_w);
    mov(x0, x20);  // context
    mov(x1, x16);  // guest address
    mov(x9, reinterpret_cast<uint64_t>(&ResolveFunction));
    blr(x9);
    mov(x9, x0);  // resolved address
  }

  if (instr->flags & hir::CALL_TAIL) {
    // Tail call: pass our return address to the callee.
    PopStackpoint();
    ldr(x0, ptr(sp, static_cast<uint32_t>(StackLayout::GUEST_RET_ADDR)));
    ldr(x30, ptr(sp, static_cast<uint32_t>(StackLayout::HOST_RET_ADDR)));
    if (stack_size() <= 4095) {
      add(sp, sp, static_cast<uint32_t>(stack_size()));
    } else {
      mov(x17, static_cast<uint64_t>(stack_size()));
      add(sp, sp, x17, UXTX);
    }
    br(x9);
  } else {
    // Regular call: pass the next call's return address.
    ldr(x0, ptr(sp, static_cast<uint32_t>(StackLayout::GUEST_CALL_RET_ADDR)));
    blr(x9);
    synchronize_stack_on_next_instruction_ = true;
  }
}

void A64Emitter::CallExtern(const hir::Instr* instr, const Function* function) {
  ForgetFpcrMode();
  bool undefined = true;
  // hakuX-Boosted: the thunk without the vector register saves, when this
  // call is the last instruction of its block (HIRBuilder::CallExtern ends
  // the block, so it always is; checked rather than assumed). No HIR value
  // outlives its block, so none is live in a vector register after the call.
  // See A64HelperEmitter::EmitGuestToHostLeanThunk for the whole argument.
  auto thunk = backend()->guest_to_host_thunk();
  if (cvars::a64_lean_extern_thunk && instr && !instr->next &&
      backend()->guest_to_host_lean_thunk()) {
    thunk = backend()->guest_to_host_lean_thunk();
  }
  if (function->behavior() == Function::Behavior::kBuiltin) {
    auto builtin_function = static_cast<const BuiltinFunction*>(function);
    if (builtin_function->handler()) {
      undefined = false;
      // GuestToHostThunk: x0=target, x1=arg0, x2=arg1
      // Thunk rearranges to: x0=context, x1=arg0, x2=arg1, calls target
      mov(x0, reinterpret_cast<uint64_t>(builtin_function->handler()));
      mov(x1, reinterpret_cast<uint64_t>(builtin_function->arg0()));
      mov(x2, reinterpret_cast<uint64_t>(builtin_function->arg1()));
      mov(x9, reinterpret_cast<uint64_t>(thunk));
      blr(x9);
    }
  } else if (function->behavior() == Function::Behavior::kExtern) {
    auto extern_function = static_cast<const GuestFunction*>(function);
    if (extern_function->extern_handler()) {
      undefined = false;
      // GuestToHostThunk: x0=target, x1=arg0
      mov(x0, reinterpret_cast<uint64_t>(extern_function->extern_handler()));
      ldr(x1, ptr(GetContextReg(), static_cast<int32_t>(offsetof(
                                       ppc::PPCContext, kernel_state))));
      mov(x9, reinterpret_cast<uint64_t>(thunk));
      blr(x9);
    }
  }
  if (undefined) {
    // Set arg0 = function pointer, then call UndefinedCallExtern via thunk.
    mov(x1, reinterpret_cast<uint64_t>(function));
    CallNativeSafe(reinterpret_cast<void*>(&UndefinedCallExtern));
  }
}

void A64Emitter::CallNative(void* fn) { CallNativeSafe(fn); }

void A64Emitter::CallNativeSafe(void* fn) {
  // hakuX-Boosted: a host function called from the middle of a sequence sees
  // a current context and may change it, like every other way out of the
  // function's code. Neither the stores nor the loads touch x0-x17, so the
  // arguments already set up and the result in x0 are undisturbed.
  const bool sync_pins = pins_live_;
  if (sync_pins) {
    EmitPinnedFlush();
  }
  // GuestToHostThunk: x0=target function, x1/x2=args (set by caller).
  // The thunk rearranges: saves x0 in x9, sets x0=context, calls x9.
  mov(x0, reinterpret_cast<uint64_t>(fn));
  mov(x9, reinterpret_cast<uint64_t>(backend()->guest_to_host_thunk()));
  blr(x9);
  if (sync_pins) {
    EmitPinnedReload();
  }
}

// hakuX-Boosted: pinned guest registers (pin_guest_registers).
//
// A pinned guest register lives in one host register from the end of the
// function's prologue to its last instruction: its context loads become
// moves from that register and its stores moves into it (the LOAD_CONTEXT
// and STORE_CONTEXT sequences ask FindPinnedGuestRegister). The context copy
// is therefore stale while the function's own code runs, and the rule that
// keeps this invisible is:
//
//   the context is made current (EmitPinnedFlush) before control leaves the
//   function's own code, and the registers are read back (EmitPinnedReload)
//   where control comes back.
//
// The ways out, all of them:
//   - guest calls, direct and indirect, conditional or not, tail or not, and
//     the possible-return form of `blr`: sync points in Emit's loop;
//   - CALL_EXTERN (kernel calls, through either thunk): the same;
//   - returns (branches to the epilog) and falling into the epilog: flush;
//   - traps and debug breaks (the handler sees the context): sync points;
//   - CONTEXT_BARRIER (the front end's own "context must be current"): sync;
//   - the scheduler's preemption check: its cold path, which is the only
//     part that leaves (EmitPreemptCheck);
//   - any host helper called from inside a sequence (MMIO access, clock,
//     spin park, pow/log, reservations, tracing): CallNativeSafe above;
//   - the resolve thunk, ResolveFunction and the stack synchronisation
//     helper are only reached from inside a guest call's sequence, after its
//     flush.
// The two host functions called with a bare blr (RSQRT_V128, the vector
// helpers beside it) take their operands in registers and never see the
// context.
// The ways in: the function's first instruction (context current, loaded by
// the prologue), the return from any of the above (reloaded), and
// ResolveFunction resuming a return site after a longjmp (through a stub
// that reloads, see pinned_resume_stubs_).
//
// What may still see a stale context copy of a pinned register: a signal
// handler or debugger that inspects PPCContext while this function's own
// code is interrupted between sync points. The MMIO fault handler does not
// read guest registers from the context (it decodes the faulting host
// instruction); a crash report would show the last synced values.
// Translated code already had this property within a block (context
// promotion keeps values in host registers and drops overwritten stores);
// pinning extends it to the function. r1, r13 and lr, which host code reads
// at arbitrary points, are never pinned.
//
// The host registers themselves: pinned registers are taken from the
// allocator's sets (x22-x28, v4-v31), which no sequence uses as scratch.
// x22-x28 are callee-saved for host functions; v4-v31 are saved by the full
// guest-to-host thunk; and after every call that does not go through it the
// registers are reloaded anyway.
void A64Emitter::EmitPinnedFlush() {
  if (pin_verify_) {
    // The context is written at every store in this mode and is the copy
    // that counts; a register that went wrong must not overwrite it.
    return;
  }
  for (const auto& pin : pins_) {
    if (!pin.written) {
      continue;  // still equal to the context copy it was loaded from
    }
    switch (pin.type) {
      case hir::INT64_TYPE:
        str(XReg(pin.host_reg), ptr(x20, pin.offset));
        break;
      case hir::FLOAT64_TYPE:
        str(DReg(pin.host_reg), ptr(x20, pin.offset));
        break;
      default:
        str(QReg(pin.host_reg), ptr(x20, pin.offset));
        break;
    }
  }
}

void A64Emitter::EmitPinnedReload() {
  for (const auto& pin : pins_) {
    switch (pin.type) {
      case hir::INT64_TYPE:
        ldr(XReg(pin.host_reg), ptr(x20, pin.offset));
        break;
      case hir::FLOAT64_TYPE:
        ldr(DReg(pin.host_reg), ptr(x20, pin.offset));
        break;
      default:
        ldr(QReg(pin.host_reg), ptr(x20, pin.offset));
        break;
    }
  }
}

static uint64_t ReportPinnedMismatch(void* raw_context, uint64_t function,
                                     uint64_t offset) {
  const uint64_t count =
      compiler::translation_stats.pin_verify_mismatches.fetch_add(
          1, std::memory_order_relaxed);
  if (count < 64) {
    XELOGE(
        "PinVerify: pinned guest register at context offset {:#x} differed "
        "from the context in guest function {:08X}",
        offset, static_cast<uint32_t>(function));
  }
  return 0;
}

void A64Emitter::EmitPinnedVerify(const PinnedGuestRegister& pin) {
  // The report below goes through the guest-to-host thunk, which leaves FPCR
  // in the scalar mode; make the mode tracking agree whichever way the
  // compare goes.
  ForgetFpcrMode();
  // No flag is written: eor and cbz only, so nothing a neighbouring sequence
  // might rely on is disturbed.
  Label& same = NewCachedLabel();
  switch (pin.type) {
    case hir::INT64_TYPE:
      ldr(x0, ptr(x20, pin.offset));
      eor(x0, x0, XReg(pin.host_reg));
      break;
    case hir::FLOAT64_TYPE:
      ldr(x0, ptr(x20, pin.offset));
      fmov(x1, DReg(pin.host_reg));
      eor(x0, x0, x1);
      break;
    default:
      ldr(QReg(0), ptr(x20, pin.offset));
      eor(VReg(0).b16, VReg(0).b16, VReg(pin.host_reg).b16);
      // Fold the 128 bits of difference into x0.
      umov(x0, VReg(0).d2[0]);
      umov(x1, VReg(0).d2[1]);
      orr(x0, x0, x1);
      break;
  }
  cbz(x0, same);
  mov(x1, static_cast<uint64_t>(current_guest_function_));
  mov(x2, static_cast<uint64_t>(pin.offset));
  // Reloads every pinned register from the context on the way back.
  CallNativeSafe(reinterpret_cast<void*>(&ReportPinnedMismatch));
  L(same);
}

A64Emitter::PinSync A64Emitter::ClassifyPinSync(const hir::Instr* instr) {
  switch (instr->GetOpcodeNum()) {
    case hir::OPCODE_CALL:
    case hir::OPCODE_CALL_INDIRECT:
      // An unconditional tail call does not come back.
      return (instr->flags & hir::CALL_TAIL) ? PinSync::kFlush
                                             : PinSync::kFlushAndReload;
    case hir::OPCODE_CALL_TRUE:
    case hir::OPCODE_CALL_INDIRECT_TRUE:
    case hir::OPCODE_CALL_EXTERN:
    case hir::OPCODE_TRAP:
    case hir::OPCODE_TRAP_TRUE:
    case hir::OPCODE_DEBUG_BREAK:
    case hir::OPCODE_DEBUG_BREAK_TRUE:
    case hir::OPCODE_CONTEXT_BARRIER:
      return PinSync::kFlushAndReload;
    case hir::OPCODE_RETURN:
    case hir::OPCODE_RETURN_TRUE:
      // Taken, it leaves; not taken, the registers are as they were.
      return PinSync::kFlush;
    default:
      return PinSync::kNone;
  }
}

void A64Emitter::SetReturnAddress(uint64_t value) {
  mov(x0, value);
  str(x0, ptr(sp, static_cast<uint32_t>(StackLayout::GUEST_CALL_RET_ADDR)));
}

void A64Emitter::ReloadMembase() {
  // Reload x21 from context->virtual_membase.
  ldr(x21, ptr(x20, static_cast<int32_t>(
                        offsetof(ppc::PPCContext, virtual_membase))));
}

bool A64Emitter::ChangeFpcrMode(FPCRMode new_mode, bool already_set) {
  if (fpcr_mode_ == new_mode) {
    return false;
  }
  fpcr_mode_ = new_mode;
  if (!already_set) {
    // Load the pre-computed FPCR value from the backend context.
    // This avoids an expensive MRS + read-modify-write cycle.
    auto bctx = GetBackendCtxReg();
    if (new_mode == FPCRMode::Vmx) {
      ldr(w0, Xbyak_aarch64::ptr(bctx, static_cast<uint32_t>(offsetof(
                                           A64BackendContext, fpcr_vmx))));
    } else {
      ldr(w0, Xbyak_aarch64::ptr(bctx, static_cast<uint32_t>(offsetof(
                                           A64BackendContext, fpcr_fpu))));
    }
    msr(3, 3, 4, 4, 0, x0);  // msr FPCR, x0
  }
  return true;
}

Label& A64Emitter::AddToTail(TailEmitCallback callback, uint32_t alignment) {
  TailEmitter tail;
  tail.alignment = alignment;
  tail.func = std::move(callback);
  tail_code_.push_back(std::move(tail));
  return tail_code_.back().label;
}

Label& A64Emitter::NewCachedLabel() {
  auto* label = new Label();
  label_cache_.push_back(label);
  return *label;
}

uint32_t A64Emitter::MapReg(const hir::Value* v, const uint32_t* map, int count,
                            const char* set_name) {
  // reg.index is a signed int32 and is -1 while unassigned, so the unsigned
  // compare catches both "never allocated" and "past the end of the set".
  const uint32_t index = static_cast<uint32_t>(v->reg.index);
  if (index >= static_cast<uint32_t>(count)) {
    XELOGE(
        "A64: value v{} (type {}, def opcode {}) has no {} register assignment "
        "(index {} of {}); codegen would emit a bogus register",
        v->ordinal, static_cast<uint32_t>(v->type),
        v->def ? hir::GetOpcodeName(v->def->GetOpcodeInfo()) : "<none>",
        set_name, index, count);
    assert_always("register allocation missed a value");
    // Clamp so the assembler still produces a decodable instruction.
    return map[0];
  }
  return map[index];
}

void A64Emitter::EmitPreemptCheck(uint32_t guest_address) {
  // Only safe at a block head, where the per-block register allocator leaves no
  // guest value live and ForgetFpcrMode has already run, so the unannounced
  // guest->host call cannot lose a register or desync the mode tracking.
  //
  // Tests the preempt flag other threads raise. The cold path clears it, a
  // deferred yield re-sets it.
  Label& after = NewCachedLabel();
  // ldrb/strb unsigned-offset encoding caps at 4095.
  static_assert(offsetof(ppc::PPCContext, preempt_requested) < 4096);
  const uint32_t flag_offset =
      static_cast<uint32_t>(offsetof(ppc::PPCContext, preempt_requested));
  // hakuX-Boosted: with pinned guest registers the yield, and only the
  // yield, is a way out of the function's code: the context is made current
  // before it and the registers are read back after. The test on the usual
  // path touches neither.
  const bool sync_pins = pins_live_;
  Label& do_yield =
      AddToTail([&after, flag_offset, sync_pins](A64Emitter& e, Label&) {
        e.strb(e.wzr, ptr(e.x20, flag_offset));
        // Null until the scheduler starts, and a stale flag can reach here
        // after it shuts down, so check before calling.
        e.mov(e.x0, reinterpret_cast<uint64_t>(
                        &xe::cpu::backend::preempt_yield_handler));
        e.ldr(e.x0, ptr(e.x0));
        e.cbz(e.x0, after);
        if (sync_pins) {
          e.EmitPinnedFlush();
        }
        e.mov(e.x9,
              reinterpret_cast<uint64_t>(e.backend()->guest_to_host_thunk()));
        e.blr(e.x9);
        if (sync_pins) {
          e.EmitPinnedReload();
        }
        e.b(after);
      });
  if (cvars::log_safepoint_pc && guest_address) {
    // Diagnostic only: costs a materialize + store on every loop back-edge, so
    // it stays off unless a wedge is being chased.
    static_assert(offsetof(ppc::PPCContext, last_safepoint_pc) < 16384);
    mov(w9, guest_address);
    str(w9, ptr(x20, static_cast<uint32_t>(
                         offsetof(ppc::PPCContext, last_safepoint_pc))));
  }
  ldrb(w8, ptr(x20, flag_offset));
  cbnz(w8, do_yield);
  L(after);
}

Label& A64Emitter::GetLabel(uint32_t label_id) {
  auto it = label_map_.find(label_id);
  if (it != label_map_.end()) {
    return *it->second;
  }
  auto* label = new Label();
  label_map_[label_id] = label;
  return *label;
}

void A64Emitter::HandleStackpointOverflowError(ppc::PPCContext* context) {
  if (debugging::IsDebuggerAttached()) {
    debugging::Break();
  }
  xe::FatalError(
      "Overflowed stackpoints! Please report this error for this title to "
      "Xenia developers.");
}

void A64Emitter::PushStackpoint() {
  if (!cvars::a64_enable_host_guest_stack_synchronization) {
    return;
  }
  // x8 = stackpoints array, w9 = current depth
  ldr(x8, ptr(x19,
              static_cast<uint32_t>(offsetof(A64BackendContext, stackpoints))));
  ldr(w9, ptr(x19, static_cast<uint32_t>(
                       offsetof(A64BackendContext, current_stackpoint_depth))));

  if (cvars::a64_compact_stackpoint_push) {
    // hakuX-Boosted: x8 += depth * 16 in one instruction. The ldr into w9
    // above zero-extended x9, so the 64-bit shifted add gives the address the
    // multiply did.
    static_assert(sizeof(A64BackendStackpoint) == 16,
                  "the shift below is the entry size");
    add(x8, x8, x9, LSL, 4);
  } else {
    // Compute offset into array: x10 = w9 * sizeof(A64BackendStackpoint)
    mov(w10, static_cast<uint32_t>(sizeof(A64BackendStackpoint)));
    umull(x10, w9, w10);
    add(x8, x8, x10);
  }

  // Store host SP.
  mov(x10, sp);
  str(x10, ptr(x8, static_cast<uint32_t>(
                       offsetof(A64BackendStackpoint, host_stack_))));
  // Store guest r1 (32-bit).
  ldr(w10, ptr(x20, static_cast<int32_t>(offsetof(ppc::PPCContext, r[1]))));
  str(w10, ptr(x8, static_cast<uint32_t>(
                       offsetof(A64BackendStackpoint, guest_stack_))));
  // Store guest LR (32-bit).
  ldr(w10, ptr(x20, static_cast<int32_t>(offsetof(ppc::PPCContext, lr))));
  str(w10, ptr(x8, static_cast<uint32_t>(
                       offsetof(A64BackendStackpoint, guest_return_address_))));

  // Increment depth.
  add(w9, w9, 1);
  str(w9, ptr(x19, static_cast<uint32_t>(
                       offsetof(A64BackendContext, current_stackpoint_depth))));

  // Check for overflow.
  mov(w10, static_cast<uint32_t>(cvars::a64_max_stackpoints));
  cmp(w9, w10);
  auto& overflow_label = AddToTail([](A64Emitter& e, Label& lbl) {
    e.CallNativeSafe(
        reinterpret_cast<void*>(A64Emitter::HandleStackpointOverflowError));
  });
  // Far-safe branch to the tail: overflow_label is emitted at the function end,
  // which can exceed B.cond's +/-1 MiB range in large functions. Invert the
  // condition and skip over an unconditional B (which reaches +/-128 MiB).
  auto& no_overflow = NewCachedLabel();
  b(LT, no_overflow);
  b(overflow_label);
  L(no_overflow);
}

void A64Emitter::PopStackpoint() {
  if (!cvars::a64_enable_host_guest_stack_synchronization) {
    return;
  }
  // Decrement current_stackpoint_depth.
  ldr(w8, ptr(x19, static_cast<uint32_t>(
                       offsetof(A64BackendContext, current_stackpoint_depth))));
  sub(w8, w8, 1);
  str(w8, ptr(x19, static_cast<uint32_t>(
                       offsetof(A64BackendContext, current_stackpoint_depth))));
}

void A64Emitter::EnsureSynchronizedGuestAndHostStack() {
  if (!cvars::a64_enable_host_guest_stack_synchronization) {
    return;
  }
  // ResolveFunction marks this when it returns a return-site address inside an
  // existing frame. The marker lives in backend context because native SP can
  // still point at a skipped frame here.
  auto& return_from_sync = NewCachedLabel();

  ldr(w16, ptr(x19, static_cast<uint32_t>(offsetof(
                        A64BackendContext, pending_stackpoint_sync_depth))));
  cbz(w16, return_from_sync);

  auto& sync_label = AddToTail([](A64Emitter& e, Label& lbl) {
    // x8 was set up in the body to point at return_from_sync; do that there
    // instead of here because adr's ±1 MiB range can't span body+tail in
    // large functions.
    //   x8 = return address (where to resume after fixup)
    e.mov(e.x10, reinterpret_cast<uint64_t>(
                     e.backend()->synchronize_guest_and_host_stack_helper()));
    e.br(e.x10);
  });
  adr(x8, return_from_sync);
  b(sync_label);

  L(return_from_sync);
}

}  // namespace a64
}  // namespace backend
}  // namespace cpu
}  // namespace xe
