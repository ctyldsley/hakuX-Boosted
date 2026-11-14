/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * hakuX-Boosted: chooses the guest registers a function keeps in host        *
 * registers. Released under the BSD license.                                 *
 ******************************************************************************
 */

#include "xenia/cpu/compiler/passes/guest_register_pinning_pass.h"

#include <algorithm>
#include <cstddef>
#include <unordered_map>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/base/math.h"
#include "xenia/cpu/compiler/translation_stats.h"
#include "xenia/cpu/hir/hir_builder.h"
#include "xenia/cpu/ppc/ppc_context.h"

DEFINE_bool(
    pin_guest_registers, false,
    "Keep the guest registers a function uses most inside its loops in host "
    "registers for the whole function, instead of loading each from the "
    "guest context at its first use in every block and storing it back at "
    "every change. The context copy is brought up to date before every guest "
    "call, host call, return, trap and scheduler yield, and read back after. "
    "Only functions with a loop are touched, and only registers for which the "
    "saving inside loops outweighs the extra traffic round calls. ARM64 "
    "backend only. Takes effect for functions translated after it is set.",
    "CPU");

DEFINE_int32(
    pin_guest_registers_max_int, 2,
    "With pin_guest_registers: the most integer guest registers one function "
    "may pin. Each takes one of the seven host registers the allocator has "
    "for integer values, so more pins mean more spills in busy blocks. "
    "Clamped to 0..3.",
    "CPU");

DEFINE_int32(
    pin_guest_registers_max_vec, 8,
    "With pin_guest_registers: the most float and vector guest registers "
    "(taken together) one function may pin, out of the 28 host vector "
    "registers. Clamped to 0..12.",
    "CPU");

// hakuX-Boosted: see GuestRegisterPinningPass::RunWide.
DEFINE_bool(
    pin_guest_registers_wide, false,
    "With pin_guest_registers: pin in every function where it removes "
    "loads and stores, not only in functions with a loop whose registers "
    "are not used round calls. Where each pinned register is written back "
    "to the guest context and read again is then worked out per call, "
    "return and exit from what the function does with the register (it is "
    "stored only where it may have changed and loaded only where it is read "
    "again), so a register held over calls costs no more traffic than "
    "without pinning. Takes effect for functions translated after it is "
    "set.",
    "CPU");

DEFINE_bool(
    pin_guest_registers_flags, false,
    "With pin_guest_registers_wide: the condition register bits, the carry "
    "bit and the link register may be pinned as well as the general, count, "
    "float and vector registers. A pinned bit takes one of the integer host "
    "registers pin_guest_registers_max_int allows.",
    "CPU");

DEFINE_bool(
    pin_guest_registers_stress, false,
    "For the translator's tests, not for play: with pin_guest_registers_wide, "
    "pin the most used registers of every function whether or not that "
    "saves anything, so that every write-back and reload the backend can "
    "emit is exercised. Slower code.",
    "CPU");

DECLARE_bool(debug);

