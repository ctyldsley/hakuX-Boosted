/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/backend/a64/a64_emitter.h"

#include <atomic>
#include <cstring>

#include "xenia/base/platform.h"
#if XE_PLATFORM_LINUX || XE_PLATFORM_ANDROID
#include <sys/auxv.h>  // hakuX-Boosted: getauxval, HostHasLrcpc
#endif

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
#include "xenia/cpu/compiler/passes/dead_context_store_pass.h"
#include "xenia/cpu/compiler/translation_stats.h"
#include "xenia/cpu/cpu_flags.h"
#include "xenia/cpu/hir/hir_builder.h"
#include "xenia/cpu/hir/label.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/processor.h"

DECLARE_int64(a64_max_stackpoints);
DECLARE_bool(a64_enable_host_guest_stack_synchronization);

DECLARE_bool(a64_lean_extern_thunk);
DECLARE_bool(log_translation_stats);  // hakuX-Boosted
DECLARE_bool(a64_linked_calls);
DECLARE_int32(eliminate_dead_context_stores);
DECLARE_bool(dead_context_store_verify);

// hakuX-Boosted: see A64Emitter::IsNearBoundLabel.
// hakuX-Boosted (jit-speed): true by default, was false
// (boosted/notes/jit-speed.md).
DEFINE_bool(
    a64_short_backward_branches, true,
    "A conditional branch (b.cond, cbz, cbnz, tbz, tbnz) to a block label "
    "already placed earlier in the same function is emitted as the one "
    "instruction it is when the distance is known to be in range, instead of "
    "an inverted branch over an unconditional one. Loop back-edges are the "
    "common case. Forward branches, whose distance is not known when they are "
    "emitted, keep the two-instruction form. Takes effect for functions "
    "translated after it is set.",
    "a64");

// hakuX-Boosted: see A64Emitter::UseShortBranch.
// hakuX-Boosted (jit-speed): true by default, was false
// (boosted/notes/jit-speed.md).
DEFINE_bool(
    a64_short_forward_branches, true,
    "A conditional branch to a block of the same function that comes later "
    "is emitted as one instruction too (a64_short_backward_branches does "
    "this for earlier blocks). The distance is not known when the branch is "
    "emitted, so a function whose code turns out too large for the short "
    "form to be certain to reach (32 KiB where a bit test was used, 1 MiB "
    "otherwise) is emitted a second time with the long form. Takes effect "
    "for functions translated after it is set.",
    "a64");
DEFINE_bool(
    a64_short_forward_branches_test_retry, false,
    "For the translator's tests: with a64_short_forward_branches, emit "
    "every function that used a short forward branch a second time with the "
    "long form, as if it had been too large.",
    "a64");

// hakuX-Boosted: strict guest memory ordering, a diagnostic for the
// ARM64-only faults (device-logs/gears-freeze/ANALYSIS-2.md section 2.4).
// Guest loads and stores are plain ldr/str, so the host may reorder what an
// x64 host (TSO) never would. Level 1 puts dmb ishld after every guest load
// (orders it before every later load and store) and dmb ishst before every
// guest store (orders every earlier store before it): together, the order a
// TSO host gives. Applied around the whole sequence of OPCODE_LOAD,
// OPCODE_LOAD_OFFSET and OPCODE_RESERVED_LOAD (barrier after) and
// OPCODE_STORE, OPCODE_STORE_OFFSET, OPCODE_MEMSET and OPCODE_RESERVED_STORE
// (barrier before). Not covered: OPCODE_LOAD_MMIO/STORE_MMIO (host device
// handlers, not guest RAM), OPCODE_ATOMIC_COMPARE_EXCHANGE (casal or
// ldaxr/stlxr, already ordered both ways), and memory touched by host code
// on the guest's behalf (kernel exports, the GPU, XMA).
//
// hakuX-Boosted: level 2, the same total-store-order visibility without the
// barriers (boosted/notes/tso-order.md). Guest loads are LDAPR (acquire,
// RCpc: no later access is performed before it) and guest stores STLR
// (release: no earlier access is performed after it). An STLR followed by an
// LDAPR of another address may still pass each other, which is exactly the
// one reordering TSO allows, so the pair gives what an x64 host gives. Done
// inside the load and store sequences (a64_seq_memory.cc, GuestLdr and
// GuestStr in a64_seq_util.h). Left plain, because no other thread can see
// them or they never change: accesses whose address is the guest stack
// pointer plus a constant, and accesses to a constant address in a page the
// title cannot write (read-only data, code). 128-bit accesses have no
// acquire or release form: a plain ldr q followed by dmb ishld, or dmb ishst
// followed by a plain str q. MEMSET (dcbz) gets dmb ishst before it. The
// reserved load (lwarx) is an LDAPR; the reserved store's CASAL is already
// ordered both ways. Without FEAT_LRCPC on the host, level 2 runs as level 1.
DEFINE_int32(
    a64_guest_memory_order, 0,
    "Order guest memory accesses as an x64 host would. 0 off (plain loads "
    "and stores, as before). 1: a dmb ishld barrier after every guest load "
    "and a dmb ishst barrier before every guest store, which gives total "
    "store order; slow, a diagnostic. 2: the same ordering from acquire "
    "loads (LDAPR) and release stores (STLR) instead of barriers, leaving "
    "stack and read-only accesses plain; runs as 1 on a CPU without "
    "FEAT_LRCPC. Takes effect for functions translated after it is set, so "
    "set it in the config before the title starts.",
    "a64");

// hakuX-Boosted: see ComputeMemoryAddress in a64_seq_util.h.
DEFINE_bool(
    a64_stack_access_no_fixup, false,
    "On hosts with pages larger than 4 KiB (16 KiB Android kernels), where "
    "every load and store with a computed address carries four instructions "
    "that move guest addresses from 0xE0000000 up by one page: leave them "
    "out for addresses formed from the guest stack pointer, which is taken "
    "never to be that high. Nothing changes on a 4 KiB host. Takes effect "
    "for functions translated after it is set.",
    "a64");
DEFINE_bool(
    a64_stack_access_verify, false,
    "Self-check for a64_stack_access_no_fixup, for a checking run only: the "
    "fix-up stays, and every stack-pointer-based address that needed it is "
    "counted. The count is in the TranslationStats line ('stack accesses at "
    "0xE0000000+'); anything but 0 means the setting is wrong for the title.",
    "a64");

// hakuX-Boosted: see A64Emitter::CallIndirect.
DEFINE_bool(
    a64_indirect_call_range_check, false,
    "An indirect guest call (bctrl, bctr, blr to a computed address) whose "
    "target is outside 0x80000000..0x9FFFFFFF, where all translatable guest "
    "code lives, traps at the call site (brk 0xF0C1) instead of reading past "
    "the indirection table, which faults at an unrelated host address or, "
    "if the stray word allows, jumps to an arbitrary one. The crash report "
    "then names the guest PC of the call, with the target in CTR or LR. "
    "Four instructions per indirect call. Takes effect for functions "
    "translated after it is set.",
    "a64");

// hakuX-Boosted: see A64Emitter::EmitCompareResult.
// hakuX-Boosted (jit-speed): true by default, was false
// (boosted/notes/jit-speed.md).
DEFINE_bool(
    a64_fuse_compare_branch, true,
    "An integer compare whose result is used only by the conditional branch "
    "directly after it is emitted as cmp and b.cond, without first turning "
    "the answer into 0 or 1 in a register and testing that. Most useful "
    "with eliminate_dead_context_stores, which removes the store that "
    "otherwise also uses the result. Takes effect for functions translated "
    "after it is set.",
    "a64");

// hakuX-Boosted: instruction selection that gives the same results in fewer
// instructions; each use is marked with the setting's name.
// hakuX-Boosted (jit-speed): true by default, was false
// (boosted/notes/jit-speed.md).
DEFINE_bool(
    a64_tight_sequences, true,
    "Shorter forms of some sequences, with the same results: AND, OR and "
    "XOR of a 64-bit value with a constant use ARM64's immediate form when "
    "the constant has one (a mask of consecutive bits, for one) instead of "
    "first loading the constant into a register. Takes effect for functions "
    "translated after it is set.",
    "a64");

// hakuX-Boosted: see ComputeMemoryAddress in a64_seq_util.h.
DEFINE_bool(
    a64_address_fixup_bit_tests, false,
    "On hosts with pages larger than 4 KiB (16 KiB Android kernels), where "
    "every load and store with a computed address moves guest addresses "
    "from 0xE0000000 up by one page: test the address's top three bits one "
    "at a time and stop at the first that is clear, instead of four "
    "instructions that never branch. One instruction (a short forward "
    "branch, taken) for addresses below 0x80000000, two or three for the "
    "rest below 0xE0000000. Nothing changes on a 4 KiB host. Takes effect "
    "for functions translated after it is set.",
    "a64");

