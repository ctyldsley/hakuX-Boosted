/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * hakuX-Boosted: constants in guest registers, carried across blocks.        *
 * Released under the BSD license.                                            *
 ******************************************************************************
 */

#include "xenia/cpu/compiler/passes/context_constant_pass.h"

#include <cstddef>
#include <unordered_map>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/cpu/compiler/translation_stats.h"
#include "xenia/cpu/hir/hir_builder.h"
#include "xenia/cpu/ppc/ppc_context.h"

DEFINE_bool(
    propagate_context_constants, false,
    "A load of an integer guest register or flag bit becomes a constant when "
    "every path to it last stored that same constant, looking across the "
    "blocks of the function (within one block this already happens). Calls "
    "and anything else that leaves the function's code forget everything. "
    "Takes effect for functions translated after it is set.",
    "CPU");

DEFINE_bool(
    context_constant_verify, false,
    "Self-check for propagate_context_constants, for a checking run only: "
    "the loads stay, and each is compared with the constant the pass worked "
    "out. A line starting 'TranslatorVerify:' means they differed. ARM64 "
    "backend only.",
    "CPU");

DECLARE_bool(debug);

namespace xe {
namespace cpu {
namespace compiler {
namespace passes {

// TODO(benvanik): remove when enums redefined.
using namespace xe::cpu::hir;

using xe::cpu::hir::Block;
using xe::cpu::hir::HIRBuilder;
using xe::cpu::hir::Instr;
using xe::cpu::ppc::PPCContext;

namespace {

// The slots handled: 64-bit integer registers (r0 to r31, CTR, LR) and the
// condition register and XER bytes. Float and vector registers are left
// alone (their constants are rare and their compare in the verify mode is
// not written).
bool IsConstantCandidate(uint32_t offset, TypeName type) {
  if (type == INT64_TYPE) {
    const uint32_t gpr_base = static_cast<uint32_t>(offsetof(PPCContext, r));
    if (offset >= gpr_base && offset < gpr_base + 32 * 8 &&
        (offset - gpr_base) % 8 == 0) {
      return true;
    }
    return offset == offsetof(PPCContext, ctr) ||
           offset == offsetof(PPCContext, lr);
  }
  if (type == INT8_TYPE) {
    const uint32_t cr_base = static_cast<uint32_t>(offsetof(PPCContext, cr0));
    if (offset >= cr_base && offset < cr_base + 32) {
      return true;
    }
    return offset == offsetof(PPCContext, xer_ca) ||
           offset == offsetof(PPCContext, xer_ov) ||
           offset == offsetof(PPCContext, xer_so);
  }
  return false;
}

// What is known of one slot at one point.
struct Known {
  enum State : uint8_t { kUnreached, kConstant, kVaries };
  State state = kUnreached;
  uint64_t value = 0;

