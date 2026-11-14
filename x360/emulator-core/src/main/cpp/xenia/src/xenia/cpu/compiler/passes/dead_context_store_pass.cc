/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * hakuX-Boosted: removes stores to guest registers and flag bits that no     *
 * path reads. Released under the BSD license.                                *
 ******************************************************************************
 */

#include "xenia/cpu/compiler/passes/dead_context_store_pass.h"

#include <algorithm>
#include <bitset>
#include <cstddef>
#include <unordered_map>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/cpu/compiler/translation_stats.h"
#include "xenia/cpu/hir/hir_builder.h"
#include "xenia/cpu/ppc/ppc_context.h"

DEFINE_int32(
    eliminate_dead_context_stores, 0,
    "Remove stores to guest state that every path overwrites before anything "
    "reads it, looking across the whole function instead of one block. "
    "0: off. 1: condition register bits and XER bits (a compare stores three "
    "bits and the branch after it reads one). 2: the general registers and "
    "CTR as well. Calls, returns, traps and the scheduler's check count as "
    "reading everything. Takes effect for functions translated after it is "
    "set.",
    "CPU");

DEFINE_bool(
    dead_context_store_verify, false,
    "Self-check for eliminate_dead_context_stores, for a checking run only: "
    "nothing is removed; a store the pass would have removed stores a marker "
    "value instead, and every load of such a slot checks for the marker. A "
    "line starting 'TranslatorVerify:' means a store the pass took for dead "
    "was read. ARM64 backend only.",
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

bool IsDeadStoreCandidateSlot(uint32_t offset, uint32_t size, int level) {
  if (level <= 0) {
    return false;
  }
  if (size == 1) {
    const uint32_t cr_base = static_cast<uint32_t>(offsetof(PPCContext, cr0));
    if (offset >= cr_base && offset < cr_base + 32) {
      return true;
    }
    return offset == offsetof(PPCContext, xer_ca) ||
           offset == offsetof(PPCContext, xer_ov) ||
           offset == offsetof(PPCContext, xer_so);
  }
  if (level >= 2 && size == 8) {
    const uint32_t gpr_base = static_cast<uint32_t>(offsetof(PPCContext, r));
    if (offset >= gpr_base && offset < gpr_base + 32 * 8 &&
        (offset - gpr_base) % 8 == 0) {
      // Not the stack pointer, r2 or the PCR pointer: host code reads those
      // at places this pass does not model (see GuestRegisterPinningPass).
      const uint32_t n = (offset - gpr_base) / 8;
      return n != 1 && n != 2 && n != 13;
    }
    return offset == offsetof(PPCContext, ctr);
  }
  return false;
}

namespace {

constexpr size_t kMaxSlots = 256;
using SlotSet = std::bitset<kMaxSlots>;

enum class Kind : uint8_t {
  kOther,
  kLoad,       // reads `slots`
  kStore,      // writes slot `slot` whole
  kReadsAll,   // code outside the function may read anything here
  kEnd,        // reads everything and control does not continue
};

struct Item {
  Instr* instr;
  Kind kind;
  int16_t slot;      // kStore: the slot; else -1
  int32_t target;    // branch target block or -1
  bool unconditional;
  uint32_t load_offset;
  uint32_t load_size;
};

}  // namespace

DeadContextStorePass::DeadContextStorePass() : CompilerPass() {}

DeadContextStorePass::~DeadContextStorePass() = default;

bool DeadContextStorePass::Run(HIRBuilder* builder) {
  const int level = cvars::eliminate_dead_context_stores;
  // With the debugger on, the context is what it shows.
  if (level <= 0 || cvars::debug) {
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
    // Within one block ContextPromotionPass has done this already.
    return true;
  }

  // The slots: every (offset, size) a candidate store in this function
  // writes.
  struct Slot {
    uint32_t offset;
    uint32_t size;
  };
  std::vector<Slot> slots;
  std::unordered_map<uint64_t, int16_t> slot_index;
  std::vector<std::vector<Item>> code(block_count);
  for (uint32_t b = 0; b < block_count; ++b) {
    for (auto i = blocks[b]->instr_head; i; i = i->next) {
      Item item = {i, Kind::kOther, -1, -1, false, 0, 0};
      const Opcode num = i->opcode->num;
      if (num == OPCODE_STORE_CONTEXT) {
        const uint32_t offset = static_cast<uint32_t>(i->src1.offset);
        const uint32_t size =
            static_cast<uint32_t>(GetTypeSize(i->src2.value->type));
        if (IsDeadStoreCandidateSlot(offset, size, level)) {
          const uint64_t key = (static_cast<uint64_t>(offset) << 8) | size;
          auto it = slot_index.find(key);
          if (it == slot_index.end()) {
            if (slots.size() >= kMaxSlots) {
              return true;  // too many to track: leave the function alone
            }
            it = slot_index.emplace(key, static_cast<int16_t>(slots.size()))
                     .first;
            slots.push_back({offset, size});
          }
          item.kind = Kind::kStore;
          item.slot = it->second;
        }
        // A store of another shape overwrites nothing this pass tracks as a
        // whole; it reads nothing either.
      } else if (num == OPCODE_LOAD_CONTEXT) {
        item.kind = Kind::kLoad;
        item.load_offset = static_cast<uint32_t>(i->src1.offset);
        item.load_size = static_cast<uint32_t>(GetTypeSize(i->dest->type));
      } else if (num == OPCODE_BRANCH) {
        item.unconditional = true;
      } else if (num == OPCODE_BRANCH_TRUE || num == OPCODE_BRANCH_FALSE) {
        // A branch within the function: followed below.
      } else if (num == OPCODE_RETURN ||
                 ((num == OPCODE_CALL || num == OPCODE_CALL_INDIRECT) &&
                  (i->flags & CALL_TAIL))) {
        item.kind = Kind::kEnd;
      } else if (num == OPCODE_CONTEXT_BARRIER) {
        // The barrier before a branch instruction is there for the per-block
        // passes (ContextBarrierFlags); any other says the context is
        // looked at.
        if (!(i->flags & CONTEXT_BARRIER_FOR_BRANCH)) {
          item.kind = Kind::kReadsAll;
        }
      } else if (i->opcode->flags & OPCODE_FLAG_VOLATILE) {
        // Calls, conditional returns, traps, debug breaks, the scheduler's
        // check and anything else marked as leaving the function's code.
        item.kind = Kind::kReadsAll;
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
  if (slots.empty()) {
    return true;
  }
  SlotSet all;
  for (size_t n = 0; n < slots.size(); ++n) {
    all.set(n);
  }
  // What a load of [offset, offset + size) reads: every slot it overlaps.
  const auto slots_read = [&](uint32_t offset, uint32_t size) {
    SlotSet read;
    for (size_t n = 0; n < slots.size(); ++n) {
      if (slots[n].offset < offset + size &&
          offset < slots[n].offset + slots[n].size) {
        read.set(n);
      }
    }
    return read;
  };

  const auto falls_through = [&](uint32_t b) {
    if (code[b].empty()) {
      return true;
    }
    const auto& last = code[b].back();
    return !(last.unconditional || last.kind == Kind::kEnd);
  };

  // Backward to a fixed point: live(slot) = some path from here reads the
  // slot before storing to it. Falling off the last block is a return.
  std::vector<SlotSet> live_in(block_count);
  const auto walk = [&](uint32_t b, bool act) {
    SlotSet state;
    if (falls_through(b)) {
      state = b + 1 < block_count ? live_in[b + 1] : all;
    }
    for (size_t n = code[b].size(); n-- > 0;) {
      Item& item = code[b][n];
      switch (item.kind) {
        case Kind::kStore:
          if (act && !state.test(item.slot)) {
            // Dead: every path from here stores the slot again first.
            item.kind = Kind::kOther;
            translation_stats.dead_context_stores_removed.fetch_add(
                1, std::memory_order_relaxed);
            if (cvars::dead_context_store_verify) {
              // Keep the store, with a value a load can recognise.
              if (slots[item.slot].size == 1) {
                item.instr->set_src2(
                    builder->LoadConstantUint8(kDeadStorePoison8));
              } else {
                item.instr->set_src2(
                    builder->LoadConstantUint64(kDeadStorePoison64));
              }
            } else {
              item.instr->UnlinkAndNOP();
            }
          } else {
            state.reset(item.slot);
          }
          break;
        case Kind::kLoad:
          state |= slots_read(item.load_offset, item.load_size);
          break;
        case Kind::kReadsAll:
          state = all;
          break;
        case Kind::kEnd:
          state = all;
          break;
        default:
          break;
      }
      if (item.target >= 0) {
        if (item.unconditional) {
          state = live_in[item.target];
        } else {
          state |= live_in[item.target];
        }
      }
    }
    return state;
  };
  for (bool changed = true; changed;) {
    changed = false;
    for (uint32_t b = block_count; b-- > 0;) {
      const SlotSet state = walk(b, false);
      if (state != live_in[b]) {
        live_in[b] = state;
        changed = true;
      }
    }
  }
  translation_stats.dead_context_stores_seen.fetch_add(
      [&] {
        uint64_t n = 0;
        for (const auto& block_code : code) {
          for (const auto& item : block_code) {
            n += item.kind == Kind::kStore;
          }
        }
        return n;
      }(),
      std::memory_order_relaxed);
  // Act on the fixed point. Removing a dead store changes no liveness: a
  // dead store's slot is not live after it, so the state before it is the
  // state after it either way.
  for (uint32_t b = 0; b < block_count; ++b) {
    walk(b, true);
  }
  return true;
}

}  // namespace passes
}  // namespace compiler
}  // namespace cpu
}  // namespace xe
