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
  // The debugger reads and writes guest registers through the context.
  if (!cvars::pin_guest_registers || cvars::debug) {
    return true;
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

}  // namespace passes
}  // namespace compiler
}  // namespace cpu
}  // namespace xe