namespace xe {
namespace cpu {
namespace compiler {
namespace passes {

// TODO(benvanik): remove when enums redefined.
using namespace xe::cpu::hir;

using xe::cpu::backend::MachineInfo;
using xe::cpu::hir::Block;
using xe::cpu::hir::HIRBuilder;
using xe::cpu::hir::Instr;
using xe::cpu::ppc::PPCContext;

namespace {

// How much one execution of a block at this loop depth counts for. Only a
// ranking device: nothing here affects correctness.
uint64_t WeightForDepth(uint32_t depth) {
  switch (depth) {
    case 0:
      return 1;
    case 1:
      return 8;
    case 2:
      return 64;
    default:
      return 512;
  }
}

// The net saving a slot must show before it is pinned: one access inside a
// loop, clear of all costs.
constexpr int64_t kMinimumNetBenefit = 8;

// Which guest register a context slot is, if it is one this pass may pin,
// and in which host register set it would live.
//
// Never pinned, so that code outside the function's own instructions that
// reads the context directly keeps seeing what it sees today:
//   r1   the stack pointer: read from the context by the prologue's
//        stackpoint push, by ResolveFunction's longjmp test and by the
//        kernel;
//   r2   reserved by the ABI, never worth it;
//   r13  the thread's PCR pointer, read by host code everywhere;
//   lr   read by the prologue and written by the host around callbacks;
//   everything that is not a general, float or vector register (condition
//   and XER bytes, FPSCR, the reservation and scheduler fields).
bool ClassifySlot(uint32_t offset, TypeName type, uint8_t* out_reg_set,
                  int* out_gpr_index) {
  *out_gpr_index = -1;
  const uint32_t gpr_base = static_cast<uint32_t>(offsetof(PPCContext, r));
  if (type == INT64_TYPE) {
    if (offset >= gpr_base && offset < gpr_base + 32 * 8 &&
        (offset - gpr_base) % 8 == 0) {
      const int n = static_cast<int>((offset - gpr_base) / 8);
      if (n == 1 || n == 2 || n == 13) {
        return false;
      }
      *out_reg_set = 0;
      *out_gpr_index = n;
      return true;
    }
    if (offset == offsetof(PPCContext, ctr)) {
      *out_reg_set = 0;
      *out_gpr_index = 32;
      return true;
    }
    return false;
  }
  if (type == FLOAT64_TYPE) {
    const uint32_t fpr_base = static_cast<uint32_t>(offsetof(PPCContext, f));
    if (offset >= fpr_base && offset < fpr_base + 32 * 8 &&
        (offset - fpr_base) % 8 == 0) {
      *out_reg_set = 1;
      return true;
    }
    return false;
  }
  if (type == VEC128_TYPE) {
    const uint32_t vr_base = static_cast<uint32_t>(offsetof(PPCContext, v));
    if (offset >= vr_base && offset < vr_base + 128 * 16 &&
        (offset - vr_base) % 16 == 0) {
      *out_reg_set = 1;
      return true;
    }
    return false;
  }
  return false;
}

struct SlotUse {
  uint32_t offset = 0;
  TypeName type = INT64_TYPE;
  uint32_t size = 0;
  uint64_t benefit = 0;
  uint32_t loads = 0;
  uint32_t stores = 0;
  bool written = false;
  bool candidate = false;
  uint8_t reg_set = 0;
  int gpr_index = -1;
  int64_t net = 0;
};

}  // namespace

GuestRegisterPinningPass::GuestRegisterPinningPass(
    const MachineInfo* machine_info)
    : CompilerPass() {
  // The integer set, and the one set that holds both float and vector
  // values. A backend that splits those two is not supported: nothing is
  // pinned in vector registers there.
  for (size_t n = 0; n < 8 && machine_info->register_sets[n].count; ++n) {
    const auto& set = machine_info->register_sets[n];
    if (set.types & MachineInfo::RegisterSet::INT_TYPES) {
      int_register_count_ = set.count;
    }
    if ((set.types & MachineInfo::RegisterSet::FLOAT_TYPES) &&
        (set.types & MachineInfo::RegisterSet::VEC_TYPES)) {
      vec_register_count_ = set.count;
    }
  }
}

GuestRegisterPinningPass::~GuestRegisterPinningPass() = default;

bool GuestRegisterPinningPass::Run(HIRBuilder* builder) {
  auto& pinned = builder->pinned_context_slots();
  pinned.clear();
  builder->pin_sync().Clear();
  // The debugger reads and writes guest registers through the context.
  if (!cvars::pin_guest_registers || cvars::debug) {
    return true;
  }
  if (cvars::pin_guest_registers_wide) {
    return RunWide(builder);
  }
  auto& stats = translation_stats;
  stats.pin_functions_considered.fetch_add(1, std::memory_order_relaxed);

  // How many host registers may be given up. The allocator always keeps at
  // least four integer and sixteen vector registers for its own values.
  int max_int = std::clamp(cvars::pin_guest_registers_max_int, 0, 3);
  int max_vec = std::clamp(cvars::pin_guest_registers_max_vec, 0, 12);
  if (int_register_count_ < 7) {
    max_int = 0;
  }
  if (vec_register_count_ < 28) {
    max_vec = 0;
  }
  if (!max_int && !max_vec) {
    return true;
  }

  // Blocks in layout order. They are laid out in guest address order, so a
  // branch to a block at or before its own is a loop back-edge; the loop is
  // taken to be every block from the target to the branch. That is exact for
  // the loops compilers emit and only an estimate otherwise, which is all a
  // weight needs.
  std::vector<Block*> blocks;
  std::unordered_map<const Block*, uint32_t> block_index;
  for (auto block = builder->first_block(); block; block = block->next) {
    block_index[block] = static_cast<uint32_t>(blocks.size());
    blocks.push_back(block);
  }
  if (blocks.empty()) {
    return true;
  }
  std::vector<uint32_t> loop_end(blocks.size(), UINT32_MAX);
  bool any_loop = false;
  for (uint32_t b = 0; b < blocks.size(); ++b) {
    for (auto i = blocks[b]->instr_head; i; i = i->next) {
      const Label* label = nullptr;
      if (i->opcode == &OPCODE_BRANCH_info) {
        label = i->src1.label;
      } else if (i->opcode == &OPCODE_BRANCH_TRUE_info ||
                 i->opcode == &OPCODE_BRANCH_FALSE_info) {
        label = i->src2.label;
      }
      if (!label || !label->block) {
        continue;
      }
      auto it = block_index.find(label->block);
      if (it == block_index.end() || it->second > b) {
        continue;
      }
      const uint32_t head = it->second;
      if (loop_end[head] == UINT32_MAX || loop_end[head] < b) {
        loop_end[head] = b;
      }
      any_loop = true;
    }
  }
  if (!any_loop) {
    // Without a loop there is nothing to gain that would pay for the
    // traffic at the function's entry and exits.
    return true;
  }
  stats.pin_functions_with_loops.fetch_add(1, std::memory_order_relaxed);
  std::vector<int32_t> depth_delta(blocks.size() + 1, 0);
  for (uint32_t head = 0; head < blocks.size(); ++head) {
    if (loop_end[head] != UINT32_MAX) {
      depth_delta[head] += 1;
      depth_delta[loop_end[head] + 1] -= 1;
    }
  }

  // One walk: every context access by (offset, type), and the weight of the
  // places where the backend will have to write pinned registers back and
  // read them again.
  std::vector<SlotUse> slots;
  std::unordered_map<uint64_t, size_t> slot_index;
  uint64_t sync_weight = 0;   // written back and read again
  uint64_t flush_weight = 0;  // written back only
  int32_t depth = 0;
  for (uint32_t b = 0; b < blocks.size(); ++b) {
    depth += depth_delta[b];
    const uint64_t weight =
        WeightForDepth(static_cast<uint32_t>(std::max(depth, 0)));
    for (auto i = blocks[b]->instr_head; i; i = i->next) {
      const bool is_load = i->opcode == &OPCODE_LOAD_CONTEXT_info;
      const bool is_store = i->opcode == &OPCODE_STORE_CONTEXT_info;
      if (is_load || is_store) {
        const uint32_t offset = static_cast<uint32_t>(i->src1.offset);
        const TypeName type = is_load ? i->dest->type : i->src2.value->type;
        const uint64_t key = (static_cast<uint64_t>(offset) << 8) |
                             static_cast<uint64_t>(type);
        auto it = slot_index.find(key);
        if (it == slot_index.end()) {
          SlotUse use;
          use.offset = offset;
          use.type = type;
          use.size = static_cast<uint32_t>(GetTypeSize(type));
          use.candidate =
              ClassifySlot(offset, type, &use.reg_set, &use.gpr_index);
          it = slot_index.emplace(key, slots.size()).first;
          slots.push_back(use);
        }
        auto& use = slots[it->second];
        use.benefit += weight;
        if (is_store) {
          use.written = true;
          ++use.stores;
        } else {
          ++use.loads;
        }
        continue;
      }
      switch (i->opcode->num) {
        case OPCODE_CALL:
        case OPCODE_CALL_INDIRECT:
          if (i->flags & CALL_TAIL) {
            flush_weight += weight;
          } else {
            sync_weight += weight;
          }
          break;
        case OPCODE_CALL_TRUE:
        case OPCODE_CALL_INDIRECT_TRUE:
        case OPCODE_CALL_EXTERN:
        case OPCODE_TRAP:
        case OPCODE_TRAP_TRUE:
        case OPCODE_DEBUG_BREAK:
        case OPCODE_DEBUG_BREAK_TRUE:
        case OPCODE_CONTEXT_BARRIER:
          sync_weight += weight;
          break;
        case OPCODE_RETURN:
        case OPCODE_RETURN_TRUE:
          flush_weight += weight;
          break;
        default:
          break;
      }
    }
  }

  // A slot may be pinned only if every access to any of its bytes in this
  // function is the same access: same offset, same type. A register that is
  // also read or written through another shape (half of it, or as another
  // type) stays in the context, where both shapes see the same bytes.
  for (auto& use : slots) {
    if (!use.candidate) {
      continue;
    }
    for (const auto& other : slots) {
      if (&other == &use) {
        continue;
      }
      if (other.offset < use.offset + use.size &&
          use.offset < other.offset + other.size) {
        use.candidate = false;
        break;
      }
    }
    if (!use.candidate) {
      continue;
    }
    // Cost: one load at entry; at each place the context must be current, a
    // store if the function writes the register and (where the function
    // carries on) a load.
    const uint64_t cost = 1 + sync_weight * (use.written ? 2 : 1) +
                          (use.written ? flush_weight : 0);
    use.net = static_cast<int64_t>(use.benefit) - static_cast<int64_t>(cost);
    if (use.net < kMinimumNetBenefit) {
      use.candidate = false;
    }
  }

  std::vector<const SlotUse*> order;
  for (const auto& use : slots) {
    if (use.candidate) {
      order.push_back(&use);
    }
  }
  if (order.empty()) {
    return true;
  }
  // Largest saving first; the offset breaks ties so that the choice does not
  // depend on hash order.
  std::sort(order.begin(), order.end(),
            [](const SlotUse* a, const SlotUse* b) {
              if (a->net != b->net) {
                return a->net > b->net;
              }
              return a->offset < b->offset;
            });

  // Host registers are taken from the top of each set: the allocator hands
  // out the lowest free index first.
  int used_int = 0;
  int used_vec = 0;
  for (const SlotUse* use : order) {
    HIRBuilder::PinnedContextSlot slot;
    slot.offset = use->offset;
    slot.type = use->type;
    slot.reg_set = use->reg_set;
    slot.written = use->written;
    if (use->reg_set == 0) {
      if (used_int >= max_int) {
        continue;
      }
      slot.reg_index =
          static_cast<uint8_t>(int_register_count_ - 1 - used_int);
      ++used_int;
      stats.pin_gpr_slots.fetch_add(1, std::memory_order_relaxed);
      if (use->gpr_index >= 0 && use->gpr_index <= 32) {
        stats.pin_by_gpr[use->gpr_index].fetch_add(1,
                                                   std::memory_order_relaxed);
      }
    } else {
      if (used_vec >= max_vec) {
        continue;
      }
      slot.reg_index =
          static_cast<uint8_t>(vec_register_count_ - 1 - used_vec);
      ++used_vec;
      (use->type == FLOAT64_TYPE ? stats.pin_fpr_slots : stats.pin_vr_slots)
          .fetch_add(1, std::memory_order_relaxed);
    }
    stats.pin_loads.fetch_add(use->loads, std::memory_order_relaxed);
    stats.pin_stores.fetch_add(use->stores, std::memory_order_relaxed);
    pinned.push_back(slot);
  }
  if (!pinned.empty()) {
    stats.pin_functions_pinned.fetch_add(1, std::memory_order_relaxed);
  }
  return true;
}


// hakuX-Boosted: pin_guest_registers_wide.
//
// The first form of this pass (Run, above) pins only where a blunt rule is
// cheap: every pinned register the function writes is stored at every way
// out of the function's code and every pinned register is loaded again after
// it. A function that makes calls pays that at each call, so the cost model
// turned down nearly everything: on the device 17 functions of 24,576 were
// pinned.
//
// This form removes the reason. For every candidate register it works out,
// over the function's control flow:
//
//   dirty   (forward, "may"): at this point the host register may hold a
//           value the context copy does not, because the function stored to
//           the register since the last point where the context was made
//           current;
//   live    (backward, "may"): the host register's value may still be used
//           after this point, by a read of the register or by a write-back.
//
// At a way out of the function's code (a "sync point": the same list of
// instructions as before, A64Emitter::ClassifyPinSync) the backend then
// stores exactly the dirty registers and, if control comes back, loads
// exactly the live ones. At entry it loads the ones live there.
//
// Why that is right. A register is *used* at a load of it and wherever it is
// written back; every instruction counts as a write-back of the registers
// dirty before it, because any sequence may call a host helper, which writes
// back what is dirty (A64Emitter::CallNativeSafe). It is *defined* at a
// store to it, at a reload, and at entry. Liveness as computed here is
// exactly "a use is reachable without passing a definition", over a set of
// edges that contains every edge the code can take. So on every real path,
// the last thing before any use is a store, a reload that was emitted
// because the register was live after its sync point, or the entry load
// that was emitted because it was live at entry: the register holds the
// guest register's value whenever it is read or written back. And the
// context is current at every sync point: a register not dirty there has not
// been stored to since the context was last current, and a dirty one is
// written back from a register that, by the above, holds the right value.
// Over-estimating either set only adds a store or a load of a correct value.
//
// With that, holding a register over a call costs at most what the
// per-block form already pays (it stores a changed register before the call
// and loads it at its next use), and usually less, so the cost model no
// longer needs loops: any register read or written in more than one block
// between sync points saves traffic.
namespace {

enum class WideKind : uint8_t {
  kOther,
  kLoad,
  kStore,
  kSyncReload,      // leaves and comes back: write back, then reload
  kSyncFlushEnd,    // leaves and does not come back
  kSyncFlushCont,   // may leave; if it does not, carries on untouched
};

struct WideInstr {
  const Instr* instr;
  WideKind kind;
  int8_t slot;          // candidate bit for kLoad and kStore, else -1
  int32_t target;       // block index a branch here may go to, or -1
  bool unconditional;   // an unconditional branch (no fall through)
  uint64_t dirty_before;
  uint64_t live_after;
};

// Like ClassifySlot, for the wide form: the link register and, with
// pin_guest_registers_flags, the condition register and carry bytes too.
// LR is safe to pin because every reader outside the function's own
// instructions runs at a point where the context is current: the callee's
// prologue (after the call's write-back) and host code (behind a sync
// point).
bool ClassifySlotWide(uint32_t offset, TypeName type, uint8_t* out_reg_set,
                      int* out_gpr_index, bool* out_is_flag) {
  *out_is_flag = false;
  if (ClassifySlot(offset, type, out_reg_set, out_gpr_index)) {
    return true;
  }
  if (!cvars::pin_guest_registers_flags) {
    return false;
  }
  if (type == INT64_TYPE && offset == offsetof(PPCContext, lr)) {
    *out_reg_set = 0;
    *out_gpr_index = 33;
    return true;
  }
  if (type == INT8_TYPE) {
    const uint32_t cr_base = static_cast<uint32_t>(offsetof(PPCContext, cr0));
    if ((offset >= cr_base && offset < cr_base + 32) ||
        offset == offsetof(PPCContext, xer_ca)) {
      *out_reg_set = 0;
      *out_gpr_index = -1;
      *out_is_flag = true;
      return true;
    }
  }
  return false;
}

}  // namespace

bool GuestRegisterPinningPass::RunWide(HIRBuilder* builder) {
  auto& pinned = builder->pinned_context_slots();
  auto& stats = translation_stats;
  stats.pin_functions_considered.fetch_add(1, std::memory_order_relaxed);

  int max_int = std::clamp(cvars::pin_guest_registers_max_int, 0, 3);
  int max_vec = std::clamp(cvars::pin_guest_registers_max_vec, 0, 16);
  if (int_register_count_ < 7) {
    max_int = 0;
  }
  if (vec_register_count_ < 28) {
    max_vec = 0;
  }
  if (!max_int && !max_vec) {
    return true;
  }

  std::vector<Block*> blocks;
  std::unordered_map<const Block*, uint32_t> block_index;
  for (auto block = builder->first_block(); block; block = block->next) {
    block_index[block] = static_cast<uint32_t>(blocks.size());
    blocks.push_back(block);
  }
  if (blocks.empty()) {
    return true;
  }
  const uint32_t block_count = static_cast<uint32_t>(blocks.size());

  // Pass 1: every instruction's kind, the branch edges, the loops (for the
  // weights only), and every context slot's accesses.
  std::vector<std::vector<WideInstr>> code(block_count);
  std::vector<SlotUse> slots;
  std::vector<bool> slot_is_flag;
  std::unordered_map<uint64_t, size_t> slot_index;
  std::vector<uint32_t> loop_end(block_count, UINT32_MAX);
  bool any_loop = false;
  struct Access {
    uint32_t block;
    uint32_t index;
    size_t slot;
  };
  std::vector<Access> accesses;
  for (uint32_t b = 0; b < block_count; ++b) {
    for (auto i = blocks[b]->instr_head; i; i = i->next) {
      WideInstr w = {i, WideKind::kOther, -1, -1, false, 0, 0};
      const bool is_load = i->opcode == &OPCODE_LOAD_CONTEXT_info;
      const bool is_store = i->opcode == &OPCODE_STORE_CONTEXT_info;
      if (is_load || is_store) {
        const uint32_t offset = static_cast<uint32_t>(i->src1.offset);
        const TypeName type = is_load ? i->dest->type : i->src2.value->type;
        const uint64_t key = (static_cast<uint64_t>(offset) << 8) |
                             static_cast<uint64_t>(type);
        auto it = slot_index.find(key);
        if (it == slot_index.end()) {
          SlotUse use;
          use.offset = offset;
          use.type = type;
          use.size = static_cast<uint32_t>(GetTypeSize(type));
          bool is_flag = false;
          use.candidate = ClassifySlotWide(offset, type, &use.reg_set,
                                           &use.gpr_index, &is_flag);
          it = slot_index.emplace(key, slots.size()).first;
          slots.push_back(use);
          slot_is_flag.push_back(is_flag);
        }
        auto& use = slots[it->second];
        if (is_store) {
          use.written = true;
          ++use.stores;
        } else {
          ++use.loads;
        }
        w.kind = is_load ? WideKind::kLoad : WideKind::kStore;
        accesses.push_back(
            {b, static_cast<uint32_t>(code[b].size()), it->second});
      } else {
        const Label* label = nullptr;
        switch (i->opcode->num) {
          case OPCODE_BRANCH:
            label = i->src1.label;
            w.unconditional = true;
            break;
          case OPCODE_BRANCH_TRUE:
          case OPCODE_BRANCH_FALSE:
            label = i->src2.label;
            break;
          case OPCODE_CALL:
          case OPCODE_CALL_INDIRECT:
            w.kind = (i->flags & CALL_TAIL) ? WideKind::kSyncFlushEnd
                                            : WideKind::kSyncReload;
            break;
          case OPCODE_CALL_TRUE:
          case OPCODE_CALL_INDIRECT_TRUE:
          case OPCODE_CALL_EXTERN:
          case OPCODE_TRAP:
          case OPCODE_TRAP_TRUE:
          case OPCODE_DEBUG_BREAK:
          case OPCODE_DEBUG_BREAK_TRUE:
            w.kind = WideKind::kSyncReload;
            break;
          case OPCODE_CONTEXT_BARRIER:
            // The barrier in front of a branch is for the per-block passes
            // (ContextBarrierFlags): this analysis follows the branch
            // itself, and the call or return a branch instruction may
            // become is a sync point in its own right. Treating these as
            // sync points is what kept the first form of this pass from
            // pinning anything in a function with branches.
            if (!(i->flags & CONTEXT_BARRIER_FOR_BRANCH)) {
              w.kind = WideKind::kSyncReload;
            }
            break;
          case OPCODE_RETURN:
            w.kind = WideKind::kSyncFlushEnd;
            break;
          case OPCODE_RETURN_TRUE:
            w.kind = WideKind::kSyncFlushCont;
            break;
          default:
            break;
        }
        if (label) {
          auto it = label->block ? block_index.find(label->block)
                                 : block_index.end();
          if (it == block_index.end()) {
            // A branch this pass cannot follow: pin nothing.
            return true;
          }
          w.target = static_cast<int32_t>(it->second);
          if (it->second <= b) {
            if (loop_end[it->second] == UINT32_MAX ||
                loop_end[it->second] < b) {
              loop_end[it->second] = b;
            }
            any_loop = true;
          }
        }
      }
      code[b].push_back(w);
    }
  }
  if (any_loop) {
    stats.pin_functions_with_loops.fetch_add(1, std::memory_order_relaxed);
  }

  // Candidates: slots that are registers this pass may pin and that are
  // only ever accessed in one shape (see Run). At most 64, the most
  // accessed first, so that the sets fit a machine word.
  std::vector<size_t> candidates;
  for (size_t n = 0; n < slots.size(); ++n) {
    auto& use = slots[n];
    if (!use.candidate) {
      continue;
    }
    if ((use.reg_set == 0 && !max_int) || (use.reg_set == 1 && !max_vec)) {
      use.candidate = false;
      continue;
    }
    for (size_t m = 0; m < slots.size(); ++m) {
      const auto& other = slots[m];
      if (m != n && other.offset < use.offset + use.size &&
          use.offset < other.offset + other.size) {
        use.candidate = false;
        break;
      }
    }
    if (use.candidate) {
      candidates.push_back(n);
    }
  }
  if (candidates.empty()) {
    return true;
  }
  if (candidates.size() > 64) {
    std::sort(candidates.begin(), candidates.end(), [&](size_t a, size_t b) {
      const uint32_t ca = slots[a].loads + slots[a].stores;
      const uint32_t cb = slots[b].loads + slots[b].stores;
      return ca != cb ? ca > cb : slots[a].offset < slots[b].offset;
    });
    candidates.resize(64);
  }
  std::vector<int8_t> bit_of_slot(slots.size(), -1);
  for (size_t n = 0; n < candidates.size(); ++n) {
    bit_of_slot[candidates[n]] = static_cast<int8_t>(n);
  }
  for (const auto& access : accesses) {
    auto& w = code[access.block][access.index];
    w.slot = bit_of_slot[access.slot];
    if (w.slot < 0) {
      w.kind = WideKind::kOther;
    }
  }

  // Whether control can run off the end of a block into the next one.
  const auto falls_through = [&](uint32_t b) {
    if (code[b].empty()) {
      return true;
    }
    const auto& last = code[b].back();
    return !(last.unconditional || last.kind == WideKind::kSyncFlushEnd);
  };

  // Pass 2, forward to a fixed point: dirty.
  std::vector<uint64_t> dirty_in(block_count, 0);
  uint64_t exit_dirty = 0;
  for (bool changed = true; changed;) {
    changed = false;
    for (uint32_t b = 0; b < block_count; ++b) {
      uint64_t state = dirty_in[b];
      const auto merge = [&](uint32_t target) {
        const uint64_t merged = dirty_in[target] | state;
        if (merged != dirty_in[target]) {
          dirty_in[target] = merged;
          changed = true;
        }
      };
      for (auto& w : code[b]) {
        w.dirty_before = state;
        if (w.target >= 0) {
          merge(static_cast<uint32_t>(w.target));
        }
        switch (w.kind) {
          case WideKind::kStore:
            state |= uint64_t(1) << w.slot;
            break;
          case WideKind::kSyncReload:
          case WideKind::kSyncFlushEnd:
          case WideKind::kSyncFlushCont:
            state = 0;
            break;
          default:
            break;
        }
      }
      if (falls_through(b)) {
        if (b + 1 < block_count) {
          merge(b + 1);
        } else {
          exit_dirty |= state;
        }
      }
    }
  }

  // Pass 3, backward to a fixed point: live.
  std::vector<uint64_t> live_in(block_count, 0);
  for (bool changed = true; changed;) {
    changed = false;
    for (uint32_t b = block_count; b-- > 0;) {
      uint64_t state = 0;
      if (falls_through(b)) {
        // Falling off the last block writes back what is dirty there.
        state = b + 1 < block_count ? live_in[b + 1] : exit_dirty;
      }
      for (size_t n = code[b].size(); n-- > 0;) {
        auto& w = code[b][n];
        w.live_after = state;
        switch (w.kind) {
          case WideKind::kLoad:
            state |= uint64_t(1) << w.slot;
            break;
          case WideKind::kStore:
            state &= ~(uint64_t(1) << w.slot);
            break;
          case WideKind::kSyncReload:
          case WideKind::kSyncFlushEnd:
            state = 0;
            break;
          default:
            break;
        }
        if (w.target >= 0) {
          if (w.unconditional) {
            state = live_in[w.target];
          } else {
            state |= live_in[w.target];
          }
        }
        // Any instruction may write back what is dirty before it.
        state |= w.dirty_before;
      }
      if (state != live_in[b]) {
        live_in[b] = state;
        changed = true;
      }
    }
  }

  // Pass 4: what each candidate would save and cost. Units are context
  // loads and stores, weighted by loop depth as in Run. Benefit: its
  // accesses, which become register moves. Cost: the loads and stores the
  // backend will emit to keep it in step.
  std::vector<int32_t> depth_delta(block_count + 1, 0);
  for (uint32_t head = 0; head < block_count; ++head) {
    if (loop_end[head] != UINT32_MAX) {
      depth_delta[head] += 1;
      depth_delta[loop_end[head] + 1] -= 1;
    }
  }
  std::vector<uint64_t> benefit(candidates.size(), 0);
  std::vector<uint64_t> cost(candidates.size(), 0);
  const auto add_bits = [](std::vector<uint64_t>& to, uint64_t mask,
                           uint64_t weight) {
    while (mask) {
      const int bit = xe::tzcnt(mask);
      mask &= mask - 1;
      to[bit] += weight;
    }
  };
  add_bits(cost, live_in[0], 1);
  add_bits(cost, exit_dirty, 1);
  int32_t depth = 0;
  for (uint32_t b = 0; b < block_count; ++b) {
    depth += depth_delta[b];
    const uint64_t weight =
        WeightForDepth(static_cast<uint32_t>(std::max(depth, 0)));
    for (const auto& w : code[b]) {
      switch (w.kind) {
        case WideKind::kLoad:
        case WideKind::kStore:
          benefit[w.slot] += weight;
          break;
        case WideKind::kSyncReload:
          add_bits(cost, w.dirty_before, weight);
          add_bits(cost, w.live_after, weight);
          break;
        case WideKind::kSyncFlushEnd:
        case WideKind::kSyncFlushCont:
          add_bits(cost, w.dirty_before, weight);
          break;
        default:
          break;
      }
    }
  }

  // Choose. A register is worth a host register only if pinning it at least
  // halves its traffic: the moves that replace its loads and stores are not
  // free, and each pin takes a register from the allocator.
  struct Choice {
    size_t candidate;
    int64_t net;
  };
  std::vector<Choice> order;
  for (size_t n = 0; n < candidates.size(); ++n) {
    if (benefit[n] >= 2 * cost[n] + 2 || cvars::pin_guest_registers_stress) {
      order.push_back({n, static_cast<int64_t>(benefit[n]) -
                              static_cast<int64_t>(cost[n])});
    }
  }
  if (order.empty()) {
    return true;
  }
  std::sort(order.begin(), order.end(), [&](const Choice& a, const Choice& b) {
    if (a.net != b.net) {
      return a.net > b.net;
    }
    return slots[candidates[a.candidate]].offset <
           slots[candidates[b.candidate]].offset;
  });
  std::vector<int8_t> pin_of_bit(candidates.size(), -1);
  int used_int = 0;
  int used_vec = 0;
  for (const Choice& choice : order) {
    const size_t slot_number = candidates[choice.candidate];
    const SlotUse& use = slots[slot_number];
    HIRBuilder::PinnedContextSlot slot;
    slot.offset = use.offset;
    slot.type = use.type;
    slot.reg_set = use.reg_set;
    slot.written = use.written;
    if (use.reg_set == 0) {
      if (used_int >= max_int) {
        continue;
      }
      slot.reg_index =
          static_cast<uint8_t>(int_register_count_ - 1 - used_int);
      ++used_int;
      if (slot_is_flag[slot_number]) {
        stats.pin_flag_slots.fetch_add(1, std::memory_order_relaxed);
      } else {
        stats.pin_gpr_slots.fetch_add(1, std::memory_order_relaxed);
      }
      if (use.gpr_index >= 0 && use.gpr_index <= 33) {
        stats.pin_by_gpr[use.gpr_index].fetch_add(1,
                                                   std::memory_order_relaxed);
      }
    } else {
      if (used_vec >= max_vec) {
        continue;
      }
      slot.reg_index =
          static_cast<uint8_t>(vec_register_count_ - 1 - used_vec);
      ++used_vec;
      (use.type == FLOAT64_TYPE ? stats.pin_fpr_slots : stats.pin_vr_slots)
          .fetch_add(1, std::memory_order_relaxed);
    }
    stats.pin_loads.fetch_add(use.loads, std::memory_order_relaxed);
    stats.pin_stores.fetch_add(use.stores, std::memory_order_relaxed);
    pin_of_bit[choice.candidate] = static_cast<int8_t>(pinned.size());
    pinned.push_back(slot);
  }
  if (pinned.empty()) {
    return true;
  }
  stats.pin_functions_pinned.fetch_add(1, std::memory_order_relaxed);
  if (!any_loop) {
    stats.pin_functions_pinned_no_loop.fetch_add(1,
                                                 std::memory_order_relaxed);
  }

  // Hand the backend the sets, renumbered to the chosen registers.
  const auto remap = [&](uint64_t mask) {
    uint32_t out = 0;
    while (mask) {
      const int bit = xe::tzcnt(mask);
      mask &= mask - 1;
      if (pin_of_bit[bit] >= 0) {
        out |= uint32_t(1) << pin_of_bit[bit];
      }
    }
    return out;
  };
  auto& sync = builder->pin_sync();
  sync.by_liveness = true;
  sync.entry_live = remap(live_in[0]);
  sync.exit_dirty = remap(exit_dirty);
  for (uint32_t b = 0; b < block_count; ++b) {
    for (const auto& w : code[b]) {
      HIRBuilder::PinSyncInfo info;
      info.dirty_before = remap(w.dirty_before);
      info.live_after = remap(w.live_after);
      if (info.dirty_before || info.live_after) {
        sync.per_instr.emplace(w.instr, info);
      }
    }
  }
  return true;
}

}  // namespace passes
}  // namespace compiler
}  // namespace cpu
}  // namespace xe