// hakuX-Boosted: see A64Emitter::lean_calls_.
DEFINE_bool(
    a64_lean_calls, false,
    "Fewer instructions round every guest call and return, with the same "
    "effect: the return address goes from the call site to the callee in a "
    "register without a round trip through the caller's stack frame; a "
    "callee that already has code is reached through an address literal; "
    "the function prologue's record for longjmp is written with paired "
    "stores and its overflow test is one bit test (when a64_max_stackpoints "
    "is a power of two); and the tests after a call and at a return branch "
    "in one instruction on the usual path. Takes effect for functions "
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
// hakuX-Boosted: see the block loop in A64Emitter::Emit.
DEFINE_bool(
    a64_fpcr_known_block_entry, false,
    "A block starts with FPCR in the mode its predecessors left it in "
    "when all of them have been emitted before it and agree (straight-line "
    "code and the arms of a branch; not a loop's first block, not the "
    "code after a call), instead of always being taken as unknown. "
    "Scalar float or VMX float code spread over several blocks then loads "
    "and writes FPCR once, not once in every block. Exact: nothing about "
    "calls or other functions changes.",
    "a64");
DEFINE_bool(
    a64_fpcr_known_block_entry_verify, false,
    "Self-check for a64_fpcr_known_block_entry: at every block entered "
    "with a known mode, FPCR is read and compared with that mode's value; "
    "a difference is counted with the self-check mismatches.",
    "a64");

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

// hakuX-Boosted: a64_guest_memory_order (see its definition).
static bool HostHasLrcpc() {
#if XE_PLATFORM_LINUX || XE_PLATFORM_ANDROID
  // HWCAP_LRCPC: LDAPR, LDAPRB, LDAPRH (FEAT_LRCPC, Armv8.3).
  return (getauxval(AT_HWCAP) & (1UL << 15)) != 0;
#elif XE_PLATFORM_MAC
  return true;  // Every Apple arm64 core has it.
#else
  return false;
#endif
}

int GuestMemoryOrderLevel() {
  static const bool has_lrcpc = HostHasLrcpc();
  const int requested = cvars::a64_guest_memory_order;
  int level = requested <= 0 ? 0 : requested >= 2 ? 2 : 1;
  if (level == 2 && !has_lrcpc) {
    level = 1;
  }
  static std::atomic<int> logged{-1};
  if (level != 0 && logged.load(std::memory_order_relaxed) != level &&
      logged.exchange(level, std::memory_order_relaxed) != level) {
    XELOGI(
        "a64_guest_memory_order: {} requested, {} in effect (host "
        "FEAT_LRCPC {})",
        requested, level, has_lrcpc ? "present" : "absent");
  }
  return level;
}

bool IsReadOnlyGuestAddress(A64Emitter& e, uint32_t address) {
  Memory* memory = e.processor() ? e.processor()->memory() : nullptr;
  if (!memory) {
    return false;
  }
  BaseHeap* heap = memory->LookupHeap(address);
  uint32_t protect = 0;
  if (!heap || !heap->QueryProtect(address, &protect)) {
    return false;
  }
  return (protect & kMemoryProtectRead) && !IsWritableProtect(protect);
}

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
// hakuX-Boosted: with a64_extended_gpr_pool, x9-x15 as well. They follow
// the original seven so that an index means the same register either way.
const uint32_t A64Emitter::gpr_reg_map_[GPR_COUNT_SCRATCH] = {
    22, 23, 24, 25, 26, 27, 28, 9, 10, 11, 12, 13, 14, 15, 4, 5, 6, 7};
int A64Emitter::gpr_count_ = A64Emitter::GPR_COUNT;
int A64Emitter::frame_chain_ = 0;  // hakuX-Boosted
int A64Emitter::shared_gpr_count_ = 0;
// In the order they are taken: the stack pointer, the first argument and
// result, then the registers a function saves and restores for its caller.
int A64Emitter::shared_gprs_[A64Emitter::kMaxSharedGprs] = {1,  3,  31,
                                                            30, 29, 28};

void A64Emitter::SetSharedGprs(int count) {
  shared_gpr_count_ = std::clamp(count, 0, kMaxSharedGprs);
}

void A64Emitter::EmitSharedGprsToContext() {
  for (int n = 0; n < shared_gpr_count_; ++n) {
    str(XReg(22 + n),
        ptr(x20, static_cast<uint32_t>(offsetof(ppc::PPCContext, r) +
                                       8 * shared_gprs_[n])));
  }
}

void A64Emitter::EmitSharedGprsFromContext() {
  for (int n = 0; n < shared_gpr_count_; ++n) {
    ldr(XReg(22 + n),
        ptr(x20, static_cast<uint32_t>(offsetof(ppc::PPCContext, r) +
                                       8 * shared_gprs_[n])));
  }
}

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
  // hakuX-Boosted: a64_short_forward_branches. The first attempt may use
  // one-instruction branches to blocks not yet placed. If the function then
  // proves larger than such a branch is certain to span (any two points of
  // a function are closer than its length), or the assembler refuses one,
  // the function is emitted again from the same HIR with the long form.
  short_forward_ok_ = cvars::a64_short_forward_branches;
  linked_calls_ok_ = cvars::a64_linked_calls;
  call_literals_allowed_ = true;
  locals_laid_out_ = false;
  for (int attempt = 0; attempt < 2; ++attempt) {
    used_short_forward19_ = false;
    used_short_forward14_ = false;
    func_info = {};
    try {
      emitted = Emit(builder, func_info);
    } catch (const Xbyak_aarch64::Error& e) {
      if (!(used_short_forward19_ || used_short_forward14_ ||
            !call_literals_.empty())) {
        XELOGE("A64: assembler error while emitting guest function {:08X}: {}",
               current_guest_function_, e.what());
      }
      emitted = false;
    }
    // (A linked call's literal load has the reach of a conditional
    // branch and is retried on the same terms.)
    const bool used_short = used_short_forward19_ || used_short_forward14_ ||
                            !call_literals_.empty();
    if (!used_short) {
      break;
    }
    const size_t size = getSize();
    const bool too_large =
        (used_short_forward14_ && size > kNearBranch14) ||
        size > kNearBranch19;
    if (emitted && !too_large &&
        !cvars::a64_short_forward_branches_test_retry) {
      break;
    }
    // Again, long form.
    compiler::translation_stats.short_forward_retries.fetch_add(
        1, std::memory_order_relaxed);
    ResetPerFunctionState();
    source_map_arena_.Reset();
    short_forward_ok_ = false;
    linked_calls_ok_ = false;
    call_literals_allowed_ = false;
    emitted = false;
  }
  short_forward_ok_ = false;
  linked_calls_ok_ = false;
  call_literals_allowed_ = false;
  if (!emitted) {
    // Emplace only runs on success, so a failed compile has to reset too.
    ResetPerFunctionState();
    return false;
  }

  // hakuX-Boosted: a64_linked_calls. The cache links these when it places
  // the function.
  {
    std::vector<std::pair<uint32_t, uint32_t>> literals;
    literals.reserve(call_literals_.size());
    for (const auto& literal : call_literals_) {
      if (literal.guest_target) {  // (not the fixed ones of a64_lean_calls)
        literals.emplace_back(literal.guest_target, literal.code_offset);
      }
    }
    static_cast<A64Function*>(function)->SetCallLiterals(std::move(literals));
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
  using hir::OpcodeSignatureType;  // for the GET_OPCODE_SIG_TYPE macros
  // hakuX-Boosted: the guest registers this function keeps in host
  // registers, as GuestRegisterPinningPass chose them. Empty unless
  // pin_guest_registers is on.
  load_expectations_ = builder->context_load_expectations().empty()
                           ? nullptr
                           : &builder->context_load_expectations();
  elided_truncates_ = builder->elided_truncates().empty()
                          ? nullptr
                          : &builder->elided_truncates();
  pin_home_verify_ = builder->pin_home_verify().empty()
                         ? nullptr
                         : &builder->pin_home_verify();
  call_literals_.clear();
  pins_.clear();
  pins_live_ = false;
  pin_verify_ = cvars::a64_pin_verify;
  pinned_resume_stubs_.clear();
  count_blocks_ = cvars::log_translation_stats;
  const auto& pin_sync = builder->pin_sync();
  uint32_t pin_all_mask = 0;
  uint32_t pin_written_mask = 0;
  for (const auto& slot : builder->pinned_context_slots()) {
    PinnedGuestRegister pin;
    pin.offset = slot.offset;
    pin.type = slot.type;
    pin.written = slot.written;
    pin.shared = slot.shared;
    // hakuX-Boosted: a shared register has no bit: no mask ever names it,
    // so it is neither loaded at entry nor written back or read again at
    // any way out. (They come after the function's own pins in the list.)
    pin.bit = slot.shared ? 0 : uint32_t(1) << pins_.size();
    pin_all_mask |= pin.bit;
    if (slot.written) {
      pin_written_mask |= pin.bit;
    }
    if (slot.reg_set == 0) {
      if (slot.reg_index >= gpr_count_ || (slot.type != hir::INT64_TYPE &&
                                          slot.type != hir::INT8_TYPE)) {
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
  if (pins_.size() > 32) {
    XELOGE("A64: too many pinned slots in {:08X}", current_guest_function_);
    return false;
  }
  // Without pin_guest_registers_wide: every written register is stored and
  // every register loaded at each sync point, as before.
  const bool pin_by_liveness = pin_sync.by_liveness;
  pin_cur_dirty_ = pin_written_mask;
  pin_cur_live_after_ = pin_all_mask;
  pin_last_sync_reload_ = pin_all_mask;

  // Calculate local variable stack offsets.
  // hakuX-Boosted: once per function, not once per attempt. Laying a local
  // out turns it into a constant holding its offset, which loses its type,
  // so a second pass over the locals would size them all as 32-bit and
  // overlap them (found by a64_short_forward_branches_test_retry).
  if (!locals_laid_out_) {
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
    locals_stack_size_ = xe::align(stack_offset, static_cast<size_t>(16));
    locals_laid_out_ = true;
  }
  const size_t stack_offset = locals_stack_size_;

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

  // hakuX-Boosted: a64_lean_calls applies to a function all of whose calls
  // that hand on a return address (every call but a tail call) come after
  // the SET_RETURN_ADDRESS that names it in the same block, which is how
  // the front end emits them. The address is then a constant at the call
  // and the frame's slot for it is not used at all.
  lean_calls_ = cvars::a64_lean_calls;
  for (auto block = builder->first_block(); block && lean_calls_;
       block = block->next) {
    bool named = false;
    for (auto i = block->instr_head; i; i = i->next) {
      switch (i->GetOpcodeNum()) {
        case hir::OPCODE_SET_RETURN_ADDRESS:
          named = true;
          break;
        case hir::OPCODE_CALL:
        case hir::OPCODE_CALL_TRUE:
        case hir::OPCODE_CALL_INDIRECT:
        case hir::OPCODE_CALL_INDIRECT_TRUE:
          if (!(i->flags & hir::CALL_TAIL) && !named) {
            lean_calls_ = false;
          }
          break;
        default:
          break;
      }
    }
  }
  pending_return_address_valid_ = false;
  x0_const_valid_ = false;
  x0_const_noted_ = false;

  static_assert(kFramePreviousOffset == StackLayout::GUEST_RESERVED &&
                    kFramePreviousOffset == StackLayout::HOST_RET_ADDR + 8 &&
                    kFrameGuestStackOffset == StackLayout::GUEST_RET_ADDR + 4,
                "the frame chain's slots");
  if (frame_chain_) {
    // hakuX-Boosted: a64_frame_chain. The call record is the frame: the
    // caller's x29 goes next to the host return address (one paired store
    // in place of the single one), the guest stack pointer of entry into
    // the upper half of the guest return address's slot, and x29 points at
    // this frame from here to the epilog. ResolveFunction walks x29 back
    // from the frame a non-local return was made from.
    stp(x30, x29, ptr(sp, static_cast<int32_t>(StackLayout::HOST_RET_ADDR)));
    str(x0, ptr(sp, static_cast<uint32_t>(StackLayout::GUEST_RET_ADDR)));
    if (SharedHostReg(1) >= 0) {  // hakuX-Boosted: r1 is in a register
      str(WReg(SharedHostReg(1)),
          ptr(sp, static_cast<uint32_t>(kFrameGuestStackOffset)));
    } else {
      ldr(w8, ptr(x20, static_cast<int32_t>(offsetof(ppc::PPCContext, r[1]))));
      str(w8, ptr(sp, static_cast<uint32_t>(kFrameGuestStackOffset)));
    }
    mov(x29, sp);
  } else {
  // Store host return address (x30/LR) so the epilog can restore it.
  str(x30, ptr(sp, static_cast<uint32_t>(StackLayout::HOST_RET_ADDR)));
  // Store guest PPC return address (passed in x0 by convention).
  str(x0, ptr(sp, static_cast<uint32_t>(StackLayout::GUEST_RET_ADDR)));
  }
  // Store zero for call return address (we haven't made a call yet).
  if (!lean_calls_) {
    str(xzr, ptr(sp, static_cast<uint32_t>(StackLayout::GUEST_CALL_RET_ADDR)));
  }

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
    EmitPinnedReload(pin_by_liveness ? pin_sync.entry_live : pin_all_mask);
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
  // hakuX-Boosted: a64_fpcr_known_block_entry. A block is entered by the
  // branches that name its label and by falling out of the block before
  // it, and by nothing else (a return site after a call is in the middle
  // of a block, and the call has already made the mode unknown there). So
  // when every branch to a block has been emitted by the time the block
  // is reached, the mode each left FPCR in is known, and if they and the
  // block before agree, that is the mode the block starts in. A block with
  // a branch to it still to come (a loop's first block) is unknown as
  // before, and so is the function's first.
  bool fpcr_flow = cvars::a64_fpcr_known_block_entry ||
                   cvars::a64_fpcr_known_block_entry_verify;
  struct FpcrEdges {
    uint32_t total = 0;  // branches in the function that name the block
    uint32_t seen = 0;   // of which emitted so far
    FPCRMode mode = FPCRMode::Unknown;
    bool agree = true;
  };
  std::unordered_map<const hir::Block*, FpcrEdges> fpcr_edges;
  const auto branch_target = [](const hir::Instr* i) -> const hir::Label* {
    switch (i->GetOpcodeNum()) {
      case hir::OPCODE_BRANCH:
        return i->src1.label;
      case hir::OPCODE_BRANCH_TRUE:
      case hir::OPCODE_BRANCH_FALSE:
        return i->src2.label;
      default:
        return nullptr;
    }
  };
  if (fpcr_flow) {
    for (auto b = builder->first_block(); b && fpcr_flow; b = b->next) {
      for (auto i = b->instr_head; i; i = i->next) {
        const hir::Opcode num = i->GetOpcodeNum();
        if (num != hir::OPCODE_BRANCH && num != hir::OPCODE_BRANCH_TRUE &&
            num != hir::OPCODE_BRANCH_FALSE) {
          continue;
        }
        const hir::Label* label = branch_target(i);
        if (!label || !label->block) {
          fpcr_flow = false;  // a branch that cannot be followed
          break;
        }
        ++fpcr_edges[label->block].total;
      }
    }
  }
  bool fpcr_first_block = true;
  bool fpcr_previous_falls_through = false;
  FPCRMode fpcr_previous_end = FPCRMode::Unknown;
  // hakuX-Boosted: a64_guest_memory_order, read once per function.
  const int guest_order_level = GuestMemoryOrderLevel();
  while (block) {
    // Reset FPCR tracking on each block entry (we don't know which
    // predecessor ran, so mode is unknown).
    FPCRMode fpcr_entry = FPCRMode::Unknown;
    if (fpcr_flow && !fpcr_first_block) {
      bool any = false;
      bool agree = true;
      uint32_t total = 0;
      uint32_t seen = 0;
      auto edges = fpcr_edges.find(block);
      if (edges != fpcr_edges.end()) {
        total = edges->second.total;
        seen = edges->second.seen;
        if (seen) {
          any = true;
          fpcr_entry = edges->second.mode;
          agree = edges->second.agree;
        }
      }
      if (fpcr_previous_falls_through) {
        if (!any) {
          any = true;
          fpcr_entry = fpcr_previous_end;
        } else if (fpcr_entry != fpcr_previous_end) {
          agree = false;
        }
      }
      if (!any || !agree || seen != total) {
        fpcr_entry = FPCRMode::Unknown;
      }
    }
    fpcr_first_block = false;
    if (fpcr_entry != FPCRMode::Unknown) {
      fpcr_mode_ = fpcr_entry;
      compiler::translation_stats.fpcr_known_entries.fetch_add(
          1, std::memory_order_relaxed);
    } else {
      ForgetFpcrMode();
    }
    flags_key_.valid = false;  // hakuX-Boosted: nor what the flags hold
    pending_return_address_valid_ = false;
    x0_const_valid_ = false;

    // Bind all labels targeting this block.
    auto label = block->label_head;
    while (label) {
      auto& host_label = GetLabel(label->id);
      L(host_label);
      // hakuX-Boosted: where it was placed, for IsNearBoundLabel.
      bound_label_offsets_[&host_label] = getSize();
      label = label->next;
    }
    // hakuX-Boosted: log_translation_stats counts the blocks that run.
    if (count_blocks_) {
      EmitBlockCounter();
    }
    // hakuX-Boosted: a64_fpcr_known_block_entry_verify, here so that every
    // way into the block runs it.
    if (fpcr_entry != FPCRMode::Unknown) {
      if (cvars::a64_fpcr_known_block_entry_verify) {
        // FPCR against the value of the mode this block is taken to start
        // in. x0 and x1 are free between instructions; no flag is written.
        auto& same = NewCachedLabel();
        mrs(x0, 3, 3, 4, 4, 0);
        ldr(w1, ptr(x19, static_cast<uint32_t>(
                             fpcr_entry == FPCRMode::Vmx
                                 ? offsetof(A64BackendContext, fpcr_vmx)
                                 : offsetof(A64BackendContext, fpcr_fpu))));
        eor(w0, w0, w1);
        cbz(w0, same);
        mov(x1, reinterpret_cast<uint64_t>(
                    &compiler::translation_stats.fpcr_entry_mismatches));
        ldr(x0, ptr(x1));
        add(x0, x0, 1);
        str(x0, ptr(x1));
        L(same);
        if (!cvars::a64_fpcr_known_block_entry) {
          // Checking only: emit what would have been emitted without.
          fpcr_mode_ = FPCRMode::Unknown;
        }
      }
    }

    // Process each instruction in the block.
    const hir::Instr* instr = block->instr_head;
    while (instr) {
      // After a guest call, check for longjmp on the next real instruction.
      // Skip SOURCE_OFFSET because the return address from the call would
      // point past the check, so it would never execute.
      if (synchronize_stack_on_next_instruction_) {
        if (instr->GetOpcodeNum() == hir::OPCODE_COMMENT) {
          // hakuX-Boosted: a comment (the front end writes one ahead of the
          // source offset when debug information is asked for, as the
          // instruction tests do) emits nothing, and the check must not
          // go in front of it: the source offset after it is where a
          // longjmp resumes, and it would resume past the check.
        } else if (instr->GetOpcodeNum() != hir::OPCODE_SOURCE_OFFSET) {
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
              {static_cast<uint32_t>(instr->src1.offset), &resume_target, 0,
               pin_last_sync_reload_});
        }
      }
      // hakuX-Boosted: what a write-back or a reload at this instruction
      // covers (also used by host calls made from inside its sequence). An
      // instruction the pass did not see (a spill the allocator added) has
      // no entry and needs none: it neither leaves the function nor calls.
      if (pin_by_liveness) {
        auto sync_it = pin_sync.per_instr.find(instr);
        if (sync_it != pin_sync.per_instr.end()) {
          pin_cur_dirty_ = sync_it->second.dirty_before;
          pin_cur_live_after_ = sync_it->second.live_after;
        } else {
          pin_cur_dirty_ = 0;
          pin_cur_live_after_ = 0;
        }
      }
      // hakuX-Boosted: pin_guest_registers_coalesce. An instruction whose
      // result the allocator had computed straight into a pin's host
      // register (its destination is that register, and it is not the load
      // of the pin itself) ends the old value of that pin: the store that
      // follows it in the same block is what made the allocator do this,
      // and only pure instructions lie between. So a host helper called
      // from inside this instruction's sequence neither writes that pin
      // back (the register may already hold part of the result) nor reads
      // it again (which would overwrite the result with the old value).
      if (pin_by_liveness && pins_live_ && instr->dest &&
          GET_OPCODE_SIG_TYPE_DEST(instr->opcode->signature) ==
              hir::OPCODE_SIG_TYPE_V &&
          instr->dest->reg.set) {
        const bool is_int = instr->dest->type <= hir::INT64_TYPE;
        const uint32_t index = static_cast<uint32_t>(instr->dest->reg.index);
        const uint32_t host =
            is_int ? (index < uint32_t(gpr_count_) ? gpr_reg_map_[index] : ~0u)
                   : (index < uint32_t(VEC_COUNT) ? vec_reg_map_[index] : ~0u);
        for (const auto& pin : pins_) {
          const bool pin_is_int =
              pin.type == hir::INT64_TYPE || pin.type == hir::INT8_TYPE;
          if (pin_is_int != is_int || pin.host_reg != host) {
            continue;
          }
          if (!(instr->GetOpcodeNum() == hir::OPCODE_LOAD_CONTEXT &&
                static_cast<uint32_t>(instr->src1.offset) == pin.offset)) {
            pin_cur_dirty_ &= ~pin.bit;
            pin_cur_live_after_ &= ~pin.bit;
          }
          break;
        }
      }
      // hakuX-Boosted: pin_guest_registers_coalesce_verify, the uses.
      if (pin_home_verify_ && pins_live_) {
        const uint32_t signature = instr->opcode->signature;
        const hir::Value* sources[3] = {
            GET_OPCODE_SIG_TYPE_SRC1(signature) == hir::OPCODE_SIG_TYPE_V
                ? instr->src1.value
                : nullptr,
            GET_OPCODE_SIG_TYPE_SRC2(signature) == hir::OPCODE_SIG_TYPE_V
                ? instr->src2.value
                : nullptr,
            GET_OPCODE_SIG_TYPE_SRC3(signature) == hir::OPCODE_SIG_TYPE_V
                ? instr->src3.value
                : nullptr,
        };
        for (int n = 0; n < 3; ++n) {
          if (!sources[n] || (n > 0 && sources[n] == sources[0]) ||
              (n > 1 && sources[n] == sources[1])) {
            continue;
          }
          auto home = pin_home_verify_->find(sources[n]);
          if (home != pin_home_verify_->end() &&
              home->second.slot < pins_.size()) {
            EmitPinHomeVerify(sources[n], pins_[home->second.slot],
                              home->second.low32);
          }
        }
      }
      // hakuX-Boosted: bring the context up to date before any instruction
      // that leaves this function's code, and read the pinned registers back
      // after any that returns here. See EmitPinnedFlush.
      PinSync pin_sync_kind =
          pins_live_ ? ClassifyPinSync(instr) : PinSync::kNone;
      // hakuX-Boosted: with pin_guest_registers_wide the barrier the front
      // end puts before a branch is not a way out of the function (the pass
      // made the same choice, and the two must agree).
      if (pin_by_liveness &&
          instr->GetOpcodeNum() == hir::OPCODE_CONTEXT_BARRIER &&
          (instr->flags & hir::CONTEXT_BARRIER_FOR_BRANCH)) {
        pin_sync_kind = PinSync::kNone;
      }
      if (pin_sync_kind != PinSync::kNone) {
        EmitPinnedFlush(pin_cur_dirty_);
        compiler::translation_stats.pin_sync_sites.fetch_add(
            1, std::memory_order_relaxed);
        pin_last_sync_reload_ = pin_cur_live_after_;
      }
      // hakuX-Boosted: a compare that only feeds the branch after it leaves
      // its answer in the flags (see EmitCompareResult).
      bool zero_test_taken = false;
      FlagsKey compare_key;
      if (cvars::a64_fuse_compare_branch) {
        bool nothing_written = true;
        if (const hir::Instr* branch =
                FindFusableBranch(instr, &nothing_written)) {
          fuse_compare_pending_ = true;
          fused_cond_valid_ = false;
          fused_zero_test_ = false;
          fused_branch_ = branch;
          compiler::translation_stats.compares_fused.fetch_add(
              1, std::memory_order_relaxed);
          // An equality test against zero whose register is certain to be
          // unchanged at the branch: nothing is emitted here.
          const hir::Opcode op = instr->GetOpcodeNum();
          if (nothing_written &&
              (op == hir::OPCODE_COMPARE_EQ || op == hir::OPCODE_COMPARE_NE)) {
            const hir::Value* a = instr->src1.value;
            const hir::Value* b = instr->src2.value;
            if (a->IsConstant() && !b->IsConstant()) {
              std::swap(a, b);
            }
            if (!a->IsConstant() && b->IsConstant() &&
                (a->type == hir::INT32_TYPE || a->type == hir::INT64_TYPE) &&
                b->IsConstantZero()) {
              fused_zero_reg_ = MapReg(a, gpr_reg_map_, gpr_count_, "gpr");
              fused_zero_is64_ = a->type == hir::INT64_TYPE;
              fused_zero_test_ = true;
              fused_cond_ = op == hir::OPCODE_COMPARE_EQ ? EQ : NE;
              fused_cond_valid_ = true;
              fuse_compare_pending_ = false;
              zero_test_taken = true;
            }
          }
        }
        // The flags cache: the same compare as the one the flags hold needs
        // no second cmp.
        if (!zero_test_taken) {
          compare_key = MakeFlagsKey(instr);
          if (compare_key.valid && compare_key == flags_key_) {
            skip_next_cmp_ = true;
            compiler::translation_stats.compares_reused_flags.fetch_add(
                1, std::memory_order_relaxed);
          }
        }
      }
      last_opcode = instr->GetOpcodeNum();
      const hir::Instr* new_tail = instr;
      bool selected = false;
      // hakuX-Boosted: a64_guest_memory_order (see its definition).
      bool order_after_load = false;
      // Level 1 here; level 2 is emitted by the sequences themselves.
      if (guest_order_level == 1 && !zero_test_taken) {
        switch (instr->GetOpcodeNum()) {
          case hir::OPCODE_STORE:
          case hir::OPCODE_STORE_OFFSET:
          case hir::OPCODE_MEMSET:
          case hir::OPCODE_RESERVED_STORE:
            dmb(Xbyak_aarch64::ISHST);
            break;
          case hir::OPCODE_LOAD:
          case hir::OPCODE_LOAD_OFFSET:
          case hir::OPCODE_RESERVED_LOAD:
            order_after_load = true;
            break;
          default:
            break;
        }
      }
      try {
        if (zero_test_taken) {
          selected = true;
          new_tail = instr->next;
        } else {
          selected = SelectSequence(this, instr, &new_tail);
        }
        if (order_after_load && selected) {
          dmb(Xbyak_aarch64::ISHLD);
        }
        skip_next_cmp_ = false;
        // hakuX-Boosted: a64_fpcr_known_block_entry. The branches this
        // sequence emitted (its own instruction, and one fused into it)
        // left FPCR in the mode tracked now: no sequence changes the mode
        // after its branch.
        if (fpcr_flow && selected) {
          for (const hir::Instr* done = instr; done && done != new_tail;
               done = done->next) {
            const hir::Label* label = branch_target(done);
            if (!label) {
              continue;
            }
            auto& edges = fpcr_edges[label->block];
            if (edges.seen++ == 0) {
              edges.mode = fpcr_mode_;
            } else if (edges.mode != fpcr_mode_) {
              edges.agree = false;
            }
          }
        }
        // hakuX-Boosted: a64_lean_calls, what x0 holds now: the constant
        // this instruction's sequence said it left there, or nothing known
        // once any other instruction that emits code has run.
        if (x0_const_noted_) {
          x0_const_noted_ = false;
          x0_const_valid_ = true;
        } else {
          switch (instr->GetOpcodeNum()) {
            case hir::OPCODE_SOURCE_OFFSET:
            case hir::OPCODE_COMMENT:
            case hir::OPCODE_NOP:
            case hir::OPCODE_CONTEXT_BARRIER:
              break;
            case hir::OPCODE_SET_RETURN_ADDRESS:
              if (!lean_calls_) {
                x0_const_valid_ = false;
              }
              break;
            default:
              x0_const_valid_ = false;
              break;
          }
        }
        // What the flags hold now.
        if (compare_key.valid) {
          flags_key_ = compare_key;
        } else if (!zero_test_taken) {
          switch (instr->GetOpcodeNum()) {
            case hir::OPCODE_SOURCE_OFFSET:
            case hir::OPCODE_COMMENT:
            case hir::OPCODE_NOP:
            case hir::OPCODE_STORE_CONTEXT:
            case hir::OPCODE_STORE_LOCAL:
            case hir::OPCODE_LOAD_LOCAL:
            case hir::OPCODE_CONTEXT_BARRIER:
              // These write no flag in any mode (see FindFusableBranch).
              break;
            default:
              flags_key_.valid = false;
              break;
          }
        }
        // hakuX-Boosted: a compare sequence that did not take the offer to
        // fuse has put its answer in the register as usual.
        fuse_compare_pending_ = false;
        if (selected && pin_sync_kind == PinSync::kFlushAndReload) {
          EmitPinnedReload(pin_cur_live_after_);
        }
        // hakuX-Boosted: pin_guest_registers_coalesce_verify. A value the
        // setting would have computed into a pin: write the pin here, where
        // the setting would have written it, from the value's own register.
        if (selected && pin_home_verify_ && pins_live_ && instr->dest &&
            GET_OPCODE_SIG_TYPE_DEST(instr->opcode->signature) ==
                hir::OPCODE_SIG_TYPE_V) {
          auto home = pin_home_verify_->find(instr->dest);
          if (home != pin_home_verify_->end() && home->second.is_def &&
              home->second.slot < pins_.size()) {
            const auto& pin = pins_[home->second.slot];
            const uint32_t index =
                static_cast<uint32_t>(instr->dest->reg.index);
            switch (pin.type) {
              case hir::INT64_TYPE:
                mov(XReg(pin.host_reg), XReg(gpr_reg_map_[index]));
                break;
              case hir::INT8_TYPE:
                mov(WReg(pin.host_reg), WReg(gpr_reg_map_[index]));
                break;
              case hir::FLOAT64_TYPE:
                fmov(DReg(pin.host_reg), DReg(vec_reg_map_[index]));
                break;
              default:
                orr(VReg(pin.host_reg).b16, VReg(vec_reg_map_[index]).b16,
                    VReg(vec_reg_map_[index]).b16);
                break;
            }
          }
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

    // hakuX-Boosted: a64_fpcr_known_block_entry, what the next block
    // inherits by position.
    fpcr_previous_end = fpcr_mode_;
    fpcr_previous_falls_through =
        !block->instr_tail ||
        (block->instr_tail->GetOpcodeNum() != hir::OPCODE_BRANCH &&
         block->instr_tail->GetOpcodeNum() != hir::OPCODE_RETURN);

    block = block->next;
  }

  // hakuX-Boosted: every branch to the epilog wrote the pinned registers
  // back first (returns are sync points). Falling off the last block into it
  // must do the same; after a plain return it already has.
  if (pins_live_ && pin_by_liveness) {
    EmitPinnedFlush(pin_sync.exit_dirty);
  } else if (pins_live_ && last_opcode != hir::OPCODE_RETURN) {
    EmitPinnedFlush(pin_written_mask);
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
  EmitRestoreHostReturn();
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
      EmitPinnedReload(stub.reload_mask);
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
  // hakuX-Boosted: the literals of the linked call sites, 8 bytes each on
  // an 8-byte boundary (functions are placed on 16-byte boundaries), each
  // holding the resolve thunk's address until the cache links it.
  if (!call_literals_.empty()) {
    if (getSize() & 7) {
      nop();
    }
    const uint64_t thunk =
        reinterpret_cast<uint64_t>(backend()->resolve_function_thunk());
    size_t linked = 0;
    for (auto& literal : call_literals_) {
      L(*literal.label);
      literal.code_offset = static_cast<uint32_t>(getSize());
      const uint64_t value = literal.guest_target ? thunk : literal.fixed;
      linked += literal.guest_target ? 1 : 0;
      dd(static_cast<uint32_t>(value));
      dd(static_cast<uint32_t>(value >> 32));
    }
    compiler::translation_stats.linked_call_sites.fetch_add(
        linked, std::memory_order_relaxed);
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
  block_labels_.clear();
  fuse_compare_pending_ = false;
  fused_cond_valid_ = false;
  fused_zero_test_ = false;
  fused_branch_ = nullptr;
  flags_key_.valid = false;
  skip_next_cmp_ = false;

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

bool A64Emitter::UseShortBranch(const Xbyak_aarch64::Label& label,
                                size_t max_distance) {
  if (IsNearBoundLabel(label, max_distance)) {
    return true;
  }
  // hakuX-Boosted: a64_lean_calls, the epilog too: it comes after every
  // block, and the test at a return branches to it.
  const bool is_epilog =
      lean_calls_ && epilog_label_ && &label == epilog_label_;
  if (!short_forward_ok_ || bound_label_offsets_.count(&label) ||
      !(block_labels_.count(&label) || is_epilog)) {
    return false;
  }
  (max_distance == kNearBranch14 ? used_short_forward14_
                                 : used_short_forward19_) = true;
  return true;
}

// hakuX-Boosted: a64_fuse_compare_branch. The instructions allowed between
// the compare and its branch are ones whose sequences emit nothing that
// writes the flags in any mode: source offsets, comments, context stores
// (str, or a register move for a pinned slot), spills to locals and the
// barrier the front end puts before the branch (nothing, or a pinned
// write-back and reload, which are stores and loads).
namespace {
bool IsIntegerCompare(hir::Opcode opcode) {
  switch (opcode) {
    case hir::OPCODE_COMPARE_EQ:
    case hir::OPCODE_COMPARE_NE:
    case hir::OPCODE_COMPARE_SLT:
    case hir::OPCODE_COMPARE_SLE:
    case hir::OPCODE_COMPARE_SGT:
    case hir::OPCODE_COMPARE_SGE:
    case hir::OPCODE_COMPARE_ULT:
    case hir::OPCODE_COMPARE_ULE:
    case hir::OPCODE_COMPARE_UGT:
    case hir::OPCODE_COMPARE_UGE:
      return true;
    default:
      return false;
  }
}
}  // namespace

// hakuX-Boosted: the key of a 32-bit or 64-bit integer compare, for the
// flags cache. (8-bit and 16-bit compares extend their operands into
// scratch registers first and are left alone.) A constant operand is keyed
// by its bits, a value by its identity: HIR values are assigned once, and a
// value that is still being used is still in its register (a spilled value
// comes back under a new identity; a value living in a pinned register has
// no use after that register is next written).
A64Emitter::FlagsKey A64Emitter::MakeFlagsKey(const hir::Instr* compare) {
  FlagsKey key;
  if (!IsIntegerCompare(compare->GetOpcodeNum()) || !compare->src1.value ||
      !compare->src2.value) {
    return key;
  }
  const hir::TypeName type = compare->src1.value->type;
  // hakuX-Boosted: float compares too, between two registers (a constant
  // operand goes through a scratch register in the sequences, and is left
  // out). The key only ever lets a second compare of the same operands skip
  // its fcmp; FlagsHoldCompare, which names integer conditions, declines it.
  if (type == hir::FLOAT32_TYPE || type == hir::FLOAT64_TYPE) {
    if (compare->src1.value->IsConstant() ||
        compare->src2.value->IsConstant() ||
        compare->src2.value->type != type) {
      return key;
    }
  } else if (type != hir::INT32_TYPE && type != hir::INT64_TYPE) {
    return key;
  }
  key.type = type;
  const hir::Value* operands[2] = {compare->src1.value, compare->src2.value};
  for (int n = 0; n < 2; ++n) {
    if (operands[n]->IsConstant()) {
      key.constant[n] = type == hir::INT32_TYPE
                            ? static_cast<uint32_t>(operands[n]->constant.i32)
                            : static_cast<uint64_t>(operands[n]->constant.i64);
    } else {
      key.value[n] = operands[n];
    }
  }
  key.valid = true;
  return key;
}

bool A64Emitter::FlagsHoldCompare(const hir::Value* result,
                                  Xbyak_aarch64::Cond* cond) {
  if (!flags_key_.valid || !result || result->IsConstant() || !result->def ||
      !(MakeFlagsKey(result->def) == flags_key_)) {
    return false;
  }
  if (flags_key_.type != hir::INT32_TYPE &&
      flags_key_.type != hir::INT64_TYPE) {
    return false;  // a float compare: the conditions below are integer ones
  }
  switch (result->def->GetOpcodeNum()) {
    case hir::OPCODE_COMPARE_EQ:
      *cond = EQ;
      break;
    case hir::OPCODE_COMPARE_NE:
      *cond = NE;
      break;
    case hir::OPCODE_COMPARE_SLT:
      *cond = LT;
      break;
    case hir::OPCODE_COMPARE_SLE:
      *cond = LE;
      break;
    case hir::OPCODE_COMPARE_SGT:
      *cond = GT;
      break;
    case hir::OPCODE_COMPARE_SGE:
      *cond = GE;
      break;
    case hir::OPCODE_COMPARE_ULT:
      *cond = LO;
      break;
    case hir::OPCODE_COMPARE_ULE:
      *cond = LS;
      break;
    case hir::OPCODE_COMPARE_UGT:
      *cond = HI;
      break;
    case hir::OPCODE_COMPARE_UGE:
      *cond = HS;
      break;
    default:
      return false;
  }
  compiler::translation_stats.branches_on_flags.fetch_add(
      1, std::memory_order_relaxed);
  return true;
}

const hir::Instr* A64Emitter::FindFusableBranch(
    const hir::Instr* compare, bool* nothing_written) const {
  *nothing_written = true;
  switch (compare->GetOpcodeNum()) {
    case hir::OPCODE_COMPARE_EQ:
    case hir::OPCODE_COMPARE_NE:
    case hir::OPCODE_COMPARE_SLT:
    case hir::OPCODE_COMPARE_SLE:
    case hir::OPCODE_COMPARE_SGT:
    case hir::OPCODE_COMPARE_SGE:
    case hir::OPCODE_COMPARE_ULT:
    case hir::OPCODE_COMPARE_ULE:
    case hir::OPCODE_COMPARE_UGT:
    case hir::OPCODE_COMPARE_UGE:
      break;
    default:
      return nullptr;
  }
  const hir::Value* result = compare->dest;
  if (!result || !compare->src1.value ||
      compare->src1.value->type > hir::INT64_TYPE) {
    return nullptr;  // float and vector compares are not handled
  }
  if (!result->use_head || result->use_head->next) {
    return nullptr;  // not exactly one use
  }
  for (const hir::Instr* i = compare->next; i; i = i->next) {
    switch (i->GetOpcodeNum()) {
      case hir::OPCODE_SOURCE_OFFSET:
      case hir::OPCODE_COMMENT:
      case hir::OPCODE_NOP:
      case hir::OPCODE_STORE_LOCAL:
      case hir::OPCODE_CONTEXT_BARRIER:
        continue;
      case hir::OPCODE_STORE_CONTEXT:
        if (!pins_.empty()) {
          const uint32_t offset = static_cast<uint32_t>(i->src1.offset);
          for (const auto& pin : pins_) {
            if (pin.offset == offset) {
              *nothing_written = false;
            }
          }
        }
        continue;
      case hir::OPCODE_LOAD_LOCAL:
        *nothing_written = false;
        continue;
      case hir::OPCODE_BRANCH_TRUE:
      case hir::OPCODE_BRANCH_FALSE:
        return i->src1.value == result ? i : nullptr;
      default:
        return nullptr;
    }
  }
  return nullptr;
}

void A64Emitter::b(const Xbyak_aarch64::Cond cond,
                   const Xbyak_aarch64::Label& label) {
  if (UseShortBranch(label, kNearBranch19)) {
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
  if (UseShortBranch(label, kNearBranch19)) {
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
  if (UseShortBranch(label, kNearBranch19)) {
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
  if (UseShortBranch(label, kNearBranch19)) {
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
  if (UseShortBranch(label, kNearBranch19)) {
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
  if (UseShortBranch(label, kNearBranch14)) {
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
  if (UseShortBranch(label, kNearBranch14)) {
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
  if (UseShortBranch(label, kNearBranch14)) {
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
  if (UseShortBranch(label, kNearBranch14)) {
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
    EmitLoadCallTarget(reinterpret_cast<uint64_t>(fn->machine_code()));
    if (!(instr->flags & hir::CALL_TAIL)) {
      // Pass the next call's guest return address in x0.
      EmitCallReturnAddress();
      blr(x9);
      synchronize_stack_on_next_instruction_ = true;
    } else {
      // Tail call: pass our return address to the callee.
      PopStackpoint();
      EmitLoadGuestReturnAddress();
      EmitRestoreHostReturn();
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
    if (linked_calls_ok_) {
      // hakuX-Boosted: a64_linked_calls. The target comes from a literal in
      // this function's tail: the resolve thunk until the callee has code,
      // the callee from then on (A64CodeCache::LinkCallSites).
      auto& literal = NewCachedLabel();
      call_literals_.push_back({function->address(), &literal, 0});
      ldr(x9, literal);
    } else if (!code_cache_->encoded_indirection()) {
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
    if (frame_chain_) {  // hakuX-Boosted: where its walk starts
      str(x29, ptr(x19, static_cast<uint32_t>(
                            offsetof(A64BackendContext, resolve_frame))));
    }
    EmitSharedGprsToContext();  // hakuX-Boosted: it reads r1
    mov(x9, reinterpret_cast<uint64_t>(&ResolveFunction));
    blr(x9);
    mov(x9, x0);  // resolved address in x9
  }

  if (instr->flags & hir::CALL_TAIL) {
    PopStackpoint();
    EmitLoadGuestReturnAddress();
    EmitRestoreHostReturn();
    if (stack_size() <= 4095) {
      add(sp, sp, static_cast<uint32_t>(stack_size()));
    } else {
      mov(x17, static_cast<uint64_t>(stack_size()));
      add(sp, sp, x17, UXTX);
    }
    br(x9);
  } else {
    EmitCallReturnAddress();
    blr(x9);
    synchronize_stack_on_next_instruction_ = true;
  }
}

void A64Emitter::EmitCallReturnAddress() {
  if (!lean_calls_) {
    ldr(x0, ptr(sp, static_cast<uint32_t>(StackLayout::GUEST_CALL_RET_ADDR)));
    return;
  }
  // (lean_calls_ is only set when a SET_RETURN_ADDRESS precedes every such
  // call in its block; a zero here would mean that check and this code
  // disagree, and is what the frame slot held before any call.)
  const uint64_t address =
      pending_return_address_valid_ ? pending_return_address_ : 0;
  if (!(x0_const_valid_ && x0_const_ == address)) {
    mov(x0, address);
  }
}

void A64Emitter::EmitLoadCallTarget(uint64_t host_address) {
  if (lean_calls_ && call_literals_allowed_) {
    auto& literal = NewCachedLabel();
    call_literals_.push_back({0, &literal, 0, host_address});
    ldr(x9, literal);
    return;
  }
  mov(x9, host_address);
}

void A64Emitter::CallIndirect(const hir::Instr* instr, int reg_index) {
  ForgetFpcrMode();
  auto target_w = WReg(reg_index);

  // Check if this is a possible return (e.g., PPC blr).
  if (instr->flags & hir::CALL_POSSIBLE_RETURN) {
    // Compare target guest address with our function's return address.
    ldr(w0, ptr(sp, static_cast<uint32_t>(StackLayout::GUEST_RET_ADDR)));
    x0_const_valid_ = false;  // hakuX-Boosted
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

      // hakuX-Boosted: a64_indirect_call_range_check. The table covers
      // 0x80000000..0x9FFFFFFF only (target >> 29 == 4); a target outside
      // it is a bad guest pointer (Gears of War, 2026-11-14: CTR
      // 0x40360000) and the read below would be past the table.
      if (cvars::a64_indirect_call_range_check) {
        Label target_in_range;
        lsr(w15, w16, 29);
        mov(w14, 4);
        cmp(w15, w14);
        b(EQ, target_in_range);
        brk(0xF0C1);
        L(target_in_range);
      }

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
    if (frame_chain_) {  // hakuX-Boosted: where its walk starts
      str(x29, ptr(x19, static_cast<uint32_t>(
                            offsetof(A64BackendContext, resolve_frame))));
    }
    EmitSharedGprsToContext();  // hakuX-Boosted: it reads r1
    mov(x9, reinterpret_cast<uint64_t>(&ResolveFunction));
    blr(x9);
    mov(x9, x0);  // resolved address
  }

  if (instr->flags & hir::CALL_TAIL) {
    // Tail call: pass our return address to the callee.
    PopStackpoint();
    EmitLoadGuestReturnAddress();
    EmitRestoreHostReturn();
    if (stack_size() <= 4095) {
      add(sp, sp, static_cast<uint32_t>(stack_size()));
    } else {
      mov(x17, static_cast<uint64_t>(stack_size()));
      add(sp, sp, x17, UXTX);
    }
    br(x9);
  } else {
    // Regular call: pass the next call's return address.
    EmitCallReturnAddress();
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
  // hakuX-Boosted: or when instructions do follow in the block and none of
  // them uses a value defined before the call. That is the case of an
  // import thunk (sc 2; blr: the blr's instructions follow the call in one
  // block and load everything afresh), which is how titles reach nearly
  // every kernel export, and the "last instruction of its block" test never
  // held for it: the setting did nothing there (the bench's host_calls
  // loop, finding 3 of bench-harness.md). Checked by position, not by
  // ordinals: a value used after the call is live across it exactly when
  // its definition is not among the instructions after the call. Pinned
  // guest registers are written back before this instruction and read back
  // after it by Emit's loop, so they do not count.
  bool nothing_live_across = instr && !instr->next;
  if (cvars::a64_lean_extern_thunk && instr && instr->next) {
    using hir::OpcodeSignatureType;
    std::unordered_set<const hir::Value*> defined_after;
    nothing_live_across = true;
    for (const hir::Instr* i = instr->next; i && nothing_live_across;
         i = i->next) {
      const uint32_t signature = i->opcode->signature;
      const hir::Value* sources[3] = {
          GET_OPCODE_SIG_TYPE_SRC1(signature) == hir::OPCODE_SIG_TYPE_V
              ? i->src1.value
              : nullptr,
          GET_OPCODE_SIG_TYPE_SRC2(signature) == hir::OPCODE_SIG_TYPE_V
              ? i->src2.value
              : nullptr,
          GET_OPCODE_SIG_TYPE_SRC3(signature) == hir::OPCODE_SIG_TYPE_V
              ? i->src3.value
              : nullptr,
      };
      for (const hir::Value* source : sources) {
        if (source && !source->IsConstant() &&
            !defined_after.count(source)) {
          nothing_live_across = false;
        }
      }
      if (GET_OPCODE_SIG_TYPE_DEST(signature) == hir::OPCODE_SIG_TYPE_V &&
          i->dest) {
        defined_after.insert(i->dest);
      }
    }
  }
  if (cvars::a64_lean_extern_thunk && nothing_live_across &&
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
    EmitPinnedFlush(pin_cur_dirty_);
  }
  // GuestToHostThunk: x0=target function, x1/x2=args (set by caller).
  // The thunk rearranges: saves x0 in x9, sets x0=context, calls x9.
  mov(x0, reinterpret_cast<uint64_t>(fn));
  // hakuX-Boosted: x16 with the extended pool, see HostCallReg.
  const XReg thunk_reg = HostCallReg();
  mov(thunk_reg, reinterpret_cast<uint64_t>(backend()->guest_to_host_thunk()));
  blr(thunk_reg);
  if (sync_pins) {
    EmitPinnedReload(pin_cur_live_after_);
  }
}

uint32_t A64Emitter::EmitSaveExtendedGprsForBareCall() {
  if (!extended_gpr_pool()) {
    return 0;
  }
  if (scratch_gpr_pool()) {  // hakuX-Boosted: and x4-x7
    stp(x9, x10, pre_ptr(sp, -96));
    stp(x11, x12, ptr(sp, 16));
    stp(x13, x14, ptr(sp, 32));
    stp(x15, x4, ptr(sp, 48));
    stp(x5, x6, ptr(sp, 64));
    str(x7, ptr(sp, 80));
    return 96;
  }
  stp(x9, x10, pre_ptr(sp, -64));
  stp(x11, x12, ptr(sp, 16));
  stp(x13, x14, ptr(sp, 32));
  str(x15, ptr(sp, 48));
  return 64;
}

void A64Emitter::EmitRestoreExtendedGprsAfterBareCall() {
  if (!extended_gpr_pool()) {
    return;
  }
  if (scratch_gpr_pool()) {
    ldr(x7, ptr(sp, 80));
    ldp(x5, x6, ptr(sp, 64));
    ldp(x15, x4, ptr(sp, 48));
    ldp(x13, x14, ptr(sp, 32));
    ldp(x11, x12, ptr(sp, 16));
    ldp(x9, x10, post_ptr(sp, 96));
    return;
  }
  ldr(x15, ptr(sp, 48));
  ldp(x13, x14, ptr(sp, 32));
  ldp(x11, x12, ptr(sp, 16));
  ldp(x9, x10, post_ptr(sp, 64));
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
//   - the two host functions called with a bare blr (RSQRT_F64, RSQRT_V128):
//     they never see the context, but nothing saves the vector registers a C
//     function may use, so they are bracketed the same way
//     (EmitPinnedSyncBeforeBareHostCall).
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
//
// In a64_pin_verify mode a difference found while a pinned register sits in
// a vector register across a bare blr is not possible either: the reload
// after it comes from the context, which that mode keeps current.
void A64Emitter::EmitPinnedFlush(uint32_t mask) {
  if (pin_verify_) {
    // The context is written at every store in this mode and is the copy
    // that counts; a register that went wrong must not overwrite it.
    return;
  }
  for (const auto& pin : pins_) {
    if (!(mask & pin.bit)) {
      continue;  // equal to its context copy, or not holding a value
    }
    compiler::translation_stats.pin_sync_stores.fetch_add(
        1, std::memory_order_relaxed);
    switch (pin.type) {
      case hir::INT64_TYPE:
        str(XReg(pin.host_reg), ptr(x20, pin.offset));
        break;
      case hir::INT8_TYPE:
        strb(WReg(pin.host_reg), ptr(x20, pin.offset));
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

void A64Emitter::EmitPinnedReload(uint32_t mask) {
  for (const auto& pin : pins_) {
    if (!(mask & pin.bit)) {
      continue;  // not read again before it is next written
    }
    compiler::translation_stats.pin_sync_loads.fetch_add(
        1, std::memory_order_relaxed);
    switch (pin.type) {
      case hir::INT64_TYPE:
        ldr(XReg(pin.host_reg), ptr(x20, pin.offset));
        break;
      case hir::INT8_TYPE:
        ldrb(WReg(pin.host_reg), ptr(x20, pin.offset));
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

// hakuX-Boosted: log_translation_stats. One counter for blocks of functions
// with pinned registers and one for the rest, bumped at each block head
// without a lock: several guest threads lose a few counts to each other,
// which a share of blocks does not need. x16 and x17 are free at a block
// head and no flag is touched.
void A64Emitter::EmitBlockCounter() {
  auto& counter = pins_.empty()
                      ? compiler::translation_stats.blocks_run_unpinned
                      : compiler::translation_stats.blocks_run_pinned;
  mov(x17, reinterpret_cast<uint64_t>(&counter));
  ldr(x16, ptr(x17));
  add(x16, x16, 1);
  str(x16, ptr(x17));
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
  if (pin.shared) {
    // hakuX-Boosted: a shared register has no context copy to compare with
    // while guest code runs.
    return;
  }
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
    case hir::INT8_TYPE:
      ldrb(w0, ptr(x20, pin.offset));
      eor(w0, w0, WReg(pin.host_reg));
      and_(x0, x0, 0xFF);
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
  // Reloads the pinned registers from the context on the way back, this one
  // among them whether or not anything reads it again: the run carries on
  // from the context's value.
  const uint32_t saved_live_after = pin_cur_live_after_;
  pin_cur_live_after_ |= pin.bit;
  CallNativeSafe(reinterpret_cast<void*>(&ReportPinnedMismatch));
  pin_cur_live_after_ = saved_live_after;
  L(same);
}

// (The mismatch path below calls a host function, which leaves the flags
// undefined: whatever they were known to hold is forgotten.)
static uint64_t ReportPinHomeMismatch(void* raw_context, uint64_t function,
                                      uint64_t offset) {
  const uint64_t count =
      compiler::translation_stats.verify_mismatches.fetch_add(
          1, std::memory_order_relaxed);
  if (count < 64) {
    XELOGE(
        "TranslatorVerify: pin_guest_registers_coalesce would have used a "
        "pinned register whose contents had changed: context offset {:#x} in "
        "guest function {:08X}",
        offset, static_cast<uint32_t>(function));
  }
  return 0;
}

void A64Emitter::EmitPinHomeVerify(const hir::Value* value,
                                   const PinnedGuestRegister& pin,
                                   bool low32) {
  ForgetFpcrMode();  // the report goes through the thunk, as EmitPinnedVerify
  flags_key_.valid = false;
  x0_const_valid_ = false;
  Label& same = NewCachedLabel();
  const uint32_t index = static_cast<uint32_t>(value->reg.index);
  switch (pin.type) {
    case hir::INT64_TYPE:
      if (low32) {
        eor(w0, WReg(gpr_reg_map_[index]), WReg(pin.host_reg));
      } else {
        eor(x0, XReg(gpr_reg_map_[index]), XReg(pin.host_reg));
      }
      break;
    case hir::INT8_TYPE:
      eor(w0, WReg(gpr_reg_map_[index]), WReg(pin.host_reg));
      and_(x0, x0, 0xFF);
      break;
    case hir::FLOAT64_TYPE:
      fmov(x0, DReg(vec_reg_map_[index]));
      fmov(x1, DReg(pin.host_reg));
      eor(x0, x0, x1);
      break;
    default:
      eor(VReg(0).b16, VReg(vec_reg_map_[index]).b16,
          VReg(pin.host_reg).b16);
      umov(x0, VReg(0).d2[0]);
      umov(x1, VReg(0).d2[1]);
      orr(x0, x0, x1);
      break;
  }
  cbz(x0, same);
  mov(x1, static_cast<uint64_t>(current_guest_function_));
  mov(x2, static_cast<uint64_t>(pin.offset));
  CallNativeSafe(reinterpret_cast<void*>(&ReportPinHomeMismatch));
  L(same);
}

static uint64_t ReportDeadStoreRead(void* raw_context, uint64_t function,
                                    uint64_t offset) {
  const uint64_t count =
      compiler::translation_stats.verify_mismatches.fetch_add(
          1, std::memory_order_relaxed);
  if (count < 64) {
    XELOGE(
        "TranslatorVerify: a store eliminate_dead_context_stores took for "
        "dead was read: context offset {:#x} in guest function {:08X}",
        offset, static_cast<uint32_t>(function));
  }
  return 0;
}

static uint64_t ReportContextConstantMismatch(void* raw_context,
                                             uint64_t function,
                                             uint64_t offset) {
  const uint64_t count =
      compiler::translation_stats.verify_mismatches.fetch_add(
          1, std::memory_order_relaxed);
  if (count < 64) {
    XELOGE(
        "TranslatorVerify: propagate_context_constants expected a constant "
        "the guest register did not hold: context offset {:#x} in guest "
        "function {:08X}",
        offset, static_cast<uint32_t>(function));
  }
  return 0;
}

void A64Emitter::EmitContextConstantVerify(const hir::Instr* load,
                                           const Xbyak_aarch64::XReg& value,
                                           uint32_t offset, uint32_t size) {
  if (!load_expectations_) {
    return;
  }
  auto it = load_expectations_->find(load);
  if (it == load_expectations_->end()) {
    return;
  }
  ForgetFpcrMode();  // the report goes through the thunk
  Label& fine = NewCachedLabel();
  if (size == 1) {
    mov(w17, static_cast<uint64_t>(it->second & 0xFF));
    eor(w0, WReg(value.getIdx()), w17);
    and_(x0, x0, 0xFF);
  } else {
    mov(x17, it->second);
    eor(x0, value, x17);
  }
  cbz(x0, fine);
  mov(x1, static_cast<uint64_t>(current_guest_function_));
  mov(x2, static_cast<uint64_t>(offset));
  CallNativeSafe(reinterpret_cast<void*>(&ReportContextConstantMismatch));
  L(fine);
}

void A64Emitter::EmitStackFixupVerify() {
  Label& back = NewCachedLabel();
  Label& count = AddToTail([&back](A64Emitter& e, Label&) {
    // x16 is borrowed and put back: some sequences hold a value in it
    // across the address computation.
    e.str(e.x16, pre_ptr(e.sp, -16));
    e.mov(e.x16, reinterpret_cast<uint64_t>(
                     &compiler::translation_stats.stack_fixup_verify_hits));
    e.ldr(e.x17, ptr(e.x16));
    e.add(e.x17, e.x17, 1);
    e.str(e.x17, ptr(e.x16));
    e.ldr(e.x16, post_ptr(e.sp, 16));
    e.CodeGenerator::b(back);
  });
  cbnz(w17, count);  // the far-safe form: tail code can be far away
  L(back);
}

void A64Emitter::EmitDeadStoreVerify(const Xbyak_aarch64::XReg& value,
                                     uint32_t offset, uint32_t size) {
  if (!compiler::passes::IsDeadStoreCandidateSlot(
          offset, size, cvars::eliminate_dead_context_stores)) {
    return;
  }
  ForgetFpcrMode();  // the report goes through the thunk, as EmitPinnedVerify
  Label& fine = NewCachedLabel();
  if (size == 1) {
    mov(w17, static_cast<uint64_t>(compiler::passes::kDeadStorePoison8));
    eor(w0, WReg(value.getIdx()), w17);
    and_(x0, x0, 0xFF);
  } else {
    mov(x17, compiler::passes::kDeadStorePoison64);
    eor(x0, value, x17);
  }
  cbnz(x0, fine);
  mov(x1, static_cast<uint64_t>(current_guest_function_));
  mov(x2, static_cast<uint64_t>(offset));
  CallNativeSafe(reinterpret_cast<void*>(&ReportDeadStoreRead));
  L(fine);
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
  if (lean_calls_) {
    // hakuX-Boosted: a64_lean_calls. Nothing here: the call materialises it.
    pending_return_address_ = value;
    pending_return_address_valid_ = true;
    return;
  }
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
    compiler::translation_stats.fpcr_sets_emitted.fetch_add(
        1, std::memory_order_relaxed);  // hakuX-Boosted
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
  const uint32_t flush_mask = pin_cur_dirty_;
  const uint32_t reload_mask = pin_cur_live_after_;
  Label& do_yield =
      AddToTail([&after, flag_offset, sync_pins, flush_mask,
                 reload_mask](A64Emitter& e, Label&) {
        e.strb(e.wzr, ptr(e.x20, flag_offset));
        // Null until the scheduler starts, and a stale flag can reach here
        // after it shuts down, so check before calling.
        e.mov(e.x0, reinterpret_cast<uint64_t>(
                        &xe::cpu::backend::preempt_yield_handler));
        e.ldr(e.x0, ptr(e.x0));
        e.cbz(e.x0, after);
        if (sync_pins) {
          e.EmitPinnedFlush(flush_mask);
        }
        // hakuX-Boosted: x16 with the extended pool (pinned registers may
        // sit in x9-x15 and only some are read back below).
        const XReg thunk_reg = e.HostCallReg();
        e.mov(thunk_reg,
              reinterpret_cast<uint64_t>(e.backend()->guest_to_host_thunk()));
        e.blr(thunk_reg);
        if (sync_pins) {
          e.EmitPinnedReload(reload_mask);
        }
        e.b(after);
      });
  if (cvars::log_safepoint_pc && guest_address) {
    // Diagnostic only: costs a materialize + store on every loop back-edge, so
    // it stays off unless a wedge is being chased.
    static_assert(offsetof(ppc::PPCContext, last_safepoint_pc) < 16384);
    // hakuX-Boosted: w17 with the extended pool, where w9 may be a pinned
    // register.
    const WReg pc_reg = extended_gpr_pool() ? w17 : w9;
    mov(pc_reg, guest_address);
    str(pc_reg, ptr(x20, static_cast<uint32_t>(
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
  block_labels_.insert(label);  // hakuX-Boosted: see UseShortBranch
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

// hakuX-Boosted: see frame_chain().
void A64Emitter::EmitRestoreHostReturn() {
  if (frame_chain_) {
    ldp(x30, x29, ptr(sp, static_cast<int32_t>(StackLayout::HOST_RET_ADDR)));
  } else {
    ldr(x30, ptr(sp, static_cast<uint32_t>(StackLayout::HOST_RET_ADDR)));
  }
}

void A64Emitter::EmitLoadGuestReturnAddress() {
  if (frame_chain_) {
    // The slot's upper half is the guest stack pointer of entry.
    ldr(w0, ptr(sp, static_cast<uint32_t>(StackLayout::GUEST_RET_ADDR)));
  } else {
    ldr(x0, ptr(sp, static_cast<uint32_t>(StackLayout::GUEST_RET_ADDR)));
  }
}

void A64Emitter::PushStackpoint() {
  if (!cvars::a64_enable_host_guest_stack_synchronization ||
      frame_chain_ == 1) {
    return;
  }
  // hakuX-Boosted: a64_lean_calls. The same record, written with fewer
  // instructions: the entry's address by a shifted add (as
  // a64_compact_stackpoint_push), the guest stack pointer and return
  // address with one paired store (they are adjacent 32-bit fields), and
  // the overflow test as a test of one bit: the depth goes up by one at a
  // time, so it reaches a limit of 2^n the first time bit n is set.
  const uint64_t max_stackpoints =
      static_cast<uint64_t>(cvars::a64_max_stackpoints);
  if (lean_calls_ && max_stackpoints >= 2 && max_stackpoints < (1ull << 31) &&
      (max_stackpoints & (max_stackpoints - 1)) == 0) {
    static_assert(sizeof(A64BackendStackpoint) == 16,
                  "the shift below is the entry size");
    static_assert(offsetof(A64BackendStackpoint, guest_return_address_) ==
                      offsetof(A64BackendStackpoint, guest_stack_) + 4,
                  "the paired store below writes both");
    ldr(x8, ptr(x19, static_cast<uint32_t>(
                         offsetof(A64BackendContext, stackpoints))));
    ldr(w9, ptr(x19, static_cast<uint32_t>(offsetof(
                         A64BackendContext, current_stackpoint_depth))));
    add(x8, x8, x9, LSL, 4);
    mov(x10, sp);
    str(x10, ptr(x8, static_cast<uint32_t>(
                         offsetof(A64BackendStackpoint, host_stack_))));
    if (SharedHostReg(1) >= 0) {  // hakuX-Boosted: r1 is in a register
      mov(w10, WReg(SharedHostReg(1)));
    } else {
      ldr(w10,
          ptr(x20, static_cast<int32_t>(offsetof(ppc::PPCContext, r[1]))));
    }
    ldr(w11, ptr(x20, static_cast<int32_t>(offsetof(ppc::PPCContext, lr))));
    stp(w10, w11, ptr(x8, static_cast<int32_t>(offsetof(A64BackendStackpoint,
                                                        guest_stack_))));
    add(w9, w9, 1);
    str(w9, ptr(x19, static_cast<uint32_t>(offsetof(
                         A64BackendContext, current_stackpoint_depth))));
    auto& overflow = AddToTail([](A64Emitter& e, Label& lbl) {
      e.CallNativeSafe(
          reinterpret_cast<void*>(A64Emitter::HandleStackpointOverflowError));
    });
    uint32_t bit = 0;
    while ((1ull << bit) != max_stackpoints) {
      ++bit;
    }
    tbnz(w9, bit, overflow);
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
  if (SharedHostReg(1) >= 0) {  // hakuX-Boosted: r1 is in a register
    mov(w10, WReg(SharedHostReg(1)));
  } else {
    ldr(w10, ptr(x20, static_cast<int32_t>(offsetof(ppc::PPCContext, r[1]))));
  }
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
  if (!cvars::a64_enable_host_guest_stack_synchronization ||
      frame_chain_ == 1) {
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
  // hakuX-Boosted: with the frame chain ResolveFunction's answer is a
  // helper that puts the stack right before the return site runs, so
  // there is nothing to check after a call.
  if (!cvars::a64_enable_host_guest_stack_synchronization ||
      frame_chain_ == 1) {
    return;
  }
  // ResolveFunction marks this when it returns a return-site address inside an
  // existing frame. The marker lives in backend context because native SP can
  // still point at a skipped frame here.
  auto& return_from_sync = NewCachedLabel();

  ldr(w16, ptr(x19, static_cast<uint32_t>(offsetof(
                        A64BackendContext, pending_stackpoint_sync_depth))));
  if (lean_calls_) {
    // hakuX-Boosted: a64_lean_calls. The label is three instructions on:
    // the one-instruction form always reaches.
    CodeGenerator::cbz(w16, return_from_sync);
  } else {
    cbz(w16, return_from_sync);
  }

  auto& sync_label = AddToTail([](A64Emitter& e, Label& lbl) {
    // x8 was set up in the body to point at return_from_sync; do that there
    // instead of here because adr's ±1 MiB range can't span body+tail in
    // large functions.
    //   x8 = return address (where to resume after fixup)
    // hakuX-Boosted: x17 with the extended pool: the return site this goes
    // back to runs with the pinned registers loaded, x10 may be one.
    const XReg helper_reg = extended_gpr_pool() ? e.x17 : e.x10;
    e.mov(helper_reg,
          reinterpret_cast<uint64_t>(
              e.backend()->synchronize_guest_and_host_stack_helper()));
    e.br(helper_reg);
  });
  adr(x8, return_from_sync);
  b(sync_label);

  L(return_from_sync);
}

}  // namespace a64
}  // namespace backend
}  // namespace cpu
}  // namespace xe