  bool operator==(const Known& other) const {
    return state == other.state &&
           (state != kConstant || value == other.value);
  }
};

Known Meet(const Known& a, const Known& b) {
  if (a.state == Known::kUnreached) {
    return b;
  }
  if (b.state == Known::kUnreached) {
    return a;
  }
  if (a.state == Known::kConstant && b.state == Known::kConstant &&
      a.value == b.value) {
    return a;
  }
  Known varies;
  varies.state = Known::kVaries;
  return varies;
}

enum class Kind : uint8_t {
  kOther,
  kLoad,        // of slot `slot`
  kStore,       // to slot `slot`, constant or not
  kStoreOther,  // a context store of another shape: clobbers what it overlaps
  kForgetAll,   // other code may have changed anything
};

struct Item {
  Instr* instr;
  Kind kind;
  int16_t slot;
  int32_t target;
  bool unconditional;
  bool ends;  // control does not continue past it
};

}  // namespace

ContextConstantPass::ContextConstantPass() : CompilerPass() {}

ContextConstantPass::~ContextConstantPass() = default;

bool ContextConstantPass::Run(HIRBuilder* builder) {
  builder->context_load_expectations().clear();
  if (!cvars::propagate_context_constants || cvars::debug) {
    return true;
  }

  std::vector<Block*> blocks;
  std::unordered_map<const Block*, uint32_t> block_index;
  for (auto block = builder->first_block(); block; block = block->next) {
    block_index[block] = static_cast<uint32_t>(blocks.size());
    blocks.push_back(block);
  }
  const uint32_t block_count = static_cast<uint32_t>(blocks.size());
  if (block_count < 2) {
    return true;  // one block: ContextPromotionPass has done it
  }

  struct Slot {
    uint32_t offset;
    uint32_t size;
    TypeName type;
  };
  std::vector<Slot> slots;
  std::unordered_map<uint64_t, int16_t> slot_index;
  const auto slot_for = [&](uint32_t offset, TypeName type) -> int16_t {
    if (!IsConstantCandidate(offset, type)) {
      return -1;
    }
    const uint64_t key =
        (static_cast<uint64_t>(offset) << 8) | static_cast<uint64_t>(type);
    auto it = slot_index.find(key);
    if (it != slot_index.end()) {
      return it->second;
    }
    if (slots.size() >= 512) {
      return -1;
    }
    const int16_t index = static_cast<int16_t>(slots.size());
    slot_index.emplace(key, index);
    slots.push_back({offset, static_cast<uint32_t>(GetTypeSize(type)), type});
    return index;
  };

  std::vector<std::vector<Item>> code(block_count);
  bool any_constant_store = false;
  for (uint32_t b = 0; b < block_count; ++b) {
    for (auto i = blocks[b]->instr_head; i; i = i->next) {
      Item item = {i, Kind::kOther, -1, -1, false, false};
      const Opcode num = i->opcode->num;
      if (num == OPCODE_STORE_CONTEXT) {
        const uint32_t offset = static_cast<uint32_t>(i->src1.offset);
        item.slot = slot_for(offset, i->src2.value->type);
        item.kind = item.slot >= 0 ? Kind::kStore : Kind::kStoreOther;
        any_constant_store |= item.slot >= 0 && i->src2.value->IsConstant();
      } else if (num == OPCODE_LOAD_CONTEXT) {
        item.slot = slot_for(static_cast<uint32_t>(i->src1.offset),
                             i->dest->type);
        if (item.slot >= 0) {
          item.kind = Kind::kLoad;
        }
      } else if (num == OPCODE_BRANCH) {
        item.unconditional = true;
        item.ends = true;
      } else if (num == OPCODE_BRANCH_TRUE || num == OPCODE_BRANCH_FALSE) {
        // followed below
      } else if (num == OPCODE_CONTEXT_BARRIER) {
        if (!(i->flags & CONTEXT_BARRIER_FOR_BRANCH)) {
          item.kind = Kind::kForgetAll;
        }
      } else if (i->opcode->flags & OPCODE_FLAG_VOLATILE) {
        // Calls, returns, traps, the scheduler's check: whatever runs there
        // may store to any register.
        item.kind = Kind::kForgetAll;
        item.ends = num == OPCODE_RETURN ||
                    ((num == OPCODE_CALL || num == OPCODE_CALL_INDIRECT) &&
                     (i->flags & CALL_TAIL));
      }
      if (num == OPCODE_BRANCH || num == OPCODE_BRANCH_TRUE ||
          num == OPCODE_BRANCH_FALSE) {
        const Label* label =
            num == OPCODE_BRANCH ? i->src1.label : i->src2.label;
        auto it = (label && label->block) ? block_index.find(label->block)
                                          : block_index.end();
        if (it == block_index.end()) {
          return true;  // a branch this pass cannot follow
        }
        item.target = static_cast<int32_t>(it->second);
      }
      code[b].push_back(item);
    }
  }
  if (slots.empty() || !any_constant_store) {
    return true;
  }
  const size_t slot_count = slots.size();
  // Slots another slot's bytes overlap (a register written in two shapes).
  std::vector<std::vector<int16_t>> overlaps(slot_count);
  for (size_t a = 0; a < slot_count; ++a) {
    for (size_t c = 0; c < slot_count; ++c) {
      if (a != c && slots[a].offset < slots[c].offset + slots[c].size &&
          slots[c].offset < slots[a].offset + slots[a].size) {
        overlaps[a].push_back(static_cast<int16_t>(c));
      }
    }
  }

  typedef std::vector<Known> State;
  Known varies;
  varies.state = Known::kVaries;
  std::vector<State> in(block_count, State(slot_count));
  // Nothing is known on entry.
  in[0].assign(slot_count, varies);

  // One walk of a block from its in-state. With `rewrite`, loads of known
  // constants are replaced (or, in the verify mode, recorded).
  bool changed = false;
  const auto merge_into = [&](uint32_t target, const State& state) {
    State& dest = in[target];
    for (size_t n = 0; n < slot_count; ++n) {
      const Known met = Meet(dest[n], state[n]);
      if (!(met == dest[n])) {
        dest[n] = met;
        changed = true;
      }
    }
  };
  const auto walk = [&](uint32_t b, bool rewrite) {
    State state = in[b];
    bool ended = false;
    for (auto& item : code[b]) {
      Instr* i = item.instr;
      switch (item.kind) {
        case Kind::kLoad: {
          const Known& known = state[item.slot];
          if (rewrite && known.state == Known::kConstant) {
            translation_stats.context_constants_propagated.fetch_add(
                1, std::memory_order_relaxed);
            if (cvars::context_constant_verify) {
              builder->context_load_expectations()[i] = known.value;
            } else {
              Value* constant =
                  slots[item.slot].type == INT8_TYPE
                      ? builder->LoadConstantUint8(
                            static_cast<uint8_t>(known.value))
                      : builder->LoadConstantUint64(known.value);
              i->opcode = &OPCODE_ASSIGN_info;
              i->set_src1(constant);
            }
          }
          break;
        }
        case Kind::kStore: {
          Known& known = state[item.slot];
          const Value* value = i->src2.value;
          if (value->IsConstant()) {
            known.state = Known::kConstant;
            known.value = slots[item.slot].type == INT8_TYPE
                              ? static_cast<uint8_t>(value->constant.i8)
                              : static_cast<uint64_t>(value->constant.i64);
          } else {
            known = varies;
          }
          for (int16_t other : overlaps[item.slot]) {
            state[other] = varies;
          }
          break;
        }
        case Kind::kStoreOther: {
          const uint32_t offset = static_cast<uint32_t>(i->src1.offset);
          const uint32_t size =
              static_cast<uint32_t>(GetTypeSize(i->src2.value->type));
          for (size_t n = 0; n < slot_count; ++n) {
            if (slots[n].offset < offset + size &&
                offset < slots[n].offset + slots[n].size) {
              state[n] = varies;
            }
          }
          break;
        }
        case Kind::kForgetAll:
          state.assign(slot_count, varies);
          break;
        default:
          break;
      }
      if (!rewrite && item.target >= 0) {
        merge_into(static_cast<uint32_t>(item.target), state);
      }
      if (item.ends) {
        ended = true;
        break;
      }
    }
    if (!rewrite && !ended && b + 1 < block_count) {
      merge_into(b + 1, state);
    }
  };

  // To a fixed point: states only ever move from unreached to constant to
  // varies, so this ends.
  do {
    changed = false;
    for (uint32_t b = 0; b < block_count; ++b) {
      walk(b, false);
    }
  } while (changed);
  for (uint32_t b = 0; b < block_count; ++b) {
    walk(b, true);
  }
  return true;
}

}  // namespace passes
}  // namespace compiler
}  // namespace cpu
}  // namespace xe
